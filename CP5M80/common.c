/* common.c - carga de ROM/gerador e montagem dos glifos (compartilhado) */
#include <stdio.h>
#include <string.h>
#include "common.h"
#ifdef ARDUINO
#include "esp_heap_caps.h"
#endif

/* CP-500 M80: ROM principal 27128 (16K: BASIC 0000-37FF + monitor/boot 3800-3FFF)
 * e gerador de caracteres 2732 (2 jogos de 256 caracteres 8x8). */
extern const unsigned char rom_cp500[16384], rom_chargen[4096];

static int load_file(const char *p, uint8_t *dst, int n)
{
    FILE *f = fopen(p, "rb");
    int r;
    if (!f) return -1;
    r = (int)fread(dst, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
}

int load_roms(dgt_t *m, const char *dir, char *err)
{
    char p[300];
    (void)err;
#ifdef ARDUINO
    (void)p; (void)dir;
    /* a ROM roda melhor na RAM (ler da flash a cada instrucao deixa o BASIC lento) */
    {
        uint8_t *r = (uint8_t *)heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!r) r = (uint8_t *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
        if (r) { memcpy(r, rom_cp500, 16384); m->rom = r; } else m->rom = rom_cp500;
        printf("CP5M80: ROM em %p\n", (void *)m->rom);
    }
    m->chargen = rom_chargen;
    return 0;
#endif
    sprintf(p, "%s/CP500M80_27128.bin", dir);
    if (load_file(p, m->rom, 16384)) memcpy(m->rom, rom_cp500, 16384);
    sprintf(p, "%s/CP500M80_2732.bin", dir);
    if (load_file(p, m->chargen, 4096)) memcpy(m->chargen, rom_chargen, 4096);
    return 0;
}

/* Glifos 8x24 da tela de 64x16: 0-127 texto, 128-191 semigraficos 2x3 (gerados
 * pelo hardware), 192-255 simbolos especiais ou katakana (porta EC bit 3). */
void build_font(dgt_t *m, uint8_t font[256][24], int kana)
{
    int c, r;
    memset(font, 0, 256 * 24);
    for (c = 0; c < 256; c++) {
        int src = c;
        if (c >= 0x80 && c < 0xC0) continue;
        if (c >= 0xC0 && !kana) src = c - 0x40;          /* especiais = 80-BF do gerador */
        for (r = 0; r < 8; r++) {
            uint8_t g = m->chargen[src * 8 + r];
            font[c][2 + r * 2] = font[c][3 + r * 2] = g;
        }
    }
    for (c = 0; c < 64; c++)
        for (r = 0; r < 24; r++) {
            int blk = r / 8;
            uint8_t g = 0;
            if (c & (1 << (blk * 2))) g |= 0xF0;
            if (c & (1 << (blk * 2 + 1))) g |= 0x0F;
            font[128 + c][r] = g;
        }
}

/* Glifos 8x16 do modo de 80 colunas: segundo jogo do gerador (inclui os
 * semigraficos 2x2 em 80-BF e os caracteres em video inverso em C0-FF). */
void build_font80(dgt_t *m, uint8_t font[256][24])
{
    int c, r;
    memset(font, 0, 256 * 24);
    for (c = 0; c < 256; c++)
        for (r = 0; r < 8; r++)
            font[c][r * 2] = font[c][r * 2 + 1] = m->chargen[2048 + c * 8 + r];
}

/* codigo da celula de video -> numero do glifo */
int glyph_of(uint8_t v)
{
    return v;
}
