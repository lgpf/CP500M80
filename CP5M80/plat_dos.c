#ifndef ARDUINO   /* no Arduino/ESP32 este arquivo fica de fora */
/* plat_dos.c - MS-DOS (DJGPP, modo protegido 32 bits)
 * Video VGA em modo texto com fonte carregada, teclado pela INT 9,
 * tempo pelo PIT (uclock), som pelo alto-falante (PIT canal 2). */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <dir.h>
#include <crt0.h>
#include <sys/farptr.h>
#include <sys/exceptn.h>
#include <time.h>
#include "plat.h"

/* trava toda a memoria (a rotina de interrupcao nao pode sofrer paginacao) */
int _crt0_startup_flags = _CRT0_FLAG_LOCK_MEMORY;

#define KBUF 256
static volatile unsigned char kbuf[KBUF];
static volatile int khead, ktail;
static _go32_dpmi_seginfo old_kb, new_kb;
static int kb_installed;

static void kb_isr(void)
{
    unsigned char s = inportb(0x60);
    int n = (khead + 1) & (KBUF - 1);
    if (n != ktail) { kbuf[khead] = s; khead = n; }
    outportb(0x20, 0x20);
}

static uint16_t screen[80 * 25];

/* ---- relogio + som: o PIT canal 0 passa a 11931 Hz (divisor 100). A cada tique
 * a rotina toca uma amostra de 1 bit no alto-falante (o som do DGT ja e 1 bit),
 * entao a onda sai identica, sem oscilar. O relogio do DOS e acertado pelo
 * relogio CMOS ao sair. */
#define TDIV 100
#define TRATE (1193182 / TDIV)
static volatile unsigned long tticks;
static _go32_dpmi_seginfo old_tm, new_tm;
static int tm_installed;
static int (*pcm_pull)(signed char *dst, int n);
static unsigned char spk_base;
static volatile int spk_on;

static void tm_isr(void)
{
    signed char smp = 0;
    tticks++;
    if (pcm_pull && pcm_pull(&smp, 1) == 1 && spk_on)
        outportb(0x61, spk_base | (smp > 0 ? 2 : 0));
    else
        outportb(0x61, spk_base);
    outportb(0x20, 0x20);
}

int plat_audio_rate(void) { return TRATE; }
void plat_audio_source(int (*pull)(signed char *dst, int n)) { pcm_pull = pull; spk_on = 1; }

static void timer_install(void)
{
    spk_base = inportb(0x61) & 0xFC;          /* canal 2 sem gate: OUT fica alto, bit1 manda */
    outportb(0x43, 0xB6); outportb(0x42, 2); outportb(0x42, 0);
    _go32_dpmi_get_protected_mode_interrupt_vector(8, &old_tm);
    new_tm.pm_offset = (unsigned long)tm_isr;
    new_tm.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&new_tm)) return;
    disable();
    _go32_dpmi_set_protected_mode_interrupt_vector(8, &new_tm);
    outportb(0x43, 0x34); outportb(0x40, TDIV & 0xFF); outportb(0x40, TDIV >> 8);
    enable();
    tm_installed = 1;
}

/* desliga a interrupcao rapida (menus, leitura/gravacao de discos e saida):
 * com 12 mil interrupcoes por segundo as chamadas ao DOS ficariam lentissimas
 * e podiam travar. O relogio do BIOS volta a 18,2 Hz. */
static void timer_slow(void)
{
    if (!tm_installed) return;
    disable();
    outportb(0x43, 0x34); outportb(0x40, 0); outportb(0x40, 0);
    _go32_dpmi_set_protected_mode_interrupt_vector(8, &old_tm);
    enable();
    _go32_dpmi_free_iret_wrapper(&new_tm);
    tm_installed = 0;
    outportb(0x61, inportb(0x61) & 0xFC);
}

