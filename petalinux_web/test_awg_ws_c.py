from websocket import create_connection, ABNF
import socket, time

WS_URL = "ws://wavegenz7.local:8765/ws"   # 或用板子的 IP

# 固定片段（18 hex）：最大增益 & 零增益（注意：零增益是 18 個 '0'）
GAIN_MAX18  = b"00000000000001FFFF"
GAIN_ZERO18 = b"000000000000000000"

def build_frame(idx_val: int) -> bytes:
    """
    組一個 336 bytes 的 frame：
      idxA(24) + gainA(144) + idxB(24) + gainB(144)
    只開 tone0，其餘 tone 都 0。
    """
    idx0  = f"{idx_val:03X}".encode("ascii")           # 3 hex
    idx24 = idx0 + (b"000" * 7)                        # 3*8 = 24

    gain144 = GAIN_MAX18 + (GAIN_ZERO18 * 7)           # 18*8 = 144

    frame = idx24 + gain144 + idx24 + gain144
    assert len(frame) == 336
    return frame

def connect():
    ws = create_connection(WS_URL)
    # 關 Nagle，減少聚包
    ws.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # 視情況調小發送緩衝區，降低批次累積（可選）
    # ws.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
    print("[DEBUG] Connected!")
    return ws

if __name__ == "__main__":
    ws = None
    try:
        ws = connect()

        frame_1k  = build_frame(0x001)   # 1 kHz 的 index
        frame_20k = build_frame(0x020)   # 20 kHz 的 index（你之前寫 30 是 0x01E，確認用 0x020）

        period_s = 0.020   # 每 2 ms 切換一次，可自己調（用節拍法更穩定）
        next_t   = time.perf_counter()
        toggle   = False

        while True:
            payload = frame_1k if not toggle else frame_20k
            ws.send(payload, opcode=ABNF.OPCODE_BINARY)

            toggle = not toggle
            next_t += period_s
            # 節拍睡眠，避免抖動累積
            dt = next_t - time.perf_counter()
            if dt > 0:
                time.sleep(dt)
            else:
                # 若落後太多，立刻追上節拍
                next_t = time.perf_counter()
    except KeyboardInterrupt:
        print("[INFO] Stopped by user")
    finally:
        if ws:
            ws.close()
            print("[DEBUG] Closed connection")
