#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# CGI: send AWG table via libgpiod v2 (no /dev/mem)
#
# Form fields (urlencoded):
#   data_chip=/dev/gpiochip0   (default)
#   wen_chip=/dev/gpiochip3    (default)
#   wen_offset=0               (default)
#   wen_active=1               (1: Active-High, 0: Active-Low; default 1)
#   wen_pulse_us=1             (pulse width in us; default 1)
#
#   idxA=0,1,2,3,4,5,6,7       (8 values, dec or 0x..)
#   idxB=8,9,10,11,12,13,14,15
#
#   兩種 GAIN 輸入二選一 (長度 8)：
#   gainA=0x00000,0x1FFFF,...  (Q1.17 20-bit，十六或十進位)
#   gainB=...
#   或
#   gainA_f=0.5,0,0,...        (浮點 0..1；會自動轉 Q1.17)
#   gainB_f=...
#
# 回傳 JSON：{ok: true/false, ...}

import os, sys, json, time
from urllib.parse import parse_qs

import gpiod
from gpiod.line import Direction, Value
from gpiod import LineSettings

# ---------- 預設 GPIO 來源（依你的 platform 調整） ----------
DEF_DATA_CHIP = "/dev/gpiochip0"   # 32-bit AXI GPIO (DATA)
DEF_WEN_CHIP  = "/dev/gpiochip3"   # 1-bit  AXI GPIO (WEN)
DEF_WEN_OFF   = 0
DEF_WEN_ACTHI = 1
DEF_WEN_US    = 1                  # 1 us

# ---------- AWG 命令打包（和你原本一致） ----------
def pack_sel(ch, tone):
    # [27]=CH, [26:24]=tone, [23:20]=0
    return ((ch & 1) << 27) | ((tone & 7) << 24)

def make_index_word(ch, tone, idx20):
    return (0x1 << 28) | pack_sel(ch, tone) | (idx20 & 0xFFFFF)

def make_gain_word(ch, tone, gain20):
    return (0x2 << 28) | pack_sel(ch, tone) | (gain20 & 0xFFFFF)

def make_commit_word():
    return (0xF << 28)

# ---------- 解析工具 ----------
def _read_form():
    method = os.environ.get("REQUEST_METHOD","GET").upper()
    if method == "POST":
        try:
            length = int(os.environ.get("CONTENT_LENGTH") or "0")
        except ValueError:
            length = 0
        data = sys.stdin.read(length) if length>0 else ""
    else:
        data = os.environ.get("QUERY_STRING","")
    q = parse_qs(data, keep_blank_values=True)
    return {k: (v[0] if v else "") for k,v in q.items()}

def _to_int_list(s, n=8, default=0):
    """支援 '1,2,0x10' 混合，回傳長度 n 的 list。"""
    out = []
    if s:
        for tok in s.split(','):
            tok = tok.strip()
            if not tok:
                continue
            base = 16 if tok.lower().startswith("0x") else 10
            out.append(int(tok, base))
    while len(out) < n:
        out.append(default)
    return out[:n]

def _to_float_list(s, n=8, default=0.0):
    out = []
    if s:
        for tok in s.split(','):
            tok = tok.strip()
            if not tok:
                continue
            out.append(float(tok))
    while len(out) < n:
        out.append(default)
    return out[:n]

def _gain_f_to_q17(f):
    # 0..1 -> Q1.17 (20-bit, 0x00000..0x1FFFF)
    if f < 0.0: f = 0.0
    if f > 1.0: f = 1.0
    return int(f * 0x1FFFF + 0.5)

# ---------- GPIO：建立 requests ----------
def open_data_request(chip_path):
    # 32 bit lines: offset 0..31 都當輸出
    cfg = LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)
    config = {i: cfg for i in range(32)}
    return gpiod.request_lines(chip_path, consumer="awg_data", config=config)

def open_wen_request(chip_path, off, active_high=True):
    init = Value.INACTIVE
    cfg = LineSettings(direction=Direction.OUTPUT, output_value=init)
    return gpiod.request_lines(chip_path, consumer="awg_wen", config={off: cfg})

def wen_pulse(wen_req, off, active_high=True, pulse_us=100):
    on  = Value.ACTIVE if active_high else Value.INACTIVE
    offv= Value.INACTIVE if active_high else Value.ACTIVE
    wen_req.set_values({off: on})
    # 注意 Python sleep 精度有限，對 WEN 通常數十 us~ms 都可接受
    time.sleep(pulse_us / 1_000_000.0)
    wen_req.set_values({off: offv})

