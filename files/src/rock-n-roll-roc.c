/* SPDX-License-Identifier: GPL-3.0-or-later
 * =============================================================================
 *  rock-n-roll-roc — son haute résolution par ROC (correction des pertes)
 *
 *  Fichier commun à rock-n-roll-pw-in (Pi) et rock-n-roll-pw-out (PC).
 *
 *  PipeWire ne sait envoyer en ROC que du 44,1 kHz / 16 bits. Ce programme
 *  utilise libroc (0.3 ou 0.4, sans modification) avec un encodage déclaré
 *  au démarrage : 192 kHz (par défaut), flottant 32 bits, stéréo, et la
 *  correction d'erreur Reed-Solomon (rs8m) ou LDPC.
 *
 *  Il ne parle pas à PipeWire : il relaie le son brut par un tube vers une
 *  commande lancée après « -- » (pw-cat en pratique). Un plantage ici ne
 *  touche donc que ce flux ROC, jamais le reste de la pile audio.
 *
 *    rock-n-roll-roc send --host IP [options] -- pw-record --raw ... -
 *        lit le son brut sur la sortie de la commande et l'envoie
 *    rock-n-roll-roc recv [options] -- pw-cat --playback --raw ... -
 *        reçoit et écrit le son brut sur l'entrée de la commande
 *
 *  Options : --source-port N (10001)  --repair-port N (10002)
 *            --plain-port N (10003 : flux sans correction)
 *            --fec auto|rs8m|ldpc|none (auto)  --rate HZ (192000)
 *            --packet-us N (750, envoi)   --latency-ms N (80, réception)
 *            --bind IP (0.0.0.0, réception)  -v (journal détaillé)
 *
 *  Correction d'erreur : toutes les libroc ne l'ont pas (Ubuntu 24.04 : sans
 *  OpenFEC). En « auto », la réception écoute le flux corrigé (rs8m, si
 *  disponible) ET un flux simple ; l'envoi utilise rs8m si sa libroc le permet,
 *  sinon le flux simple. Toutes les combinaisons fonctionnent ; la correction
 *  n'est active que si les deux côtés l'ont.
 *
 *  Format sur le tube : flottant 32 bits, ordre de la machine, 2 canaux
 *  entrelacés (pw-cat --raw --format f32 --channels 2).
 * =============================================================================
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <roc/config.h>
#include <roc/context.h>
#include <roc/endpoint.h>
#include <roc/frame.h>
#include <roc/log.h>
#include <roc/receiver.h>
#include <roc/sender.h>
#include <roc/version.h>

#define CHANNELS 2
#define SAMPLE_BYTES 4                  /* float32 */
#define FRAME_BYTES (CHANNELS * SAMPLE_BYTES)
#define ENCODING_ID 100                 /* type de charge RTP dynamique */
#define CHUNK_FRAMES 480                /* 2,5 ms à 192 kHz */
#define MAX_PACKET 2048
#define MAX_FRAME 16384

static volatile sig_atomic_t stop_requested = 0;
static pid_t child = -1;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void die(const char *msg)
{
    fprintf(stderr, "rock-n-roll-roc : %s\n", msg);
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
        "usage : rock-n-roll-roc send --host IP [options] -- COMMANDE...\n"
        "        rock-n-roll-roc recv [options] -- COMMANDE...\n"
        "options : --source-port N --repair-port N --plain-port N\n"
        "          --fec auto|rs8m|ldpc|none --rate HZ\n"
        "          --packet-us N (envoi) --latency-ms N --bind IP (réception) -v\n");
    exit(2);
}

static long parse_num(const char *s, long lo, long hi, const char *name)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end || v < lo || v > hi) {
        fprintf(stderr, "rock-n-roll-roc : %s invalide : %s (%ld à %ld)\n", name, s, lo, hi);
        exit(2);
    }
    return v;
}

/* Lance la commande ; son entrée (to_child) ou sa sortie est reliée à un tube.
 * Renvoie l'extrémité côté parent. */
static int spawn(char **argv, int to_child)
{
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0)
        die("pipe impossible");
    child = fork();
    if (child < 0)
        die("fork impossible");
    if (child == 0) {
        /* le relais s'arrête si ce programme meurt */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (to_child)
            dup2(fds[0], STDIN_FILENO);
        else
            dup2(fds[1], STDOUT_FILENO);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        fprintf(stderr, "rock-n-roll-roc : impossible de lancer %s : %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    if (to_child) {
        close(fds[0]);
        /* petit tube : peu de latence ajoutée (≈ 10 ms à 192 kHz) */
        fcntl(fds[1], F_SETPIPE_SZ, 16384);
        return fds[1];
    }
    close(fds[1]);
    return fds[0];
}

