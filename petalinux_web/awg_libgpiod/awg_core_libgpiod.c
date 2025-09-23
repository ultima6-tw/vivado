// =============================================================
// awg_core.c  —  High-speed AWG GPIO core (C, libgpiod v2)
// -------------------------------------------------------------
// Build:
//   gcc -O3 -fPIC -shared -o libawg_core.so awg_core.c -lgpiod
//
// Minimal Python usage (ctypes):
//   import ctypes
//   lib = ctypes.CDLL("./libawg_core.so")
//   assert lib.awg_init() == 0
//   lib.awg_send_hex4(b"...24hex...", b"...144hex...", b"...24hex...", b"...144hex...")
//   lib.awg_close()
//
// -------------------------------------------------------------
// Input Format (FOUR HEX STRINGS, fixed length):
//   1) idxA_hex  : 24 hex chars  (3 hex per tone * 8 tones)
//   2) gainA_hex : 144 hex chars (18 hex per tone * 8 tones)
//   3) idxB_hex  : 24 hex chars
//   4) gainB_hex : 144 hex chars
//
//   - Index (idx): each tone uses 3 hex chars, 0x000 .. 0x383 (0..899)
//   - Gain (Q1.17): each tone uses 18 hex chars (72 bits) BUT ONLY THE
//     LOWEST 20 BITS ARE USED by hardware (mask to 0x1FFFF).
//     This matches the original Q1.17 format (0..0x1FFFF).
//
//   Total per channel: 24 + 144 = 168 hex
//   Both channels (A+B): 336 hex total, but passed as 4 groups to improve readability.
//   Order within each group is tone 0..7 (exactly 8 tones).
//
// -------------------------------------------------------------
// HW Notes:
//   - DATA bus: /dev/gpiochip0, 32-bit output (offsets 0..31)
//   - WEN line: /dev/gpiochip3, 1-bit output (offset DEF_WEN_OFF)
//   - WEN pulse: fastest possible edge (no delay).
//   - Commit word sent once at the end.
//
// -------------------------------------------------------------
// Words on the bus (32-bit):
//   [31:28] cmd: 0x1 = INDEX, 0x2 = GAIN, 0xF = COMMIT
//   [27]    ch : 0=A, 1=B
//   [26:24] tone: 0..7
//   [23:20] reserved (0)
//   [19:0]  payload: idx20 or gain20 (Q1.17 low 20 bits)
// =============================================================

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <gpiod.h>

// ------------------ Tunables ------------------
#define DEF_DATA_CHIP   "/dev/gpiochip0"
#define DEF_WEN_CHIP    "/dev/gpiochip3"
#define DEF_WEN_OFF     0

#define DEF_WEN_ACTHI   1   // active-high WEN
#define DEF_WEN_US      0   // 0 = edge only (fastest)

// 32-bit data bus offsets (customize if not 0..31)
static const unsigned DATA_OFFSETS[32] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
};

// ------------------ Globals ------------------
static struct gpiod_chip         *g_data_chip = NULL;
static struct gpiod_chip         *g_wen_chip  = NULL;
static struct gpiod_line_request *g_data_req  = NULL;
static struct gpiod_line_request *g_wen_req   = NULL;
static enum gpiod_line_value      g_vals32[32];

// ------------------ Tiny helpers ------------------
static inline void busy_wait_us(unsigned us) {
    if (us == 0) return;
    struct timespec ts = {0, (long)us * 1000L};
    nanosleep(&ts, NULL);
}

static inline void map_word_to_values(uint32_t w, enum gpiod_line_value *vals32) {
    for (int i = 0; i < 32; ++i)
        vals32[i] = (w & (1u << i)) ? GPIOD_LINE_VALUE_ACTIVE
                                    : GPIOD_LINE_VALUE_INACTIVE;
}

// ---------- Word packing ----------
static inline uint32_t pack_sel(int ch, int tone) {
    return ((uint32_t)(ch & 1) << 27) | ((uint32_t)(tone & 7) << 24);
}
static inline uint32_t make_index_word(int ch, int tone, uint32_t idx20) {
    return (0x1u << 28) | pack_sel(ch, tone) | (idx20 & 0xFFFFFu);
}
static inline uint32_t make_gain_word(int ch, int tone, uint32_t g20) {
    return (0x2u << 28) | pack_sel(ch, tone) | (g20 & 0xFFFFFu);
}
static inline uint32_t make_commit_word(void) {
    return (0xFu << 28);
}

// ---------- Ultra-fast hex helpers (NO validation) ----------
static inline uint32_t parse_hex_n(const char *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        unsigned int h = (c <= '9') ? (c - '0') : ((c | 32) - 'a' + 10);
        v = (v << 4) | h;
    }
    return v;
}

// idx: 3-hex; gain: 18-hex (take lowest 5 hex = 20 bits)
static inline uint32_t parse_idx3_fast(const char *p3) {
    return parse_hex_n(p3, 3);              // 12-bit -> 後面再 & 0xFFFFF
}
static inline uint32_t parse_gain18_low5_fast(const char *p18) {
    return parse_hex_n(p18 + 13, 5);        // 只取最後 5 個 hex = 20-bit
}

// ------------------ Low-level I/O ------------------
static inline void write_word32(uint32_t w) {
    map_word_to_values(w, g_vals32);
    gpiod_line_request_set_values(g_data_req, g_vals32);
}

