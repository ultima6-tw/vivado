#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import time
import gpiod
from gpiod.line import Direction, Value
from gpiod import LineSettings

# 預設（可按需改）
DEF_DATA_CHIP = "/dev/gpiochip0"
DEF_WEN_CHIP  = "/dev/gpiochip3"
DEF_WEN_OFF   = 0
DEF_WEN_ACTHI = True
DEF_WEN_US    = 1  # 建議 0 或 1；若只看邊緣可 0

# 全域 GPIO handle（程序存活期間重用）
_cfg_out = LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)
_data_req = gpiod.request_lines(
    DEF_DATA_CHIP, consumer="awg_data", config={i: _cfg_out for i in range(32)}
)
_wen_req = gpiod.request_lines(
    DEF_WEN_CHIP, consumer="awg_wen", config={DEF_WEN_OFF: _cfg_out}
)

def pack_sel(ch, tone): return ((ch & 1) << 27) | ((tone & 7) << 24)
def make_index_word(ch, tone, idx20): return (0x1<<28) | pack_sel(ch,tone) | (idx20 & 0xFFFFF)
def make_gain_word(ch, tone, g20):    return (0x2<<28) | pack_sel(ch,tone) | (g20 & 0xFFFFF)
def make_commit_word():               return (0xF<<28)

def to_int_list(s, n=8, default=0):
    out=[]; 
    if s:
        for tok in s.split(','):
            tok=tok.strip()
            if tok:
                base=16 if tok.lower().startswith('0x') else 10
                out.append(int(tok, base))
    while len(out)<n: out.append(default)
    return out[:n]

def to_float_list(s, n=8, default=0.0):
    out=[]
    if s:
        for tok in s.split(','):
            tok=tok.strip()
            if tok: out.append(float(tok))
    while len(out)<n: out.append(default)
    return out[:n]

def gain_f_to_q17(f):
    if f<0: f=0.0
    if f>1: f=1.0
    return int(f*0x1FFFF + 0.5)

def _wen_edge(active_high=True, pulse_us=DEF_WEN_US):
    on  = Value.ACTIVE if active_high else Value.INACTIVE
    off = Value.INACTIVE if active_high else Value.ACTIVE
    _wen_req.set_values({DEF_WEN_OFF: on})
    if pulse_us>0:
        t0 = time.monotonic_ns(); target = t0 + int(pulse_us)*1_000
        while time.monotonic_ns() < target: pass
    _wen_req.set_values({DEF_WEN_OFF: off})

def _write_word(word:int):
    _data_req.set_values({i: (Value.ACTIVE if ((word>>i)&1) else Value.INACTIVE) for i in range(32)})

def build_words(idxA, gainA, idxB, gainB):
    words=[]
    for t in range(8): words.append(make_index_word(0,t,idxA[t]))
    for t in range(8): words.append(make_gain_word(0,t,gainA[t]))
    for t in range(8): words.append(make_index_word(1,t,idxB[t]))
    for t in range(8): words.append(make_gain_word(1,t,gainB[t]))
    return words

def send_words(words, do_commit=True, wen_active_high=DEF_WEN_ACTHI, pulse_us=DEF_WEN_US):
    for w in words:
        _write_word(w); _wen_edge(wen_active_high, pulse_us)
    if do_commit:
        _write_word(make_commit_word()); _wen_edge(wen_active_high, pulse_us)
    return len(words) + (1 if do_commit else 0)