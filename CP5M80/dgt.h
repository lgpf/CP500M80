/* dgt.h - nucleo da maquina DGT-100 */
#ifndef DGT_H
#define DGT_H
#include <stdint.h>
#include "z80.h"
#include "disk.h"

#define NDRIVES 4
#ifdef ARDUINO
#define AUD_N 4096
#define FMT_N 8192
#else
#define AUD_N 16384
#define FMT_N 16384
#endif

typedef struct {
    z80 cpu;
#ifdef ARDUINO
    uint8_t *mem;               /* 64K em RAM interna estatica (rapida) */
#else
    uint8_t mem[65536];
#endif
    uint8_t vram[4096];         /* video: 64x16 (1K) ou 80x24 (paginas de 1K) */
    int map;                    /* 0 = ROM/Model III, 1 = RAM 64K (CP/M) */
    uint8_t irq_mask, nmi_mask, port_ec; int nmi_line, reset_btn; uint8_t map_reg; int hirom, hirom_done; int intrq_delay; unsigned long long intrq_at;
    int snd_mach;
    uint16_t keytab; int kbd_virt, vkey_state; unsigned long long vkey_next; int vpage, col80;
#ifdef ARDUINO
    const uint8_t *rom;         /* na flash (economiza RAM interna) */
    const uint8_t *chargen;
#else
    uint8_t rom[0x4000];
    uint8_t chargen[4096];
#endif
    uint8_t kbd[8];             /* matriz do teclado: linha -> bits */
    long clock_hz;              /* 2400000 */
    int rtc_hz;                 /* interrupcao periodica */
    unsigned long long rtc_next;
    uint8_t irq_latch;          /* bit7 RTC, bit6 FDC */
    uint8_t port_ff;            /* modo 32 colunas etc */
    int vram_dirty;
    unsigned long snd_toggles;  /* mudancas na saida de som */
    /* som: nivel da saida do cassete (porta FF bits 0-1) */
    int snd_level;
    unsigned long long snd_last_cyc; unsigned long snd_hp_sum; int snd_hp_n;   /* meio-periodos (p/ alto-falante do PC) */
    int snd_rate;               /* amostras/s geradas (0 = nao gera PCM) */
    unsigned long long snd_next_fp;  /* proxima amostra, em ciclos*256 */
    int8_t audio[AUD_N]; int a_h, a_t;
    unsigned long long mute_until;   /* silencia o clique do teclado */
    int click_off;
    int ramtop;                 /* fim da RAM (0xFFFF = 48K) */

    /* controladora WD1793 */
    struct {
        uint8_t status, track, sector, data, cmd;
        int busy_until;         /* em ciclos relativos */
        unsigned long long t_done;
        int drive, side, dden;
        int dir;                /* +1 / -1 */
        int phase;              /* 0 ocioso,1 lendo,2 gravando,3 formatando,4 read address */
        uint8_t *buf; int pos, len;
        uint8_t fmtbuf[FMT_N]; int fmtlen;
        sector_t *sec;
        int multi;
        int intrq;
        unsigned long long motor_off;
        int head[NDRIVES];      /* trilha fisica de cada drive */
    } fdc;
    disk_t *drv[NDRIVES];
    int disk_activity;          /* p/ LED */
    unsigned long long led_until[2];   /* LED dos drives 0/2 e 1/3 (ESP32) */
    /* caracteres injetados direto na rotina de teclado (simbolos que o
     * teclado do DGT nao gera naquele sistema) */
    uint8_t inj[64]; int inj_h, inj_t;
    uint16_t ivec_watch; int ivec_broken; uint8_t ivec_orig;   /* rotina de interrupcao sobrescrita */
} dgt_t;

void dgt_init(dgt_t *m);
void dgt_reset(dgt_t *m);
void dgt_run(dgt_t *m, long cycles);
int  dgt_mount(dgt_t *m, int drive, const char *path, char *err);
void dgt_eject(dgt_t *m, int drive);
void dgt_insert(dgt_t *m, int drive, disk_t *d);
void dgt_flush(dgt_t *m);

/* teclado: tecla na matriz */
void dgt_key(dgt_t *m, int row, int bit, int down);
void dgt_inject(dgt_t *m, uint8_t ch);

#endif
