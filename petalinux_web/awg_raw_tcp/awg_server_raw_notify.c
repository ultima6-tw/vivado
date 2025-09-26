// awg_server_raw_notify.c — MODIFIED FOR TIMESTAMP LOGGING
// Notification server for precise, per-list AWG status updates.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <time.h>       // --- [NEW] --- For timestamp functions
#include <sys/time.h>   // --- [NEW] --- For gettimeofday()

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
        printf("%s [NOTIFY] " fmt, get_timestamp(ts_buf, sizeof(ts_buf)), ##__VA_ARGS__); \
    } while (0)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif


// --- Module-specific global variables ---
static volatile int g_stop_notify = 0;
static int g_listen_notify = -1;
static pthread_t g_accept_thread_notify;
static bool g_accept_thread_running = false;
static volatile int g_notify_fd = -1;
static int g_last_sent_status[2] = { -1, -1 };

// --- Shared global variables (defined in this file) ---
pthread_mutex_t g_notify_mutex;
volatile int g_list_status[2] = { LIST_IDLE, LIST_IDLE };

// --- Public Function Implementation ---
void send_status_update(int list_id) {
    if (list_id < 0 || list_id > 1) return;
    pthread_mutex_lock(&g_notify_mutex);
    if (g_notify_fd >= 0) {
        // This check remains the same
        if (g_list_status[list_id] != g_last_sent_status[list_id]) {
            const char* status_strings[] = {"IDLE", "LOADING", "READY"};
            const char* status_str = status_strings[g_list_status[list_id]];
            char buf[32];
            // snprintf does not need timestamp, DPRINT will add it
            snprintf(buf, sizeof(buf), "LIST%d:%s\n", list_id, status_str);
            if (send(g_notify_fd, buf, strlen(buf), MSG_NOSIGNAL) < 0) {
                // Manually add timestamp here for error logging if needed, or rely on perror
                perror("send notification failed");
                close(g_notify_fd);
                g_notify_fd = -1;
            } else {
                // DPRINT will automatically add the timestamp
                DPRINT("Sent notification: %s", buf); // buf already has a newline
                g_last_sent_status[list_id] = g_list_status[list_id];
            }
        }
    }
    pthread_mutex_unlock(&g_notify_mutex);
}

// --- Internal Logic ---
static void* accept_loop_notify(void* arg) {
    (void)arg;
    while (!g_stop_notify) {
        int fd = accept(g_listen_notify, NULL, NULL);
        if (fd < 0) {
            if (g_stop_notify) break;
            if (errno == EINTR) continue;
            perror("[NOTIFY] accept"); continue;
        }
        DPRINT("Notification client connected (fd=%d)\n", fd);
        pthread_mutex_lock(&g_notify_mutex);
        if (g_notify_fd >= 0) close(g_notify_fd);
        g_notify_fd = fd;
        g_last_sent_status[0] = -1; g_last_sent_status[1] = -1;
        pthread_mutex_unlock(&g_notify_mutex);
        // Trigger sending initial status for both lists
        send_status_update(0);
        send_status_update(1);
    }
    DPRINT("Accept loop thread exiting.\n");
    return NULL;
}

int start_notify_server(unsigned short port) {
    g_stop_notify = 0;
    pthread_mutex_init(&g_notify_mutex, NULL);
    g_listen_notify = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_notify < 0) { perror("[NOTIFY] socket"); return -1; }
    int one=1;
    setsockopt(g_listen_notify, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr={0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port);
    if (bind(g_listen_notify, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("[NOTIFY] bind"); close(g_listen_notify); return -2; }
    if (listen(g_listen_notify, 1) < 0) { perror("[NOTIFY] listen"); close(g_listen_notify); return -3; }
    if (pthread_create(&g_accept_thread_notify, NULL, accept_loop_notify, NULL) != 0) {
        perror("[NOTIFY] pthread_create"); close(g_listen_notify); return -4;
    }
    g_accept_thread_running = true;
    return 0;
}

void stop_notify_server(void) {
    DPRINT("Stopping notification server...\n");
    g_stop_notify = 1;

    // Use a dummy signal to interrupt the blocking accept() call
    if (g_accept_thread_running) { 
        pthread_kill(g_accept_thread_notify, SIGUSR1); 
    }

    if (g_listen_notify >= 0) { 
        close(g_listen_notify); 
        g_listen_notify = -1; 
    }

    pthread_mutex_lock(&g_notify_mutex);
    if (g_notify_fd >= 0) { 
        shutdown(g_notify_fd, SHUT_RDWR); 
        close(g_notify_fd); 
        g_notify_fd = -1; 
    }
    pthread_mutex_unlock(&g_notify_mutex);

    if (g_accept_thread_running) { 
        pthread_join(g_accept_thread_notify, NULL); 
        g_accept_thread_running = false; 
    }
    DPRINT("Notification server stopped.\n");
}