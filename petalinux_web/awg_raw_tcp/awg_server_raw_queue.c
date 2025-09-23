// awg_server_queue.c — Queue-mode AWG server (2 lists ping-pong, hold-last-frame)
// Transport: raw TCP
//
// Build (link with your mmap core):
//   gcc -O2 -Wall -pthread -o awg_server_queue awg_server_queue.c awg_core_mmap.c
//   // or if awg_core_mmap is a .so:
//   // gcc -O2 -Wall -pthread -o awg_server_queue awg_server_queue.c -L. -lawg_core_mmap -Wl,-rpath,'$ORIGIN'
//
// Run (root needed for /dev/mem):
//   sudo ./awg_server_queue 9100
//
// Depends on awg_core.h / awg_core_mmap.c:
//   int  awg_init(void);
//   int  awg_send_words32(const uint32_t *words32, int count);
//   void awg_close(void);
//
// Protocol (binary, big-endian; single-client at a time):
//   'Z' RESET           : clear both lists and stop playback. Leaves last HW output as-is.
//   'X' ABORT           : stop, clear both lists AND actively zero output (send gains=0 + COMMIT).
//   'I' INIT_LIST       : [u8 list_id(0/1)] [u32 max_frames_hint]
//   'B' PRELOAD_BEGIN   : [u8 list_id] [u32 total_frames]   (clears list and sets capacity)
//   'P' PRELOAD_PUSH    : [u8 list_id] [u16 count] [count * u32 word_be]   (append ONE frame)
//   'E' PRELOAD_END     : [u8 list_id]                     (mark READY; if id==0 and idle -> auto-start)
//   'T' SET_PERIOD_US   : [u32 period_us]                  (default 1000 us)
//   'Q' QUERY_STATUS    : server replies 16 bytes:
//        [u8 playing][u8 cur_list][u32 cur_frame][u32 list0_free][u32 list1_free]
//   'S' STATS           : server replies 32 bytes (counters; see do_stats_reply)
// All multi-byte fields are big-endian.
//
// Playback semantics:
// - Two lists (0/1). PRELOAD_* fills one list with a sequence of frames.
// - Player wakes every period_us; each frame is an array of 32-bit words sent via awg_send_words32().
// - When a list finishes:
//     * If the other list is READY: switch to it AND auto-clear the finished list.
//     * If not READY: stop (go idle) AND auto-clear the finished list.
// - While idle, no words are sent (HW holds its last value); when the other list becomes READY,
//   the player picks it up automatically (at the next tick after you send 'E').
//
// Notes:
// - Per-read timeouts drop the connection but keep already preloaded data.
// - Debug prints: build with -DDEBUG.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "awg_core.h"

// ---------- Debug macro ----------
#ifdef DEBUG
  #define DPRINT(fmt, ...) printf("[QSRV] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

// ---------- Tunables ----------
#define DEFAULT_PORT        9100
#define IO_TIMEOUT_MS       200     // per-read timeout (ms)
#define MAX_WORDS_PER_FRAME 64      // safety cap for one frame
#define GROW_WORDS_STEP     4096    // grow chunk (uint32_t words)

// ---------- Endian helpers ----------
static inline uint32_t be32_to_host(uint32_t x){ return ntohl(x); }
static inline uint16_t be16_to_host(uint16_t x){ return ntohs(x); }
static inline uint32_t host_to_be32(uint32_t x){ return htonl(x); }
static inline uint16_t host_to_be16(uint16_t x){ return htons(x); }

// ---------- Timers / IO ----------
static volatile int g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }

// read exactly n bytes with a per-read timeout; returns 1=ok, 0=peer closed, -2=timeout, -1=error
static int read_n_timeout(int fd, void *buf, size_t n, int timeout_ms){
  uint8_t *p = (uint8_t*)buf;
  size_t got = 0;
  while(got < n){
    struct pollfd pfd = {.fd=fd,.events=POLLIN};
    int pr = poll(&pfd,1,timeout_ms);
    if (pr==0) return -2;                // timeout
    if (pr<0){ if(errno==EINTR) continue; return -1; }
    if (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) return 0; // peer closed
    ssize_t r = recv(fd, p+got, n-got, 0);
    if (r==0) return 0;
    if (r<0){
      if(errno==EINTR) continue;
      if(errno==EAGAIN||errno==EWOULDBLOCK) continue;
      return -1;
    }
    got += (size_t)r;
  }
  return 1;
}

