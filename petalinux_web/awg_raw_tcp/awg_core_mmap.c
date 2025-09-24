// =============================================================
// awg_core_mmap.c  —  High-speed AWG GPIO core (C, mmap /dev/mem)
// -------------------------------------------------------------
// Build (shared lib):
//   gcc -O3 -fPIC -shared -o libawg_core.so awg_core.c
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
// HW Notes (AXI GPIO, single channel each):
//   - DATA bus AXI GPIO (32-bit wide):
//       BASE  = DATA_GPIO_BASE
//       DATA  = BASE + 0x00 (GPIO_DATA)
//       TRI   = BASE + 0x04 (GPIO_TRI)  : 0 = output
//   - WEN line AXI GPIO (1-bit used: WEN_BIT):
//       BASE  = WEN_GPIO_BASE
//       DATA  = BASE + 0x00
//       TRI   = BASE + 0x04
//   - We toggle WEN per word (edge only, no extra delay).
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
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

// ------------------ AXI GPIO base addresses (EDIT THESE) ------------------
// >>>>> EDIT these two BASE physical addresses according to your design (Vivado Address Editor) <<<<<
#define DATA_GPIO_BASE   0x41200000u   // gpiochip0: 32-bit DATA bus
#define WEN_GPIO_BASE    0x41210000u   // gpiochip3: 1-bit  WEN
// <<<<<

// Register offsets (for single-channel AXI GPIO config)
#define GPIO_DATA_OFFSET 0x00u
#define GPIO_TRI_OFFSET  0x04u

// Bit used for WEN (typically bit 0)
#define WEN_BIT          0

// WEN polarity and pulse width (edge only)
#define DEF_WEN_ACTHI    1  // 1: active-high, 0: active-low
#define DEF_WEN_US       0  // 0 = edge only (fastest)

// Hex length (for reference only; this version skips strlen checks for max speed)
enum { IDX_HEX_LEN = 24, GAIN_HEX_LEN = 144 };

// ------------------ MMAP globals ------------------
static int                g_fd_mem     = -1;
static volatile uint32_t *g_data_regs  = NULL;  // points to DATA GPIO base
static volatile uint32_t *g_wen_regs   = NULL;  // points to WEN  GPIO base
static size_t             g_map_size   = 0x1000; // 4KB is sufficient for AXI GPIO

// ------------------ Barriers & tiny helpers ------------------
static inline void cpu_mb(void) {
    __sync_synchronize(); // a cheap full memory barrier
}

static inline void busy_wait_us(unsigned us) {
    if (us == 0) return;
    struct timespec ts = {0, (long)us * 1000L};
    nanosleep(&ts, NULL);
}

// ------------------ AXI GPIO R/W ------------------
static inline void gpio_write(volatile uint32_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)((uintptr_t)base + off) = v;
    cpu_mb();
}

static inline uint32_t gpio_read(volatile uint32_t *base, uint32_t off) {
    uint32_t v = *(volatile uint32_t *)((uintptr_t)base + off);
    cpu_mb();
    return v;
}

// ------------------ Word packing (matches your original version) ------------------
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

// ------------------ Ultra-fast hex helpers (NO validation) ------------------
// These parsers assume the input is always valid hex (0-9, a-f, A-F).
// No validation is performed for maximum speed.
static inline uint32_t parse_hex_n(const char *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        unsigned int h = (c <= '9') ? (c - '0') : ((c | 32) - 'a' + 10);
        v = (v << 4) | h;
    }
    return v;
}

// idx: 3-hex; gain: 18-hex (only the last 5 hex = 20 bits are used)
static inline uint32_t parse_idx3_fast(const char *p3) {
    return parse_hex_n(p3, 3);               // 0..0xFFF (will be masked with 0xFFFFF later)
}
static inline uint32_t parse_gain18_low5_fast(const char *p18) {
    return parse_hex_n(p18 + 13, 5);         // only parse the lowest 20 bits
}

// ------------------ Low-level AWG strobes ------------------
static inline void write_word32(uint32_t w) {
    gpio_write(g_data_regs, GPIO_DATA_OFFSET, w);
}

static inline void wen_edge(int active_high, int pulse_us) {
    uint32_t val = gpio_read(g_wen_regs, GPIO_DATA_OFFSET);
    uint32_t on  = active_high ? (val |  (1u << WEN_BIT))
                               : (val & ~(1u << WEN_BIT));
    uint32_t off = active_high ? (val & ~(1u << WEN_BIT))
                               : (val |  (1u << WEN_BIT));
    gpio_write(g_wen_regs, GPIO_DATA_OFFSET, on);
    busy_wait_us(pulse_us); // 0 means edge-only (fastest)
    gpio_write(g_wen_regs, GPIO_DATA_OFFSET, off);
}

