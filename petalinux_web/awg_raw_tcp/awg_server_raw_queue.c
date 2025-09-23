// awg_server_queue.c — Queue-mode AWG server (2 lists ping-pong, hold-last-frame)
//
// This file implements the queue-mode server as a module. It is started and
// managed by awg_server_main.c. This version includes fixes for graceful
// shutdown, statistics counters, and portability (unaligned memory access).
//
// Exported API:
//   int  start_queue_server(unsigned short port);
//   void stop_queue_server(void);
//
// Build (as part of the main server):
//   gcc -O2 -pthread -Wall -o awg_server \
//       awg_server_main.c awg_server_direct.c awg_server_queue.c awg_core_mmap.c
//
// Run (via the main launcher):
//   sudo ./awg_server
//
// Depends on awg_core.h:
//   int  awg_send_words32(const uint32_t *words32, int count);
//
// Protocol (binary, big-endian; single-client at a time):
//   'Z' RESET           : clear both lists and stop playback. Leaves last HW output as-is.
//   'X' ABORT           : stop, clear both lists AND actively zero output (send gains=0 + COMMIT).
//   'I' INIT_LIST       : [u8 list_id(0/1)] [u32 max_frames_hint]
//   'B' PRELOAD_BEGIN   : [u8 list_id] [u32 total_frames]   (clears list and sets capacity)
//   'P' PRELOAD_PUSH    : [u8 list_id] [u16 count] [count * u32 word_be]   (append ONE frame)
//   'E' PRELOAD_END     : [u8 list_id]                     (mark READY; if id==0 and idle -> auto-start)
//   'T' SET_PERIOD_US   : [u32 period_us]                  (default 1000 us)
//   'Q' QUERY_STATUS    : server replies 16 bytes.
//   'S' STATS           : server replies 32 bytes.
// All multi-byte fields are big-endian.
//
// Playback semantics:
// - Two lists (0/1). PRELOAD_* fills one list with a sequence of frames.
// - Player wakes every period_us; each frame is sent via awg_send_words32().
// - When a list finishes:
//     * If the other list is READY: switch to it AND auto-clear the finished list.
//     * If not READY: stop (go idle) AND auto-clear the finished list.
// - While idle, no words are sent (HW holds its last value); when the other list becomes
//   READY, the player picks it up automatically.

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

#ifdef DEBUG
  #define DPRINT(fmt, ...) printf("[QSRV] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

#define IO_TIMEOUT_MS       200
#define MAX_WORDS_PER_FRAME 64
#define GROW_WORDS_STEP     4096

// --- Static functions ---
static inline uint32_t be32_to_host(uint32_t x){ return ntohl(x); }
static inline uint16_t be16_to_host(uint16_t x){ return ntohs(x); }
static inline uint32_t host_to_be32(uint32_t x){ return htonl(x); }
static inline uint16_t host_to_be16(uint16_t x){ return htons(x); }

// --- Data models ---
typedef struct {
  uint32_t *offsets;
  uint16_t *counts;
  uint32_t  max_frames;
  uint32_t  total_frames;
  uint32_t  loaded_frames;
  bool      ready;
  uint32_t *words;
  uint32_t  words_cap;
  uint32_t  words_used;
} awg_list_t;

typedef struct {
  pthread_mutex_t mtx;
  pthread_t       player_thread_h;
  bool            player_thread_running;
  awg_list_t      list[2];
  bool            playing;
  int             cur_list;
  uint32_t        cur_frame;
  int             next_list;
  uint32_t        period_us;
  uint64_t        bytes_rx;
  uint64_t        frames_pushed;
  uint64_t        switches;
  uint64_t        holds;
  uint64_t        resets;
  uint64_t        aborts;
} awg_srv_t;

// --- Global state for this module ---
static awg_srv_t G;
static volatile int g_stop_queue = 0;
static int g_listen_queue = -1;
static pthread_t g_accept_thread_queue;
static bool g_accept_thread_running = false; // [FIX#2] Boolean flag for safely joining the thread

// --- Function implementations ---