static inline void wen_edge(int active_high, int pulse_us) {
    enum gpiod_line_value on  = active_high ? GPIOD_LINE_VALUE_ACTIVE
                                            : GPIOD_LINE_VALUE_INACTIVE;
    enum gpiod_line_value off = active_high ? GPIOD_LINE_VALUE_INACTIVE
                                            : GPIOD_LINE_VALUE_ACTIVE;
    gpiod_line_request_set_value(g_wen_req, DEF_WEN_OFF, on);
    busy_wait_us(pulse_us);
    gpiod_line_request_set_value(g_wen_req, DEF_WEN_OFF, off); 
}

// ------------------ Public API ------------------
int awg_init(void)
{
    // open chips
    g_data_chip = gpiod_chip_open(DEF_DATA_CHIP);
    if (!g_data_chip) { perror("gpiod_chip_open(data)"); return -1; }

    g_wen_chip = gpiod_chip_open(DEF_WEN_CHIP);
    if (!g_wen_chip) { perror("gpiod_chip_open(wen)"); return -2; }

    // common out settings
    struct gpiod_line_settings *ls_out = gpiod_line_settings_new();
    if (!ls_out) { fprintf(stderr, "line_settings_new failed\n"); return -3; }
    gpiod_line_settings_set_direction(ls_out, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(ls_out, GPIOD_LINE_VALUE_INACTIVE);

    struct gpiod_request_config *rcfg = gpiod_request_config_new();
    if (!rcfg) { fprintf(stderr, "request_config_new failed\n"); gpiod_line_settings_free(ls_out); return -4; }
    gpiod_request_config_set_consumer(rcfg, "awg_core");

    // data bus
    struct gpiod_line_config *lcfg_data = gpiod_line_config_new();
    if (!lcfg_data) { fprintf(stderr, "line_config_new(data) failed\n"); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -5; }
    if (gpiod_line_config_add_line_settings(lcfg_data, DATA_OFFSETS, 32, ls_out) < 0) {
        perror("line_config_add_line_settings(data)");
        gpiod_line_config_free(lcfg_data); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -6;
    }
    g_data_req = gpiod_chip_request_lines(g_data_chip, rcfg, lcfg_data);
    if (!g_data_req) {
        perror("chip_request_lines(data)");
        gpiod_line_config_free(lcfg_data); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -7;
    }

    // WEN
    struct gpiod_line_config *lcfg_wen = gpiod_line_config_new();
    if (!lcfg_wen) { fprintf(stderr, "line_config_new(wen) failed\n"); gpiod_line_request_release(g_data_req); g_data_req=NULL; gpiod_line_config_free(lcfg_data); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -8; }
    const unsigned wen_off = DEF_WEN_OFF;
    if (gpiod_line_config_add_line_settings(lcfg_wen, &wen_off, 1, ls_out) < 0) {
        perror("line_config_add_line_settings(wen)");
        gpiod_line_config_free(lcfg_wen); gpiod_line_request_release(g_data_req); g_data_req=NULL; gpiod_line_config_free(lcfg_data); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -9;
    }
    g_wen_req = gpiod_chip_request_lines(g_wen_chip, rcfg, lcfg_wen);
    if (!g_wen_req) {
        perror("chip_request_lines(wen)");
        gpiod_line_config_free(lcfg_wen); gpiod_line_request_release(g_data_req); g_data_req=NULL; gpiod_line_config_free(lcfg_data); gpiod_request_config_free(rcfg); gpiod_line_settings_free(ls_out); return -10;
    }

    // cleanup one-shot objects
    gpiod_line_config_free(lcfg_wen);
    gpiod_line_config_free(lcfg_data);
    gpiod_request_config_free(rcfg);
    gpiod_line_settings_free(ls_out);

    // init levels
    for (int i = 0; i < 32; ++i) g_vals32[i] = GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_values(g_data_req, g_vals32);
    gpiod_line_request_set_value(g_wen_req, DEF_WEN_OFF, GPIOD_LINE_VALUE_INACTIVE);

    return 0;
}

void awg_close(void)
{
    if (g_wen_req)   { gpiod_line_request_release(g_wen_req); g_wen_req = NULL; }
    if (g_data_req)  { gpiod_line_request_release(g_data_req); g_data_req = NULL; }
    if (g_wen_chip)  { gpiod_chip_close(g_wen_chip);  g_wen_chip  = NULL; }
    if (g_data_chip) { gpiod_chip_close(g_data_chip); g_data_chip = NULL; }
}

// ---- Fast path: accept four HEX blocks, parse & stream immediately ----
int awg_send_hex4(const char *idxA_hex, const char *gainA_hex,
                  const char *idxB_hex, const char *gainB_hex)
{
    if (!g_data_req || !g_wen_req) return -1;
    if (!idxA_hex || !gainA_hex || !idxB_hex || !gainB_hex) return -2;

    // 直接解析 + 送出
    // Channel A: 8*index, 8*gain
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_idx3_fast(idxA_hex + 3 * t) & 0xFFFFF;
        write_word32(make_index_word(0, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_gain18_low5_fast(gainA_hex + 18 * t) & 0xFFFFF;
        write_word32(make_gain_word(0, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }

    // Channel B: 8*index, 8*gain
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_idx3_fast(idxB_hex + 3 * t) & 0xFFFFF;
        write_word32(make_index_word(1, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_gain18_low5_fast(gainB_hex + 18 * t) & 0xFFFFF;
        write_word32(make_gain_word(1, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }

    // Commit once
    write_word32(make_commit_word());
    wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);

    return 0;
}