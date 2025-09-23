#!/usr/bin/env python3
import socket, struct, time

HOST = "wavegenz7.local"     # 先可改成 "172.31.25.3" 測試
PORT = 9000

CH_A, TONE_0 = 0, 0
IDX_1K, IDX_20K = 0x001, 0x020
GAIN_MAX = 0x1FFFF
GAP_MS = 0.2

def pack_word(cmd, ch, tone, data20):
    return ((cmd & 0xF) << 28) | ((ch & 1) << 27) | ((tone & 7) << 24) | (data20 & 0xFFFFF)
def make_index_word(ch, tone, idx20): return pack_word(0x1, ch, tone, idx20)
def make_gain_word (ch, tone, g20):   return pack_word(0x2, ch, tone, g20)
def make_commit_word():                return (0xF << 28)

def send_frame(sock, words):
    count = len(words)
    hdr = struct.pack(">H", count)
    payload = b"".join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
    sock.sendall(hdr + payload)

def connect_with_timeout(host, port, timeout=3.0):
    # 解析並印出
    ip = socket.gethostbyname(host)
    print(f"[W-CLIENT] resolving {host} -> {ip}")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256*1024)
    except OSError: pass
    s.settimeout(timeout)
    s.connect((ip, port))
    s.settimeout(None)
    return s

def main():
    print(f"[W-CLIENT] connecting to {HOST}:{PORT} …")
    try:
        sock = connect_with_timeout(HOST, PORT, timeout=3.0)
        print("[W-CLIENT] connected")

        toggle = 0
        while True:
            if toggle == 0:
                words = [
                    make_index_word(CH_A, TONE_0, IDX_1K),
                    make_gain_word (CH_A, TONE_0, GAIN_MAX),
                    make_commit_word(),
                ]
            else:
                words = [
                    make_index_word(CH_A, TONE_0, IDX_20K),
                    make_gain_word (CH_A, TONE_0, GAIN_MAX),
                    make_commit_word(),
                ]
            send_frame(sock, words)
            toggle ^= 1
            if GAP_MS > 0:
                time.sleep(GAP_MS/1000.0)
    except Exception as e:
        print("[W-CLIENT] error:", e)

if __name__ == "__main__":
    main()