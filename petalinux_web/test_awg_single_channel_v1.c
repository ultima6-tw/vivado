// test_awg.c — A.ch0 toggle 1k/20k, commit as a separate word
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "awg_core.h"   // must provide awg_init(), awg_send_words32(), awg_close()

// ======== Calibrated indices (0..899) ========
#define IDX_1K    0x001
#define IDX_20K   0x020

// Q1.17 max amplitude (low 20 bits)
#define GAIN_Q1_17_MAX  0x1FFFFu

// ---------- Word packing (must match your RTL) ----------
static inline uint32_t pack_sel(int ch, int tone) {
    return ((uint32_t)(ch & 1) << 27) | ((uint32_t)(tone & 7) << 24);
}
// 0x1 = INDEX, 0x2 = GAIN, 0xF = COMMIT
static inline uint32_t make_index_word(int ch, int tone, uint32_t idx20) {
    return (0x1u << 28) | pack_sel(ch, tone) | (idx20 & 0xFFFFFu);
}
static inline uint32_t make_gain_word(int ch, int tone, uint32_t g20) {
    return (0x2u << 28) | pack_sel(ch, tone) | (g20 & 0xFFFFFu);
}
static inline uint32_t make_commit_word(void) {
    return (0xFu << 28);
}

static void sleep_ns(long ns) {
    struct timespec ts;
    ts.tv_sec  = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

int main(void) {
    if (awg_init() < 0) {
        fprintf(stderr, "Failed to init AWG core\n");
        return 1;
    }

    const int CH_A   = 0;
    const int TONE_0 = 0;
    const long GAP_NS = 200000;  // 0.2 ms — adjust to taste

    printf("Loop: A.tone0 -> 1k(max) [commit] -> 20k(max) [commit] -> repeat\n");

    for (;;) {
        // ---- Set 1 kHz on A.tone0, then commit ----
        {
            uint32_t w[3];
            w[0] = make_index_word (CH_A, TONE_0, IDX_1K);
            w[1] = make_gain_word  (CH_A, TONE_0, GAIN_Q1_17_MAX);
            w[2] = make_commit_word();
            awg_send_words32(w, 3);
        }
        sleep_ns(GAP_NS);

        // ---- Set 20 kHz on A.tone0, then commit ----
        {
            uint32_t w[3];
            w[0] = make_index_word (CH_A, TONE_0, IDX_20K);
            w[1] = make_gain_word  (CH_A, TONE_0, GAIN_Q1_17_MAX);
            w[2] = make_commit_word();
            awg_send_words32(w, 3);
        }
        sleep_ns(GAP_NS);
    }

    // Not reached in this demo
    // awg_close();
    // return 0;
}