// ---------- Data model ----------
typedef struct {
  // frame meta
  uint32_t *offsets;     // per-frame offset into words[]
  uint16_t *counts;      // per-frame word count
  uint32_t  max_frames;  // hint from INIT (optional)
  uint32_t  total_frames;// from PRELOAD_BEGIN
  uint32_t  loaded_frames;// number of frames pushed in current load
  bool      ready;       // set by PRELOAD_END

  // words storage (all frames flattened)
  uint32_t *words;
  uint32_t  words_cap;   // capacity in words
  uint32_t  words_used;  // used words
} awg_list_t;

typedef struct {
  pthread_mutex_t mtx;
  pthread_t       th;
  bool            th_running;

  // two lists
  awg_list_t list[2];

  // player state
  bool     playing;
  int      cur_list;        // 0 or 1
  uint32_t cur_frame;       // frame index of current list
  int      next_list;       // other list id
  uint32_t period_us;       // tick period, default 1000 us

  // stats
  uint64_t bytes_rx;
  uint64_t frames_pushed;
  uint64_t switches;
  uint64_t holds;
  uint64_t resets;
  uint64_t aborts;

} awg_srv_t;

static awg_srv_t G;

// ---------- List helpers ----------
static void free_list(awg_list_t *L){
  free(L->offsets); L->offsets=NULL;
  free(L->counts);  L->counts =NULL;
  free(L->words);   L->words  =NULL;
  L->max_frames=L->total_frames=L->loaded_frames=0;
  L->words_cap=L->words_used=0;
  L->ready=false;
}

// clear by id
static void clear_list_id(int id){
  if (id < 0 || id > 1) return;
  awg_list_t *L = &G.list[id];
  free_list(L);
}

static bool ensure_frame_meta(awg_list_t *L, uint32_t total_frames){
  L->offsets = (uint32_t*)realloc(L->offsets, total_frames*sizeof(uint32_t));
  L->counts  = (uint16_t*)realloc(L->counts,  total_frames*sizeof(uint16_t));
  if (!L->offsets || !L->counts) return false;
  L->total_frames = total_frames;
  L->loaded_frames=0;
  L->ready=false;
  return true;
}

static bool ensure_words_cap(awg_list_t *L, uint32_t need_more){
  if (L->words_used + need_more <= L->words_cap) return true;
  uint32_t want = L->words_used + need_more;
  uint32_t cap  = L->words_cap ? L->words_cap : GROW_WORDS_STEP;
  while (cap < want) cap += GROW_WORDS_STEP;
  uint32_t *nw = (uint32_t*)realloc(L->words, cap*sizeof(uint32_t));
  if (!nw) return false;
  L->words = nw; L->words_cap = cap;
  return true;
}

// push one frame (count words), words are host-endian here
static bool push_frame(awg_list_t *L, const uint32_t *w, uint16_t count){
  if (L->loaded_frames >= L->total_frames) return false;
  if (count==0 || count>MAX_WORDS_PER_FRAME) return false;
  if (!ensure_words_cap(L, count)) return false;

  uint32_t off = L->words_used;
  memcpy(&L->words[off], w, count*sizeof(uint32_t));
  L->words_used += count;

  L->offsets[L->loaded_frames] = off;
  L->counts [L->loaded_frames] = count;
  L->loaded_frames++;
  return true;
}

static void init_lists(){
  memset(&G, 0, sizeof(G));
  pthread_mutex_init(&G.mtx, NULL);
  G.period_us = 1000; // default
  G.cur_list = 0; G.next_list = 1;
}

// ---------- ABORT helper: actively zero output ----------
static void send_zero_output(void){
  // Build: write GAIN=0 for A.tone0..7 and B.tone0..7, then COMMIT
  // Word format (must match your RTL):
  //   [31:28]=0x2 (GAIN), [27]=ch, [26:24]=tone, [19:0]=gain20
  //   COMMIT: [31:28]=0xF
  uint32_t words[16 + 1];
  int idx = 0;

  for (int ch = 0; ch < 2; ++ch) {
    for (int tone = 0; tone < 8; ++tone) {
      uint32_t w = (0x2u << 28)
                 | ((uint32_t)(ch & 1) << 27)
                 | ((uint32_t)(tone & 7) << 24)
                 | 0u;                     // gain20 = 0
      words[idx++] = w;
    }
  }
  words[idx++] = (0xFu << 28);             // COMMIT
  awg_send_words32(words, idx);
}

