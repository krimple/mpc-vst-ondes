/* MIDI Probe (spike S0, docs/DESIGN.md section 9): a silent instrument that shows what MIDI MPC delivers to a VST2
 * plugin. The screen shows the last six messages, the pitch-bend range seen, counts per message type and the CC numbers
 * seen; every message is also appended to midiprobe.log next to the .so (written by a thread, never the audio thread).
 * vst.json turns off the wrapper's CC 20-35 -> Q-Link and NRPN handling, so every CC reaches the engine. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"
#include "plugin_dir.h"

#define RING 1024

typedef struct { uint8_t m[3]; uint64_t frame; } ev_t;

typedef struct {
    char line[6][64];
    int pb_min, pb_max, pb_last;
    unsigned cnt_note, cnt_cc, cnt_pb, cnt_at, cnt_pat, cnt_pc, cnt_other;
    uint8_t cc_seen[128];
    unsigned rev;
    uint64_t frame;
    /* log ring: written under the engine lock (midi), read by the writer thread */
    ev_t ring[RING];
    volatile unsigned head, tail;
    pthread_t th; volatile int stop; int th_on;
    char path[600];
} probe_t;

static void describe(const uint8_t *m, char *b, size_t n) {
    const int st = m[0] & 0xF0, ch = (m[0] & 0x0F) + 1;
    switch (st) {
    case 0x90: snprintf(b, n, "NOTE ON ch%d n%d v%d", ch, m[1], m[2]); break;
    case 0x80: snprintf(b, n, "NOTE OFF ch%d n%d v%d", ch, m[1], m[2]); break;
    case 0xA0: snprintf(b, n, "POLY AT ch%d n%d = %d", ch, m[1], m[2]); break;
    case 0xB0: snprintf(b, n, "CC%d ch%d = %d", m[1], ch, m[2]); break;
    case 0xC0: snprintf(b, n, "PROG ch%d = %d", ch, m[1]); break;
    case 0xD0: snprintf(b, n, "CHAN AT ch%d = %d", ch, m[1]); break;
    case 0xE0: { int v = m[1] | (m[2] << 7); snprintf(b, n, "BEND ch%d = %d (%+d)", ch, v, v - 8192); break; }
    default: snprintf(b, n, "RAW %02X %02X %02X", m[0], m[1], m[2]); break;
    }
}

static void *writer(void *arg) {
    probe_t *p = arg;
    FILE *f = NULL;
    while (!p->stop) {
        while (p->tail != __atomic_load_n(&p->head, __ATOMIC_ACQUIRE)) {
            if (!f) {
                f = fopen(p->path, "a");
                if (!f) break;
                time_t t = time(NULL);
                fprintf(f, "# MIDI Probe session %s", ctime(&t));
            }
            const ev_t e = p->ring[p->tail % RING];
            char d[64];
            describe(e.m, d, sizeof d);
            fprintf(f, "%10.4f  %02X %02X %02X  %s\n", e.frame / 44100.0, e.m[0], e.m[1], e.m[2], d);
            __atomic_store_n(&p->tail, p->tail + 1, __ATOMIC_RELEASE);
        }
        if (f) fflush(f);
        usleep(200000);
    }
    if (f) fclose(f);
    return NULL;
}

static void *create(const char *dir) {
    (void)dir;
    probe_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->pb_min = 16384; p->pb_max = -1; p->pb_last = -1;
    char d[512];
    if (mpc_plugin_dir(d, sizeof d)) snprintf(p->path, sizeof p->path, "%s/midiprobe.log", d);
    else snprintf(p->path, sizeof p->path, "/tmp/midiprobe.log");
    p->th_on = pthread_create(&p->th, NULL, writer, p) == 0;
    return p;
}
static void destroy(void *i) {
    probe_t *p = i;
    if (p->th_on) { p->stop = 1; pthread_join(p->th, NULL); }
    free(p);
}

static void midi(void *i, const uint8_t *m, int len) {
    probe_t *p = i;
    uint8_t msg[3] = {len > 0 ? m[0] : 0, len > 1 ? m[1] : 0, len > 2 ? m[2] : 0};
    const int st = msg[0] & 0xF0;
    switch (st) {
    case 0x80: case 0x90: p->cnt_note++; break;
    case 0xB0: p->cnt_cc++; p->cc_seen[msg[1] & 0x7F] = 1; break;
    case 0xE0: {
        int v = msg[1] | (msg[2] << 7);
        p->cnt_pb++; p->pb_last = v;
        if (v < p->pb_min) p->pb_min = v;
        if (v > p->pb_max) p->pb_max = v;
        break;
    }
    case 0xD0: p->cnt_at++; break;
    case 0xA0: p->cnt_pat++; break;
    case 0xC0: p->cnt_pc++; break;
    default: p->cnt_other++; break;
    }
    memmove(p->line[1], p->line[0], sizeof p->line[0] * 5);
    describe(msg, p->line[0], sizeof p->line[0]);
    p->rev++;
    if (p->head - __atomic_load_n(&p->tail, __ATOMIC_ACQUIRE) < RING) {   /* full: drop (the counts still see it) */
        ev_t *e = &p->ring[p->head % RING];
        memcpy(e->m, msg, 3); e->frame = p->frame;
        __atomic_store_n(&p->head, p->head + 1, __ATOMIC_RELEASE);
    }
}

static void set_param(void *i, const char *k, const char *v) {
    probe_t *p = i;
    if (!strcmp(k, "clear") && atof(v) > 0.5) {
        memset(p->line, 0, sizeof p->line);
        p->pb_min = 16384; p->pb_max = -1; p->pb_last = -1;
        p->cnt_note = p->cnt_cc = p->cnt_pb = p->cnt_at = p->cnt_pat = p->cnt_pc = p->cnt_other = 0;
        memset(p->cc_seen, 0, sizeof p->cc_seen);
        p->rev++;
    }
}

static int get_param(void *i, const char *k, char *b, int n) {
    probe_t *p = i;
    if (!strcmp(k, "display_rev")) return snprintf(b, n, "%u", p->rev);
    if (k[0] == 'm' && k[1] >= '1' && k[1] <= '6' && !k[2]) {
        const char *l = p->line[k[1] - '1'];
        return snprintf(b, n, "%s", l[0] ? l : "-");
    }
    if (!strcmp(k, "bend")) {
        if (p->pb_last < 0) return snprintf(b, n, "no bend yet");
        return snprintf(b, n, "%d  min %d max %d", p->pb_last, p->pb_min, p->pb_max);
    }
    if (!strcmp(k, "counts"))
        return snprintf(b, n, "N%u CC%u PB%u AT%u PAT%u PC%u ?%u", p->cnt_note, p->cnt_cc, p->cnt_pb, p->cnt_at,
                        p->cnt_pat, p->cnt_pc, p->cnt_other);
    if (!strcmp(k, "ccs")) {
        int len = snprintf(b, n, "CC:");
        for (int c = 0; c < 128 && len < n - 5; c++) if (p->cc_seen[c]) len += snprintf(b + len, n - len, " %d", c);
        if (len == 3) len = snprintf(b, n, "no CC yet");
        return len;
    }
    if (!strcmp(k, "clear")) return snprintf(b, n, "0");
    return 0;
}

static void render(void *i, int16_t *o, int f) {
    ((probe_t *)i)->frame += f;
    memset(o, 0, (size_t)f * 4);
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
