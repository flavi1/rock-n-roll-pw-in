#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Test réel de rock-n-roll-roc (libroc requise, sans PipeWire) : un sinus envoyé
# en boucle locale, reçu, analysé (trous, sauts, fréquence).
#   bash tests/test-roc.sh build
#       compile dans $BIN (défaut ./rock-n-roll-roc)
#   bash tests/test-roc.sh loop [FEC] [ATTENDU]
#       envoi + réception avec $BIN
#   bash tests/test-roc.sh pair "PRÉFIXE-RÉCEPTION" "PRÉFIXE-ENVOI" [FEC] [ATTENDU]
#       réception et envoi lancés chacun avec un préfixe (ex. « docker exec c env
#       BIN=/w/x ») : essais entre versions de libroc. À lancer depuis la racine du
#       dépôt (montée en /w dans les conteneurs, même dossier courant).
#   ATTENDU : clean (défaut, aucun trou) ou glitches (pertes non corrigées visibles)
# Pertes simulées : netem sur « lo », posé à part (tc qdisc add dev lo root netem loss 5%).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$PWD/rock-n-roll-roc}"
SECS="${SECS:-9}"
ROC_CASE="${ROC_CASE:-}"
cmd="${1:?build|loop|pair|recv|send}"

build() {
    local flags
    flags="$(pkg-config --cflags --libs roc 2>/dev/null || echo -lroc)"
    # shellcheck disable=SC2086
    gcc -std=c99 -O2 -Wall -Wextra -Werror -o "$BIN" "$HERE/files/src/rock-n-roll-roc.c" $flags
    echo "compilé : $BIN (libroc $(pkg-config --modversion roc 2>/dev/null || echo '?'))"
}

# chemins relatifs : valables sur l'hôte et dans les conteneurs (/w = dépôt)
recv() {   # recv FICHIER-BILAN [FEC]
    "$BIN" recv --fec "${2:-auto}" --latency-ms 100 -- python3 tests/roc-sine.py check "$SECS" "$1"
}

send() {   # send IP [FEC]
    "$BIN" send --host "$1" --fec "${2:-auto}" -- python3 tests/roc-sine.py gen $((SECS + 3)) || true
}

# journaux de l'essai en annotations GitHub (les journaux de CI ne sont pas toujours lisibles)
annotate() {   # annotate NIVEAU TITRE FICHIER
    [[ -n "${GITHUB_ACTIONS:-}" && -s "$3" ]] || return 0
    local msg
    msg="$(tail -n 40 "$3" | sed ':a;N;$!ba;s/%/%25/g;s/\r/%0D/g;s/\n/%0A/g')"
    echo "::$1 title=$2::$msg"
}

verdict() {   # verdict FICHIER ATTENDU  -> 0 si conforme
    python3 - "$@" <<'PY'
import json, os, sys
try:
    r = json.load(open(sys.argv[1]))
except (OSError, ValueError):
    r = None
want = sys.argv[2]
clean = bool(r) and r["analysed_s"] > 5 and r["jumps"] == 0 and r["silent_blocks"] == 0 \
    and abs(r["freq"] - 1000) < 20
ok = clean if want == "clean" else (bool(r) and r["analysed_s"] > 5 and not clean)
case = os.environ.get("ROC_CASE", "")
print(("OK   " if ok else "ÉCHEC"), want, r)
if os.environ.get("GITHUB_ACTIONS"):
    print("::%s title=ROC %s (%s)::%s" % ("notice" if ok else "error", case, want, r))
sys.exit(0 if ok else 1)
PY
}

pair() {   # pair PRÉFIXE-RÉCEPTION PRÉFIXE-ENVOI FEC ATTENDU
    local rpre="$1" spre="$2" fec="${3:-auto}" want="${4:-clean}"
    local res=roc-res.json rlog slog rp sp
    rlog="$(mktemp)" slog="$(mktemp)"
    rm -f "$res"
    # shellcheck disable=SC2086
    $rpre bash tests/test-roc.sh recv "$res" "$fec" 2>"$rlog" & rp=$!
    sleep 1.5
    # shellcheck disable=SC2086
    $spre bash tests/test-roc.sh send 127.0.0.1 "$fec" 2>"$slog" & sp=$!
    wait "$rp" || true
    kill "$sp" 2>/dev/null || true; wait "$sp" 2>/dev/null || true
    echo "== $ROC_CASE (FEC $fec, attendu : $want)"
    cat "$rlog" "$slog" >&2
    if verdict "$res" "$want"; then
        annotate notice "journal réception ($ROC_CASE)" "$rlog"
        annotate notice "journal envoi ($ROC_CASE)" "$slog"
        return 0
    fi
    annotate warning "journal réception ($ROC_CASE)" "$rlog"
    annotate warning "journal envoi ($ROC_CASE)" "$slog"
    return 1
}

case "$cmd" in
    build) build ;;
    recv) recv "${2:?fichier}" "${3:-auto}" ;;
    send) send "${2:?ip}" "${3:-auto}" ;;
    loop) pair "env BIN=$BIN" "env BIN=$BIN" "${2:-auto}" "${3:-clean}" ;;
    pair) pair "${2:?préfixe réception}" "${3:?préfixe envoi}" "${4:-auto}" "${5:-clean}" ;;
    *) echo "commande inconnue : $cmd"; exit 2 ;;
esac