// ---------- Player thread ----------
static void *player_thread(void *arg){
  (void)arg;

  // steady timer
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);

  while (!g_stop){
    // sleep until next tick
    uint32_t us;
    pthread_mutex_lock(&G.mtx);
    us = G.period_us;
    pthread_mutex_unlock(&G.mtx);

    ts.tv_nsec += (long)us * 1000L;
    while (ts.tv_nsec >= 1000000000L){ ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

    pthread_mutex_lock(&G.mtx);

    if (!G.playing){
      pthread_mutex_unlock(&G.mtx);
      continue;
    }

    awg_list_t *CL = &G.list[G.cur_list];
    awg_list_t *NL = &G.list[G.next_list];

    if (G.cur_frame < CL->loaded_frames){
      // play current frame
      uint32_t off = CL->offsets[G.cur_frame];
      uint16_t cnt = CL->counts [G.cur_frame];
      uint32_t *w  = &CL->words[off];
      pthread_mutex_unlock(&G.mtx);
      // send out of lock (avoid long critical section)
      awg_send_words32(w, cnt);
      pthread_mutex_lock(&G.mtx);

      G.cur_frame++;
      if (G.cur_frame >= CL->loaded_frames){
        // finished this list
        if (NL->ready && NL->loaded_frames > 0){
          int prev = G.cur_list;
          int tmp  = G.cur_list; G.cur_list = G.next_list; G.next_list = tmp;
          G.cur_frame = 0;
          G.switches++;
          clear_list_id(prev); // auto-clear finished list
          DPRINT("switch to list=%d (cleared prev=%d)\n", G.cur_list, prev);
        }else{
          // no next list ready -> go idle and clear finished list
          int prev = G.cur_list;
          G.playing   = false;
          G.cur_frame = 0;
          clear_list_id(prev);
          G.holds++;
          DPRINT("end of list=%d, no next ready -> stop+clear (idle)\n", prev);
        }
      }
    }else{
      // no frames left in current list
      if (NL->ready && NL->loaded_frames > 0){
        int prev = G.cur_list;
        int tmp  = G.cur_list; G.cur_list = G.next_list; G.next_list = tmp;
        G.cur_frame = 0;
        G.switches++;
        clear_list_id(prev);
        DPRINT("switch (idle) to list=%d (cleared prev=%d)\n", G.cur_list, prev);
      } else {
        // stay idle; do not send anything
        G.holds++;
      }
    }

    pthread_mutex_unlock(&G.mtx);
  }
  return NULL;
}

static void stop_player(){
  if (G.th_running){
    g_stop = 1;
    pthread_join(G.th, NULL);
    G.th_running = false;
  }
}

static void start_player_if_needed(){
  if (!G.th_running){
    g_stop = 0;
    pthread_create(&G.th, NULL, player_thread, NULL);
    G.th_running = true;
  }
}

// ---------- Protocol handlers ----------
static void do_reset(){
  pthread_mutex_lock(&G.mtx);
  G.playing=false;
  G.cur_list=0; G.next_list=1; G.cur_frame=0;
  free_list(&G.list[0]);
  free_list(&G.list[1]);
  G.resets++;
  pthread_mutex_unlock(&G.mtx);
  DPRINT("RESET\n");
}

static void do_abort(){
  // stop + clear + actively zero output
  pthread_mutex_lock(&G.mtx);
  G.playing=false;
  G.cur_list=0; G.next_list=1; G.cur_frame=0;
  free_list(&G.list[0]);
  free_list(&G.list[1]);
  G.aborts++;
  pthread_mutex_unlock(&G.mtx);

  send_zero_output(); // push gains=0 + COMMIT
  DPRINT("ABORT (zero output)\n");
}

static bool do_init_list(uint8_t list_id, uint32_t max_frames){
  if (list_id>1) return false;
  pthread_mutex_lock(&G.mtx);
  free_list(&G.list[list_id]);
  G.list[list_id].max_frames=max_frames;
  pthread_mutex_unlock(&G.mtx);
  DPRINT("INIT list=%u max_frames=%u\n", list_id, max_frames);
  return true;
}

