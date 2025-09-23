/*
 * awg_server_main.c — Top-level launcher
 * - Initializes AWG (/dev/mem mmap)
 * - Starts two listeners:
 *     port 9000 -> direct (no-queue) server
 *     port 9001 -> queued (single-writer) server
 *
 * Build:
 *   gcc -O2 -pthread -Wall -o awg_server \
 *       awg_server_main.c awg_server_direct.c awg_server_queue.c awg_core_mmap.c
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

// exported by the two backends
int start_direct_server(unsigned short port);
int start_queue_server (unsigned short port);
void stop_direct_server(void);
void stop_queue_server (void);

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
    if (start_queue_server(9001) != 0) {
        fprintf(stderr, "failed to start queue server on 9001\n");
        return 3;
    }

    printf("[MAIN] servers up. Ports: 9000=direct, 9001=queued\n");
    while (!g_stop) usleep(100000); // 100 ms tick

    // best-effort stop
    stop_direct_server();
    stop_queue_server();
    awg_close();
    printf("[MAIN] stopped\n");
    return 0;
}