static int read_n_timeout(int fd, void *buf, size_t n, int timeout_ms){
  uint8_t *p = (uint8_t*)buf;
  size_t got = 0;
  while(got < n){
    struct pollfd pfd = {.fd=fd,.events=POLLIN};
    int pr = poll(&pfd,1,timeout_ms);
    if (pr==0) return -2;
    if (pr<0){ if(errno==EINTR) continue; return -1; }
    if (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) return 0;
    ssize_t r = recv(fd, p+got, n-got, 0);
    if (r==0) return 0;
    if (r<0){ if(errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK) continue; return -1; }
    got += (size_t)r;
  }
  return 1;
}

static void free_list(awg_list_t *L){
  free(L->offsets); L->offsets=NULL;
  free(L->counts);  L->counts =NULL;
  free(L->words);   L->words  =NULL;
  L->max_frames=L->total_frames=L->loaded_frames=0;
  L->words_cap=L->words_used=0;
  L->ready=false;
}

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
  G.period_us = 1000;
  G.cur_list = 0; G.next_list = 1;
}

static void send_zero_output(void){
  uint32_t words[17];
  int idx = 0;
  for (int ch = 0; ch < 2; ++ch) {
    for (int tone = 0; tone < 8; ++tone) {
      words[idx++] = (0x2u << 28) | ((uint32_t)(ch & 1) << 27) | ((uint32_t)(tone & 7) << 24);
    }
  }
  words[idx++] = (0xFu << 28);
  awg_send_words32(words, idx);
}

static void *player_thread(void *arg){
  (void)arg;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  while (!g_stop_queue){
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
      uint32_t off = CL->offsets[G.cur_frame];
      uint16_t cnt = CL->counts [G.cur_frame];
      uint32_t *w  = &CL->words[off];
      pthread_mutex_unlock(&G.mtx);
      awg_send_words32(w, cnt);
      pthread_mutex_lock(&G.mtx);
      G.cur_frame++;
      if (G.cur_frame >= CL->loaded_frames){
        if (NL->ready && NL->loaded_frames > 0){
          int tmp  = G.cur_list; G.cur_list = G.next_list; G.next_list = tmp;
          G.cur_frame = 0;
          G.switches++;
          free_list(CL);
        }else{
          G.playing   = false;
          G.cur_frame = 0;
          free_list(CL);
          G.holds++;
        }
      }
    }else{
      if (NL->ready && NL->loaded_frames > 0){
        int tmp  = G.cur_list; G.cur_list = G.next_list; G.next_list = tmp;
        G.cur_frame = 0;
        G.switches++;
        free_list(CL);
      } else {
        G.holds++;
      }
    }
    pthread_mutex_unlock(&G.mtx);
  }
  return NULL;
}

static void start_player_if_needed(){
  if (!G.player_thread_running){
    pthread_create(&G.player_thread_h, NULL, player_thread, NULL);
    G.player_thread_running = true;
  }
}

// (The `do_*` helper functions are not shown here for brevity,
// but their corrected `memcpy` versions are included below in `serve_client`)

static bool do_preload_push(int fd){
    uint8_t hdr[3];
    if (read_n_timeout(fd, hdr, 3, IO_TIMEOUT_MS) <= 0) return false;

    uint8_t  list_id = hdr[0];
    uint16_t be_count;
    // [FIX#1] Use memcpy to avoid unaligned memory access and strict-aliasing issues.
    memcpy(&be_count, &hdr[1], sizeof(be_count));
    uint16_t count = be16_to_host(be_count);

    if (list_id > 1 || count == 0 || count > MAX_WORDS_PER_FRAME) return false;

    uint32_t tmp[MAX_WORDS_PER_FRAME];
    if (read_n_timeout(fd, tmp, count * 4, IO_TIMEOUT_MS) <= 0) return false;
    for (int i = 0; i < count; ++i) tmp[i] = be32_to_host(tmp[i]);

    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    bool ok = push_frame(L, tmp, count);
    if (ok) {
        // Correctly update statistics counters
        G.frames_pushed++;
        G.bytes_rx += (uint64_t)count * 4 + 3; // 3 bytes for header
    }

    if (ok && L->loaded_frames == L->total_frames) {
        L->ready = true;
        if (!G.playing && list_id == 0) {
            G.cur_list = 0; G.next_list = 1; G.cur_frame = 0; G.playing = true;
            pthread_mutex_unlock(&G.mtx);
            start_player_if_needed();
            return true;
        }
    }
    pthread_mutex_unlock(&G.mtx);
    return ok;
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

// ... definitions for do_reset, do_abort, do_init_list, etc. ...
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
  pthread_mutex_lock(&G.mtx);
  G.playing=false;
  G.cur_list=0; G.next_list=1; G.cur_frame=0;
  free_list(&G.list[0]);
  free_list(&G.list[1]);
  G.aborts++;
  pthread_mutex_unlock(&G.mtx);
  send_zero_output();
  DPRINT("ABORT (zero output)\n");
}

