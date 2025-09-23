#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# CGI: control two RGB LEDs via libgpiod v2
# LED0 -> gpio offsets 0,1,2 ; LED1 -> 3,4,5 (on the same chip)
#
# Form fields (application/x-www-form-urlencoded):
#   chip=/dev/gpiochip2            (optional, default /dev/gpiochip2)
#   r0,g0,b0   in {"0","1"}        (LED0)
#   r1,g1,b1   in {"0","1"}        (LED1)
# Backward compatibility:
#   r,g,b      -> apply to LED0 (r0,g0,b0)
#
# Return: JSON object with what was applied or error message.

import os, sys, json
from urllib.parse import parse_qs

import gpiod
from gpiod.line import Direction, Value

# ---- configuration: change if你走線不同 ----
LED0_OFFSETS = (2, 1, 0)          # R0,G0,B0
LED1_OFFSETS = (5, 4, 3)          # R1,G1,B1
DEFAULT_CHIP = "/dev/gpiochip2"

def _read_form():
    """Read urlencoded body for POST or query for GET, return dict[str, str]."""
    method = os.environ.get("REQUEST_METHOD", "GET").upper()
    if method == "POST":
        try:
            length = int(os.environ.get("CONTENT_LENGTH") or "0")
        except ValueError:
            length = 0
        data = sys.stdin.read(length) if length > 0 else ""
    else:
        data = os.environ.get("QUERY_STRING", "")
    q = parse_qs(data, keep_blank_values=True)
    # flatten: take first value
    return {k: (v[0] if v else "") for k, v in q.items()}

def _to_bit(s, default=0):
    """Normalize various truthy strings to 0/1."""
    if s is None:
        return default
    s = s.strip().lower()
    return 1 if s in ("1", "true", "on", "yes") else 0

def _build_targets(form):
    """
    Decide which offsets to drive and to what values.
    Returns (chip_path, mapping{offset:int0/1}, detail_dict).
    """
    chip = form.get("chip", DEFAULT_CHIP) or DEFAULT_CHIP

    # Accept new fields r0,g0,b0,r1,g1,b1
    r0 = form.get("r0"); g0 = form.get("g0"); b0 = form.get("b0")
    r1 = form.get("r1"); g1 = form.get("g1"); b1 = form.get("b1")

    # Backward compat: r,g,b -> LED0
    if r0 is None and g0 is None and b0 is None:
        if any(k in form for k in ("r","g","b")):
            r0 = form.get("r"); g0 = form.get("g"); b0 = form.get("b")

    targets = {}
    detail  = {"chip": chip, "apply": []}

    def add_led(offs, r, g, b, tag):
        # skip if all None (means not provided)
        if r is None and g is None and b is None:
            return
        vals = (_to_bit(r, 0), _to_bit(g, 0), _to_bit(b, 0))
        mapping = {offs[0]: vals[0], offs[1]: vals[1], offs[2]: vals[2]}
        targets.update(mapping)
        detail["apply"].append({"led": tag,
                                "offsets": list(offs),
                                "rgb": {"r": vals[0], "g": vals[1], "b": vals[2]}})

    add_led(LED0_OFFSETS, r0, g0, b0, "LED0")
    add_led(LED1_OFFSETS, r1, g1, b1, "LED1")

    # If user gave nothing at all, default to turn both off (safety)
    if not targets:
        mapping = {o:0 for o in (*LED0_OFFSETS, *LED1_OFFSETS)}
        targets.update(mapping)
        detail["apply"].append({"led":"LED0+LED1:default_off",
                                "offsets": list(mapping.keys()),
                                "rgb":"all 0"})

    return chip, targets, detail

def _apply(chip_path, targets):
    """
    Use libgpiod v2 to request lines as OUTPUT and set values in one shot.
    'targets' is {offset:int0/1}
    """
    # Build line settings (all as OUTPUT, default INACTIVE)
    cfg = gpiod.LineSettings(direction=Direction.OUTPUT,
                             output_value=Value.INACTIVE)

    config = {off: cfg for off in targets.keys()}

    req = gpiod.request_lines(
        chip_path,
        consumer="web_led_toggle",
        config=config
    )

    # Translate 0/1 to Value enum
    valmap = {off: (Value.ACTIVE if bit else Value.INACTIVE)
              for off, bit in targets.items()}

    # One call pushes all pins
    req.set_values(valmap)

    # optional: keep them as-is (do not release). If you prefer to free:
    req.release()

def main():
    # --- CGI header ---
    print("Content-Type: application/json; charset=utf-8")
    print()

    form = _read_form()
    try:
        chip, targets, detail = _build_targets(form)
        _apply(chip, targets)

        resp = {
            "ok": True,
            "chip": chip,
            "applied": detail["apply"],
            "whoami": os.getuid(),
            "group_info": os.getgroups(),
        }
        sys.stdout.write(json.dumps(resp, ensure_ascii=False))
    except Exception as e:
        # show reason (e.g., permission denied)
        err = {
            "ok": False,
            "error": str(e),
            "chip": form.get("chip", DEFAULT_CHIP),
            "received": form,
        }
        sys.stdout.write(json.dumps(err, ensure_ascii=False))

if __name__ == "__main__":
    main()

