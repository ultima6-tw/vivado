// awg_server_raw_mmap.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

#include "awg_core.h"   // 你已有的 header，內含 awg_init/awg_close/awg_send_words32

#define SERVER_PORT 9000
#define FRAME_SIZE  (32 * 4)   // 128 bytes

static int listen_fd = -1, conn_fd = -1;

static void cleanup(int sig) {
    if (conn_fd >= 0) close(conn_fd);
    if (listen_fd >= 0) close(listen_fd);
    awg_close();
    printf("\n[SRV] stopped\n");
    exit(0);
}

int main(void) {
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    if (awg_init() != 0) {
        fprintf(stderr, "[SRV] awg_init failed\n");
        return 1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        return 1;
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    printf("[SRV] Listening on port %d (Raw TCP)\n", SERVER_PORT);

    while (1) {
        conn_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (conn_fd < 0) {
            perror("accept");
            continue;
        }
        printf("[SRV] Client connected\n");

        uint8_t buf[FRAME_SIZE];
        ssize_t got = 0;
        while (1) {
            got = recv(conn_fd, buf, FRAME_SIZE, MSG_WAITALL);
            if (got <= 0) {
                if (got == 0) printf("[SRV] Client disconnected\n");
                else perror("recv");
                break;
            }
            if (got != FRAME_SIZE) {
                printf("[SRV] Incomplete frame (%zd bytes)\n", got);
                continue;
            }

            // Cast to uint32_t* 並呼叫 FPGA API
            const uint32_t *words = (const uint32_t *)buf;
            int r = awg_send_words32(words, 32);
            if (r != 0) {
                printf("[SRV] awg_send_words32 error=%d\n", r);
            }
        }

        close(conn_fd);
        conn_fd = -1;
    }

    cleanup(0);
    return 0;
}