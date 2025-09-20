// =============================================================
// awg_core.c  —  High-speed AWG GPIO core (C, mmap /dev/mem)
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
// >>>>> 依你的設計修改下列兩個 BASE 實體位址（Vivado Address Editor）
#define DATA_GPIO_BASE   0x41200000u   // gpiochip0: 32-bit DATA bus
#define WEN_GPIO_BASE    0x41210000u   // gpiochip3: 1-bit  WEN
// <<<<<

// 寄存器 offset（AXI GPIO 單通道配置）
#define GPIO_DATA_OFFSET 0x00u
#define GPIO_TRI_OFFSET  0x04u

// WEN 所使用的 bit（通常用 bit0）
#define WEN_BIT          0

// WEN 極性與脈寬（edge only）
#define DEF_WEN_ACTHI    1  // 1: active-high, 0: active-low
#define DEF_WEN_US       0  // 0 = edge only（最快）

// Hex 長度（僅供註解參考；此版本不做 strlen 檢查以求極速）
enum { IDX_HEX_LEN = 24, GAIN_HEX_LEN = 144 };

// ------------------ MMAP globals ------------------
static int                g_fd_mem     = -1;
static volatile uint32_t *g_data_regs  = NULL;  // 指向 DATA GPIO base
static volatile uint32_t *g_wen_regs   = NULL;  // 指向 WEN  GPIO base
static size_t             g_map_size   = 0x1000; // 4KB 足夠 AXI GPIO

// ------------------ Barriers & tiny helpers ------------------
static inline void cpu_mb(void) {
    __sync_synchronize(); // 便宜的 full barrier
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

// ------------------ Word packing（與你原版一致） ------------------
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
// 這些解析假設輸入一定是合法的 0-9 / a-f / A-F，為了極速不做檢查
static inline uint32_t parse_hex_n(const char *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        unsigned int h = (c <= '9') ? (c - '0') : ((c | 32) - 'a' + 10);
        v = (v << 4) | h;
    }
    return v;
}

// idx: 3-hex; gain: 18-hex（僅取最後 5 hex = 20 bits）
static inline uint32_t parse_idx3_fast(const char *p3) {
    return parse_hex_n(p3, 3);               // 0..0xFFF（後續再 & 0xFFFFF）
}
static inline uint32_t parse_gain18_low5_fast(const char *p18) {
    return parse_hex_n(p18 + 13, 5);         // 僅取最低 20 bits
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
    busy_wait_us(pulse_us); // 0 表示純 edge
    gpio_write(g_wen_regs, GPIO_DATA_OFFSET, off);
}

// ------------------ Public API ------------------
int awg_init(void)
{
    // 開 /dev/mem 並映射兩個 AXI GPIO
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

    // 設成輸出：AXI GPIO TRI = 0 表輸出
    //gpio_write(g_data_regs, GPIO_TRI_OFFSET, 0x00000000u);          // 32bit 全輸出
    //gpio_write(g_wen_regs,  GPIO_TRI_OFFSET, ~(1u << WEN_BIT));     // 只保證 WEN_BIT 輸出；其餘隨意

    // 初始值
    gpio_write(g_data_regs, GPIO_DATA_OFFSET, 0x00000000u);
    // 依極性把 WEN 拉到「不觸發」的一側
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

    // 直接解析 + 送出（不做長度/格式檢查以爭取極速）
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

// 極速版：直接吃 32 個 32-bit words（已經是正確格式的 index/gain words）
// 由本函式在結尾自動送一次 commit
int awg_send_words32(const uint32_t *words32, int count)
{
    if (!g_data_req || !g_wen_req) return -1;
    if (!words32 || count != 32)   return -2;   // 固定 32 筆（A: idx8+gain8, B: idx8+gain8）

    for (int i = 0; i < 32; ++i) {
        write_word32(words32[i]);
        wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    }
    // 一次性 commit
    write_word32(make_commit_word());
    wen_edge(DEF_WEN_ACTHI, DEF_WEN_US);
    return 0;
}