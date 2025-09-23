#!/usr/bin/env python3
import os, signal, asyncio, ctypes, socket, http
import websockets

# uvloop 可選；若安裝不上可拿掉，不影響正確性
try:
    import uvloop
    uvloop.install()
except Exception:
    pass

WS_HOST, WS_PORT, WS_PATH = "0.0.0.0", 8765, "/ws"
LIB_PATH = os.path.join(os.path.dirname(__file__), "libawg_core_mmap.so")

lib = ctypes.CDLL(LIB_PATH)
lib.awg_init.restype = ctypes.c_int
lib.awg_send_hex4.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                              ctypes.c_char_p, ctypes.c_char_p]
lib.awg_send_hex4.restype = ctypes.c_int
lib.awg_close.restype = None

FRAME_LEN   = 336
A_IDX_OFF   = 0;   A_IDX_END   = 24
A_GAIN_OFF  = 24;  A_GAIN_END  = 168
B_IDX_OFF   = 168; B_IDX_END   = 192
B_GAIN_OFF  = 192; B_GAIN_END  = 336

async def process_request(connection, request):
    if request.path != WS_PATH:
        body = b"404 Not Found (expected /ws)\n"
        return (http.HTTPStatus.NOT_FOUND,
                 [("Content-Type", "text/plain"),
                  ("Content-Length", str(len(body)))],
                body)
    return None  # 符合路徑就繼續做 WS 握手

async def ws_handler(ws):
    # 盡量關掉 Nagle
    try:
        sock = ws.transport.get_extra_info("socket")
        if isinstance(sock, socket.socket):
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except Exception:
        pass

    print("[WS] connected:", ws.remote_address)
    try:
        async for msg in ws:
            # 二進位固定長度 336 bytes：最快路徑
            if isinstance(msg, (bytes, bytearray)) and len(msg) == FRAME_LEN:
                mv = memoryview(msg)
                r = lib.awg_send_hex4(
                    ctypes.c_char_p(mv[A_IDX_OFF:A_IDX_END].tobytes()),
                    ctypes.c_char_p(mv[A_GAIN_OFF:A_GAIN_END].tobytes()),
                    ctypes.c_char_p(mv[B_IDX_OFF:B_IDX_END].tobytes()),
                    ctypes.c_char_p(mv[B_GAIN_OFF:B_GAIN_END].tobytes()),
                )
                if r != 0:
                    print("[WS] awg_send_hex4 ret=", r)
                continue

            # 相容 JSON（較慢）
            if isinstance(msg, str):
                try:
                    import json
                    d = json.loads(msg)
                    idxA  = d.get("idxA")  or d.get("idxA_hex")
                    gainA = d.get("gainA") or d.get("gainA_hex")
                    idxB  = d.get("idxB")  or d.get("idxB_hex")
                    gainB = d.get("gainB") or d.get("gainB_hex")
                    if idxA and gainA and idxB and gainB:
                        r = lib.awg_send_hex4(idxA.encode("ascii"),
                                              gainA.encode("ascii"),
                                              idxB.encode("ascii"),
                                              gainB.encode("ascii"))
                        if r != 0:
                            print("[WS] awg_send_hex4 ret=", r)
                except Exception:
                    pass
    except Exception as e:
        print("[WS] closed:", e)

async def main():
    r = lib.awg_init()
    if r != 0:
        raise RuntimeError(f"awg_init failed: {r}")

    print(f"[WS] Serving on ws://{WS_HOST}:{WS_PORT}{WS_PATH}")
    async with websockets.serve(
        ws_handler,
        WS_HOST,
        WS_PORT,
        process_request=process_request,
        compression=None,
        ping_interval=None,
        max_queue=0,
        reuse_port=True
    ):
        stop = asyncio.Future()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, stop.set_result, None)
            except NotImplementedError:
                pass
        await stop

    lib.awg_close()
    print("[WS] stopped")

if __name__ == "__main__":
    asyncio.run(main())