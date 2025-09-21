// w_server.c — Raw TCP "W" protocol server (count + words, no auto-commit)
// Protocol:
//   [2 bytes] COUNT (big-endian, number of 32-bit words, 1..MAX_WORDS)
//   [4*COUNT] WORDS[] (each 32-bit, big-endian)
// Server pushes exactly COUNT words to awg_send_words32(words, COUNT).
//
// Build (link with your mmap core):
//   gcc -O2 -Wall -o w_server w_server.c awg_core_mmap.c
//   // or if awg_core_mmap is a .so: gcc -O2 -Wall -o w_server w_server.c -L. -lawg_core_mmap
//
// Run (root needed for /dev/mem):
//   sudo ./w_server 9000
//
// Debug prints on:
//   gcc -O2 -Wall -DDEBUG -o w_server w_server.c awg_core_mmap.c

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "awg_core.h"

// ---------- Debug macro ----------
#ifdef DEBUG
  #define DPRINT(fmt, ...) printf("[DEBUG] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do {} while (0)
#endif

// ---------- Tunables ----------
#define DEFAULT_PORT       9000
#define MAX_WORDS          64          // Safe upper bound (adjust to 16/32/64 as needed)
#define SOCK_RCVBUF        (256*1024)  // Server recv buffer
#define IO_TIMEOUT_MS      100         // Per-read timeout (ms). Resets after each successful read.

// If you want a "whole-frame" deadline (header+payload must finish within X ms),
// set FRAME_TIMEOUT_MS > 0. If 0, per-read timeout only.
#define FRAME_TIMEOUT_MS   0

static volatile int g_stop = 0;

// Graceful stop on SIGINT/SIGTERM
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// Get current time in milliseconds (monotonic)
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// poll+recv until n bytes are read.
// Return values: 1=ok, 0=peer closed, -2=timeout, -1=error (errno set).
// If deadline_ms >= 0, use it as absolute deadline (whole-frame).
// If deadline_ms < 0, use per-read timeout IO_TIMEOUT_MS.
static int read_n_timeout(int fd, void *buf, size_t n, int64_t deadline_ms) {
    uint8_t *p = (uint8_t*)buf;
    size_t got = 0;

    while (got < n) {
        int timeout_ms;
        if (deadline_ms >= 0) {
            int64_t now = now_ms();
            int64_t remain = deadline_ms - now;
            if (remain <= 0) return -2; // frame deadline exceeded
            timeout_ms = (remain > 1000*60) ? 1000*60 : (int)remain; // cap to 60s for safety
        } else {
            timeout_ms = IO_TIMEOUT_MS; // per-read timeout
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)      return -2;          // timeout
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;                        // poll error
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return 0;                         // peer closed / error
        }

        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0)     return 0;             // peer closed
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;                        // recv error
        }
        got += (size_t)r;
        // Note: in per-read mode, the next loop iteration has a fresh IO_TIMEOUT_MS.
        // In frame-deadline mode, the timeout is derived from the fixed deadline.
    }
    return 1; // ok
}

// Convert big-endian 32-bit array in-place to host endian
static void be32_to_host(uint32_t *w, int count) {
    for (int i = 0; i < count; ++i) w[i] = ntohl(w[i]);
}

int main(int argc, char **argv) {
    int port = (argc >= 2) ? atoi(argv[1]) : DEFAULT_PORT;

    if (awg_init() != 0) {
        fprintf(stderr, "awg_init failed\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 2; }

    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(srv, SOL_SOCKET, SO_RCVBUF, &(int){SOCK_RCVBUF}, sizeof(int));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(srv); return 3;
    }
    if (listen(srv, 1) < 0) {
        perror("listen"); close(srv); return 4;
    }

    printf("[W-SERVER] listening on 0.0.0.0:%d (count + words; NO auto-commit)\n", port);

    uint8_t  header[2];
    uint32_t words[MAX_WORDS];

    while (!g_stop) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int fd = accept(srv, (struct sockaddr*)&cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){SOCK_RCVBUF}, sizeof(int));

        DPRINT("client connected\n");

        for (;;) {
            // Establish a frame deadline if enabled
            int64_t deadline = (FRAME_TIMEOUT_MS > 0) ? (now_ms() + FRAME_TIMEOUT_MS) : -1;

            // 1) Read 2-byte COUNT (big-endian)
            int ok = read_n_timeout(fd, header, 2, deadline);
            if (ok == 0)  { DPRINT("peer closed\n"); break; }
            if (ok == -2) { DPRINT("timeout on count\n");    break; }
            if (ok < 0)   { perror("read count");            break; }

            uint16_t be_cnt; memcpy(&be_cnt, header, 2);
            int count = (int)ntohs(be_cnt);

            if (count <= 0 || count > MAX_WORDS) {
                DPRINT("bad count=%d (max=%d)\n", count, MAX_WORDS);
                break; // protocol error → drop connection
            }

            // 2) Read payload: 4*COUNT bytes
            size_t need = (size_t)count * 4;
            ok = read_n_timeout(fd, words, need, deadline);
            if (ok == 0)  { DPRINT("peer closed during data\n"); break; }
            if (ok == -2) { DPRINT("timeout during data\n");     break; }
            if (ok < 0)   { perror("read data");                 break; }

            // 3) Convert & push
            be32_to_host(words, count);
            int r = awg_send_words32(words, count);
            if (r != 0) {
                DPRINT("awg_send_words32 ret=%d\n", r);
                // Keep the connection; or break here if you want to drop on error
            }
            // No ACK (lowest latency). Add a tiny ACK if you really need it.
        }

        close(fd);
        DPRINT("client disconnected\n");
    }

    close(srv);
    awg_close();
    printf("[W-SERVER] stopped\n");
    return 0;
}