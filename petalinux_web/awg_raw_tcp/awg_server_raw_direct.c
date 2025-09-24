/*
 * awg_server_direct.c — Direct (no-queue) W-protocol server
 * Protocol:
 *   [2 bytes] COUNT (big-endian, number of 32-bit words; >0)
 *   [4*COUNT] WORDS (each 32-bit big-endian)
 * Each frame is applied immediately: awg_send_words32(words, COUNT).
 * Build:
 *  gcc -O2 -pthread -Wall -DDEBUG -o awg_server \
 *       awg_server_raw_top.c \
 *       awg_server_raw_direct.c \
 *       awg_server_raw_queue.c \
 *       awg_server_raw_notify.c \
 *       awg_core_mmap.c
 * 
 * Exported API:
 *   int  start_direct_server(unsigned short port);
 *   void stop_direct_server(void);
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
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
  #define DPRINT(fmt, ...) printf("[DIRECT] " fmt, ##__VA_ARGS__)
#else
  #define DPRINT(fmt, ...) do{}while(0)
#endif

#define SOCK_RCVBUF       (256*1024)
#define IO_TIMEOUT_MS     100
#define FRAME_TIMEOUT_MS  0     // 0 = per-read only
#define MAX_WORDS         64

static volatile int g_stop_direct = 0;
static int g_listen = -1;

static int64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

// return 1 ok, 0 peer closed, -2 timeout, -1 error
static int read_n_timeout(int fd, void* buf, size_t n, int64_t deadline) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        int to;
        if (deadline >= 0) {
            int64_t rem = deadline - now_ms();
            if (rem <= 0) return -2;
            to = (rem > 60000) ? 60000 : (int)rem;
        } else {
            to = IO_TIMEOUT_MS;
        }
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, to);
        if (pr == 0) return -2;
        if (pr < 0) { if (errno==EINTR) continue; return -1; }
        if (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) return 0;

        ssize_t r = recv(fd, p+got, n-got, 0);
        if (r == 0) return 0;
        if (r < 0) {
            if (errno==EINTR) continue;
            if (errno==EAGAIN || errno==EWOULDBLOCK) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 1;
}

static void be32_to_host(uint32_t* w, int count){
    for (int i=0;i<count;i++) w[i]=ntohl(w[i]);
}

static void* client_thread(void* arg){
    int fd = (int)(intptr_t)arg;
    int one=1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){SOCK_RCVBUF}, sizeof(int));

    uint8_t  hdr[2];
    uint32_t words[MAX_WORDS];

    while (!g_stop_direct) {
        int64_t deadline = (FRAME_TIMEOUT_MS>0)? (now_ms()+FRAME_TIMEOUT_MS) : -1;

        int ok = read_n_timeout(fd, hdr, 2, deadline);
        if (ok == 0)  break;
        if (ok == -2) { DPRINT("timeout on count\n"); break; }
        if (ok < 0)   { DPRINT("read count error\n"); break; }

        uint16_t be_cnt; memcpy(&be_cnt, hdr, 2);
        int count = (int)ntohs(be_cnt);
        if (count <= 0 || count > MAX_WORDS) { DPRINT("bad count=%d\n",count); break; }

        size_t need = (size_t)count * 4;
        ok = read_n_timeout(fd, words, need, deadline);
        if (ok == 0)  break;
        if (ok == -2) { DPRINT("timeout during data\n"); break; }
        if (ok < 0)   { DPRINT("read data error\n"); break; }

        be32_to_host(words, count);
        int r = awg_send_words32(words, count);
        if (r != 0) DPRINT("awg_send_words32 ret=%d\n", r);
    }

    close(fd);
    return NULL;
}

static void* accept_loop(void* arg){
    (void)arg;
    while (!g_stop_direct) {
        struct sockaddr_in cli; socklen_t cl=sizeof(cli);
        int fd = accept(g_listen, (struct sockaddr*)&cli, &cl);
        if (fd < 0) {
            if (errno==EINTR) continue;
            perror("[DIRECT] accept"); continue;
        }
        pthread_t th; pthread_create(&th, NULL, client_thread, (void*)(intptr_t)fd);
        pthread_detach(th);
    }
    return NULL;
}

int start_direct_server(unsigned short port){
    g_stop_direct = 0;
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) { perror("[DIRECT] socket"); return -1; }
    int one=1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(g_listen, SOL_SOCKET, SO_RCVBUF, &(int){SOCK_RCVBUF}, sizeof(int));

    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY); a.sin_port=htons(port);
    if (bind(g_listen,(struct sockaddr*)&a,sizeof(a))<0){ perror("[DIRECT] bind"); close(g_listen); return -2; }
    if (listen(g_listen,8)<0){ perror("[DIRECT] listen"); close(g_listen); return -3; }

    pthread_t acc; pthread_create(&acc, NULL, accept_loop, NULL);
    pthread_detach(acc);
    printf("[DIRECT] listening on %u (no-queue)\n", port);
    return 0;
}

void stop_direct_server(void){
    g_stop_direct = 1;
    if (g_listen>=0){ close(g_listen); g_listen=-1; }
}