static void timer_remove(void)
{
    __dpmi_regs r;
    if (!tm_installed) return;
    disable();
    outportb(0x43, 0x34); outportb(0x40, 0); outportb(0x40, 0);   /* 18,2 Hz de novo */
    _go32_dpmi_set_protected_mode_interrupt_vector(8, &old_tm);
    enable();
    _go32_dpmi_free_iret_wrapper(&new_tm);
    tm_installed = 0;
    outportb(0x61, inportb(0x61) & 0xFC);
    /* acerta a hora do DOS pelo relogio CMOS (INT 1Ah/02h -> INT 21h/2Dh) */
    memset(&r, 0, sizeof r);
    r.h.ah = 0x02;
    __dpmi_int(0x1A, &r);
    if (!(r.x.flags & 1)) {
        int h = (r.h.ch >> 4) * 10 + (r.h.ch & 15), mi = (r.h.cl >> 4) * 10 + (r.h.cl & 15), se = (r.h.dh >> 4) * 10 + (r.h.dh & 15);
        memset(&r, 0, sizeof r);
        r.h.ah = 0x2D; r.h.ch = h; r.h.cl = mi; r.h.dh = se; r.h.dl = 0;
        __dpmi_int(0x21, &r);
    }
}
static int in_menu;
static int led_st[2] = { -2, -2 };

int plat_init(void)
{
    __djgpp_set_ctrl_c(0);
    _go32_dpmi_get_protected_mode_interrupt_vector(9, &old_kb);
    new_kb.pm_offset = (unsigned long)kb_isr;
    new_kb.pm_selector = _go32_my_cs();
    if (_go32_dpmi_allocate_iret_wrapper(&new_kb)) return -1;
    _go32_dpmi_set_protected_mode_interrupt_vector(9, &new_kb);
    kb_installed = 1;
    return 0;
}

static void set_mode3(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
    /* sem piscar: fundo claro */
    r.x.ax = 0x1003; r.h.bl = 0;
    __dpmi_int(0x10, &r);
    /* esconde o cursor */
    r.h.ah = 0x01; r.x.cx = 0x2000;
    __dpmi_int(0x10, &r);
}

void plat_shutdown(void)
{
    __dpmi_regs r;
    plat_sound(0);
    timer_remove();
    if (kb_installed) {
        _go32_dpmi_set_protected_mode_interrupt_vector(9, &old_kb);
        _go32_dpmi_free_iret_wrapper(&new_kb);
        kb_installed = 0;
        /* limpa o estado de shift/ctrl/alt da BIOS */
        _farpokeb(_dos_ds, 0x417, 0);
        _farpokeb(_dos_ds, 0x418, 0);
    }
    memset(&r, 0, sizeof r);
    r.x.ax = 0x0003;
    __dpmi_int(0x10, &r);
}

unsigned long plat_ticks(void)
{
    if (tm_installed) return tticks * TDIV;
    return (unsigned long)_farpeekl(_dos_ds, 0x46C) * 65536UL;   /* menus: relogio do BIOS */
}

int plat_scancode(void)
{
    int v;
    if (khead == ktail) return -1;
    disable();
    v = kbuf[ktail];
    ktail = (ktail + 1) & (KBUF - 1);
    enable();
    return v;
}

void plat_idle(void)
{
    /* nada: DOS puro, sem multitarefa */
}

static const unsigned char dac[4][3] = {
    { 16, 63, 24 },     /* verde fosforo */
    { 63, 63, 63 },     /* branco */
    { 44, 44, 44 },     /* cinza */
    { 63, 45, 0 },      /* ambar */
};

static int emode;
void plat_emu_mode(uint8_t font[256][24], int color, int mode)
{
    __dpmi_regs r;
    unsigned char v;
    static uint8_t f16[256][16];
    int i, h = mode ? 16 : 24;
    set_mode3();
    in_menu = 0;
    emode = mode;
    if (!tm_installed) timer_install();
    /* fonte 8x24 (64x16: 400/24 = 16 linhas) ou 8x16 (80x24 em 80x25) */
    if (mode) {
        for (i = 0; i < 256; i++) memcpy(f16[i], font[i], 16);
        dosmemput(f16, 256 * 16, __tb);
    } else dosmemput(font, 256 * 24, __tb);
    memset(&r, 0, sizeof r);
    r.x.ax = 0x1110;
    r.h.bh = h; r.h.bl = 0;
    r.x.cx = 256; r.x.dx = 0;
    r.x.es = __tb >> 4; r.x.bp = __tb & 15;
    __dpmi_int(0x10, &r);
    r.h.ah = 0x01; r.x.cx = 0x2000;
    __dpmi_int(0x10, &r);
    /* caracteres de 8 pontos (640 pixels) para os semigraficos ficarem continuos */
    outportb(0x3C4, 0x00); outportb(0x3C5, 0x01);
    outportb(0x3C4, 0x01); v = inportb(0x3C5); outportb(0x3C5, v | 0x01);
    v = inportb(0x3CC); outportb(0x3C2, v & ~0x0C);
    outportb(0x3C4, 0x00); outportb(0x3C5, 0x03);
    inportb(0x3DA); outportb(0x3C0, 0x13 | 0x20); outportb(0x3C0, 0x00);
    /* cor do fosforo: atributo 0x0F -> registrador de paleta 15 -> DAC 63 */
    outportb(0x3C8, 63);
    for (i = 0; i < 3; i++) outportb(0x3C9, dac[color & 3][i]);
    led_st[0] = led_st[1] = -2;
    /* limpa */
    for (i = 0; i < 80 * 25; i++) _farpokew(_dos_ds, 0xB8000 + i * 2, 0x0F20);
}

