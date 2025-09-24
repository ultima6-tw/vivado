// awg_server_raw_notify.c — Notification server for AWG system status updates.
// Sends "IDLE" if at least one list is free, "FULL" if both are busy.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>   // [FIX] Added for errno and EINTR
#include <signal.h>  // [FIX] Added for SIGUSR1 and pthread_kill

#include "awg_server_raw_shared.h" // Include our shared declarations

#ifdef DEBUG
  #define DPRINT(fmt, ...) printf("[NOTIFY] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

// --- Module-specific global variables ---
static volatile int g_stop_notify = 0;
static int g_listen_notify = -1;
static pthread_t g_accept_thread_notify;
static bool g_accept_thread_running = false;
static int g_last_sent_status = -1; // -1 means uninitialized

// --- Shared global variables (defined in this file) ---
pthread_mutex_t g_notify_mutex;
volatile int g_list_status[2] = { LIST_IDLE, LIST_IDLE };
static volatile int g_notify_fd = -1; // The connected client for notifications


// --- Public Function Implementation ---

// [MODIFIED] This function now sends a system-wide status (IDLE/FULL).
// It's called by other modules when a list's state might have changed.
void send_system_status(void) {
    pthread_mutex_lock(&g_notify_mutex);
    if (g_notify_fd >= 0) {
        // [NEW LOGIC] Determine the current system status based on both lists.
        int current_system_status = (g_list_status[0] == LIST_IDLE || g_list_status[1] == LIST_IDLE)
                                    ? LIST_IDLE : LIST_FULL;

        // Only send a notification if the overall system state has changed.
        if (current_system_status != g_last_sent_status) {
            const char* status_str = (current_system_status == LIST_IDLE) ? "IDLE\n" : "FULL\n";
            if (send(g_notify_fd, status_str, strlen(status_str), MSG_NOSIGNAL) < 0) {
                DPRINT("send error, client likely disconnected. Closing notify socket.\n");
                close(g_notify_fd);
                g_notify_fd = -1;
            } else {
                DPRINT("Sent system status notification: %s", status_str);
                g_last_sent_status = current_system_status; // Update the last sent status
            }
        }
    }
    pthread_mutex_unlock(&g_notify_mutex);
}

// --- Internal Logic ---

static void* accept_loop_notify(void* arg) {
    (void)arg;
    DPRINT("Accept loop started\n");
    while (!g_stop_notify) {
        int fd = accept(g_listen_notify, NULL, NULL);
        if (fd < 0) {
            if (g_stop_notify) break;
            // [ADD] Add EINTR check, same as in the queue server
            if (errno == EINTR) continue;
            perror("[NOTIFY] accept");
            continue;
        }

        DPRINT("Notification client connected (fd=%d)\n", fd);

        pthread_mutex_lock(&g_notify_mutex);
        if (g_notify_fd >= 0) {
            close(g_notify_fd); // Only allow one notification client at a time
        }
        g_notify_fd = fd;
        g_last_sent_status = -1; // Reset last status to force an update for the new client
        pthread_mutex_unlock(&g_notify_mutex);

        // Upon connection, immediately send the current system status
        send_system_status();
    }
    DPRINT("Accept loop finished\n");
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
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(g_listen_notify, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[NOTIFY] bind"); close(g_listen_notify); return -2;
    }
    if (listen(g_listen_notify, 1) < 0) {
        perror("[NOTIFY] listen"); close(g_listen_notify); return -3;
    }

    if (pthread_create(&g_accept_thread_notify, NULL, accept_loop_notify, NULL) != 0) {
        perror("[NOTIFY] pthread_create"); close(g_listen_notify); return -4;
    }
    g_accept_thread_running = true;

    // This printf is now in awg_server_raw_top.c
    // printf("[NOTIFY] listening on %u (notification channel)\n", port);
    return 0;
}

void stop_notify_server(void) {
    DPRINT("stop_notify_server() entered.\n");
    g_stop_notify = 1;

    // [ADD] Actively interrupt the accept thread with a signal
    if (g_accept_thread_running) {
        DPRINT("Sending SIGUSR1 to notify accept thread to unblock it...\n");
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
        DPRINT("Waiting for notify accept thread to join...\n");
        pthread_join(g_accept_thread_notify, NULL);
        DPRINT("Notify accept thread has joined.\n");
        g_accept_thread_running = false;
    }

    // [FIX] Removed pthread_mutex_destroy(&g_notify_mutex);
    DPRINT("stop_notify_server() finished.\n");
}