/* Offline checks of the PRESET field (VST programs): built-in names and loads, saved presets read from .xpl files the
 * way MPC writes them (JUCE base64 of an fxb chunk set around the state), empty slots, and state restore not re-loading
 * a preset. Build and run: tests/run.sh */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "engine.h"

static const mpc_engine_t *E;
static int fails;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    printf("%s ", ok ? "ok  " : "FAIL"); vprintf(fmt, a); printf("\n");
    va_end(a);
    if (!ok) fails++;
}
static void get(void *s, const char *k, char *b, int n) { b[0] = 0; E->get_param(s, k, b, n); }

/* JUCE MemoryBlock::toBase64Encoding, as tools/xpl.py b64enc */
static void b64enc(const unsigned char *b, int n, char *out) {
    static const char T[] = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = out + snprintf(out, 16, "%d.", n);
    for (int i = 0; i < (n * 8 + 5) / 6; i++) {
        int start = i * 6, byte = start >> 3, off = start & 7, res = 0, sofar = 0, nb = 6;
        while (nb > 0 && byte < n) {
            int t = nb < 8 - off ? nb : 8 - off;
            res |= ((b[byte] & ((0xff >> (8 - t)) << off)) >> off) << sofar;
            sofar += t; nb -= t; byte++; off = 0;
        }
        *o++ = T[res];
    }
    *o = 0;
}
static void put32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void write_xpl(const char *path, const char *chunk, unsigned uid) {
    unsigned char bin[4096] = {0};
    int cl = (int)strlen(chunk);
    memcpy(bin, "CcnK", 4); memcpy(bin + 8, "FBCh", 4);
    put32(bin + 12, 1); put32(bin + 16, uid); put32(bin + 20, 100); put32(bin + 24, 1);
    put32(bin + 0x9C, cl); memcpy(bin + 0xA0, chunk, cl);
    static char st[8192];
    b64enc(bin, 0xA0 + cl, st);
    FILE *f = fopen(path, "w");
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n<pluginstate>\n  <PLUGIN name=\"Ondes\" uid=\"%08x\"/>\n"
               "  <preset></preset>\n  <state>%s</state>\n</pluginstate>\n", uid, st);
    fclose(f);
}

int main(void) {
    E = mpc_engine();
    char b[64];

    /* a folder of saved presets, as MPC writes them */
    char dir[] = "/tmp/ondes-presets-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char p1[256], p2[256], p3[256], p4[256];
    snprintf(p1, sizeof p1, "%s/Warm Square.xpl", dir);
    snprintf(p2, sizeof p2, "%s/another sound.xpl", dir);
    snprintf(p3, sizeof p3, "%s/Not Ondes.xpl", dir);
    snprintf(p4, sizeof p4, "%s/notes.txt", dir);
    write_xpl(p1, "t_o=0\nt_g=1\nvolume=-12\npreset=3\n", 0x4f6e644d);   /* 'OndM' */
    write_xpl(p2, "t_8=1\ngrave=5\n", 0x4f6e644d);
    write_xpl(p3, "t_8=1\n", 0x12345678);                               /* another plugin's uid: skipped */
    FILE *f = fopen(p4, "w"); fputs("x", f); fclose(f);
    setenv("ONDES_USER_PRESETS", dir, 1);

    void *s = E->create(NULL);
    for (int i = 0; i < 200; i++) { get(s, "preset:10", b, sizeof b); if (strcmp(b, "-")) break; usleep(5000); }   /* the scan thread */

    get(s, "preset:0", b, sizeof b); check(!strcmp(b, "Pure Sine"), "program 0 is Pure Sine (%s)", b);
    get(s, "preset:9", b, sizeof b); check(!strcmp(b, "Poly Organ"), "program 9 is Poly Organ (%s)", b);
    get(s, "preset:10", b, sizeof b); check(!strcmp(b, "another sound"), "program 10 is the first saved preset, by name (%s)", b);
    get(s, "preset:11", b, sizeof b); check(!strcmp(b, "Warm Square"), "program 11 is the second (%s)", b);
    get(s, "preset:12", b, sizeof b); check(!strcmp(b, "-"), "another plugin's .xpl and other files are skipped; slot 12 is empty (%s)", b);
    get(s, "preset:39", b, sizeof b); check(!strcmp(b, "-"), "the last slot, 39, is empty (%s)", b);

    /* built-in: back to the defaults, then its own settings */
    E->set_param(s, "state", "grave=7\nt_s=1\n");
    E->set_param(s, "preset", "2");                 /* Hollow Clarinet */
    char o[8], c[8], g[16], sb[8];
    get(s, "t_o", o, sizeof o); get(s, "t_c", c, sizeof c); get(s, "grave", g, sizeof g); get(s, "t_s", sb, sizeof sb);
    check(!strcmp(o, "0") && !strcmp(c, "1") && atof(g) == 3 && !strcmp(sb, "0"),
          "Hollow Clarinet: C on, O off, Bass 3, earlier edits (Breath) cleared (t_o=%s t_c=%s grave=%s t_s=%s)", o, c, g, sb);
    get(s, "preset", b, sizeof b); check(atoi(b) == 2, "the current program is 2 (%s)", b);

    /* saved preset: its whole state, but its own "preset=3" does not load program 3 */
    E->set_param(s, "preset", "11");
    char tg[8], vol[16], cc[8];
    get(s, "t_g", tg, sizeof tg); get(s, "volume", vol, sizeof vol); get(s, "t_c", cc, sizeof cc);
    check(!strcmp(tg, "1") && atof(vol) == -12 && !strcmp(cc, "0"), "Warm Square loads: G on, volume -12, the clarinet's C gone (t_g=%s vol=%s t_c=%s)", tg, vol, cc);
    get(s, "preset", b, sizeof b); check(atoi(b) == 11, "the current program is 11, not the 3 stored in the file (%s)", b);

    /* an empty slot does nothing */
    E->set_param(s, "preset", "30");
    get(s, "t_g", tg, sizeof tg); get(s, "preset", b, sizeof b);
    check(!strcmp(tg, "1") && atoi(b) == 30, "picking an empty slot keeps the sound; it becomes current, as the wrapper expects (t_g=%s, program %s)", tg, b);

    /* a project reload: the chunk restores the sound and the program number, without re-loading the preset */
    E->set_param(s, "state", "preset=2\nt_o=1\nt_c=0\ngrave=-4\n");
    get(s, "t_o", o, sizeof o); get(s, "grave", g, sizeof g); get(s, "preset", b, sizeof b);
    check(!strcmp(o, "1") && atof(g) == -4 && atoi(b) == 2, "state restore keeps its own values and only notes program 2 (t_o=%s grave=%s)", o, g);
    E->destroy(s);

    /* no folder at all: built-ins only */
    setenv("ONDES_USER_PRESETS", "/nonexistent-ondes-dir", 1);
    s = E->create(NULL);
    usleep(50000);
    get(s, "preset:10", b, sizeof b); check(!strcmp(b, "-"), "no saved presets: slot 10 is empty (%s)", b);
    E->destroy(s);

    unlink(p1); unlink(p2); unlink(p3); unlink(p4); rmdir(dir);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
