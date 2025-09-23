#!/usr/bin/env python3
import os, socket, ctypes, signal

HOST, PORT = "0.0.0.0", 8766           # 換成你要的 UDP 監聽埠
LIB_PATH   = os.path.join(os.path.dirname(__file__), "libawg_core_mmap.so")

# 載入 C 函式庫
lib = ctypes.CDLL(LIB_PATH)
lib.awg_init.restype = ctypes.c_int
lib.awg_send_hex4.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                              ctypes.c_char_p, ctypes.c_char_p]
lib.awg_send_hex4.restype = ctypes.c_int
lib.awg_close.restype = None

# 固定 336 bytes 的 ASCII-hex 幀切片
FRAME = 336
A_IDX = slice(0, 24)
A_GAN = slice(24, 168)
B_IDX = slice(168, 192)
B_GAN = slice(192, 336)

def main():
    # 初始化硬體
    r = lib.awg_init()
    if r != 0:
        raise RuntimeError(f"awg_init failed: {r}")

    # 建立 UDP socket（blocking，最省開銷）
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # 低延遲/大緩衝
    try: sock.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x10)   # Low Delay
    except OSError: pass
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1<<20)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1<<20)
    sock.bind((HOST, PORT))

    print(f"[UDP] Serving on udp://{HOST}:{PORT}  (expect {FRAME} bytes per datagram)")

    buf = bytearray(FRAME)         # 接收區
    mv  = memoryview(buf)

    try:
        while True:
            n, addr = sock.recvfrom_into(buf)   # 一次一包
            if n != FRAME:
                # 丟掉非固定長度的包，必要時可加 JSON fallback
                continue

            # 直接把 4 段切給 C（避免多餘複製）
            r = lib.awg_send_hex4(
                ctypes.c_char_p(mv[A_IDX].tobytes()),
                ctypes.c_char_p(mv[A_GAN].tobytes()),
                ctypes.c_char_p(mv[B_IDX].tobytes()),
                ctypes.c_char_p(mv[B_GAN].tobytes()),
            )
            # r!=0 可印錯，但為了極速建議關閉
            # if r != 0: print("[UDP] awg_send_hex4 ret=", r)

    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        lib.awg_close()
        print("[UDP] stopped")

if __name__ == "__main__":
    main()