#ifndef ARDUINO   /* no Arduino/ESP32 este arquivo fica de fora */
/* plat_lnx.c - plataforma de TESTE em Linux: tela em memoria, teclas por script
 * DGT_SCRIPT="w:3000 t:dir\n d k:F9 d ..." */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "plat.h"
#include "kbd.h"

static uint16_t screen[80 * 25];
static uint8_t evram[2048]; static int emode;
static int in_menu, color;
static unsigned long vt;              /* tempo virtual em tiques */
static unsigned long wait_until;
static int q[4096], qh, qt;
static char *script, *sp;

static void push(int v) { q[qh++ & 4095] = v; }
static void press(int code, int shift)
{
    if (shift) push(0x2A);
    if (code & 0x80) push(0xE0);
    push(code & 0x7F);
    push(-2);
    if (code & 0x80) push(0xE0);
    push((code & 0x7F) | 0x80);
    if (shift) push(0xAA);
    push(-2);
}
static void dump(void)
{
    int r, c;
    printf("---- %s ----\n", in_menu ? "MENU" : "EMU");
    if (in_menu) for (r = 0; r < 25; r++) { for (c = 0; c < 80; c++) { int ch = screen[r * 80 + c] & 0xFF; putchar(ch >= 32 && ch < 127 ? ch : ' '); } putchar('\n'); }
    else { int W = emode ? 80 : 64, H = emode ? 24 : 16; for (r = 0; r < H; r++) { for (c = 0; c < W; c++) { uint8_t v = evram[r * W + c]; putchar(v >= 0x80 ? '#' : (v < 32 ? '.' : v)); } putchar('\n'); } }
}
static int findchar(int ch, int *sh)
{
    int c, s;
    for (s = 0; s < 2; s++) for (c = 1; c < 0x80; c++) if (kbd_char(c, s) == ch) { *sh = s; return c; }
    return -1;
}
static void next_token(void)
{
    char tok[256]; int n = 0;
    while (*sp == ' ') sp++;
    if (!*sp) { if (in_menu) exit(0); press(SC_F10, 1); return; }
    while (*sp && *sp != ' ') tok[n++] = *sp++;
    tok[n] = 0;
    if (tok[0] == 'w') wait_until = vt + (unsigned long)atoi(tok + 2) * (PLAT_HZ / 1000);
    else if (tok[0] == 'd') dump();
    else if (tok[0] == 'x') { int r, c; for (r = 0; r < 16; r++) { for (c = 0; c < 64; c++) printf("%02x", evram[r * 64 + c]); printf("\n"); } }
    else if (tok[0] == 's' || tok[0] == 'u') press((int)strtol(tok + 2, NULL, 16), tok[0] == 's');
    else if (tok[0] == 't') {
        char *p;
        for (p = tok + 2; *p; p++) {
            int sh, c;
            if (*p == '_') { press(SC_SPACE, 0); continue; }
            if (*p == '|') { press(SC_ENTER, 0); continue; }
            c = findchar(*p, &sh);
            if (c > 0) press(c, sh);
        }
    } else if (tok[0] == 'k') {
        static const struct { const char *n; int c; } t[] = {
            {"F1",SC_F1},{"F3",SC_F3},{"F5",SC_F5},{"F9",SC_F9},{"F10",SC_F10},{"ESC",SC_ESC},{"ENTER",SC_ENTER},
            {"UP",SC_UP},{"DOWN",SC_DOWN},{"LEFT",SC_LEFT},{"RIGHT",SC_RIGHT},{"TAB",SC_TAB},{"BKSP",SC_BKSP},
            {"DEL",SC_DEL},{"PGUP",SC_PGUP},{"PGDN",SC_PGDN},{"HOME",SC_HOME},{"END",SC_END},{NULL,0}};
        int i, sh = 0; char *nm = tok + 2;
        if (!strncmp(nm, "S+", 2)) { sh = 1; nm += 2; }
        for (i = 0; t[i].n; i++) if (!strcmp(t[i].n, nm)) press(t[i].c, sh);
    }
}
int plat_init(void) { script = getenv("DGT_SCRIPT"); sp = script ? script : ""; return 0; }
void plat_shutdown(void) {}
unsigned long plat_ticks(void) { vt += PLAT_HZ / 1000; return vt; }
int plat_scancode(void)
{
    if (vt < wait_until) return -1;
    while (qt == qh) {
        if (vt < wait_until) return -1;
        next_token();
        if (qt == qh && vt < wait_until) return -1;
    }
    {
        int v = q[qt++ & 4095];
        if (v == -2) { wait_until = vt + 60 * (PLAT_HZ / 1000); return -1; }
        return v;
    }
}
void plat_idle(void) { vt += PLAT_HZ / 1000; }
void plat_emu_mode(uint8_t font[256][24], int c, int mode) { (void)font; (void)mode; color = c; in_menu = 0; }
void plat_emu_draw(const uint8_t *v, int mode, int wide) { (void)wide; emode = mode; memcpy(evram, v, mode ? 1920 : 1024); }
void plat_menu_mode(void) { in_menu = 1; memset(screen, 0, sizeof screen); }
void plat_put(int x, int y, int attr, const char *s) { if (y < 0 || y > 24) return; for (; *s && x < 80; s++, x++) if (x >= 0) screen[y * 80 + x] = (attr << 8) | (unsigned char)*s; }
void plat_fill(int x, int y, int w, int h, int attr, int ch) { int i, j; for (j = y; j < y + h && j < 25; j++) for (i = x; i < x + w && i < 80; i++) screen[j * 80 + i] = (attr << 8) | ch; }
void plat_present(void) {}
void plat_sound(int hz) { static int last; if (hz != last && getenv("DGT_SND")) printf("[som %d Hz]\n", hz); last = hz; }
int plat_drives(char *l) { strcpy(l, "C"); return 1; }

int plat_quit_requested(void) { return 0; }

int plat_audio_rate(void) { return 0; }
void plat_audio_source(int (*pull)(signed char *dst, int n)) { (void)pull; }

void plat_leds(int a, int b) { (void)a; (void)b; }
#endif
