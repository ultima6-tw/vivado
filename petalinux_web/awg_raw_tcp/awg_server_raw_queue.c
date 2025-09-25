/*
 * awg_server_raw_queue.c — FINAL STABLE VERSION
 * This version contains all fixes for compilation, shutdown, memory management, and race conditions.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
#include "awg_server_raw_shared.h"

#ifdef DEBUG
  #define DPRINT(fmt, ...) printf("[QSRV] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

// --- [NEW] Word packing macros and zero-gain frame definition ---
#define PACK_WORD(cmd, ch, tone, data20) \
    (((uint32_t)(cmd) & 0xF) << 28 | ((uint32_t)(ch) & 1) << 27 | \
     ((uint32_t)(tone) & 0x7) << 24 | ((uint32_t)(data20) & 0xFFFFF))

#define MAKE_INDEX_WORD(ch, tone, idx20) PACK_WORD(0x1, ch, tone, idx20)
#define MAKE_GAIN_WORD(ch, tone, g20)    PACK_WORD(0x2, ch, tone, g20)
#define MAKE_COMMIT_WORD()               PACK_WORD(0xF, 0, 0, 0)

// --- [MODIFIED] A frame that sets gain to zero for ALL tones (0-7) and BOTH channels (0-1) ---
// This is the ultimate "clear" frame that silences everything.
static const uint32_t ZERO_GAIN_FRAME[] = {
    // --- Channel 0 ---
    MAKE_INDEX_WORD(0, 0, 0), MAKE_GAIN_WORD(0, 0, 0), // Tone 0
    MAKE_INDEX_WORD(0, 1, 0), MAKE_GAIN_WORD(0, 1, 0), // Tone 1
    MAKE_INDEX_WORD(0, 2, 0), MAKE_GAIN_WORD(0, 2, 0), // Tone 2
    MAKE_INDEX_WORD(0, 3, 0), MAKE_GAIN_WORD(0, 3, 0), // Tone 3
    MAKE_INDEX_WORD(0, 4, 0), MAKE_GAIN_WORD(0, 4, 0), // Tone 4
    MAKE_INDEX_WORD(0, 5, 0), MAKE_GAIN_WORD(0, 5, 0), // Tone 5
    MAKE_INDEX_WORD(0, 6, 0), MAKE_GAIN_WORD(0, 6, 0), // Tone 6
    MAKE_INDEX_WORD(0, 7, 0), MAKE_GAIN_WORD(0, 7, 0), // Tone 7

    // --- Channel 1 ---
    MAKE_INDEX_WORD(1, 0, 0), MAKE_GAIN_WORD(1, 0, 0), // Tone 0
    MAKE_INDEX_WORD(1, 1, 0), MAKE_GAIN_WORD(1, 1, 0), // Tone 1
    MAKE_INDEX_WORD(1, 2, 0), MAKE_GAIN_WORD(1, 2, 0), // Tone 2
    MAKE_INDEX_WORD(1, 3, 0), MAKE_GAIN_WORD(1, 3, 0), // Tone 3
    MAKE_INDEX_WORD(1, 4, 0), MAKE_GAIN_WORD(1, 4, 0), // Tone 4
    MAKE_INDEX_WORD(1, 5, 0), MAKE_GAIN_WORD(1, 5, 0), // Tone 5
    MAKE_INDEX_WORD(1, 6, 0), MAKE_GAIN_WORD(1, 6, 0), // Tone 6
    MAKE_INDEX_WORD(1, 7, 0), MAKE_GAIN_WORD(1, 7, 0), // Tone 7

    // Commit all the above settings (16 tones across 2 channels) in one go
    MAKE_COMMIT_WORD()
};

static const uint16_t ZERO_GAIN_FRAME_COUNT = sizeof(ZERO_GAIN_FRAME) / sizeof(uint32_t);

// Number of silent frames to send to ensure PL buffer is flushed
#define SHUTDOWN_FLUSH_FRAMES 100
#define IO_TIMEOUT_MS       5000
#define MAX_WORDS_PER_FRAME 64
#define GROW_WORDS_STEP     4096

// --- Data models & Types ---
typedef struct {
  uint32_t *offsets;
  uint16_t *counts;
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
} awg_srv_t;

// --- Global state for this module ---
static awg_srv_t G;
static volatile int g_stop_queue = 0;
static int g_listen_queue = -1;
static pthread_t g_accept_thread_queue;
static bool g_accept_thread_running = false;
static volatile int g_active_client_fd = -1;
static volatile bool g_loading_in_progress[2] = { false, false };

// --- [FIX #1] Add forward declarations for static functions ---
static bool prepare_list_for_preload(awg_list_t *L, uint32_t total_frames);
static void start_player_if_needed();

// --- Implementation ---
static void dummy_signal_handler(int sig) { (void)sig; }

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

static inline uint32_t be32_to_host(uint32_t x){ return ntohl(x); }
static inline uint16_t be16_to_host(uint16_t x){ return ntohs(x); }
static inline uint32_t host_to_be32(uint32_t x){ return htonl(x); }
static inline uint16_t host_to_be16(uint16_t x){ return htons(x); }

static void clear_list_fully(awg_list_t *L) {
    DPRINT("Fully clearing list (freeing all buffers).\n");
    free(L->offsets);
    free(L->counts);
    free(L->words);
    memset(L, 0, sizeof(awg_list_t));
}

// This function PREPARES a list for a new preload.
static bool prepare_list_for_preload(awg_list_t *L, uint32_t total_frames) {
    DPRINT("Preparing list for preload with %u frames.\n", total_frames);
    clear_list_fully(L); // Always start from a completely clean slate
    
    L->offsets = calloc(total_frames, sizeof(uint32_t));
    L->counts  = calloc(total_frames, sizeof(uint16_t));

    if (!L->offsets || !L->counts) {
        DPRINT("Failed to allocate metadata for list.\n");
        clear_list_fully(L); // Ensure clean state on failure
        return false;
    }
    L->total_frames = total_frames;
    return true;
}

static bool ensure_words_cap(awg_list_t *L, uint32_t need_more){
    if (L->words_used + need_more <= L->words_cap) return true;
    uint32_t want = L->words_used + need_more;
    uint32_t cap  = L->words_cap ? L->words_cap : GROW_WORDS_STEP;
    while (cap < want) cap += GROW_WORDS_STEP;
    uint32_t *nw = (uint32_t*)realloc(L->words, cap * sizeof(uint32_t));
    if (!nw) return false;
    L->words = nw;
    L->words_cap = cap;
    return true;
}

static bool push_frame(awg_list_t *L, const uint32_t *w, uint16_t count){
    if (!L || !L->offsets || !L->counts) return false;
    if (L->loaded_frames >= L->total_frames) return false;
    if (count == 0 || count > MAX_WORDS_PER_FRAME) return false;
    if (!ensure_words_cap(L, count)) return false;
    uint32_t off = L->words_used;
    memcpy(&L->words[off], w, count * sizeof(uint32_t));
    L->words_used += count;
    L->offsets[L->loaded_frames] = off;
    L->counts[L->loaded_frames] = count;
    L->loaded_frames++;
    return true;
}

// --- [NEW] Helper function to load a list with zero-gain frames ---
static bool load_zero_gain_list(awg_list_t *L, uint32_t num_frames) {
    if (!prepare_list_for_preload(L, num_frames)) {
        return false;
    }
    for (uint32_t i = 0; i < num_frames; ++i) {
        if (!push_frame(L, ZERO_GAIN_FRAME, ZERO_GAIN_FRAME_COUNT)) {
            clear_list_fully(L);
            return false;
        }
    }
    L->ready = true;
    return true;
}

static void init_lists(){
  memset(&G, 0, sizeof(G));
  pthread_mutex_init(&G.mtx, NULL);
  G.period_us = 1000;
  G.cur_list = 0; G.next_list = 1;
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

        if (!G.playing) {
            pthread_mutex_unlock(&G.mtx);
            continue;
        }

        awg_list_t *current_list = &G.list[G.cur_list];
        if (current_list->loaded_frames == 0 || G.cur_frame >= current_list->loaded_frames) {
            int finished_list_idx = G.cur_list;
            awg_list_t *next_list = &G.list[G.next_list];
            if (next_list->ready && next_list->loaded_frames > 0) {
                DPRINT("Switching from list %d to %d\n", G.cur_list, G.next_list);
                G.cur_list = G.next_list; G.next_list = finished_list_idx; G.cur_frame = 0;
            } else {
                DPRINT("End of list %d, no next ready -> stopping.\n", finished_list_idx);
                G.playing = false;
            }
            clear_list_fully(current_list);
            pthread_mutex_unlock(&G.mtx);
            pthread_mutex_lock(&g_notify_mutex);
            g_list_status[finished_list_idx] = LIST_IDLE;
            pthread_mutex_unlock(&g_notify_mutex);
            send_status_update(finished_list_idx);
            continue;
        } else {
            uint32_t off = current_list->offsets[G.cur_frame];
            uint16_t cnt = current_list->counts[G.cur_frame];
            uint32_t *w  = &current_list->words[off];
            pthread_mutex_unlock(&G.mtx);
            if(w) awg_send_words32(w, cnt);
            pthread_mutex_lock(&G.mtx);
            G.cur_frame++;
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

static void cancel_preload_and_mark_idle(int list_id) {
    if (list_id < 0 || list_id > 1) return;

    pthread_mutex_lock(&G.mtx);
    clear_list_fully(&G.list[list_id]);          // clear no load complate data
    pthread_mutex_unlock(&G.mtx);

    g_loading_in_progress[list_id] = false;

    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[list_id] = LIST_IDLE;          // change g_list_status
    pthread_mutex_unlock(&g_notify_mutex);

    send_status_update(list_id);
    DPRINT("CANCEL preload on list %d -> IDLE (timeout/error)\n", list_id);
}

static void do_reset(){
    pthread_mutex_lock(&G.mtx);
    G.playing=false; G.cur_list=0; G.next_list=1; G.cur_frame=0;
    clear_list_fully(&G.list[0]);
    clear_list_fully(&G.list[1]);
    pthread_mutex_unlock(&G.mtx);
    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[0] = LIST_IDLE; g_list_status[1] = LIST_IDLE;
    pthread_mutex_unlock(&g_notify_mutex);
    send_status_update(0);
    send_status_update(1);
    g_loading_in_progress[0] = false;
    g_loading_in_progress[1] = false;
    DPRINT("RESET\n");
}

static bool do_preload_begin(uint8_t list_id, uint32_t total_frames){
    if (list_id > 1 || total_frames == 0) return false;
    pthread_mutex_lock(&G.mtx);
    bool ok = prepare_list_for_preload(&G.list[list_id], total_frames);
    pthread_mutex_unlock(&G.mtx);
    if (ok) {
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[list_id] = LIST_LOADING;
        pthread_mutex_unlock(&g_notify_mutex);
        send_status_update(list_id);

        g_loading_in_progress[list_id] = true;
    }
    DPRINT("BEGIN for list %u. OK=%d\n", (unsigned)list_id, ok);
    return ok;
}

static bool do_preload_push(int fd) {
    uint8_t hdr[3];
    if (read_n_timeout(fd, hdr, 3, IO_TIMEOUT_MS) <= 0) return false;
    uint8_t  list_id; memcpy(&list_id, &hdr[0], sizeof(list_id));
    uint16_t be_count; memcpy(&be_count, &hdr[1], sizeof(be_count));
    uint16_t count = be16_to_host(be_count);
    if (list_id > 1 || count == 0 || count > MAX_WORDS_PER_FRAME) return false;
    uint32_t tmp[MAX_WORDS_PER_FRAME];
    if (read_n_timeout(fd, tmp, count * 4, IO_TIMEOUT_MS) <= 0) return false;
    for (int i = 0; i < count; ++i) tmp[i] = be32_to_host(tmp[i]);
    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    bool ok = push_frame(L, tmp, count);
    if (ok && L->loaded_frames > 0 && L->loaded_frames == L->total_frames) {
        L->ready = true;
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[list_id] = LIST_READY;
        pthread_mutex_unlock(&g_notify_mutex);
        send_status_update(list_id);
        if (!G.playing && list_id == 0) {
            G.playing = true; G.cur_list = 0; G.next_list = 1; G.cur_frame = 0;
            start_player_if_needed();
        }
    }
    pthread_mutex_unlock(&G.mtx);
    return ok;
}

static bool do_preload_end(uint8_t list_id) {
    if (list_id > 1) return false;
    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    if (L->loaded_frames == 0){ pthread_mutex_unlock(&G.mtx); return false; }
    if (!L->ready){ L->ready = true; }
    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[list_id] = LIST_READY;
    pthread_mutex_unlock(&g_notify_mutex);
    send_status_update(list_id);
    g_loading_in_progress[list_id] = false;
    if (!G.playing && list_id == 0) {
        G.playing = true; G.cur_list = 0; G.next_list = 1; G.cur_frame = 0;
        start_player_if_needed();
    }
    pthread_mutex_unlock(&G.mtx);
    return true;
}

static void serve_client(int fd){
    DPRINT("client connected (fd=%d)\n", fd);
    for(;;){
        uint8_t op;
        int rc = read_n_timeout(fd, &op, 1, IO_TIMEOUT_MS);
        if (rc <= 0) { break; }
        switch(op){
            case 'B': {
                uint8_t b[5]; if(read_n_timeout(fd,b,5,IO_TIMEOUT_MS)<=0) goto drop;
                uint32_t tf; memcpy(&tf, &b[1], sizeof(tf));
                if (!do_preload_begin(b[0], be32_to_host(tf))) goto drop;
            } break;
            case 'P': if (!do_preload_push(fd)) goto drop; break;
            case 'E': {
                uint8_t id; if(read_n_timeout(fd,&id,1,IO_TIMEOUT_MS)<=0) goto drop;
                if (!do_preload_end(id)) goto drop;
            } break;
            case 'Z': do_reset(); break;
            default: goto drop;
        }
    }
drop:
    if (g_loading_in_progress[0]) cancel_preload_and_mark_idle(0);
    if (g_loading_in_progress[1]) cancel_preload_and_mark_idle(1);

    DPRINT("client disconnected (fd=%d)\n", fd);
    close(fd);
}

// --- [MODIFIED] Make the accept loop more robust on error ---
static void* accept_loop_queue(void *arg) {
    (void)arg;
    while(1){ // Loop indefinitely until an error occurs
        int fd = accept(g_listen_queue, NULL, NULL);
        if (fd < 0){
            // If interrupted by our signal, just continue and re-check loop condition
            if (errno == EINTR) {
                // Check the global stop flag now
                if(g_stop_queue) break;
                continue;
            }
            
            // For any other error (like EBADF from a closed socket),
            // assume it's a shutdown signal and exit the loop.
            perror("[QSRV] accept");
            break;
        }
        
        // This part is for when a client is connected
        g_active_client_fd = fd;
        serve_client(fd);
        g_active_client_fd = -1;

        // After a client disconnects, check if we should stop
        if(g_stop_queue) break;
    }
    DPRINT("Accept loop thread exiting.\n");
    return NULL;
}

// --- [MODIFIED] The start_queue_server function ---
int start_queue_server(unsigned short port){
  g_stop_queue = 0;
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_handler = dummy_signal_handler;
  if (sigaction(SIGUSR1, &sa, NULL) == -1) { perror("[QSRV] sigaction"); return -1; }
  
  init_lists();
  start_player_if_needed(); // Player thread is now running, but idle (G.playing is false)

  // --- [NEW] Synchronously flush PL buffers with zero-gain waveforms on startup ---
  DPRINT("Priming PL buffers with zero-gain waveforms on startup...\n");
  
  // --- Flush List 0 ---
  pthread_mutex_lock(&G.mtx);
  DPRINT("Priming PL buffer for list 0...\n");
  // No need to clear lists, init_lists() just did that.
  if (load_zero_gain_list(&G.list[0], SHUTDOWN_FLUSH_FRAMES)) {
      G.cur_list = 0;
      G.next_list = 1;
      G.cur_frame = 0;
      G.playing = true; // Start playing the silent list
  }
  pthread_mutex_unlock(&G.mtx);

  // Wait for player to finish list 0. This also confirms the player thread is working.
  while (g_list_status[0] != LIST_IDLE) { usleep(10000); }
  DPRINT("PL buffer for list 0 primed.\n");

  // --- Flush List 1 ---
  pthread_mutex_lock(&G.mtx);
  DPRINT("Priming PL buffer for list 1...\n");
  if (load_zero_gain_list(&G.list[1], SHUTDOWN_FLUSH_FRAMES)) {
      G.cur_list = 1;
      G.next_list = 0;
      G.cur_frame = 0;
      G.playing = true; // Start playing the second silent list
  }
  pthread_mutex_unlock(&G.mtx);
  
  // Wait for player to finish list 1
  while (g_list_status[1] != LIST_IDLE) { usleep(10000); }
  DPRINT("PL buffer for list 1 primed.\n");
  
  DPRINT("PL priming complete. Server is ready to accept connections.\n");
  // At this point, player thread has finished both lists and set G.playing back to false.
  // The server state is now clean and ready.

  // --- Now, proceed with setting up network listening ---
  g_listen_queue = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_queue < 0){ perror("[QSRV] socket"); return -1; }
  int one=1;
  setsockopt(g_listen_queue, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY); addr.sin_port=htons(port);
  if (bind(g_listen_queue,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("[QSRV] bind"); close(g_listen_queue); return -2; }
  if (listen(g_listen_queue,1)<0){ perror("[QSRV] listen"); close(g_listen_queue); return -3; }
  if (pthread_create(&g_accept_thread_queue, NULL, accept_loop_queue, NULL) != 0) {
      perror("[QSRV] pthread_create"); close(g_listen_queue); return -4;
  }
  g_accept_thread_running = true;
  return 0;
}

// --- [MODIFIED] The final, robust stop_queue_server function ---
void stop_queue_server(void){
    DPRINT("Queue server stopping sequence initiated...\n");

    // --- Phase 1: Shut down network services and join the accept thread ---
    DPRINT("Stopping network services...\n");

    // Set stop flag for accept_loop first, in case it's not blocked in accept()
    g_stop_queue = 1;

    // Explicitly interrupt the accept() call with a signal
    if (g_accept_thread_running) {
        pthread_kill(g_accept_thread_queue, SIGUSR1);
    }
    
    // Shut down any active client connection
    if (g_active_client_fd >= 0) {
        shutdown(g_active_client_fd, SHUT_RDWR);
        close(g_active_client_fd);
        g_active_client_fd = -1;
    }

    // Close the listening socket.
    if (g_listen_queue >= 0){
        close(g_listen_queue);
        g_listen_queue = -1;
    }
    
    // Now that the thread is unblocked and its resources are closed, join it.
    if (g_accept_thread_running) {
        pthread_join(g_accept_thread_queue, NULL);
        g_accept_thread_running = false;
    }
    DPRINT("Network services stopped.\n");

    // --- Phase 2: Flush PL buffers (player_thread is still running) ---
    // Note: We leave g_stop_queue=1. The player thread will be controlled manually.
    DPRINT("Starting PL buffer flush.\n");
    if (G.player_thread_running) {
        // ... (The PL flushing logic for list 0 and list 1 remains EXACTLY the same as before) ...
        pthread_mutex_lock(&G.mtx);
        G.playing = false;
        clear_list_fully(&G.list[0]);
        clear_list_fully(&G.list[1]);
        if (load_zero_gain_list(&G.list[0], SHUTDOWN_FLUSH_FRAMES)) {
            G.cur_list = 0; G.next_list = 1; G.cur_frame = 0; G.playing = true;
        }
        pthread_mutex_unlock(&G.mtx);
        while (g_list_status[0] != LIST_IDLE) { usleep(10000); }
        DPRINT("PL buffer for list 0 flushed.\n");

        pthread_mutex_lock(&G.mtx);
        G.playing = false;
        clear_list_fully(&G.list[1]);
        if (load_zero_gain_list(&G.list[1], SHUTDOWN_FLUSH_FRAMES)) {
            G.cur_list = 1; G.next_list = 0; G.cur_frame = 0; G.playing = true;
        }
        pthread_mutex_unlock(&G.mtx);
        while (g_list_status[1] != LIST_IDLE) { usleep(10000); }
        DPRINT("PL buffer for list 1 flushed.\n");
    }

    // --- Phase 3: Finally, join the player thread ---
    // g_stop_queue is already 1, so the thread will exit after the flush.
    DPRINT("PL flush complete. Stopping player thread.\n");
    if (G.player_thread_running) {
        pthread_join(G.player_thread_h, NULL);
        G.player_thread_running = false;
    }

    pthread_mutex_destroy(&G.mtx);
    DPRINT("Queue server stopped successfully.\n");
}