void plat_emu_draw(const uint8_t *vram, int mode, int wide)
{
    int r, c;
    _farsetsel(_dos_ds);
    if (mode) {
        for (r = 0; r < 24; r++)
            for (c = 0; c < 80; c++)
                _farnspokew(0xB8000 + (r * 80 + c) * 2, (uint16_t)(0x0F00 | vram[r * 80 + c]));
        return;
    }
    for (r = 0; r < 16; r++)
        for (c = 0; c < 64; c++) {
            int g = vram[r * 64 + c];
            if (wide && (c & 1)) g = ' ';
            _farnspokew(0xB8000 + (r * 80 + c + 8) * 2, (uint16_t)(0x0F00 | g));
        }
}

void plat_menu_mode(void)
{
    timer_slow();
    set_mode3();
    in_menu = 1;
    memset(screen, 0, sizeof screen);
}

void plat_put(int x, int y, int attr, const char *s)
{
    if (y < 0 || y >= 25) return;
    for (; *s && x < 80; s++, x++)
        if (x >= 0) screen[y * 80 + x] = (uint16_t)((attr << 8) | (unsigned char)*s);
}

void plat_fill(int x, int y, int w, int h, int attr, int ch)
{
    int i, j;
    for (j = y; j < y + h && j < 25; j++)
        for (i = x; i < x + w && i < 80; i++)
            if (i >= 0 && j >= 0) screen[j * 80 + i] = (uint16_t)((attr << 8) | ch);
}

void plat_present(void)
{
    /* so copia no modo menu (no modo emulador a tela ja foi escrita) */
    if (in_menu) dosmemput(screen, sizeof screen, 0xB8000);
}

void plat_sound(int hz)
{
    static int cur = -1;
    if (hz == cur) return;
    cur = hz;
    if (hz <= 0) { outportb(0x61, inportb(0x61) & ~3); return; }
    {
        unsigned div = (unsigned)(PLAT_HZ / (unsigned long)hz);
        outportb(0x43, 0xB6);
        outportb(0x42, div & 0xFF);
        outportb(0x42, div >> 8);
        outportb(0x61, inportb(0x61) | 3);
    }
}

int plat_drives(char *list)
{
    unsigned cur, n = 0, d, tmp, total;
    _dos_getdrive(&cur);
    for (d = 1; d <= 26; d++) {
        _dos_setdrive(d, &total);
        _dos_getdrive(&tmp);
        if (tmp == d) list[n++] = (char)('A' + d - 1);
    }
    _dos_setdrive(cur, &total);
    list[n] = 0;
    return (int)n;
}

int plat_quit_requested(void) { return 0; }



/* LEDs dos drives: celulas com fundo vermelho (escuro = apagado, vivo = aceso).
 * 64 colunas: linha 0, colunas 75 e 77 (margem direita); 80 colunas: linha 24. */
void plat_leds(int a, int b)
{
    int v[2], i, row;
    if (in_menu) { led_st[0] = led_st[1] = -2; return; }
    v[0] = a; v[1] = b;
    row = emode ? 24 : 0;
    for (i = 0; i < 2; i++) {
        uint16_t cell;
        if (v[i] == led_st[i]) continue;
        led_st[i] = v[i];
        cell = v[i] < 0 ? 0x0020 : v[i] ? 0xC020 : 0x4020;
        _farpokew(_dos_ds, 0xB8000 + (row * 80 + 75 + i * 2) * 2, cell);
    }
}
#endif
