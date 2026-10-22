# rock-n-roll-pw-in

Réception, sur la Raspberry Pi, du son d'un PC par le réseau local, **sans perte et
sans compression** : RTP, 192 kHz / 24 bits, tampon de 10 ms en Ethernet. En option
(activée par défaut) : **ROC**, même fréquence, avec réparation des paquets perdus,
pour le Wi-Fi.

Module de la famille [pi-server](https://github.com/flavi1/pi-server), à installer
**après [pi-sound-server](https://github.com/flavi1/pi-sound-server)** : le son reçu
entre dans `MASTER` comme n'importe quelle autre source (volume du knob, toutes les
sorties). Côté PC : [rock-n-roll-pw-out](https://github.com/flavi1/rock-n-roll-pw-out).

```
PC (PipeWire)                                     Raspberry Pi
 appli → « Hifi — RTP » ─RTP 192 kHz/24 bits, UDP 46000──────▶ rock-n-roll.rtp ─┐
 appli → « Hifi — ROC » ─ROC 192 kHz/f32, UDP 10001-10003───▶ rock-n-roll.roc ─┴▶ MASTER ─▶ DAC
                          réseau local, un seul émetteur à la fois
```

## Installation

Avec pi-server :

```
sudo pi-server add net
```

Ou à la main :

```
git clone https://github.com/flavi1/rock-n-roll-pw-in.git
sudo bash rock-n-roll-pw-in/install.sh
```

Puis, sur le PC, voir [rock-n-roll-pw-out](https://github.com/flavi1/rock-n-roll-pw-out).

## Fonctionnement

- **Un seul émetteur à la fois**, **réseau local uniquement**, sans mot de passe :
  c'est le pare-feu de pi-server qui l'assure (ensemble nftables à une seule place).
  Le premier PC qui envoie garde la place **tant qu'il envoie, même du silence** ;
  elle se libère `release-seconds` (5 s) après son dernier paquet. Les paquets d'un
  second PC sont rejetés par le noyau en attendant : jamais de mélange de deux flux.
- **Format fixe** : 192 kHz, 24 bits, stéréo, identique des deux côtés. Le PC
  convertit ce qu'il joue vers 192 kHz (qualité PipeWire) ; la Pi n'a rien à
  renégocier.
- Le récepteur tourne dans **son propre processus PipeWire** (service utilisateur
  `rock-n-roll-rtp` de l'utilisateur `hifi`) : il peut être relancé sans toucher au
  reste (Mopidy, radios, Bluetooth continuent).
- Les radios et la musique de Mopidy, le Bluetooth, la platine… restent utilisables en
  parallèle : tout se mélange dans `MASTER`.
- Sans pare-feu pi-server, le port n'est pas filtré et rien n'empêche deux émetteurs
  de se mélanger (avertissement à l'installation).

## ROC (optionnel, activé par défaut)

- Programme `rock-n-roll-roc` (bibliothèque libroc de Debian, non modifiée) compilé
  par `install.sh` (`libroc-dev` et `gcc` installés au besoin). En cas d'échec : ROC
  indisponible (signalé par `status`), RTP fonctionne normalement.
- 192 kHz, flottant 32 bits, stéréo, tampon de 80 ms. Réparation des pertes
  (Reed-Solomon) si le PC l'a aussi ; la Pi écoute en même temps un flux sans
  correction pour les PC dont la libroc n'en a pas (Ubuntu).
- **Processus à part** (service utilisateur `rock-n-roll-roc`, redémarrages
  limités) : une panne de ROC n'affecte ni RTP ni le reste de la pile audio.
- **Garde-fou** : rien d'invalide ni au-delà de la pleine échelle n'est envoyé à
  `MASTER`. Si libroc produit des valeurs aberrantes, le flux est coupé et le
  service relancé.
- Le pare-feu ouvre les ports ROC **seulement si le programme est présent**, et
  ils partagent la place unique avec RTP.
- Désactiver : `[roc]` `enabled = no` dans `local.conf`, puis `apply`.

## Limitations connues

- **Aucune perte de paquet n'est tolérée** : son sans compression ni correction
  d'erreur, chaque paquet perdu (1 ms) provoque une resynchronisation, donc une
  coupure. Diagnostic : `log-level = 3` dans `local.conf`, puis le journal (voir
  Dépannage) affiche `unexpected seq` à chaque perte ; compter les paquets reçus
  pendant la lecture (≈ 10 000 en 10 s attendus) :
  `nstat -n >/dev/null; sleep 10; nstat UdpInDatagrams`.
- **Économie d'énergie Ethernet (EEE) du PC** : cause fréquente de pertes (constaté :
  carte Intel `e1000e` derrière une box SFR, ~13 % de paquets perdus). Remède côté
  PC : README de [rock-n-roll-pw-out](https://github.com/flavi1/rock-n-roll-pw-out),
  « Couper l'EEE ».
- **ROC n'est pas transparent.** Le rattrapage de l'écart d'horloge PC / Pi passe par
  le rééchantillonneur de libroc. À 192 kHz, seul l'intégré fonctionne (Speex
  déborde au-delà de ~65 kHz). Gain de +0,9 dB, corrigé par rock-n-roll-roc ; résidu
  de bruit et de distorsion d'environ −50 dB, mesuré sur un sinus de 1 kHz. RTP reste
  la référence ; ROC est un secours pour le Wi-Fi.

## Configuration

`/etc/rock-n-roll-pw-in/pw-in.conf` est **déployé depuis le dépôt** (l'ancien est gardé :
`pw-in.conf.avant-maj`). Réglages propres à votre Pi : `/etc/rock-n-roll-pw-in/local.conf`
(mêmes sections, lu après, jamais écrasé). Après modification :

```
sudo rock-n-roll-pw-in apply
```

| Section | Clé | Défaut | Rôle |
|---|---|---|---|
| `[global]` | `user` | `auto` | utilisateur PipeWire (celui de pi-sound-server, sinon `hifi`) |
| | `target` | `auto` | sortie visée (`MASTER` de pi-sound-server) |
| | `resample.quality` | `auto` | qualité de l'ajustement d'horloge (celle de pi-sound-server : 10) |
| `[rtp]` | `enabled` | `yes` | récepteur actif |
| | `port` | `46000` | port UDP |
| | `latency-ms` | `10` | tampon : Ethernet 5 à 10, Wi-Fi 50 à 200 |
| | `rate`, `format`, `channels` | `192000`, `S24BE`, `2` | **doivent être identiques côté PC** |
| | `release-seconds` | `5` | silence réseau avant qu'un autre PC puisse prendre la place |
| | `description` | `Réseau (RTP)` | nom affiché |
| `[roc]` | `enabled` | `yes` | récepteur ROC (si le programme a pu être compilé) |
| | `source-port`, `repair-port`, `plain-port` | `10001`, `10002`, `10003` | flux corrigé (source + réparation), flux simple |
| | `fec` | `auto` | `auto` (correction si possible + flux simple), `rs8m`, `ldpc`, `none` |
| | `latency-ms` | `80` | latence visée : Ethernet 40 à 80, Wi-Fi 80 à 200 |
| | `rate` | `192000` | identique côté PC |
| | `description` | `Réseau (ROC)` | nom affiché |

## État

```
sudo rock-n-roll-pw-in status
```

```
  RTP         : port UDP 46000, 192000 Hz S24BE 2 canaux, tampon 10 ms, place libérée après 5 s
  ROC         : ports UDP 10001, 10002, 10003, 192000 Hz f32 stéréo, correction rs8m si possible, sinon flux simple, latence 80 ms
  service RTP : active
  flux RTP    : en réception
  service ROC : active
  flux ROC    : présent
  émetteur    : 192.168.1.20
```

## Dépannage

| Symptôme | Piste |
|---|---|
| Pas de son, `flux : en attente d'un émetteur` | côté PC, la sortie « Hifi — Ethernet » est-elle choisie ? Même port ? `sudo nft list set inet filter rnr_sender` : un autre PC tient la place ? |
| `émetteur : 192.168.1.x` qui n'est pas le bon PC | l'autre PC envoie encore (même du silence) : arrêtez-le ou changez sa sortie ; la place se libère 5 s après |
| PC sur un autre sous-réseau | `LAN4` de pi-server (`/etc/pi-server/pi-server.conf`) |
| Coupures régulières, journal `unexpected seq` | paquets perdus avant la Pi : voir « Limitations connues » (EEE du PC, câble, box) |
| Son haché | réseau chargé ou Wi-Fi : `latency-ms = 30` (ou plus) dans `local.conf`, puis `apply` |
| Son trop aigu / grave, craquements réguliers | format différent des deux côtés : `rate` / `format` |
| `ROC : … INDISPONIBLE` | libroc / gcc non installables : `sudo apt install libroc-dev gcc pkg-config`, puis `sudo pi-server update rock-n-roll-pw-in` |
| ROC : pas de son | même Pi, mêmes ports, côté PC sortie « Hifi — réseau (ROC 192 kHz) » ; journal ROC ci-dessous |
| Journal | RTP : `sudo journalctl _SYSTEMD_USER_UNIT=rock-n-roll-rtp.service -f` ; ROC : `sudo journalctl _SYSTEMD_USER_UNIT=rock-n-roll-roc.service -f` |

## Tests

```
bash tests/test-config.sh          # configuration et fichiers générés
sudo python3 tests/test-firewall.py   # pare-feu réel dans un espace réseau isolé
bash tests/test-roc.sh build && bash tests/test-roc.sh loop   # ROC réel (libroc requise)
```

En CI (`.github/workflows/roc.yml`), un sinus est envoyé puis reçu en vrai, sans
PipeWire, pour vérifier : libroc 0.3 de Debian trixie (comme la Pi), 0.3 d'Ubuntu
24.04 sans correction, 0.4 d'Ubuntu 26.04, et les combinaisons entre elles ; 5 % de
paquets perdus (corrigés avec Reed-Solomon) ; niveau, résidu, dérive d'horloge, et
le garde-fou sous dérive extrême.

```
```

## Licence

GPL-3.0-or-later