static void stop_child(void)
{
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        child = -1;
    }
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR && !stop_requested)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void make_uris(const char *fec, const char *host, int sport, int rport,
                      char *src, char *rep, size_t n, roc_fec_encoding *enc)
{
    if (!strcmp(fec, "rs8m")) {
        snprintf(src, n, "rtp+rs8m://%s:%d", host, sport);
        snprintf(rep, n, "rs8m://%s:%d", host, rport);
        *enc = ROC_FEC_ENCODING_RS8M;
    } else if (!strcmp(fec, "ldpc")) {
        snprintf(src, n, "rtp+ldpc://%s:%d", host, sport);
        snprintf(rep, n, "ldpc://%s:%d", host, rport);
        *enc = ROC_FEC_ENCODING_LDPC_STAIRCASE;
    } else if (!strcmp(fec, "none")) {
        snprintf(src, n, "rtp://%s:%d", host, sport);
        rep[0] = 0;
        *enc = ROC_FEC_ENCODING_DISABLE;
    } else {
        die("--fec doit valoir rs8m, ldpc ou none");
    }
}

static roc_endpoint *endpoint(const char *uri)
{
    roc_endpoint *ep = NULL;
    if (roc_endpoint_allocate(&ep) != 0 || roc_endpoint_set_uri(ep, uri) != 0) {
        fprintf(stderr, "rock-n-roll-roc : adresse invalide : %s\n", uri);
        exit(1);
    }
    return ep;
}

/* Ouvre l'émetteur avec la correction demandée ; 0 si impossible (libroc sans
 * cette correction). */
static roc_sender *open_sender(roc_context *ctx, const roc_media_encoding *enc,
                               long packet_us, const char *fec, const char *host,
                               int sport, int rport, int pport, char *desc, size_t n)
{
    char src[128], rep[128];
    roc_fec_encoding fe;
    make_uris(fec, host, !strcmp(fec, "none") ? pport : sport, rport, src, rep, sizeof(src), &fe);
    roc_sender_config sc;
    memset(&sc, 0, sizeof(sc));
    sc.frame_encoding = *enc;
    sc.packet_encoding = (roc_packet_encoding)ENCODING_ID;
    sc.packet_length = (unsigned long long)packet_us * 1000ULL;
    sc.fec_encoding = fe;
    /* le rythme est celui de la commande (PipeWire), pas une horloge interne */
    sc.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;
    roc_sender *snd = NULL;
    if (roc_sender_open(ctx, &sc, &snd) != 0)
        return NULL;
    roc_endpoint *es = endpoint(src);
    int bad = roc_sender_connect(snd, ROC_SLOT_DEFAULT, ROC_INTERFACE_AUDIO_SOURCE, es) != 0;
    roc_endpoint_deallocate(es);
    if (!bad && rep[0]) {
        roc_endpoint *er = endpoint(rep);
        bad = roc_sender_connect(snd, ROC_SLOT_DEFAULT, ROC_INTERFACE_AUDIO_REPAIR, er) != 0;
        roc_endpoint_deallocate(er);
    }
    if (bad) {
        roc_sender_close(snd);
        return NULL;
    }
    snprintf(desc, n, "%s%s%s", src, rep[0] ? " + " : "", rep);
    return snd;
}

