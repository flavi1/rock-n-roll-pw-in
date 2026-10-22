#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Banc d'essai de rock-n-roll-roc, sans PipeWire (rôle de pw-cat) :
#   roc-sine.py gen SECONDES           sinus 1 kHz, f32 stéréo, au rythme réel, sur stdout
#   roc-sine.py check SECONDES FICHIER lit stdin au rythme réel, puis écrit un bilan JSON
# ROC_DRIFT_PPM (gen) : horloge de l'émetteur décalée (ex. 100 = 100 ppm plus rapide),
# pour simuler l'écart entre l'horloge du PC et celle de la Pi.
import json
import math
import os
import struct
import sys
import time

RATE = int(os.environ.get("ROC_RATE", "192000"))
FREQ = 1000.0
AMP = 0.5
CHUNK = 960                    # 5 ms
FRAME = 8                      # 2 × float32


def gen(seconds):
    out = sys.stdout.buffer
    ppm = float(os.environ.get("ROC_DRIFT_PPM", "0"))
    pace = RATE * (1 + ppm / 1e6)          # échantillons par seconde réelle
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
        delay = t0 + n / pace - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def analyse(win, start_s):
    blk = RATE // 100                                   # blocs de 10 ms
    nb = len(win) // blk
    w = 2 * math.pi * FREQ / RATE
    max_step = w * AMP
    try:
        import numpy as np
        x = np.asarray(win[:nb * blk], dtype=np.float64)
        b = x.reshape(nb, blk)
        rms_b = np.sqrt((b * b).mean(axis=1))
        jump_idx = np.nonzero(np.abs(np.diff(x)) > 3 * max_step)[0]
        t = np.arange(blk)
        cs = (b * np.cos(w * t)).mean(axis=1) * 2
        sn = (b * np.sin(w * t)).mean(axis=1) * 2
        a1_b = np.hypot(cs, sn)
        res2 = np.maximum(0, (b * b).mean(axis=1) - a1_b ** 2 / 2)
        crossings = int(np.count_nonzero((x[:-1] < 0) & (x[1:] >= 0)))
        silent_idx = np.nonzero(rms_b < 0.1)[0]
        a1, resid, peak = float(a1_b.mean()), float(np.sqrt(res2.mean())), float(np.abs(x).max())
        rms = float(np.sqrt((x * x).mean()))
        jumps = [int(i) for i in jump_idx]
        silent = [int(i) for i in silent_idx]
    except ImportError:
        blocks = [win[i * blk:(i + 1) * blk] for i in range(nb)]
        silent = [i for i, bb in enumerate(blocks) if math.sqrt(sum(v * v for v in bb) / blk) < 0.1]
        jumps = [i for i, (a, c) in enumerate(zip(win, win[1:])) if abs(c - a) > 3 * max_step]
        a1s, r2 = [], []
        for bb in blocks:
            cs = sum(v * math.cos(w * i) for i, v in enumerate(bb)) * 2 / blk
            sn = sum(v * math.sin(w * i) for i, v in enumerate(bb)) * 2 / blk
            a = math.hypot(cs, sn)
            a1s.append(a)
            r2.append(max(0.0, sum(v * v for v in bb) / blk - a * a / 2))
        a1 = sum(a1s) / len(a1s) if a1s else 0.0
        resid = math.sqrt(sum(r2) / len(r2)) if r2 else 0.0
        peak = max((abs(v) for v in win), default=0.0)
        rms = math.sqrt(sum(v * v for v in win) / len(win)) if win else 0.0
        crossings = sum(1 for a, c in zip(win, win[1:]) if a < 0 <= c)
    # moments des incidents (s depuis le début de la réception)
    events = sorted(set([round(start_s + j / RATE, 1) for j in jumps] +
                        [round(start_s + i * 0.01, 1) for i in silent]))
    return {"analysed_s": round(len(win) / RATE, 2), "a1": round(a1, 4),
            "resid_db": round(20 * math.log10(max(resid, 1e-9) / (AMP / math.sqrt(2))), 1),
            "peak": round(peak, 4), "silent_blocks": len(silent), "blocks": nb,
            "jumps": len(jumps), "rms": round(rms, 4),
            "freq": round(crossings / (len(win) / RATE), 1) if win else 0.0,
            "events_s": events[:12], "n_events": len(events)}


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
    skip = int(4 * RATE)                                  # après mise en route
    res = {"frames": len(samples)}
    res.update(analyse(samples[skip:], 4.0))
    with open(path, "w") as f:
        json.dump(res, f)
    print(json.dumps(res), file=sys.stderr)


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen(float(sys.argv[2]))
    else:
        check(float(sys.argv[2]), sys.argv[3])