def write_word(data_req, word):
    # 將 32-bit word 拆成 {offset: Value}
    m = {}
    for i in range(32):
        bit = (word >> i) & 1
        m[i] = Value.ACTIVE if bit else Value.INACTIVE
    data_req.set_values(m)

# ---------- 主流程：送表 ----------
def send_awg_table(data_chip, wen_chip, wen_off, wen_acthi, wen_us,
                   idxA, gainA, idxB, gainB):
    data_req = open_data_request(data_chip)
    wen_req  = open_wen_request(wen_chip, wen_off, active_high=wen_acthi)

    words = []

    # A: INDEX 0..7
    for t in range(8):
        w = make_index_word(0, t, idxA[t])
        words.append(w)
        write_word(data_req, w)
        wen_pulse(wen_req, wen_off, wen_acthi, wen_us)

    # A: GAIN 0..7
    for t in range(8):
        w = make_gain_word(0, t, gainA[t])
        words.append(w)
        write_word(data_req, w)
        wen_pulse(wen_req, wen_off, wen_acthi, wen_us)

    # B: INDEX 0..7
    for t in range(8):
        w = make_index_word(1, t, idxB[t])
        words.append(w)
        write_word(data_req, w)
        wen_pulse(wen_req, wen_off, wen_acthi, wen_us)

    # B: GAIN 0..7
    for t in range(8):
        w = make_gain_word(1, t, gainB[t])
        words.append(w)
        write_word(data_req, w)
        wen_pulse(wen_req, wen_off, wen_acthi, wen_us)

    # COMMIT
    w = make_commit_word()
    words.append(w)
    write_word(data_req, w)
    wen_pulse(wen_req, wen_off, wen_acthi, wen_us)

    # 保持輸出狀態即可；若要釋放：
    data_req.release()
    wen_req.release()
    return words

# ---------- CGI main ----------
def main():
    print("Content-Type: application/json; charset=utf-8")
    print()

    form = _read_form()
    try:
        data_chip = form.get("data_chip", DEF_DATA_CHIP) or DEF_DATA_CHIP
        wen_chip  = form.get("wen_chip",  DEF_WEN_CHIP)  or DEF_WEN_CHIP
        wen_off   = int(form.get("wen_offset", DEF_WEN_OFF) or DEF_WEN_OFF)
        wen_acthi = 1 if str(form.get("wen_active", DEF_WEN_ACTHI)).strip() not in ("0","false","False") else 0
        wen_us    = int(form.get("wen_pulse_us", DEF_WEN_US) or DEF_WEN_US)

        # index（必填，或預設）
        idxA = _to_int_list(form.get("idxA"), 8, 0)
        idxB = _to_int_list(form.get("idxB"), 8, 8)

        # gain 可傳 Q1.17（gainA/gainB）或 float（gainA_f/gainB_f）
        if "gainA_f" in form or "gainB_f" in form:
            gainA_f = _to_float_list(form.get("gainA_f"), 8, 0.0)
            gainB_f = _to_float_list(form.get("gainB_f"), 8, 0.0)
            gainA = [_gain_f_to_q17(x) for x in gainA_f]
            gainB = [_gain_f_to_q17(x) for x in gainB_f]
        else:
            gainA = _to_int_list(form.get("gainA"), 8, 0)
            gainB = _to_int_list(form.get("gainB"), 8, 0)

        words = send_awg_table(data_chip, wen_chip, wen_off, bool(wen_acthi), wen_us,
                               idxA, gainA, idxB, gainB)

        resp = {
            "ok": True,
            "data_chip": data_chip,
            "wen_chip":  wen_chip,
            "wen_offset": wen_off,
            "wen_active_high": bool(wen_acthi),
            "wen_pulse_us": wen_us,
            "idxA": idxA, "gainA": gainA,
            "idxB": idxB, "gainB": gainB,
            "words_sent": len(words)
        }
        sys.stdout.write(json.dumps(resp, ensure_ascii=False))
    except Exception as e:
        sys.stdout.write(json.dumps({
            "ok": False,
            "error": str(e),
            "received": form
        }, ensure_ascii=False))

if __name__ == "__main__":
    main()