/* Écoute un flux (slot) ; 0 si impossible. */
static int bind_slot(roc_receiver *rcv, roc_slot slot, const char *fec, const char *ip,
                     int sport, int rport, char *desc, size_t n)
{
    char src[128], rep[128];
    roc_fec_encoding fe;
    make_uris(fec, ip, sport, rport, src, rep, sizeof(src), &fe);
    roc_endpoint *es = endpoint(src);
    int bad = roc_receiver_bind(rcv, slot, ROC_INTERFACE_AUDIO_SOURCE, es) != 0;
    roc_endpoint_deallocate(es);
    if (!bad && rep[0]) {
        roc_endpoint *er = endpoint(rep);
        bad = roc_receiver_bind(rcv, slot, ROC_INTERFACE_AUDIO_REPAIR, er) != 0;
        roc_endpoint_deallocate(er);
    }
    if (bad) {
        roc_receiver_unlink(rcv, slot);
        return 0;
    }
    size_t l = strlen(desc);
    snprintf(desc + l, n - l, "%s%s%s%s", l ? " ; " : "", src, rep[0] ? " + " : "", rep);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        usage();
    const char *mode = argv[1];
    int sending = !strcmp(mode, "send");
    if (!sending && strcmp(mode, "recv"))
        usage();

    const char *host = NULL, *bind_ip = "0.0.0.0", *fec = "auto";
    long sport = 10001, rport = 10002, pport = 10003, rate = 192000, packet_us = 750, latency_ms = 80;
    int verbose = 0, cmd = -1;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--")) { cmd = i + 1; break; }
        else if (!strcmp(a, "-v")) { verbose++; continue; }
        if (!v)
            usage();
        if (!strcmp(a, "--host")) host = v;
        else if (!strcmp(a, "--bind")) bind_ip = v;
        else if (!strcmp(a, "--fec")) fec = v;
        else if (!strcmp(a, "--source-port")) sport = parse_num(v, 1, 65535, "--source-port");
        else if (!strcmp(a, "--repair-port")) rport = parse_num(v, 1, 65535, "--repair-port");
        else if (!strcmp(a, "--plain-port")) pport = parse_num(v, 1, 65535, "--plain-port");
        else if (!strcmp(a, "--rate")) rate = parse_num(v, 8000, 768000, "--rate");
        else if (!strcmp(a, "--packet-us")) packet_us = parse_num(v, 100, 20000, "--packet-us");
        else if (!strcmp(a, "--latency-ms")) latency_ms = parse_num(v, 5, 5000, "--latency-ms");
        else usage();
        i++;
    }
    if (cmd < 0 || cmd >= argc)
        usage();
    if (sending && !host)
        die("--host obligatoire pour l'envoi");
    if (strcmp(fec, "auto") && strcmp(fec, "rs8m") && strcmp(fec, "ldpc") && strcmp(fec, "none"))
        die("--fec doit valoir auto, rs8m, ldpc ou none");

    /* paquet : son + en-têtes RTP / FEC / IP / UDP, sous la taille d'un paquet réseau */
    long payload = rate * packet_us / 1000000 * FRAME_BYTES;
    if (sending && payload + 12 + 16 + 28 > 1472)
        die("--packet-us trop grand : le paquet dépasserait 1472 octets (fragmentation)");

    roc_log_set_level(verbose >= 2 ? ROC_LOG_DEBUG : verbose ? ROC_LOG_INFO : ROC_LOG_ERROR);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;            /* pas de SA_RESTART : read/write interrompus */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    roc_context_config cc;
    memset(&cc, 0, sizeof(cc));
    cc.max_packet_size = MAX_PACKET;
    cc.max_frame_size = MAX_FRAME;
    roc_context *ctx = NULL;
    if (roc_context_open(&cc, &ctx) != 0)
        die("roc_context_open a échoué");

    roc_media_encoding enc;
    memset(&enc, 0, sizeof(enc));
    enc.rate = (unsigned int)rate;
    enc.format = ROC_FORMAT_PCM_FLOAT32;
    enc.channels = ROC_CHANNEL_LAYOUT_STEREO;
    if (roc_context_register_encoding(ctx, ENCODING_ID, &enc) != 0)
        die("roc_context_register_encoding a échoué (libroc trop ancienne ?)");

    char desc[512] = "";
    float buf[CHUNK_FRAMES * CHANNELS];
    int rc = 0;

    if (sending) {
        roc_sender *snd = NULL;
        const char *used = fec;
        if (!strcmp(fec, "auto")) {
            used = "rs8m";
            snd = open_sender(ctx, &enc, packet_us, "rs8m", host, (int)sport, (int)rport, (int)pport,
                              desc, sizeof(desc));
            if (!snd) {
                fprintf(stderr, "rock-n-roll-roc : correction d'erreur indisponible dans cette libroc : "
                                "flux sans correction\n");
                used = "none";
            }
        }
        if (!snd)
            snd = open_sender(ctx, &enc, packet_us, used, host, (int)sport, (int)rport, (int)pport,
                              desc, sizeof(desc));
        if (!snd)
            die("ouverture de l'envoi impossible (voir messages de libroc ci-dessus)");
        fprintf(stderr, "rock-n-roll-roc : envoi vers %s (%ld Hz, f32, stéréo, paquets de %ld µs)\n",
                desc, rate, packet_us);

        int in = spawn(&argv[cmd], 0);
        size_t have = 0;
        char *raw = (char *)buf;
        while (!stop_requested) {
            ssize_t r = read(in, raw + have, sizeof(buf) - have);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "rock-n-roll-roc : lecture : %s\n", strerror(errno));
                rc = 1;
                break;
            }
            if (r == 0) {
                fprintf(stderr, "rock-n-roll-roc : la commande s'est arrêtée\n");
                rc = 1;
                break;
            }
            have += (size_t)r;
            size_t whole = have - have % FRAME_BYTES;
            if (whole) {
                roc_frame f;
                f.samples = raw;
                f.samples_size = whole;
                if (roc_sender_write(snd, &f) != 0) {
                    fprintf(stderr, "rock-n-roll-roc : roc_sender_write a échoué\n");
                    rc = 1;
                    break;
                }
                memmove(raw, raw + whole, have - whole);
                have -= whole;
            }
        }
        close(in);
        roc_sender_close(snd);
    } else {
        roc_receiver_config rcfg;
        memset(&rcfg, 0, sizeof(rcfg));
        rcfg.frame_encoding = enc;
        /* rythme imposé par la commande (lecture du tube) : horloge de la Pi */
        rcfg.clock_source = ROC_CLOCK_SOURCE_EXTERNAL;
        rcfg.target_latency = (unsigned long long)latency_ms * 1000000ULL;
        /* rattrapage de la dérive d'horloge : le rééchantillonneur Speex échoue à
         * 192 kHz (« Ratio overflow ») ; celui de libroc fonctionne à toute fréquence */
        rcfg.resampler_backend = ROC_RESAMPLER_BACKEND_BUILTIN;
        /* Un récepteur par protocole (libroc refuse de les mêler dans un même
         * récepteur) : flux corrigé (rs8m) si disponible, et flux simple. Leurs
         * sorties sont additionnées (un seul émetteur à la fois : pare-feu). */
        roc_receiver *rcv[2] = { NULL, NULL };
        int n = 0;
        const char *fecs[2] = { fec, NULL };
        int ports[2] = { !strcmp(fec, "none") ? (int)pport : (int)sport, 0 };
        if (!strcmp(fec, "auto")) {
            fecs[0] = "rs8m"; ports[0] = (int)sport;
            fecs[1] = "none"; ports[1] = (int)pport;
        }
        for (int k = 0; k < 2; k++) {
            if (!fecs[k])
                continue;
            if (roc_receiver_open(ctx, &rcfg, &rcv[k]) != 0)
                die("roc_receiver_open a échoué");
            if (bind_slot(rcv[k], ROC_SLOT_DEFAULT, fecs[k], bind_ip, ports[k], (int)rport,
                          desc, sizeof(desc))) {
                n++;
            } else {
                roc_receiver_close(rcv[k]);
                rcv[k] = NULL;
                if (!strcmp(fecs[k], "rs8m") && !strcmp(fec, "auto"))
                    fprintf(stderr, "rock-n-roll-roc : correction d'erreur indisponible dans cette libroc : "
                                    "flux sans correction seulement\n");
            }
        }
        if (!n)
            die("écoute impossible (port déjà utilisé ? voir messages de libroc ci-dessus)");
        fprintf(stderr, "rock-n-roll-roc : écoute sur %s (%ld Hz, f32, stéréo, latence %ld ms)\n",
                desc, rate, latency_ms);

        float tmp[CHUNK_FRAMES * CHANNELS];
        int out = spawn(&argv[cmd], 1);
        while (!stop_requested) {
            int first = 1;
            for (int k = 0; k < 2; k++) {
                if (!rcv[k])
                    continue;
                roc_frame f;
                f.samples = first ? buf : tmp;
                f.samples_size = sizeof(buf);
                /* sans émetteur : silence, le flux reste en place */
                if (roc_receiver_read(rcv[k], &f) != 0)
                    memset(f.samples, 0, sizeof(buf));
                if (!first)
                    for (size_t i = 0; i < CHUNK_FRAMES * CHANNELS; i++)
                        buf[i] += tmp[i];
                first = 0;
            }
            if (write_all(out, buf, sizeof(buf)) != 0) {
                if (!stop_requested) {
                    fprintf(stderr, "rock-n-roll-roc : la commande s'est arrêtée\n");
                    rc = 1;
                }
                break;
            }
        }
        close(out);
        for (int k = 0; k < 2; k++)
            if (rcv[k])
                roc_receiver_close(rcv[k]);
    }

    stop_child();
    roc_context_close(ctx);
    return rc;
}
