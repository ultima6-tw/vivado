/*
 * awg_server_raw_queue.c — MODIFIED FOR DETAILED LOGGING
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
#include <sys/time.h> // --- [MODIFIED] --- For gettimeofday()

#include "awg_core.h"
#include "awg_server_raw_shared.h"

// --- [MODIFIED] DPRINT macro to include a timestamp ---
#ifdef DEBUG
    // Helper function to get a formatted timestamp
    static inline char* get_timestamp(char* buffer, size_t len) {
        struct timeval tv;
        struct tm tm_info;
        gettimeofday(&tv, NULL);
        localtime_r(&tv.tv_sec, &tm_info);
        int ms = tv.tv_usec / 1000;
        snprintf(buffer, len, "[%02d:%02d:%02d.%03d]", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ms);
        return buffer;
    }
    #define DPRINT(fmt, ...) do { \
        char ts_buf[32]; \
        printf("%s [QSRV] " fmt, get_timestamp(ts_buf, sizeof(ts_buf)), ##__VA_ARGS__); \
    } while (0)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

// --- [NEW] Helper function to print a hex dump, wrapped in #ifdef ---
#ifdef DEBUG
static void print_hex_dump(const char *desc, const void *addr, int len) {
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char*)addr;

    if (desc != NULL) DPRINT("%s (%d bytes):\n", desc, len);
    if (len <= 0) {
        DPRINT("  ZERO or NEGATIVE LENGTH: %d\n", len);
        return;
    }
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            if (i != 0) DPRINT("  %s\n", buff);
            DPRINT("  %04x ", i);
        }
        DPRINT(" %02x", pc[i]);
        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) {
            buff[i % 16] = '.';
        } else {
            buff[i % 16] = pc[i];
        }
        buff[(i % 16) + 1] = '\0';
    }
    while ((i % 16) != 0) {
        DPRINT("   ");
        i++;
    }
    DPRINT("  %s\n", buff);
}
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

// --- Forward declarations for static functions ---
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
    if (pr==0) { 
        DPRINT("read_n_timeout: timed out after %d ms\n", timeout_ms);
        return -2; // Timeout
    }
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
    L->offsets = NULL;
    free(L->counts);
    L->counts = NULL;
    free(L->words);
    L->words = NULL;
    memset(L, 0, sizeof(awg_list_t));
}

static bool prepare_list_for_preload(awg_list_t *L, uint32_t total_frames) {
    DPRINT("Preparing list for preload with %u frames.\n", total_frames);
    clear_list_fully(L);
    L->offsets = calloc(total_frames, sizeof(uint32_t));
    L->counts  = calloc(total_frames, sizeof(uint16_t));

    if (!L->offsets || !L->counts) {
        DPRINT("ERROR: Failed to allocate metadata for list.\n");
        clear_list_fully(L);
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
    if (!nw) {
        DPRINT("ERROR: Failed to realloc words buffer.\n");
        return false;
    }
    L->words = nw;
    L->words_cap = cap;
    return true;
}

static bool push_frame(awg_list_t *L, const uint32_t *w, uint16_t count){
    if (!L || !L->offsets || !L->counts) return false;
    if (L->loaded_frames >= L->total_frames) {
        DPRINT("ERROR: Attempt to push frame when list is already full (%u/%u).\n", L->loaded_frames, L->total_frames);
        return false;
    }
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

// --- [MODIFIED] player_thread to eliminate the 1ms gap during list switching ---
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
        if (!current_list->ready || G.cur_frame >= current_list->total_frames) {
            int finished_list_idx = G.cur_list;
            awg_list_t *next_list = &G.list[G.next_list];

            if (next_list->ready && next_list->total_frames > 0) {
                DPRINT("Switching from list %d to %d\n", G.cur_list, G.next_list);
                // Perform the switch
                G.cur_list = G.next_list; 
                G.next_list = finished_list_idx; 
                G.cur_frame = 0;
                // The list to be cleared is the *old* one
                current_list = &G.list[G.cur_list]; // IMPORTANT: Update current_list pointer
            } else {
                DPRINT("End of list %d, no next ready -> stopping.\n", finished_list_idx);
                G.playing = false;
                clear_list_fully(&G.list[finished_list_idx]);
                pthread_mutex_unlock(&G.mtx);

                pthread_mutex_lock(&g_notify_mutex);
                g_list_status[finished_list_idx] = LIST_IDLE;
                pthread_mutex_unlock(&g_notify_mutex);
                send_status_update(finished_list_idx);
                continue; // Stop playing
            }

            // Clean up the OLD list and send notification AFTER the switch
            clear_list_fully(&G.list[G.next_list]); // G.next_list now holds the old list index
            pthread_mutex_unlock(&G.mtx); // Unlock before notify

            pthread_mutex_lock(&g_notify_mutex);
            g_list_status[finished_list_idx] = LIST_IDLE;
            pthread_mutex_unlock(&g_notify_mutex);
            send_status_update(finished_list_idx);

            pthread_mutex_lock(&G.mtx); // Re-lock to continue to the send logic below
        } 

        // This logic will now be executed for the first frame of the new list
        // in the same timeslice as the switch.
        if (G.playing && current_list->loaded_frames > G.cur_frame) {
            uint32_t off = current_list->offsets[G.cur_frame];
            uint16_t cnt = current_list->counts[G.cur_frame];
            uint32_t *w  = &current_list->words[off];
            G.cur_frame++;
            pthread_mutex_unlock(&G.mtx);
            if(w) awg_send_words32(w, cnt);
        } else {
            pthread_mutex_unlock(&G.mtx);
        }
    }
    DPRINT("Player thread exiting.\n");
    return NULL;
}

// --- [MODIFIED] start_player_if_needed with real-time priority setting ---
static void start_player_if_needed(){
  if (!G.player_thread_running){
    DPRINT("Starting player thread...\n");
    if (pthread_create(&G.player_thread_h, NULL, player_thread, NULL) != 0) {
        DPRINT("ERROR: Failed to create player thread: %s\n", strerror(errno));
        return;
    }

    // --- [NEW] Set thread to real-time priority ---
    struct sched_param params;
    
    // Set a high priority (e.g., 98 out of a max of 99 for SCHED_FIFO)
    params.sched_priority = 98; 
    
    // Use First-In, First-Out real-time scheduling policy
    if (pthread_setschedparam(G.player_thread_h, SCHED_FIFO, &params) != 0) {
        // This might fail if not run as root, but your server is.
        DPRINT("WARNING: Failed to set real-time priority for player thread: %s\n", strerror(errno));
    } else {
        DPRINT("Player thread set to real-time priority %d with SCHED_FIFO.\n", params.sched_priority);
    }
    
    G.player_thread_running = true;
  }
}

static void cancel_preload_and_mark_idle(int list_id) {
    if (list_id < 0 || list_id > 1) return;
    
    if (!g_loading_in_progress[list_id]) return;

    DPRINT("CANCEL preload on list %d -> IDLE (client disconnected)\n", list_id);
    g_loading_in_progress[list_id] = false;

    pthread_mutex_lock(&G.mtx);
    clear_list_fully(&G.list[list_id]);
    pthread_mutex_unlock(&G.mtx);

    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[list_id] = LIST_IDLE;
    pthread_mutex_unlock(&g_notify_mutex);

    send_status_update(list_id);
}

static void do_reset(){
    DPRINT("RESET command received. Initiating synchronized zero-gain flush for both lists (silent until complete).\n");

    pthread_mutex_lock(&G.mtx);
    // 1. Stop current playback and fully clear all list data
    G.playing = false; 
    G.cur_frame = 0;

    // Clear data and state for both lists, preparing to load zero-gain frames
    clear_list_fully(&G.list[0]);
    clear_list_fully(&G.list[1]);
    g_loading_in_progress[0] = false; // Ensure preload status is reset
    g_loading_in_progress[1] = false;
    pthread_mutex_unlock(&G.mtx); // Unlock because load_zero_gain_list will also use the mutex, and we need to wait for notifications

    // Ensure the player_thread is running so it can process the subsequent zero-gain playback
    start_player_if_needed(); 

    // --- Phase 1: Flush List 0 with zero-gain frames ---
    DPRINT("  -> Loading zero-gain frames into List 0.\n");
    pthread_mutex_lock(&G.mtx); // Re-lock to operate on G.list
    if (load_zero_gain_list(&G.list[0], SHUTDOWN_FLUSH_FRAMES)) {
        G.cur_list = 0; // Start playback from List 0
        G.next_list = 1; // List 1 is ready as the next
        G.cur_frame = 0; // Start from the first frame of List 0
        G.playing = true; // Start the player

        // Update internal status, but do NOT send notifications to the client
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[0] = LIST_READY; // Internal status set to READY/PLAYING
        g_list_status[1] = LIST_IDLE;  // Internal List 1 remains IDLE (waiting to be processed)
        pthread_mutex_unlock(&g_notify_mutex);
    } else {
        DPRINT("ERROR: Failed to load zero-gain list 0 during RESET. Aborting.\n");
        pthread_mutex_unlock(&G.mtx);
        // If failed at this point, mark both as IDLE in the final step and return
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[0] = LIST_IDLE; g_list_status[1] = LIST_IDLE;
        pthread_mutex_unlock(&g_notify_mutex);
        // Do not call send_status_update here, will be handled at the end
        return; 
    }
    pthread_mutex_unlock(&G.mtx); // Unlock, preparing to wait for notification

    // Wait for List 0 to finish playback and become IDLE
    DPRINT("  -> Waiting for List 0 to become IDLE (zero-gain flush).\n");
    while (g_list_status[0] != LIST_IDLE && !g_stop_queue) {
        usleep(10000); 
    }
    DPRINT("  -> List 0 zero-gain flush complete.\n");

    // --- Phase 2: Flush List 1 with zero-gain frames ---
    DPRINT("  -> Loading zero-gain frames into List 1.\n");
    pthread_mutex_lock(&G.mtx); // Re-lock to operate on G.list
    G.playing = false; // Ensure player_thread doesn't suddenly read from old List 1 before configuration
    if (load_zero_gain_list(&G.list[1], SHUTDOWN_FLUSH_FRAMES)) {
        G.cur_list = 1; // Set player to start from List 1
        G.next_list = 0; // List 0 is now idle, serving as the next
        G.cur_frame = 0; 
        G.playing = true; // Restart the player
        
        // Update internal status, but do NOT send notifications to the client
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[0] = LIST_IDLE;   // Internal List 0 is now IDLE
        g_list_status[1] = LIST_READY; // Internal List 1 status
        pthread_mutex_unlock(&g_notify_mutex);
    } else {
        DPRINT("ERROR: Failed to load zero-gain list 1 during RESET. Aborting.\n");
        pthread_mutex_unlock(&G.mtx);
        // If failed at this point, mark both as IDLE in the final step and return
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[0] = LIST_IDLE; g_list_status[1] = LIST_IDLE;
        pthread_mutex_unlock(&g_notify_mutex);
        // Do not call send_status_update here, will be handled at the end
        return;
    }
    pthread_mutex_unlock(&G.mtx); // Unlock, preparing to wait for notification

    // Wait for List 1 to finish playback and become IDLE
    DPRINT("  -> Waiting for List 1 to become IDLE (zero-gain flush).\n");
    while (g_list_status[1] != LIST_IDLE && !g_stop_queue) {
        usleep(10000);
    }
    DPRINT("  -> List 1 zero-gain flush complete.\n");

    // --- Final internal state cleanup after both lists are flushed ---
    pthread_mutex_lock(&G.mtx);
    G.playing = false; // Ensure player stops after zero-gain frames are played
    clear_list_fully(&G.list[0]); // Clear zero-gain List 0 (now it's IDLE)
    clear_list_fully(&G.list[1]); // Clear zero-gain List 1 (now it's IDLE)
    g_loading_in_progress[0] = false;
    g_loading_in_progress[1] = false;
    pthread_mutex_unlock(&G.mtx);

    // --- ONLY NOW send the final IDLE notifications to the client ---
    // This ensures the client receives IDLE status only after all zeroing operations are complete
    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[0] = LIST_IDLE; 
    g_list_status[1] = LIST_IDLE;
    pthread_mutex_unlock(&g_notify_mutex);
    
    send_status_update(0);
    send_status_update(1);
    DPRINT("RESET command fully processed: both lists flushed and now truly IDLE. Notifications sent.\n");
}

static bool do_preload_begin(uint8_t list_id, uint32_t total_frames){
    if (list_id > 1) return false;
    if (total_frames == 0 || total_frames > 2000000) { 
        DPRINT("ERROR: Invalid total_frames (%u) in BEGIN command for list %u.\n", total_frames, (unsigned)list_id);
        return false;
    }

    DPRINT("BEGIN for list %u with %u frames.\n", (unsigned)list_id, total_frames);
    
    pthread_mutex_lock(&G.mtx);
    bool ok = prepare_list_for_preload(&G.list[list_id], total_frames);
    pthread_mutex_unlock(&G.mtx);
    
    if (ok) {
        g_loading_in_progress[list_id] = true;
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[list_id] = LIST_LOADING;
        pthread_mutex_unlock(&g_notify_mutex);
        send_status_update(list_id);
    }
    return ok;
}

// --- [MODIFIED] do_preload_push function with conditional logging ---
static bool do_preload_push(int fd) {
    uint8_t hdr[3];
    int rc = read_n_timeout(fd, hdr, 3, IO_TIMEOUT_MS);
    if (rc <= 0) {
        if (rc == -2) DPRINT("Timeout while reading PUSH header.\n");
        return false;
    }
    
#ifdef DEBUG
    print_hex_dump("PUSH Header Raw Bytes", hdr, 3);
#endif

    uint8_t  list_id; memcpy(&list_id, &hdr[0], sizeof(list_id));
    uint16_t be_count; memcpy(&be_count, &hdr[1], sizeof(be_count));
    uint16_t count = be16_to_host(be_count);

    DPRINT("PUSH Hdr Decoded -> list_id: %u, be_count: 0x%04X, host_count: %u\n", 
           (unsigned)list_id, be_count, count);

    if (list_id > 1 || count == 0 || count > MAX_WORDS_PER_FRAME) {
        DPRINT("ERROR: Invalid header in PUSH command.\n");
        return false;
    }
    
    uint32_t network_order_tmp[MAX_WORDS_PER_FRAME];
    rc = read_n_timeout(fd, network_order_tmp, count * 4, IO_TIMEOUT_MS);
    if (rc <= 0) {
        if (rc == -2) DPRINT("Timeout while reading PUSH payload.\n");
        return false;
    }

#ifdef DEBUG
    print_hex_dump("PUSH Payload Raw Bytes (Big-Endian)", network_order_tmp, count * 4);
#endif

    uint32_t tmp[MAX_WORDS_PER_FRAME];
    for (int i = 0; i < count; ++i) tmp[i] = be32_to_host(network_order_tmp[i]);

#ifdef DEBUG
    for (int i = 0; i < count; ++i) {
        DPRINT("PUSH Payload Decoded Word[%d]: 0x%08X\n", i, tmp[i]);
    }
#endif

    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    
    DPRINT("List %u status before push: %u/%u frames loaded.\n", (unsigned)list_id, L->loaded_frames, L->total_frames);

    bool ok = push_frame(L, tmp, count);

    if (ok && L->loaded_frames == L->total_frames) {
        DPRINT("List %u is now fully loaded. Marking as READY.\n", (unsigned)list_id);
        L->ready = true;
        g_loading_in_progress[list_id] = false; 
        
        pthread_mutex_lock(&g_notify_mutex);
        g_list_status[list_id] = LIST_READY;
        pthread_mutex_unlock(&g_notify_mutex);
        send_status_update(list_id);
        
        if (!G.playing) {
            DPRINT("This is the first ready list. Auto-starting player...\n");
            G.playing = true; 
            G.cur_list = list_id; 
            G.next_list = 1 - list_id; 
            G.cur_frame = 0;
            start_player_if_needed();
        }
    }
    pthread_mutex_unlock(&G.mtx);
    return ok;
}


static bool do_preload_end(uint8_t list_id) {
    if (list_id > 1) return false;
    DPRINT("END received for list %u.\n", (unsigned)list_id);
    
    g_loading_in_progress[list_id] = false;

    pthread_mutex_lock(&G.mtx);
    awg_list_t *L = &G.list[list_id];
    if (L->loaded_frames == 0){ 
        DPRINT("ERROR: END received for an empty list %u.\n", (unsigned)list_id);
        pthread_mutex_unlock(&G.mtx); 
        return false; 
    }
    if (!L->ready){ 
        DPRINT("List %u marked as READY by END command.\n", (unsigned)list_id);
        L->ready = true; 
    }

    pthread_mutex_lock(&g_notify_mutex);
    g_list_status[list_id] = LIST_READY;
    pthread_mutex_unlock(&g_notify_mutex);
    send_status_update(list_id);

    if (!G.playing) {
        DPRINT("This is the first ready list. Auto-starting player after END.\n");
        G.playing = true; 
        G.cur_list = list_id; 
        G.next_list = 1 - list_id; 
        G.cur_frame = 0;
        start_player_if_needed();
    }
    pthread_mutex_unlock(&G.mtx);
    return true;
}

static void serve_client(int fd){
    DPRINT("client connected (fd=%d)\n", fd);
    g_loading_in_progress[0] = false;
    g_loading_in_progress[1] = false;

    for(;;){
        uint8_t op;
        int rc = read_n_timeout(fd, &op, 1, IO_TIMEOUT_MS);
        if (rc <= 0) { 
            if (rc == -2) DPRINT("Timeout waiting for command from client.\n");
            else DPRINT("read_n_timeout returned %d, client likely disconnected.\n", rc);
            break; 
        }
        switch(op){
            case 'B': {
                uint8_t b[5]; 
                rc = read_n_timeout(fd,b,5,IO_TIMEOUT_MS);
                if(rc <= 0) goto drop;
                uint32_t tf; memcpy(&tf, &b[1], sizeof(tf));
                if (!do_preload_begin(b[0], be32_to_host(tf))) goto drop;
            } break;
            case 'P': {
                if (!do_preload_push(fd)) goto drop; 
            } break;
            case 'E': {
                uint8_t id; 
                rc = read_n_timeout(fd,&id,1,IO_TIMEOUT_MS);
                if(rc <= 0) goto drop;
                if (!do_preload_end(id)) goto drop;
            } break;
            case 'Z': do_reset(); break;
            case 'X': {
                DPRINT("SHUTDOWN command received. Initiating system poweroff.\n");
                do_reset(); 
                system("poweroff");
                goto drop;
            } break;
            default: 
                DPRINT("ERROR: Unknown command received: 0x%02X\n", op);
                goto drop;
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
    while(!g_stop_queue){
        int fd = accept(g_listen_queue, NULL, NULL);
        if (fd < 0){
            if (errno == EINTR) continue;
            if (g_stop_queue) break;
            DPRINT("accept() failed with error %d (%s), exiting accept loop.\n", errno, strerror(errno));
            break;
        }
        
        if (g_active_client_fd >= 0) {
            DPRINT("Another client tried to connect. Closing old fd=%d and accepting new fd=%d.\n", g_active_client_fd, fd);
            close(g_active_client_fd);
        }
        g_active_client_fd = fd;
        serve_client(fd);
        g_active_client_fd = -1;
    }
    DPRINT("Accept loop thread exiting.\n");
    return NULL;
}

// --- [MODIFIED] The start_queue_server function ---
int start_queue_server(unsigned short port){
  g_stop_queue = 0;
  struct sigaction sa; memset(&sa, 0, sizeof(sa));
  sa.sa_handler = dummy_signal_handler;
  if (sigaction(SIGUSR1, &sa, NULL) == -1) { DPRINT("sigaction failed: %s\n", strerror(errno)); return -1; }
  
  init_lists();
  start_player_if_needed(); 

  // --- [NEW] Synchronously flush PL buffers with zero-gain waveforms on startup ---
  DPRINT("Priming PL buffers with zero-gain waveforms on startup...\n");
  
  if (G.player_thread_running) {
      // --- Flush List 0 ---
      pthread_mutex_lock(&G.mtx);
      DPRINT("Priming PL buffer for list 0...\n");
      if (load_zero_gain_list(&G.list[0], SHUTDOWN_FLUSH_FRAMES)) {
          G.cur_list = 0; G.next_list = 1; G.cur_frame = 0; G.playing = true;
      }
      pthread_mutex_unlock(&G.mtx);
      while (g_list_status[0] != LIST_IDLE) { usleep(10000); }
      DPRINT("PL buffer for list 0 primed.\n");

      // --- Flush List 1 ---
      pthread_mutex_lock(&G.mtx);
      DPRINT("Priming PL buffer for list 1...\n");
      if (load_zero_gain_list(&G.list[1], SHUTDOWN_FLUSH_FRAMES)) {
          G.cur_list = 1; G.next_list = 0; G.cur_frame = 0; G.playing = true;
      }
      pthread_mutex_unlock(&G.mtx);
      while (g_list_status[1] != LIST_IDLE) { usleep(10000); }
      DPRINT("PL buffer for list 1 primed.\n");
  }
  
  DPRINT("PL priming complete. Server is ready to accept connections.\n");

  // --- Now, proceed with setting up network listening ---
  g_listen_queue = socket(AF_INET, SOCK_STREAM, 0);
  if (g_listen_queue < 0){ DPRINT("socket() failed: %s\n", strerror(errno)); return -1; }
  int one=1;
  setsockopt(g_listen_queue, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr={0};
  addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY); addr.sin_port=htons(port);
  if (bind(g_listen_queue,(struct sockaddr*)&addr,sizeof(addr))<0){ DPRINT("bind() failed: %s\n", strerror(errno)); close(g_listen_queue); return -2; }
  if (listen(g_listen_queue,1)<0){ DPRINT("listen() failed: %s\n", strerror(errno)); close(g_listen_queue); return -3; }
  if (pthread_create(&g_accept_thread_queue, NULL, accept_loop_queue, NULL) != 0) {
      DPRINT("pthread_create() failed: %s\n", strerror(errno)); close(g_listen_queue); return -4;
  }
  g_accept_thread_running = true;
  return 0;
}

// --- [MODIFIED] The final, robust stop_queue_server function ---
void stop_queue_server(void){
    DPRINT("Queue server stopping sequence initiated...\n");

    // --- Phase 1: Shut down network services and join the accept thread ---
    DPRINT("Stopping network services...\n");
    g_stop_queue = 1;
    if (g_listen_queue >= 0) {
        shutdown(g_listen_queue, SHUT_RDWR); // Help unblock accept()
        close(g_listen_queue);
        g_listen_queue = -1;
    }
    if (g_active_client_fd >= 0) {
        shutdown(g_active_client_fd, SHUT_RDWR);
        close(g_active_client_fd);
        g_active_client_fd = -1;
    }
    if (g_accept_thread_running) {
        pthread_kill(g_accept_thread_queue, SIGUSR1); // Interrupt if still blocked
        pthread_join(g_accept_thread_queue, NULL);
        g_accept_thread_running = false;
    }
    DPRINT("Network services stopped.\n");

    // --- Phase 2: Flush PL buffers (player_thread is still running) ---
    DPRINT("Starting PL buffer flush.\n");
    if (G.player_thread_running) {
        // --- Flush List 0 ---
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

        // --- Flush List 1 ---
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
    DPRINT("PL flush complete. Stopping player thread.\n");
    if (G.player_thread_running) {
        pthread_join(G.player_thread_h, NULL);
        G.player_thread_running = false;
    }

    pthread_mutex_destroy(&G.mtx);
    DPRINT("Queue server stopped successfully.\n");
}