static bool do_preload_begin(uint8_t list_id, uint32_t total_frames){
  if (list_id>1 || total_frames==0) return false;
  pthread_mutex_lock(&G.mtx);
  awg_list_t *L=&G.list[list_id];
  free(L->offsets); free(L->counts); L->offsets=NULL; L->counts=NULL;
  L->words_used=0; // keep words capacity for reuse
  L->ready=false;
  bool ok = ensure_frame_meta(L, total_frames);
  pthread_mutex_unlock(&G.mtx);
  DPRINT("BEGIN list=%u total_frames=%u (%s)\n", list_id, total_frames, ok?"ok":"OOM");
  return ok;
}

static bool do_preload_push(int fd){
    // header: [u8 list_id][u16 count][count*u32 big-endian]
    uint8_t hdr[3];
    int rc = read_n_timeout(fd, hdr, 3, IO_TIMEOUT_MS);
    if (rc <= 0) return false;

    uint8_t  list_id = hdr[0];
    uint16_t count   = be16_to_host(*(uint16_t*)&hdr[1]);
    if (list_id > 1 || count == 0 || count > MAX_WORDS_PER_FRAME) return false;

    uint32_t tmp[MAX_WORDS_PER_FRAME];
    if (read_n_timeout(fd, tmp, count * 4, IO_TIMEOUT_MS) <= 0) return false;
    for (int i = 0; i < count; ++i) tmp[i] = be32_to_host(tmp[i]);

    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    bool ok = push_frame(L, tmp, count);

    if (ok && L->loaded_frames == L->total_frames) {
        // ---- Auto finalize when reached total_frames ----
        L->ready = true;
        if (!G.playing && list_id == 0) {
            G.cur_list = 0; G.next_list = 1; G.cur_frame = 0; G.playing = true;
            pthread_mutex_unlock(&G.mtx);
            start_player_if_needed();
            DPRINT("AUTO-END list=0 -> AUTO START\n");
            return true;
        }
        DPRINT("AUTO-END list=%u (ready)\n", list_id);
    }
    pthread_mutex_unlock(&G.mtx);
    return ok;
}

static bool do_preload_end(uint8_t list_id){
  if (list_id>1) return false;
  pthread_mutex_lock(&G.mtx);
  awg_list_t *L=&G.list[list_id];
  if (L->loaded_frames==0){ pthread_mutex_unlock(&G.mtx); return false; }
  L->ready = true;

  // Auto-start: if idle and list_id==0 -> start from list0
  if (!G.playing && list_id==0){
    G.cur_list = 0; G.next_list=1; G.cur_frame=0; G.playing=true;
    pthread_mutex_unlock(&G.mtx);
    start_player_if_needed();
    DPRINT("END list=0 -> AUTO START\n");
    return true;
  }
  pthread_mutex_unlock(&G.mtx);
  DPRINT("END list=%u (ready)\n", list_id);
  return true;
}

static void do_set_period(uint32_t period_us){
  if (period_us==0) period_us=1;
  pthread_mutex_lock(&G.mtx);
  G.period_us = period_us;
  pthread_mutex_unlock(&G.mtx);
  DPRINT("SET_PERIOD %u us\n", period_us);
}

static void do_query(int fd){
  // reply 16 bytes: [u8 playing][u8 cur_list][u32 cur_frame][u32 list0_free][u32 list1_free]
  uint8_t out[16]={0};
  pthread_mutex_lock(&G.mtx);
  out[0] = G.playing ? 1:0;
  out[1] = (uint8_t)G.cur_list;
  uint32_t curf = G.cur_frame;
  uint32_t free0 = (G.list[0].total_frames>G.list[0].loaded_frames)
                   ? (G.list[0].total_frames - G.list[0].loaded_frames) : 0;
  uint32_t free1 = (G.list[1].total_frames>G.list[1].loaded_frames)
                   ? (G.list[1].total_frames - G.list[1].loaded_frames) : 0;
  pthread_mutex_unlock(&G.mtx);

  uint32_t be_curf  = host_to_be32(curf);
  uint32_t be_free0 = host_to_be32(free0);
  uint32_t be_free1 = host_to_be32(free1);
  memcpy(&out[2],  &be_curf,  4);
  memcpy(&out[6],  &be_free0, 4);
  memcpy(&out[10], &be_free1, 4);
  // last 4 bytes left as zero
  send(fd, out, sizeof(out), MSG_NOSIGNAL);
}

