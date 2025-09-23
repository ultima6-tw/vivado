// awg_core.h
#ifndef AWG_CORE_H
#define AWG_CORE_H

#include <stdint.h>

// 初始化 GPIO (只要呼叫一次)
int awg_init(void);

// 傳送一組 hex string (4 條字串：24, 144, 24, 144)
int awg_send_hex4(
    const char *idxA_hex,   // 24 hex chars
    const char *gainA_hex,  // 144 hex chars
    const char *idxB_hex,   // 24 hex chars
    const char *gainB_hex   // 144 hex chars
);

int awg_send_words32(const uint32_t *words32, int count);

// 結束 (釋放資源)
void awg_close(void);

#endif