#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Test réel du pare-feu : UN SEUL émetteur à la fois, réseau local uniquement.
# Dans un espace réseau isolé (unshare -n, root requis) : le pare-feu de pi-server
# (copie ci-dessous) + le fragment généré, puis de vrais paquets UDP envoyés depuis
# plusieurs adresses (127.0.0.2, .3 = « réseau local » ; .4 = hors réseau local).
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CMD = os.path.join(HERE, "..", "files", "bin", "rock-n-roll-pw-in")
PORT = 46000
RELEASE = 2
ROC_PORT = 10001

if os.geteuid() != 0:
    print("root requis")
    sys.exit(1)
if os.environ.get("RNR_IN_NETNS") != "1":
    # relance dans un espace réseau neuf : la machine n'est pas touchée
    env = dict(os.environ, RNR_IN_NETNS="1")
    sys.exit(subprocess.run(["unshare", "-n", sys.executable] + sys.argv, env=env).returncode)

FAILS = 0


def check(cond, msg):
    global FAILS
    print("  %s : %s" % ("ok  " if cond else "FAIL", msg), flush=True)
    if not cond:
        FAILS += 1
        if os.environ.get("GITHUB_ACTIONS"):
            print("::error title=test-firewall::%s" % msg)


def lo_up():
    import fcntl
    import struct
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ifr = struct.pack("16sH14s", b"lo", 0, b"")
    flags = struct.unpack("16sH14s", fcntl.ioctl(s, 0x8913, ifr))[1]          # SIOCGIFFLAGS
    fcntl.ioctl(s, 0x8914, struct.pack("16sH14s", b"lo", flags | 1, b""))     # SIOCSIFFLAGS IFF_UP
    s.close()


# Pare-feu pi-server (files/nftables.conf), sans « iif lo accept » : ici tout passe par lo
PI_SERVER = """flush ruleset
include "{d}/00-lan.nft"
table inet filter {{
    chain input {{
        type filter hook input priority filter; policy drop;
        ct state established,related accept
        ct state invalid drop
        meta l4proto {{ icmp, ipv6-icmp }} accept
        ip  saddr $LAN4 tcp dport 22 accept
    }}
}}
include "{d}/[1-9]*.nft"
"""

with tempfile.TemporaryDirectory() as w:
    d = os.path.join(w, "nftables.d")
    os.makedirs(d)
    open(os.path.join(d, "00-lan.nft"), "w").write(
        "define LAN4 = { 127.0.0.2, 127.0.0.3 }\ndefine LAN6 = { fe80::/10, fc00::/7 }\n")
    conf = os.path.join(w, "pw-in.conf")
    open(conf, "w").write("[rtp]\nport = %d\nrelease-seconds = %d\n" % (PORT, RELEASE))
    # programme ROC simulé : le pare-feu doit aussi ouvrir les ports ROC
    fake = os.path.join(w, "roc")
    open(fake, "w").write("#!/bin/sh\nexit 0\n")
    os.chmod(fake, 0o755)
    env = dict(os.environ, PW_IN_CONF=conf, PW_IN_SOUND_CONF=os.path.join(w, "absent"),
               PW_IN_ROC_BIN=fake)
    subprocess.run([sys.executable, CMD, "generate", w], env=env, check=True, capture_output=True)
    main = os.path.join(w, "nftables.conf")
    open(main, "w").write(PI_SERVER.format(d=d))
    r = subprocess.run(["nft", "-f", main], text=True, capture_output=True)
    check(r.returncode == 0, "pare-feu pi-server + fragment chargés" + ("" if r.returncode == 0 else " : " + r.stderr))
    if r.returncode != 0:
        sys.exit(1)
    lo_up()

    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", PORT))
    rx.settimeout(0.3)
    rxroc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rxroc.bind(("127.0.0.1", ROC_PORT))
    rxroc.settimeout(0.3)
    senders = {}
    for ip in ("127.0.0.2", "127.0.0.3", "127.0.0.4"):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((ip, 0))
        senders[ip] = s

    def send(ip, port=PORT):
        """Envoie un paquet depuis ip ; renvoie l'adresse reçue (ou None)."""
        senders[ip].sendto(b"rtp", ("127.0.0.1", port))
        try:
            return (rx if port == PORT else rxroc).recvfrom(64)[1][0]
        except socket.timeout:
            return None

    check(send("127.0.0.2") == "127.0.0.2", "1er émetteur accepté")
    check(send("127.0.0.3") is None, "2e émetteur refusé pendant que le 1er envoie")
    # le 1er continue d'envoyer (même du silence) : il garde la place au-delà du délai
    t0 = time.time()
    held = True
    while time.time() - t0 < RELEASE + 1.5:
        held &= send("127.0.0.2") == "127.0.0.2"
        held &= send("127.0.0.3") is None
        time.sleep(0.2)
    check(held, "le 1er garde la place tant qu'il envoie (%.1f s > délai de %d s)" % (time.time() - t0, RELEASE))
    check(send("127.0.0.4") is None, "émetteur hors réseau local refusé")
    time.sleep(RELEASE + 0.5)
    check(send("127.0.0.3") == "127.0.0.3", "place libérée %d s après le dernier paquet : 2e émetteur accepté" % RELEASE)
    check(send("127.0.0.2") is None, "l'ancien émetteur est refusé à son tour")
    out = subprocess.run(["nft", "list", "set", "inet", "filter", "rnr_sender"],
                         text=True, capture_output=True).stdout
    check("127.0.0.3" in out, "émetteur en place visible (« rock-n-roll-pw-in status »)")
    # RTP et ROC partagent la place : un émetteur ROC bloque un autre émetteur RTP
    check(send("127.0.0.3", ROC_PORT) == "127.0.0.3", "le même émetteur peut passer en ROC")
    check(send("127.0.0.2", ROC_PORT) is None, "un autre émetteur est refusé en ROC aussi")
    time.sleep(RELEASE + 0.5)
    check(send("127.0.0.2", ROC_PORT) == "127.0.0.2", "ROC : place libérée puis prise")
    check(send("127.0.0.3") is None, "émetteur ROC en place : un autre émetteur RTP est refusé")

print()
print("%d échec(s)" % FAILS)
sys.exit(1 if FAILS else 0)
