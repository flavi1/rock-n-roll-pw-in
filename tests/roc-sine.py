#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Banc d'essai de rock-n-roll-roc, sans PipeWire (rôle de pw-cat) :
#   roc-sine.py gen SECONDES           sinus 1 kHz, f32 stéréo, au rythme réel, sur stdout
#   roc-sine.py check SECONDES FICHIER lit stdin au rythme réel, puis écrit un bilan JSON
import json
import math
import struct
import sys
import time

RATE = 192000
FREQ = 1000.0
AMP = 0.5
CHUNK = 960                    # 5 ms
FRAME = 8                      # 2 × float32


def gen(seconds):
    out = sys.stdout.buffer
    n = 0
    t0 = time.monotonic()
    w = 2 * math.pi * FREQ / RATE
    total = int(seconds * RATE)
    while n < total:
        vals = []
        for i in range(n, n + CHUNK):
            v = AMP * math.sin(w * i)
            vals.append(v)
            vals.append(v)
        try:
            out.write(struct.pack("<%df" % len(vals), *vals))
            out.flush()
        except BrokenPipeError:
            return
        n += CHUNK
        delay = t0 + n / RATE - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def check(seconds, path):
    inp = sys.stdin.buffer
    samples = []
    t0 = time.monotonic()
    n = 0
    total = int(seconds * RATE)
    while n < total:
        want = CHUNK * FRAME
        data = b""
        while len(data) < want:
            d = inp.read(want - len(data))
            if not d:
                break
            data += d
        if len(data) < want:
            break
        vals = struct.unpack("<%df" % (CHUNK * 2), data)
        samples.extend(vals[0::2])
        n += CHUNK
        delay = t0 + n / RATE - time.monotonic()
        if delay > 0:
            time.sleep(delay)
    # bilan sur la fin (après mise en route) : blocs de 10 ms
    skip = int(2.5 * RATE)
    win = samples[skip:]
    blk = RATE // 100
    blocks = [win[i:i + blk] for i in range(0, len(win) - blk + 1, blk)]
    silent = sum(1 for b in blocks if math.sqrt(sum(x * x for x in b) / len(b)) < 0.1)
    max_step = 2 * math.pi * FREQ / RATE * AMP
    jumps = sum(1 for a, b in zip(win, win[1:]) if abs(b - a) > 3 * max_step)
    rms = math.sqrt(sum(x * x for x in win) / len(win)) if win else 0.0
    crossings = sum(1 for a, b in zip(win, win[1:]) if a < 0 <= b)
    freq = crossings / (len(win) / RATE) if win else 0.0
    res = {"frames": len(samples), "analysed_s": round(len(win) / RATE, 2),
           "silent_blocks": silent, "blocks": len(blocks), "jumps": jumps,
           "rms": round(rms, 4), "freq": round(freq, 1)}
    with open(path, "w") as f:
        json.dump(res, f)
    print(json.dumps(res), file=sys.stderr)


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(float(sys.argv[2]))
    else:
        check(float(sys.argv[2]), sys.argv[3])