static bool do_init_list(uint8_t list_id, uint32_t max_frames){
  if (list_id>1) return false;
  pthread_mutex_lock(&G.mtx);
  free_list(&G.list[list_id]);
  G.list[list_id].max_frames = max_frames;
  pthread_mutex_unlock(&G.mtx);
  DPRINT("INIT list=%u max_frames=%u\n", list_id, max_frames);
  return true;
}

static bool do_preload_begin(uint8_t list_id, uint32_t total_frames){
  if (list_id>1 || total_frames==0) return false;
  pthread_mutex_lock(&G.mtx);
  awg_list_t *L=&G.list[list_id];
  free(L->offsets); free(L->counts); L->offsets=NULL; L->counts=NULL;
  L->words_used=0;           // 保留 words buffer 容量以重用
  L->ready=false;
  bool ok = true;
  L->offsets = (uint32_t*)realloc(L->offsets, total_frames*sizeof(uint32_t));
  L->counts  = (uint16_t*)realloc(L->counts,  total_frames*sizeof(uint16_t));
  if (!L->offsets || !L->counts) ok=false;
  L->total_frames  = ok ? total_frames : 0;
  L->loaded_frames = 0;
  pthread_mutex_unlock(&G.mtx);
  DPRINT("BEGIN list=%u total_frames=%u (%s)\n", list_id, total_frames, ok?"ok":"OOM");
  return ok;
}

