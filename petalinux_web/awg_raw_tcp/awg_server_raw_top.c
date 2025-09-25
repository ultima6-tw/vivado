/*
 * awg_server_raw_top.c — Top-level launcher
 * - Initializes AWG (/dev/mem mmap)
 * - Starts two listeners:
 *     port 9000 -> direct (no-queue) server
 *     port 9100 -> queued (single-writer) server
 *     port 9101 -> queued-notify server
 *
 * Build:
 *  gcc -O2 -pthread -Wall -DDEBUG -o awg_server \
 *       awg_server_raw_top.c \
 *       awg_server_raw_direct.c \
 *       awg_server_raw_queue.c \
 *       awg_server_raw_notify.c \
 *       awg_core_mmap.c
 *
 * Run (root for /dev/mem):
 *   sudo ./awg_server
 *
 * Debug prints:
 *   add -DDEBUG to build line
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include "awg_core.h"

// [ADD] Define a debug print macro specific to this file
#ifdef DEBUG
  #define DPRINT_MAIN(fmt, ...) printf("[MAIN] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT_MAIN(fmt, ...) do{}while(0)
#endif

// exported by the two backends
int start_direct_server(unsigned short port);
int start_queue_server (unsigned short port);
int start_notify_server(unsigned short port);
void stop_direct_server(void);
void stop_queue_server (void);
void stop_notify_server(void);

static volatile int g_stop = 0;
static void on_signal(int sig){ (void)sig; g_stop = 1; }

int main(void) {
    if (awg_init() != 0) {
        fprintf(stderr, "awg_init failed\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // start both modes
    if (start_direct_server(9000) != 0) {
        fprintf(stderr, "failed to start direct server on 9000\n");
        return 2;
    }
    
    if (start_notify_server(9101) != 0) {
        fprintf(stderr, "failed to start notify server on 9101\n");
        return 4;
    }

    if (start_queue_server(9100) != 0) {
        fprintf(stderr, "failed to start queue server on 9100\n");
        return 3;
    }


    

    printf("[MAIN] servers up. Ports: 9000=direct, 9100=queued, 9101=notify\n");
    while (!g_stop) usleep(100000); // 100 ms tick

    DPRINT_MAIN("\nStop signal received. Shutting down...\n");

    DPRINT_MAIN("Stopping direct server...\n");
    stop_direct_server();
    DPRINT_MAIN("Direct server stopped.\n");

    DPRINT_MAIN("Stopping queue server...\n");
    stop_queue_server();
    DPRINT_MAIN("Queue server stopped.\n");

    DPRINT_MAIN("Stopping notify server...\n");
    stop_notify_server();
    DPRINT_MAIN("Notify server stopped.\n");

    // [MODIFIED] Add the zero-out call before closing the core hardware interface.
    DPRINT_MAIN("Setting hardware to a safe (zero) state...\n");
    awg_zero_output();

    DPRINT_MAIN("Closing AWG core...\n");
    awg_close();
    DPRINT_MAIN("AWG core closed.\n");

    printf("[MAIN] stopped\n");
    return 0;
}