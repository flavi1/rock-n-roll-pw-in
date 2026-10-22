#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests hors matériel : validation de pw-in.conf et des fichiers générés.
# Usage : bash tests/test-config.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMD=(python3 "$HERE/files/bin/rock-n-roll-pw-in")
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
FAILS=0
ok()   { echo "  ok   : $*"; }
fail() { echo "  FAIL : $*"; FAILS=$((FAILS+1)); }
export PW_IN_CONF="$W/conf/pw-in.conf" PW_IN_SOUND_CONF="$W/none.conf" PW_IN_ROC_BIN="$W/absent"
mkdir -p "$W/conf"
cp "$HERE/pw-in.conf" "$PW_IN_CONF"

echo "== check / generate (sans pi-sound-server)"
"${CMD[@]}" check >/dev/null && ok "check" || fail "check"
"${CMD[@]}" generate "$W/gen" >/dev/null && ok "generate" || fail "generate"
PWC="$W/gen/.config/pipewire/rock-n-roll-rtp.conf"
UNIT="$W/gen/.config/systemd/user/rock-n-roll-rtp.service"
NFT="$W/gen/nftables.d/30-rock-n-roll-pw-in.nft"
for f in "$PWC" "$UNIT" "$NFT"; do [[ -s "$f" ]] && ok "${f#"$W"/gen/}" || fail "$f manquant"; done
grep -q 'libpipewire-module-rtp-source' "$PWC" && ok "module rtp-source" || fail "rtp-source absent"
grep -q 'source.port = 46000' "$PWC" && ok "port 46000" || fail "port"
grep -q 'audio.rate = 192000' "$PWC" && ok "192 kHz" || fail "fréquence"
grep -q 'audio.format = "S24BE"' "$PWC" && ok "24 bits (L24)" || fail "format"
grep -q 'sess.latency.msec = 10' "$PWC" && ok "tampon 10 ms" || fail "tampon"
grep -q 'sess.ignore-ssrc = true' "$PWC" && ok "nouveau flux du même émetteur accepté" || fail "ignore-ssrc"
grep -q 'target.object = "MASTER"' "$PWC" && ok "vers MASTER" || fail "cible"
grep -q 'log.level = 2' "$PWC" && ok "journal : avertissements" || fail "log.level"
grep -q 'resample.quality = 10' "$PWC" && ok "qualité 10 (pi-sound-server par défaut)" || fail "qualité"
grep -q 'ExecStart=/usr/bin/pipewire -c rock-n-roll-rtp.conf' "$UNIT" && ok "processus PipeWire séparé" || fail "ExecStart"
grep -q 'size 1;' "$NFT" && grep -q 'timeout 5s' "$NFT" && ok "pare-feu : 1 place, libérée après 5 s" || fail "fragment nft"
grep -q 'ip saddr $LAN4 udp dport { 46000 }' "$NFT" && ok "pare-feu : réseau local, RTP seul (ROC indisponible)" || fail "LAN4"
"${CMD[@]}" check | grep -q "ROC.*INDISPONIBLE" && ok "ROC sans programme : signalé indisponible" || fail "ROC indisponible non signalé"

echo "== ROC (programme présent)"
printf '#!/bin/sh\nexit 0\n' > "$W/roc"; chmod +x "$W/roc"
PW_IN_ROC_BIN="$W/roc" "${CMD[@]}" generate "$W/genr" >/dev/null
NR="$W/genr/nftables.d/30-rock-n-roll-pw-in.nft"
grep -q 'udp dport { 10001, 10002, 10003, 46000 } update @rnr_sender' "$NR" \
    && ok "pare-feu : RTP + ROC, une seule place pour tous" || fail "ports ROC : $(cat "$NR")"
RU="$W/genr/.config/systemd/user/rock-n-roll-roc.service"
grep -q 'ExecStart=/usr/local/bin/rock-n-roll-pw-in run-roc' "$RU" && grep -q 'StartLimitBurst=5' "$RU" \
    && ok "service ROC séparé, redémarrages limités" || fail "service ROC"