// ------------------ Public API ------------------
int awg_init(void)
{
    // Open /dev/mem and map the two AXI GPIO regions
    g_fd_mem = open("/dev/mem", O_RDWR | O_SYNC);
    if (g_fd_mem < 0) { perror("open /dev/mem"); return -1; }

    g_data_regs = (volatile uint32_t *)mmap(NULL, g_map_size, PROT_READ|PROT_WRITE,
                                            MAP_SHARED, g_fd_mem, DATA_GPIO_BASE);
    if (g_data_regs == MAP_FAILED) {
        perror("mmap DATA");
        g_data_regs = NULL; close(g_fd_mem); g_fd_mem = -1;
        return -2;
    }

    g_wen_regs = (volatile uint32_t *)mmap(NULL, g_map_size, PROT_READ|PROT_WRITE,
                                           MAP_SHARED, g_fd_mem, WEN_GPIO_BASE);
    if (g_wen_regs == MAP_FAILED) {
        perror("mmap WEN");
        munmap((void*)g_data_regs, g_map_size); g_data_regs = NULL;
        close(g_fd_mem); g_fd_mem = -1;
        return -3;
    }

    // Set GPIO direction to output: AXI GPIO TRI=0 means output
    //gpio_write(g_data_regs, GPIO_TRI_OFFSET, 0x00000000u);          // all 32 bits as output
    //gpio_write(g_wen_regs,  GPIO_TRI_OFFSET, ~(1u << WEN_BIT));     // ensure WEN_BIT is output; others don't care

    // Set initial values
    gpio_write(g_data_regs, GPIO_DATA_OFFSET, 0x00000000u);
    // Pull WEN to the inactive state according to its polarity
    uint32_t w = gpio_read(g_wen_regs, GPIO_DATA_OFFSET);
    if (DEF_WEN_ACTHI) w &= ~(1u << WEN_BIT); else w |= (1u << WEN_BIT);
    gpio_write(g_wen_regs, GPIO_DATA_OFFSET, w);

    return 0;
}

void awg_close(void)
{
    if (g_data_regs) { munmap((void*)g_data_regs, g_map_size); g_data_regs = NULL; }
    if (g_wen_regs)  { munmap((void*)g_wen_regs,  g_map_size); g_wen_regs  = NULL; }
    if (g_fd_mem >= 0) { close(g_fd_mem); g_fd_mem = -1; }
}

// ---- Fast path: accept four HEX blocks, parse & stream immediately ----
int awg_send_hex4(const char *idxA_hex, const char *gainA_hex,
                  const char *idxB_hex, const char *gainB_hex)
{
    if (!g_data_regs || !g_wen_regs) return -1;
    if (!idxA_hex || !gainA_hex || !idxB_hex || !gainB_hex) return -2;

    // Directly parse and send (no length/format checks for max speed)
    // Channel A: 8*index, 8*gain
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_idx3_fast(idxA_hex + 3 * t) & 0xFFFFFu;
        write_word32(make_index_word(0, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_gain18_low5_fast(gainA_hex + 18 * t) & 0xFFFFFu;
        write_word32(make_gain_word(0, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }

    // Channel B: 8*index, 8*gain
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_idx3_fast(idxB_hex + 3 * t) & 0xFFFFFu;
        write_word32(make_index_word(1, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    for (int t = 0; t < 8; ++t) {
        uint32_t v20 = parse_gain18_low5_fast(gainB_hex + 18 * t) & 0xFFFFFu;
        write_word32(make_gain_word(1, t, v20));
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }

    // Commit once
    write_word32(make_commit_word());
    wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);

    return 0;
}

// Flexible version: stream exactly "count" words (caller decides commit)
int awg_send_words32(const uint32_t *words32, int count)
{
    if (!g_data_regs || !g_wen_regs) return -1;
    if (!words32 || count <= 0) return -2;

    for (int i = 0; i < count; ++i) {
        write_word32(words32[i]);
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    return 0;
}

// [NEW] Sets all tone gains to zero and issues a commit command.
// This is a safety function to ensure the hardware is in a known safe state.
int awg_zero_output(void)
{
    if (!g_data_regs || !g_wen_regs) return -1;

    // Build: write GAIN=0 for A.tone0..7 and B.tone0..7, then COMMIT
    uint32_t words[16 + 1];
    int idx = 0;

    for (int ch = 0; ch < 2; ++ch) {
        for (int tone = 0; tone < 8; ++tone) {
            uint32_t w = (0x2u << 28)
                       | ((uint32_t)(ch & 1) << 27)
                       | ((uint32_t)(tone & 7) << 24)
                       | 0u; // gain20 = 0
            words[idx++] = w;
        }
    }
    words[idx++] = (0xFu << 28); // COMMIT
    
    // Use the existing awg_send_words32 to send the sequence
    return awg_send_words32(words, idx);
}