static void do_stats_reply(int fd){
  // 32-byte stats reply (big-endian u64 counters):
  // [0:8) bytes_rx
  // [8:16) frames_pushed
  // [16:24) switches
  // [24:32) holds
  uint8_t buf[32];
  pthread_mutex_lock(&G.mtx);
  uint64_t a=G.bytes_rx, b=G.frames_pushed, c=G.switches, d=G.holds;
  pthread_mutex_unlock(&G.mtx);

  // host->big endian for u64
  for(int i=0;i<8;i++) buf[i]      = (uint8_t)((a >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[8+i]    = (uint8_t)((b >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[16+i]   = (uint8_t)((c >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[24+i]   = (uint8_t)((d >> (56 - 8*i)) & 0xFF);

  send(fd, buf, sizeof(buf), MSG_NOSIGNAL);
}

// ---------- Connection loop ----------
static void serve_client(int fd){
  int one=1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){256*1024}, sizeof(int));
  DPRINT("client connected\n");

  for(;;){
    uint8_t op;
    int rc = read_n_timeout(fd, &op, 1, IO_TIMEOUT_MS);
    if (rc==0){ DPRINT("peer closed\n"); break; }
    if (rc<0){ DPRINT("timeout/error on op\n"); break; }

    switch(op){
      case 'Z': // RESET
        do_reset();
        break;

      case 'X': // ABORT (stop+clear+zero output)
        do_abort();
        break;

      case 'I': { // INIT_LIST
        uint8_t buf[5];
        if (read_n_timeout(fd, buf, 5, IO_TIMEOUT_MS)<=0) goto drop;
        uint8_t  id = buf[0];
        uint32_t mf; memcpy(&mf, &buf[1], 4); mf = be32_to_host(mf);
        if (!do_init_list(id, mf)) goto drop;
        } break;

      case 'B': { // PRELOAD_BEGIN
        uint8_t buf[5];
        if (read_n_timeout(fd, buf, 5, IO_TIMEOUT_MS)<=0) goto drop;
        uint8_t  id = buf[0];
        uint32_t tf; memcpy(&tf, &buf[1], 4); tf = be32_to_host(tf);
        if (!do_preload_begin(id, tf)) goto drop;
        } break;

      case 'P': // PRELOAD_PUSH (one frame)
        if (!do_preload_push(fd)) goto drop;
        break;

      case 'E': { // PRELOAD_END
        uint8_t id;
        if (read_n_timeout(fd, &id, 1, IO_TIMEOUT_MS)<=0) goto drop;
        if (!do_preload_end(id)) goto drop;
        } break;

      case 'T': { // SET_PERIOD_US
        uint32_t bep;
        if (read_n_timeout(fd, &bep, 4, IO_TIMEOUT_MS)<=0) goto drop;
        do_set_period(be32_to_host(bep));
        } break;

      case 'Q': // QUERY_STATUS
        do_query(fd);
        break;

      case 'S': // STATS
        do_stats_reply(fd);
        break;

      default:
        DPRINT("unknown op=0x%02X\n", op);
        goto drop;
    }
  }
  close(fd);
  DPRINT("client disconnected\n");
  return;

drop:
  DPRINT("protocol error -> drop\n");
  close(fd);
}

int main(int argc, char **argv){
  int port = (argc>=2)? atoi(argv[1]) : DEFAULT_PORT;

  signal(SIGINT, on_sig);
  signal(SIGTERM,on_sig);

  if (awg_init()!=0){ fprintf(stderr,"awg_init failed\n"); return 1; }
  init_lists();
  start_player_if_needed(); // thread runs idle until G.playing=true

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv<0){ perror("socket"); return 2; }

  int one=1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(srv, SOL_SOCKET, SO_RCVBUF, &(int){256*1024}, sizeof(int));

  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=htonl(INADDR_ANY);
  addr.sin_port=htons((uint16_t)port);

  if (bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); return 3; }
  if (listen(srv,1)<0){ perror("listen"); return 4; }

  printf("[QSRV] listening on 0.0.0.0:%d (queue-mode, auto-clear, idle-at-end)\n", port);

  while(!g_stop){
    struct sockaddr_in cli; socklen_t cl=sizeof(cli);
    int fd = accept(srv,(struct sockaddr*)&cli,&cl);
    if (fd<0){ if(errno==EINTR) continue; perror("accept"); break; }
    serve_client(fd);
  }

  close(srv);
  awg_close();
  return 0;
}