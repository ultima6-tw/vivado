// test_awg.c
#include <stdio.h>
#include <time.h>
#include <string.h>
#include "awg_core.h"

// === 依你的表校正為對應的 index（0..899）===
#define IDX_1K   0x001   // TODO: 改成你硬體上 1 kHz 的 index
#define IDX_20K  0x020   // TODO: 改成你硬體上 20 kHz 的 index

// Q1.17 最大增益 0x1FFFF → 18-hex（低 5 位有效）："0000000000000" + "1FFFF"
static const char *GAIN_MAX18 = "00000000000001FFFF";
static const char *GAIN_ZERO18 = "000000000000000000";

// 簡單 ns sleep（可照需求調整間隔長度觀察切換）
static void sleep_ns(long ns) {
    struct timespec ts;
    ts.tv_sec  = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

// 產生「單一 peak」的 A 通道封包：idx 放在 tone[0]，其他 tone 清 0；B 通道全清 0
static void build_single_peak_hex(unsigned idxA,
                                  char idxA_hex[24+1],  char gainA_hex[144+1],
                                  char idxB_hex[24+1],  char gainB_hex[144+1]) {
    // ---- A: index ----
    // 8*3 = 24 chars
    // tone0 = %03X，其餘 "000"
    char *p = idxA_hex;
    snprintf(p, 4, "%03X", (idxA & 0xFFF)); p += 3;
    for (int i = 1; i < 8; ++i) { memcpy(p, "000", 3); p += 3; }
    *p = '\0';

    // ---- A: gain ----
    // tone0 = GAIN_MAX18，其餘 GAIN_ZERO18
    p = gainA_hex;
    memcpy(p, GAIN_MAX18, 18); p += 18;
    for (int i = 1; i < 8; ++i) { memcpy(p, GAIN_ZERO18, 18); p += 18; }
    *p = '\0';

    // ---- B: 全部清零 ----
    p = idxB_hex;
    for (int i = 0; i < 8; ++i) { memcpy(p, "000", 3); p += 3; }
    *p = '\0';

    p = gainB_hex;
    for (int i = 0; i < 8; ++i) { memcpy(p, GAIN_ZERO18, 18); p += 18; }
    *p = '\0';
}

int main(void) {
    if (awg_init() < 0) {
        fprintf(stderr, "Failed to init AWG core\n");
        return 1;
    }

    // 預先配置四段字串（固定長度 + 結尾 '\0'）
    char idxA_hex[25],  gainA_hex[145];
    char idxB_hex[25],  gainB_hex[145];

    // 兩次送包之間的間隔（越短越快切換；視示波器觸發調整）
    const long GAP_NS = 200000;   // 0.2 ms = 200,000 ns（自行調整）

    printf("Loop: one-peak 1kHz (max amp) -> one-peak 20kHz (max amp) -> repeat\n");

    for (;;) {
        // 一次 1 kHz、單一 peak、滿幅
        build_single_peak_hex(IDX_1K, idxA_hex, gainA_hex, idxB_hex, gainB_hex);
        awg_send_hex4(idxA_hex, gainA_hex, idxB_hex, gainB_hex);

        sleep_ns(GAP_NS);

        // 一次 20 kHz、單一 peak、滿幅
        build_single_peak_hex(IDX_20K, idxA_hex, gainA_hex, idxB_hex, gainB_hex);
        awg_send_hex4(idxA_hex, gainA_hex, idxB_hex, gainB_hex);

        sleep_ns(GAP_NS);
    }

    return 0;
}