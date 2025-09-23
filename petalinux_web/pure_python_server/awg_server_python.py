#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import json
import threading
import asyncio
from flask import Flask, request, send_from_directory, Response

import awg_core as core

# -------------------------------
# 基本設定
# -------------------------------
BASE_DIR = os.path.abspath(os.path.dirname(__file__))
WWW_DIR  = os.path.join(BASE_DIR, "www")

HTTP_HOST = "0.0.0.0"
HTTP_PORT = 5000

WS_HOST   = "0.0.0.0"
WS_PORT   = 8765            # 標準 WebSocket 端口
WS_PATH   = "/ws"

app = Flask(__name__, static_folder=WWW_DIR)

# -------------------------------
# 公用工具
# -------------------------------
def _as_bool(v, default=True):
    if v is None:
        return default
    s = str(v).strip().lower()
    return not (s in ("0", "false", "no", "off", ""))

def normalize_list(val, n, default=0, cast=int):
    """
    支援:
      - 字串: "1,2,3"
      - list: [1,2,3]
      - 其他: 視為空
    轉為固定長度 n 的 list，元素用 cast 轉型，缺值補 default。
    """
    if isinstance(val, str):
        items = [x.strip() for x in val.split(",") if x.strip() != ""]
    elif isinstance(val, list):
        items = val
    else:
        items = []

    out = []
    for x in items:
        try:
            out.append(cast(x))
        except Exception:
            out.append(default)

    if len(out) < n:
        out += [default] * (n - len(out))
    else:
        out = out[:n]
    return out

def _apply_payload(payload):
    """
    將解析好的 dict 套用到 AWG（不回傳任何資料）。
    僅依外部傳入的值運作，不再在伺服器端造 list 或預設增補。
    """
    # 控制參數
    wen_acthi = _as_bool(payload.get("wen_active", 1), default=True)
    pulse_us  = int(payload.get("wen_pulse_us", core.DEF_WEN_US) or core.DEF_WEN_US)

    # index/gain（支援 *_f 與整數 Q1.17）
    idxA = normalize_list(payload.get("idxA"), 8, 0, int)
    idxB = normalize_list(payload.get("idxB"), 8, 8, int)

    if ("gainA_f" in payload) or ("gainB_f" in payload):
        gA_f = normalize_list(payload.get("gainA_f"), 8, 0.0, float)
        gB_f = normalize_list(payload.get("gainB_f"), 8, 0.0, float)
        gainA = [core.gain_f_to_q17(x) for x in gA_f]
        gainB = [core.gain_f_to_q17(x) for x in gB_f]
    else:
        gainA = normalize_list(payload.get("gainA"), 8, 0, int)
        gainB = normalize_list(payload.get("gainB"), 8, 0, int)

    # 打包與送出（內部使用已常駐的 GPIO handle）
    words = core.build_words(idxA, gainA, idxB, gainB)
    core.send_words(words, do_commit=True, wen_active_high=wen_acthi, pulse_us=pulse_us)

# -------------------------------
# Flask：靜態檔與 HTTP POST API
# -------------------------------
@app.route("/")
def root():
    return send_from_directory(WWW_DIR, "index.html")

@app.route("/<path:path>")
def static_files(path):
    return send_from_directory(WWW_DIR, path)

@app.route("/apply", methods=["POST"])
def apply_awg():
    """
    表單或 JSON 都可；為了省時間，固定回 204 No Content。
    """
    try:
        if request.is_json:
            payload = request.get_json(silent=True) or {}
        else:
            f = request.form
            payload = {k: f.get(k) for k in f.keys()}

        _apply_payload(payload)
        return Response(status=204)
    except Exception as e:
        print("[AWG error]", str(e))
        return Response(status=204)

# -------------------------------
# 標準 WebSocket 伺服器（websockets）
# -------------------------------
# 模組層常數（與 HTTP 區保持一致）
WS_HOST   = "0.0.0.0"
WS_PORT   = 8765
WS_PATH   = "/ws"

def _run_ws_server():
    import http
    import json
    import websockets
    from websockets.exceptions import ConnectionClosedOK, ConnectionClosedError

    # 讓握手前就能擋掉錯誤路徑
    async def process_request(connection, request):
        if request.path != WS_PATH:
            body = b"404 Not Found (expected /ws)\n"
            return (http.HTTPStatus.NOT_FOUND,
                    [("Content-Type", "text/plain"),
                     ("Content-Length", str(len(body)))],
                    body)
        return None  # 符合路徑就繼續做 WS 握手

    async def ws_handler(websocket):
        # 這裡 request 物件可取得路徑
        try:
            req = websocket.request
            path = getattr(req, "path", "")
        except Exception:
            path = ""
        print(f"[WS] new connection from {getattr(websocket, 'remote_address', None)}, path={path}")

        if path != WS_PATH:
            # 雙重保險（握手後也再檢一次）
            print(f"[WS] reject connection: invalid path '{path}'")
            await websocket.close(code=1008, reason="Invalid path")
            return

        print(f"[WS] client connected on {path}")

        try:
            async for msg in websocket:
                try:
                    data = json.loads(msg)
                except Exception as e:
                    # 只吃 JSON；不是 JSON 就忽略
                    # print(f"[WS] not JSON, ignored: {e}")
                    continue

                if isinstance(data, dict):
                    _apply_payload(data)   # 你的既有邏輯
                else:
                    # print(f"[WS] unsupported JSON type: {type(data)}")
                    continue
            # 走到這裡表示 client 正常關閉
        except (ConnectionClosedOK, ConnectionClosedError):
            pass
        except Exception as e:
            print(f"[WS] connection error: {e}")

    async def main():
        async with websockets.serve(
            ws_handler,
            WS_HOST,
            WS_PORT,
            process_request=process_request,  # ← 現在簽名正確
            max_size=None,
            max_queue=0,                      # 降延遲（可留著）
            ping_interval=None,               # 減少心跳干擾
        ):
            print(f"[WS] Serving on ws://{WS_HOST}:{WS_PORT}{WS_PATH}")
            await asyncio.Future()  # run forever

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(main())
    loop.run_forever()
# -------------------------------
# main
# -------------------------------
if __name__ == "__main__":
    # 背景啟動 WebSocket 伺服器
    t = threading.Thread(target=_run_ws_server, daemon=True)
    t.start()

    print(f"[HTTP] Serving on http://{HTTP_HOST}:{HTTP_PORT}/")
    app.run(host=HTTP_HOST, port=HTTP_PORT, threaded=False)