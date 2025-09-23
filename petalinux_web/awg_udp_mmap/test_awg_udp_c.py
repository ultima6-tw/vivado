#!/usr/bin/env python3
import socket, time

DEST = ("wavegenz7.local", 8766)   # 改成你的板子 IP / 埠

def idx_hex(active_idx):
    # 8 tones，每個 3 hex（tone0=active，其餘 000）
    out = [f"{active_idx:03X}"] + ["000"]*7
    return "".join(out)  # 24 chars

def gain_hex():
    # tone0 最大，其餘 0；每 tone 18 hex
    g1 = "00000000000001FFFF"
    g0 = "000000000000000000"
    return g1 + g0*7       # 144 chars

def build_frame(idxA, idxB):
    a_idx = idx_hex(idxA)
    a_gan = gain_hex()
    b_idx = idx_hex(idxB)
    b_gan = gain_hex()
    frame = (a_idx + a_gan + b_idx + b_gan).encode("ascii")
    assert len(frame) == 336
    return frame

if __name__ == "__main__":
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 大緩衝/低延遲
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1<<20)
    try: s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, 0x10)
    except OSError: pass

    f1 = build_frame(1, 1)    # 1 kHz
    f2 = build_frame(20, 20)  # 20 kHz
    toggle = False

    try:
        while True:
            s.sendto(f1 if not toggle else f2, DEST)
            toggle = not toggle
            time.sleep(0.001)   # 1 ms 交替；依需求調整
    except KeyboardInterrupt:
        pass
    finally:
        s.close()