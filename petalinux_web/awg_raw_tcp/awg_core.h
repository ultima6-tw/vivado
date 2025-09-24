// awg_core.h
#ifndef AWG_CORE_H
#define AWG_CORE_H

#include <stdint.h>

// Initialize GPIO (call only once at startup)
int awg_init(void);

// Send a set of hex strings (4 strings: 24, 144, 24, 144 chars)
int awg_send_hex4(
    const char *idxA_hex,   // 24 hex chars
    const char *gainA_hex,  // 144 hex chars
    const char *idxB_hex,   // 24 hex chars
    const char *gainB_hex   // 144 hex chars
);

// Send an array of pre-packed 32-bit words to the hardware
int awg_send_words32(const uint32_t *words32, int count);

// Zeros all output gains for safety
int awg_zero_output(void);

// Deinitialize and release resources
void awg_close(void);

#endif // AWG_CORE_H