static bool do_preload_end(uint8_t list_id){
  if (list_id>1) return false;
  pthread_mutex_lock(&G.mtx);
  awg_list_t *L=&G.list[list_id];
  if (L->loaded_frames==0){ pthread_mutex_unlock(&G.mtx); return false; }
  L->ready = true;
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

static void do_stats_reply(int fd){
  uint8_t buf[32];
  pthread_mutex_lock(&G.mtx);
  uint64_t a=G.bytes_rx, b=G.frames_pushed, c=G.switches, d=G.holds;
  pthread_mutex_unlock(&G.mtx);
  for(int i=0;i<8;i++) buf[i]    = (uint8_t)((a >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[8+i]  = (uint8_t)((b >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[16+i] = (uint8_t)((c >> (56 - 8*i)) & 0xFF);
  for(int i=0;i<8;i++) buf[24+i] = (uint8_t)((d >> (56 - 8*i)) & 0xFF);
  send(fd, buf, sizeof(buf), MSG_NOSIGNAL);
}


static void do_query(int fd){
  uint8_t out[16]={0};
  pthread_mutex_lock(&G.mtx);
  out[0] = G.playing ? 1:0;
  out[1] = (uint8_t)G.cur_list;
  uint32_t curf = G.cur_frame;
  uint32_t free0 = (G.list[0].total_frames > G.list[0].loaded_frames) ? (G.list[0].total_frames - G.list[0].loaded_frames) : 0;
  uint32_t free1 = (G.list[1].total_frames > G.list[1].loaded_frames) ? (G.list[1].total_frames - G.list[1].loaded_frames) : 0;
  pthread_mutex_unlock(&G.mtx);

  // [FIX#1] Use memcpy for safe, portable data packing.
  uint32_t be_curf  = host_to_be32(curf);
  uint32_t be_free0 = host_to_be32(free0);
  uint32_t be_free1 = host_to_be32(free1);
  memcpy(&out[2], &be_curf, sizeof(be_curf));
  memcpy(&out[6], &be_free0, sizeof(be_free0));
  memcpy(&out[10], &be_free1, sizeof(be_free1));

  send(fd, out, sizeof(out), MSG_NOSIGNAL);
}

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
      case 'Z': do_reset(); break;
      case 'X': do_abort(); break;
      case 'I': {
        uint8_t b[5];
        if(read_n_timeout(fd,b,5,IO_TIMEOUT_MS)<=0) goto drop;
        uint32_t be_mf;
        // [FIX#1] Use memcpy to avoid unaligned access.
        memcpy(&be_mf, &b[1], sizeof(be_mf));
        if(!do_init_list(b[0], be32_to_host(be_mf))) goto drop;
      } break;
      case 'B': {
        uint8_t b[5];
        if(read_n_timeout(fd,b,5,IO_TIMEOUT_MS)<=0) goto drop;
        uint32_t be_tf;
        // [FIX#1] Use memcpy to avoid unaligned access.
        memcpy(&be_tf, &b[1], sizeof(be_tf));
        if(!do_preload_begin(b[0], be32_to_host(be_tf))) goto drop;
      } break;
      case 'P': if (!do_preload_push(fd)) goto drop; break;
      case 'E': { uint8_t id; if(read_n_timeout(fd,&id,1,IO_TIMEOUT_MS)<=0) goto drop; if(!do_preload_end(id)) goto drop; } break;
      case 'T': { uint32_t p; if(read_n_timeout(fd,&p,4,IO_TIMEOUT_MS)<=0) goto drop; do_set_period(be32_to_host(p)); } break;
      case 'Q': do_query(fd); break;
      case 'S': do_stats_reply(fd); break;
      default: DPRINT("unknown op=0x%02X\n", op); goto drop;
    }
  }
drop:
  close(fd);
  DPRINT("client disconnected\n");
}

// Accept loop for the queue server
static void* accept_loop_queue(void *arg) {
    (void)arg;
    while(!g_stop_queue){
        struct sockaddr_in cli; socklen_t cl=sizeof(cli);
        int fd = accept(g_listen_queue,(struct sockaddr*)&cli,&cl);
        if (fd<0){
            if (g_stop_queue) break; // Normal stop
            if (errno==EINTR) continue;
            perror("[QSRV] accept");
            continue;
        }
        // Queue mode is single-client, so we serve it directly in this thread
        serve_client(fd);
    }
    return NULL;
}

// ---- Public API Functions ----

int start_queue_server(unsigned short port){
  g_stop_queue = 0;
  init_lists();
  start_player_if_needed();

  g_listen_queue = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_queue < 0){ perror("[QSRV] socket"); return -1; }

  int one=1;
  setsockopt(g_listen_queue, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(g_listen_queue, SOL_SOCKET, SO_RCVBUF, &(int){256*1024}, sizeof(int));

  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET;
  addr.sin_addr.s_addr=htonl(INADDR_ANY);
  addr.sin_port=htons((uint16_t)port);

  if (bind(g_listen_queue,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("[QSRV] bind"); close(g_listen_queue); return -2; }
  if (listen(g_listen_queue,1)<0){ perror("[QSRV] listen"); close(g_listen_queue); return -3; }

  if (pthread_create(&g_accept_thread_queue, NULL, accept_loop_queue, NULL) != 0) {
      perror("[QSRV] pthread_create for accept loop");
      close(g_listen_queue);
      return -4;
  }
  
  // [FIX#2] Set the running flag only after a successful thread creation.
  g_accept_thread_running = true;

  printf("[QSRV] listening on %u (queue-mode, single-client)\n", port);
  return 0;
}

void stop_queue_server(void){
    g_stop_queue = 1;

    if (g_listen_queue >= 0){
        close(g_listen_queue);
        g_listen_queue = -1;
    }
    
    // [FIX#2] Use the boolean flag for a reliable check before joining.
    if (g_accept_thread_running) {
        pthread_join(g_accept_thread_queue, NULL);
        g_accept_thread_running = false;
    }

    if (G.player_thread_running) {
        pthread_join(G.player_thread_h, NULL);
        G.player_thread_running = false;
    }
}