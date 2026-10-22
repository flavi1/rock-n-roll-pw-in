#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Test réel de rock-n-roll-roc (libroc requise, sans PipeWire) : un sinus envoyé
# en boucle locale, reçu, analysé (trous, sauts, fréquence).
#   bash tests/test-roc.sh build                 compile dans $BIN (défaut ./rock-n-roll-roc)
#   bash tests/test-roc.sh loop [FEC] [PERTE%]   envoi + réception sur cette machine
#   bash tests/test-roc.sh send IP | recv RES    un seul côté (essais entre versions)
# Perte simulée : netem sur « lo » (root requis).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$PWD/rock-n-roll-roc}"
SINE="$HERE/tests/roc-sine.py"
SECS="${SECS:-9}"
cmd="${1:?build|loop|send|recv}"

build() {
    local flags
    flags="$(pkg-config --cflags --libs roc 2>/dev/null || echo -lroc)"
    # shellcheck disable=SC2086
    gcc -std=c99 -O2 -Wall -Wextra -Werror -o "$BIN" "$HERE/files/src/rock-n-roll-roc.c" $flags
    echo "compilé : $BIN ($(pkg-config --modversion roc 2>/dev/null || echo 'version ?'))"
}

recv() {   # recv FICHIER-BILAN [FEC]
    "$BIN" recv --fec "${2:-rs8m}" --latency-ms 100 -- python3 "$SINE" check "$SECS" "$1"
}

send() {   # send IP [FEC]
    "$BIN" send --host "$1" --fec "${2:-rs8m}" -- python3 "$SINE" gen $((SECS + 3)) || true
}

verdict() {   # verdict FICHIER MAX-SAUTS MAX-BLOCS-SILENCIEUX
    python3 - "$@" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); mj, ms = int(sys.argv[2]), int(sys.argv[3])
ok = r["analysed_s"] > 5 and r["jumps"] <= mj and r["silent_blocks"] <= ms and abs(r["freq"] - 1000) < 20
print(("OK   " if ok else "ÉCHEC"), r)
import os
if os.environ.get("GITHUB_ACTIONS"):
    print("::%s title=ROC %s::%s" % ("notice" if ok else "error", os.environ.get("ROC_CASE", ""), r))
sys.exit(0 if ok else 1)
PY
}

case "$cmd" in
    build) build ;;
    recv) recv "${2:?fichier}" "${3:-rs8m}" ;;
    send) send "${2:?ip}" "${3:-rs8m}" ;;
    loop)
        fec="${2:-rs8m}" loss="${3:-0}"
        res="$(mktemp)"
        if [[ "$loss" != 0 ]]; then
            tc qdisc add dev lo root netem loss "${loss}%"
            trap 'tc qdisc del dev lo root 2>/dev/null || true' EXIT
        fi
        recv "$res" "$fec" & rp=$!
        sleep 1
        send 127.0.0.1 "$fec" & sp=$!
        wait "$rp" || true
        kill "$sp" 2>/dev/null || true; wait "$sp" 2>/dev/null || true
        echo "== FEC $fec, perte ${loss} %"
        if [[ "$fec" == none && "$loss" != 0 ]]; then
            # sans correction, les pertes DOIVENT s'entendre : le banc les voit-il ?
            if GITHUB_ACTIONS= verdict "$res" 0 0 >/dev/null; then echo "ÉCHEC : pertes non détectées par le banc"; exit 1; fi
            echo "OK    pertes bien visibles sans correction"
        else
            verdict "$res" 0 0
        fi
        ;;
    *) echo "commande inconnue : $cmd"; exit 2 ;;
esac
