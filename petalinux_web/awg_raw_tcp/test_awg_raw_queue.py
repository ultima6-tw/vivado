#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Queue-mode AWG client (with mDNS hostname resolve)
- Connects to wavegenz7.local:9100 (resolve hostname -> IP)
- Protocol ops: Z/X/I/B/P/E/T/Q/S (all big-endian)
- Preloads list0 & list1 with 1000 frames each, auto-starts, prints status/stats.

Run:
  python3 queue_client_mdns.py
"""

import socket, struct, time

# ---------- Connection settings ----------
HOST = "wavegenz7.local"     # mDNS hostname (avahi)
PORT = 9100
CONNECT_TIMEOUT = 3.0
SO_SNDBUF = 256 * 1024

# ---------- Word helpers (match AWG bus format) ----------
def pack_word(cmd: int, ch: int, tone: int, data20: int) -> int:
    """[31:28]=cmd, [27]=ch, [26:24]=tone, [19:0]=data"""
    return ((cmd & 0xF) << 28) | ((ch & 1) << 27) | ((tone & 0x7) << 24) | (data20 & 0xFFFFF)

def make_index_word(ch: int, tone: int, idx20: int) -> int:
    return pack_word(0x1, ch, tone, idx20)

def make_gain_word(ch: int, tone: int, g20: int) -> int:
    return pack_word(0x2, ch, tone, g20)

def make_commit_word() -> int:
    return (0xF << 28)

# Example values (依你的校正值調整)
CH_A    = 0
TONE_0  = 0
IDX_1K  = 0x001
IDX_20K = 0x020
GAIN_FS = 0x1FFFF

# ---------- Tiny socket helpers (with mDNS resolve) ----------
def connect_with_mdns(host=HOST, port=PORT, timeout=CONNECT_TIMEOUT) -> socket.socket:
    # 先解析 mDNS 名稱成 IP
    ip = socket.gethostbyname(host)
    print(f"[CLIENT] resolving {host} -> {ip}")

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, SO_SNDBUF)
    except OSError:
        pass
    s.connect((ip, port))
    s.settimeout(None)  # 後續用 blocking I/O
    return s

def sendall(s: socket.socket, data: bytes):
    s.sendall(data)

def recv_exact(s: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("server closed")
        buf += chunk
    return bytes(buf)

# ---------- Protocol ops (big-endian) ----------
def op_Z_reset(s):     sendall(s, b'Z')
def op_X_abort(s):     sendall(s, b'X')
def op_I_init(s, list_id: int, max_frames_hint: int):
    sendall(s, b'I' + struct.pack(">B I", list_id & 1, max_frames_hint))
def op_B_begin(s, list_id: int, total_frames: int):
    sendall(s, b'B' + struct.pack(">B I", list_id & 1, total_frames))
def op_P_push(s, list_id: int, words):
    payload = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
    sendall(s, b'P' + struct.pack(">B H", list_id & 1, len(words)) + payload)
def op_E_end(s, list_id: int):
    sendall(s, b'E' + struct.pack(">B", list_id & 1))
def op_T_period(s, period_us: int):
    sendall(s, b'T' + struct.pack(">I", max(1, period_us)))
def op_Q_query(s):
    sendall(s, b'Q')
    r = recv_exact(s, 16)
    playing  = r[0]
    cur_list = r[1]
    cur_frame, free0, free1 = struct.unpack(">III", r[2:14])
    return dict(playing=playing, cur_list=cur_list, cur_frame=cur_frame, free0=free0, free1=free1)
def op_S_stats(s):
    sendall(s, b'S')
    r = recv_exact(s, 32)
    a = int.from_bytes(r[ 0: 8], "big")
    b = int.from_bytes(r[ 8:16], "big")
    c = int.from_bytes(r[16:24], "big")
    d = int.from_bytes(r[24:32], "big")
    return dict(bytes_rx=a, frames_pushed=b, switches=c, holds=d)

# ---------- Demo frames ----------
def frame_1k():
    return [
        make_index_word(CH_A, TONE_0, IDX_1K),
        make_gain_word (CH_A, TONE_0, GAIN_FS),
        make_commit_word(),
    ]

def frame_20k():
    return [
        make_index_word(CH_A, TONE_0, IDX_20K),
        make_gain_word (CH_A, TONE_0, GAIN_FS),
        make_commit_word(),
    ]

# ---------- Main ----------
def main():
    print(f"[CLIENT] connect {HOST}:{PORT} ...")
    s = connect_with_mdns()
    print("[CLIENT] connected")

    # 基本設定
    print("[CLIENT] RESET")
    op_Z_reset(s)
    print("[CLIENT] set period = 1000 us")
    op_T_period(s, 1000)

    # 預載 list0: 1000 frames（1k/20k 交替）
    nframes = 1000
    print(f"[CLIENT] preload list0 ({nframes} frames)")
    op_I_init(s, 0, nframes)
    op_B_begin(s, 0, nframes)
    for i in range(nframes):
        op_P_push(s, 0, frame_1k() if (i % 2 == 0) else frame_20k())
    op_E_end(s, 0)   # 自動開始播放

    # 背景預載 list1: 1000 frames（反向交替）
    print(f"[CLIENT] preload list1 ({nframes} frames)")
    op_I_init(s, 1, nframes)
    op_B_begin(s, 1, nframes)
    for i in range(nframes):
        op_P_push(s, 1, frame_20k() if (i % 2 == 0) else frame_1k())
    op_E_end(s, 1)

    # 查詢一段時間
    t0 = time.time()
    while time.time() - t0 < 5.0:
        st = op_Q_query(s)
        print(f"[Q] playing={st['playing']} list={st['cur_list']} frame={st['cur_frame']} free0={st['free0']} free1={st['free1']}")
        time.sleep(0.25)

    stats = op_S_stats(s)
    print(f"[S] {stats}")

    s.close()
    print("[CLIENT] done")

if __name__ == "__main__":
    main()