python3 - "$W/genr/roc-argv.json" <<'PY' && ok "commande ROC : 192 kHz, auto, vers MASTER, sans attendre d'émetteur" || fail "argv ROC"
import json, sys
a = json.load(open(sys.argv[1]))
assert a[1] == "recv" and a[a.index("--fec") + 1] == "auto" and a[a.index("--rate") + 1] == "192000"
i = a.index("--")
pw = a[i + 1:]
assert pw[:3] == ["pw-cat", "--playback", "--raw"] and pw[pw.index("--target") + 1] == "MASTER"
assert pw[pw.index("--format") + 1] == "f32" and pw[pw.index("--quality") + 1] == "10"
p = json.loads(pw[pw.index("-P") + 1])
assert p["node.name"] == "rock-n-roll.roc" and p["node.always-process"] is True
PY
printf '[roc]\nfec = none\n' > "$W/conf/local.conf"
PW_IN_ROC_BIN="$W/roc" "${CMD[@]}" generate "$W/genr2" >/dev/null
grep -q 'udp dport { 10003, 46000 }' "$W/genr2/nftables.d/30-rock-n-roll-pw-in.nft" && ok "fec = none : seul le port simple" || fail "fec none"
printf '[roc]\nenabled = no\n' > "$W/conf/local.conf"
PW_IN_ROC_BIN="$W/roc" "${CMD[@]}" generate "$W/genr3" >/dev/null
grep -q 'udp dport { 46000 }' "$W/genr3/nftables.d/30-rock-n-roll-pw-in.nft" && ok "ROC désactivé : ports fermés" || fail "ROC désactivé"
rm -f "$W/conf/local.conf"

echo "== valeurs reprises de pi-sound-server"
cat > "$W/sound.conf" <<'EOF'
[global]
user = musique
master.name = SORTIE
resample.quality = 12
EOF
PW_IN_SOUND_CONF="$W/sound.conf" "${CMD[@]}" generate "$W/gen2" >/dev/null
G2="$W/gen2/.config/pipewire/rock-n-roll-rtp.conf"
grep -q 'target.object = "SORTIE"' "$G2" && ok "cible = master.name" || fail "master.name non repris"
grep -q 'resample.quality = 12' "$G2" && ok "qualité reprise" || fail "qualité non reprise"
PW_IN_SOUND_CONF="$W/sound.conf" "${CMD[@]}" check | grep -q 'utilisateur : musique' && ok "utilisateur repris" || fail "utilisateur"

echo "== local.conf"
printf '[rtp]\nlatency-ms = 30\nrelease-seconds = 9\n' > "$W/conf/local.conf"
"${CMD[@]}" generate "$W/gen3" >/dev/null
grep -q 'sess.latency.msec = 30' "$W/gen3/.config/pipewire/rock-n-roll-rtp.conf" && ok "local.conf prioritaire" || fail "local.conf ignoré"
grep -q 'timeout 9s' "$W/gen3/nftables.d/30-rock-n-roll-pw-in.nft" && ok "release-seconds -> délai du pare-feu" || fail "release-seconds"
rm "$W/conf/local.conf"

echo "== erreurs détectées"
bad() {  # bad "description" SECTION LIGNE
    cp "$HERE/pw-in.conf" "$W/conf/pw-in.conf"
    printf '[%s]\n%s\n' "$2" "$3" > "$W/conf/local.conf"
    if "${CMD[@]}" check >/dev/null 2>&1; then fail "$1 non détecté"; else ok "$1 refusé"; fi
    rm -f "$W/conf/local.conf"
}
bad "fréquence invalide" rtp "rate = 50000"
bad "format invalide" rtp "format = F32"
bad "port invalide" rtp "port = 80"
bad "option inconnue" rtp "portt = 46000"
bad "section inconnue" foo "enabled = yes"
bad "ROC : ports en double" roc "plain-port = 46000"
bad "ROC : fec invalide" roc "fec = xor"
cp "$HERE/pw-in.conf" "$W/conf/pw-in.conf"

echo "== état du flux (pw-dump)"
python3 - "$HERE/files/bin/rock-n-roll-pw-in" <<'PY' && ok "rtp.receiving : booléen JSON ou texte" || fail "is_true"
import importlib.machinery, importlib.util, sys
loader = importlib.machinery.SourceFileLoader("x", sys.argv[1])
spec = importlib.util.spec_from_loader("x", loader); m = importlib.util.module_from_spec(spec); loader.exec_module(m)
assert m.is_true(True) and m.is_true("true")
assert not m.is_true(False) and not m.is_true("false") and not m.is_true(None)
PY

if command -v spa-json-dump >/dev/null; then
    spa-json-dump "$PWC" >/dev/null && ok "SPA-JSON valide" || fail "SPA-JSON invalide"
else
    echo "  (spa-json-dump absent : syntaxe SPA-JSON non vérifiée)"
fi

echo
echo "$FAILS échec(s)"
[[ $FAILS -eq 0 ]]
