#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import socket
import struct
import time

# =========================
# Hard-coded settings
# =========================
HOST = "wavegenz7.local"
PORT = 9000

CH_A   = 0
TONE_0 = 0

IDX_1K   = 0x001      # your calibrated index for 1 kHz
IDX_20K  = 0x020      # your calibrated index for 20 kHz
GAIN_MAX = 0x1FFFF    # Q1.17 full-scale (low 20 bits)

GAP_MS = 15.0          # pause between steps (ms). set 0 for fastest
RECONNECT_DELAY_S = 0.2

# =========================
# Protocol helpers (W mode)
# =========================
def pack_word(cmd: int, ch: int, tone: int, data20: int) -> int:
    """[31:28]=cmd, [27]=ch, [26:24]=tone, [19:0]=data"""
    return ((cmd & 0xF) << 28) | ((ch & 1) << 27) | ((tone & 0x7) << 24) | (data20 & 0xFFFFF)

def make_index_word(ch: int, tone: int, idx20: int) -> int:
    return pack_word(0x1, ch, tone, idx20)

def make_gain_word(ch: int, tone: int, g20: int) -> int:
    return pack_word(0x2, ch, tone, g20)

def make_commit_word() -> int:
    return (0xF << 28)

def send_frame(sock: socket.socket, words):
    """Send one W-frame: [2-byte big-endian COUNT] + COUNT * [4-byte big-endian word]."""
    count = len(words)
    hdr = struct.pack(">H", count)
    payload = b"".join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
    sock.sendall(hdr + payload)

def connect():
    """Low-latency TCP connection (TCP_NODELAY, larger send buffer)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
    except OSError:
        pass
    s.connect((HOST, PORT))
    return s

def main():
    print(f"[W-CLIENT] connecting to {HOST}:{PORT} ... (Ctrl+C to stop)")
    toggle = 0
    sock = None
    try:
        sock = connect()
        print("[W-CLIENT] connected")

        while True:
            if toggle == 0:
                # A.tone0 = 1k @ max, then commit
                words = [
                    make_index_word(CH_A, TONE_0, IDX_1K),
                    make_gain_word (CH_A, TONE_0, GAIN_MAX),
                    make_commit_word(),
                ]
                # print("[W-CLIENT] -> 1 kHz")  # uncomment if you need debug
            else:
                # A.tone0 = 20k @ max, then commit
                words = [
                    make_index_word(CH_A, TONE_0, IDX_20K),
                    make_gain_word (CH_A, TONE_0, GAIN_MAX),
                    make_commit_word(),
                ]
                # print("[W-CLIENT] -> 20 kHz")

            send_frame(sock, words)
            toggle ^= 1

            if GAP_MS > 0:
                time.sleep(GAP_MS / 1000.0)

    except KeyboardInterrupt:
        print("\n[W-CLIENT] stopped by user")
    except Exception as e:
        print(f"[W-CLIENT] error: {e}")
    finally:
        if sock:
            try: sock.close()
            except Exception: pass

if __name__ == "__main__":
    main()
