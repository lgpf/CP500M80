#include <stdlib.h>
#include <stdio.h>
/* dgt.c - memoria, E/S, interrupcoes e controladora de disco (WD1793) do DGT-100
 *
 * Mapa (igual ao TRS-80 Model I + Expansion Interface):
 *   0000-37DF  ROM (DIGBASIC II)
 *   37E0       le: latch de interrupcao (b7 relogio, b6 disco) / escreve 37E1: drive
 *   37EC-37EF  FDC: comando/status, trilha, setor, dados
 *   3800-3BFF  teclado (matriz)
 *   3C00-3FFF  video 64x16
 *   4000-FFFF  RAM 48K
 */
#include <string.h>
#include <stdio.h>
#include "dgt.h"

#define FDC_DELAY 60       /* ciclos ate o comando "terminar" */

static void nmi_check(dgt_t *m);
static void fdc_set_intrq(dgt_t *m)
{
    m->fdc.intrq = 1;
    nmi_check(m);
}

static disk_t *cur_disk(dgt_t *m)
{
    return m->fdc.drive >= 0 ? m->drv[m->fdc.drive] : NULL;
}

static int index_pulse(dgt_t *m)
{
    /* 300 rpm -> 5 voltas/s; pulso ~4ms */
    unsigned long rev = m->clock_hz / 5;
    return (m->cpu.cyc % rev) < (unsigned long)(m->clock_hz / 250);
}

static void fdc_end(dgt_t *m, uint8_t err);
static uint8_t fdc_status(dgt_t *m)
{
    disk_t *d = cur_disk(m);
    uint8_t s = m->fdc.status;
    if (m->fdc.phase == 0 && (m->fdc.status & 1) && m->cpu.cyc >= m->fdc.t_done) {
        m->fdc.status &= ~1;
        s = m->fdc.status;
        fdc_set_intrq(m);
    }
    /* read address sem leitura dos dados: o campo ID passa e o comando termina
     * (a BIOS do DGPM usa isso so para ver se o drive esta ativo) */
    if (m->fdc.phase == 4 && m->cpu.cyc >= m->fdc.t_done + 5000) { fdc_end(m, m->fdc.pos < m->fdc.len ? 0x04 : 0); s = m->fdc.status; }
    if (!d) return (s & 0x01) | 0x80 | ((m->fdc.cmd & 0x80) ? 0 : (m->fdc.head[m->fdc.drive] == 0 ? 0x04 : 0));  /* sem disco: sem indice */
    if (m->cpu.cyc >= m->fdc.motor_off) return (s & 0x01) | 0x80;  /* motor parado */
    s &= ~0x80;
    if ((m->fdc.cmd & 0x80) == 0 || (m->fdc.cmd & 0xF0) == 0xD0) {   /* tipo I */
        if (d) {
            if (index_pulse(m)) s |= 0x02; else s &= ~0x02;
            if (m->fdc.head[m->fdc.drive] == 0) s |= 0x04; else s &= ~0x04;
            if (d->wp) s |= 0x40;
            s |= 0x20;
        }
    }
    return s;
}

static void fdc_end(dgt_t *m, uint8_t err)
{
    int xfer = m->fdc.phase == 1 || m->fdc.phase == 2;
    m->fdc.phase = 0;
    m->fdc.status = (m->fdc.status & ~0x03) | err;
    m->fdc.status &= ~1;
    /* fim de leitura/gravacao: o INTRQ chega depois do ultimo byte; com os
     * wait states do CP-500 o INI/OUTI seguinte ainda completa antes do NMI */
    if (xfer) { m->intrq_delay = 1; m->intrq_at = m->cpu.cyc + 400; }
    else fdc_set_intrq(m);
}

static void fdc_start_read(dgt_t *m)
{
    disk_t *d = cur_disk(m);
    int trk;
    sector_t *s;
    if (!d) { fdc_end(m, 0x80); return; }
    trk = m->fdc.head[m->fdc.drive];
    s = disk_find(d, trk, m->fdc.side, m->fdc.sector);
    if (!s) { m->fdc.status = 0x10; fdc_end(m, 0x10); return; }
    m->fdc.sec = s;
    m->fdc.buf = s->data; m->fdc.pos = 0; m->fdc.len = s->size;
    m->fdc.phase = 1;
    m->fdc.status = 0x03 | (s->dam == 0xF8 ? 0x20 : 0);
    m->fdc.data = s->data[0];
    m->disk_activity = 20;
}

static void fdc_start_write(dgt_t *m)
{
    disk_t *d = cur_disk(m);
    sector_t *s;
    if (!d) { fdc_end(m, 0x80); return; }
    if (d->wp) { fdc_end(m, 0x40); return; }
    s = disk_find(d, m->fdc.head[m->fdc.drive], m->fdc.side, m->fdc.sector);
    if (!s) { fdc_end(m, 0x10); return; }
    m->fdc.sec = s;
    s->dam = (m->fdc.cmd & 1) ? 0xF8 : 0xFB;
    m->fdc.buf = s->data; m->fdc.pos = 0; m->fdc.len = s->size;
    m->fdc.phase = 2;
    m->fdc.status = 0x03;
    d->dirty = 1;
    m->disk_activity = 20;
}

static void fdc_command(dgt_t *m, uint8_t c)
{
    disk_t *d = cur_disk(m);
    /* LED de atividade (ESP32): drives 0/2 e 1/3, aceso ate 0,5 s depois do acesso */
    if (d && m->fdc.drive >= 0) m->led_until[m->fdc.drive & 1] = m->cpu.cyc + m->clock_hz / 2;
    int *hp = &m->fdc.head[m->fdc.drive < 0 ? 0 : m->fdc.drive];
    /* duplicador de densidade (padrao Percom, usado pelo DGT): FE = simples, FF = dupla */
    if (c == 0xFE || c == 0xFF) { m->fdc.dden = c & 1; return; }
    m->fdc.intrq = 0;
    if ((c & 0xF0) == 0xD0) {               /* force interrupt */
        m->fdc.phase = 0;
        m->fdc.status &= ~0x03;
        m->fdc.cmd = c;
        if (c & 0x0F) fdc_set_intrq(m);
        return;
    }
    m->fdc.cmd = c;
    m->fdc.t_done = m->cpu.cyc + FDC_DELAY;
    if (!(c & 0x80)) {                      /* tipo I */
        int target;
        switch (c & 0xF0) {
        case 0x00: *hp = 0; m->fdc.track = 0; break;
        case 0x10:
            target = m->fdc.data;
            *hp += target - m->fdc.track;
            if (*hp < 0) *hp = 0;
            m->fdc.track = target;
            break;
        case 0x20: case 0x30: *hp += m->fdc.dir; if (c & 0x10) m->fdc.track += m->fdc.dir; break;
        case 0x40: case 0x50: m->fdc.dir = 1; (*hp)++; if (c & 0x10) m->fdc.track++; break;
        case 0x60: case 0x70: m->fdc.dir = -1; (*hp)--; if (c & 0x10) m->fdc.track--; break;
        }
        if (*hp < 0) *hp = 0;
        if (*hp > 84) *hp = 84;
        m->fdc.status = 0x01;
        if ((c & 0x04) && d && d->t[*hp][0].nsec && d->t[*hp][0].s[0].c != m->fdc.track)
            m->fdc.status |= 0x10;          /* erro de verificacao */
        m->fdc.phase = 0;                   /* termina em fdc_status/dgt_run */
        return;
    }
    switch (c & 0xF0) {
    case 0x80: case 0x90:                   /* read sector */
        m->fdc.multi = (c & 0x10) != 0;
        if (c & 0x02) m->fdc.side = (c >> 3) & 1;
        fdc_start_read(m);
        break;
    case 0xA0: case 0xB0:                   /* write sector */
        m->fdc.multi = (c & 0x10) != 0;
        if (c & 0x02) m->fdc.side = (c >> 3) & 1;
        fdc_start_write(m);
        break;
    case 0xC0: {                            /* read address */
        static uint8_t id[6];
        track_t *t;
        static int rot;
        if (!d) { fdc_end(m, 0x80); break; }
        t = &d->t[*hp][m->fdc.side];
        if (!t->nsec) { fdc_end(m, 0x10); break; }
        rot = (rot + 1) % t->nsec;
        id[0] = t->s[rot].c; id[1] = t->s[rot].h; id[2] = t->s[rot].r; id[3] = t->s[rot].n;
        id[4] = id[5] = 0;
        m->fdc.sector = id[0];
        m->fdc.buf = id; m->fdc.pos = 0; m->fdc.len = 6;
        m->fdc.sec = NULL;
        m->fdc.phase = 4;
        m->fdc.status = 0x03;
        m->fdc.data = id[0];
        break; }
    case 0xE0:                              /* read track: nao suportado, devolve vazio */
        fdc_end(m, 0x00);
        break;
    case 0xF0:                              /* write track (formatar) */
        if (!d) { fdc_end(m, 0x80); break; }
        if (d->wp) { fdc_end(m, 0x40); break; }
        m->fdc.fmtlen = 0;
        m->fdc.phase = 3;
        m->fdc.status = 0x03;
        m->disk_activity = 20;
        break;
    }
}

static uint8_t fdc_read_data(dgt_t *m)
{
    uint8_t v = m->fdc.data;
    if (m->fdc.phase == 1 || m->fdc.phase == 4) {
        v = m->fdc.buf[m->fdc.pos++];
        if (m->fdc.pos < m->fdc.len) m->fdc.data = m->fdc.buf[m->fdc.pos];
        else if (m->fdc.phase == 1 && m->fdc.multi) {
            m->fdc.sector++;
            {
                disk_t *d = cur_disk(m);
                sector_t *s = disk_find(d, m->fdc.head[m->fdc.drive], m->fdc.side, m->fdc.sector);
                if (s) { m->fdc.buf = s->data; m->fdc.pos = 0; m->fdc.len = s->size; m->fdc.data = s->data[0]; }
                else fdc_end(m, 0x10);
            }
        } else {
            uint8_t rt = m->fdc.status & 0x20;
            fdc_end(m, 0);
            m->fdc.status = (m->fdc.status & ~0x20) | rt;
        }
    }
    return v;
}

static void fdc_write_data(dgt_t *m, uint8_t v)
{
    m->fdc.data = v;
    if (m->fdc.phase == 2) {
        m->fdc.buf[m->fdc.pos++] = v;
        if (m->fdc.pos >= m->fdc.len) {
            if (m->fdc.multi) { m->fdc.sector++; fdc_start_write(m); }
            else fdc_end(m, 0);
        }
    } else if (m->fdc.phase == 3) {
        if (m->fdc.fmtlen < (int)sizeof m->fdc.fmtbuf) m->fdc.fmtbuf[m->fdc.fmtlen++] = v;
        /* uma trilha MFM tem ~6250 bytes, FM ~3125 */
        if (m->fdc.fmtlen >= (m->fdc.dden ? 6250 : 3125)) {
            disk_format_track(cur_disk(m), m->fdc.head[m->fdc.drive], m->fdc.side,
                              m->fdc.fmtbuf, m->fdc.fmtlen, m->fdc.dden);
            fdc_end(m, 0);
        }
    }
}

/* ---------------- memoria e portas (CP-500 = TRS-80 Model III) ---------------- */
static int tr_on = -1;
static void tr(dgt_t *m, const char *k, int p, int v)
{
    if (tr_on < 0) tr_on = getenv("CP5_TRACE") != NULL;
    if (tr_on) fprintf(stderr, "%s %02X=%02X pc=%04X\n", k, p, v, m->cpu.pc);
}

static uint8_t kbd_read(dgt_t *m, uint16_t a)
{
    uint8_t v = 0; int i;
    for (i = 0; i < 8; i++) if (a & (1 << i)) v |= m->kbd[i];
    if (m->kbd_virt && (a & 0x08)) v |= 0x08;    /* tecla virtual (linha 3, bit 3) */
    return v;
}

/* NMI: INTRQ da controladora (se habilitado em E4) */
static void nmi_check(dgt_t *m)
{
    int act = (m->nmi_mask & 0x80) && m->fdc.intrq;
    if (act && !m->nmi_line) z80_gen_nmi(&m->cpu);
    m->nmi_line = act;
}

static uint8_t mem_read(void *u, uint16_t a)
{
    dgt_t *m = (dgt_t *)u;
    if (m->map == 0) {
        if (a >= 0x4000) return a <= m->ramtop ? m->mem[a] : 0xFF;
        if (a >= 0x3C00) return m->vram[a - 0x3C00];
        if (a >= 0x3800) return kbd_read(m, a);
        if (m->hirom && a >= 0x3000) return m->rom[a + 0x800];
        return m->rom[a];
    }
    if (m->map == 2 && a >= 0x3800 && a < 0x4000) {
        if (a >= 0x3C00) return m->vram[m->vpage * 1024 + (a - 0x3C00)];
        return kbd_read(m, a);
    }
    return m->mem[a];
}

static void mem_write(void *u, uint16_t a, uint8_t v)
{
    dgt_t *m = (dgt_t *)u;
    if (a == m->ivec_watch && m->ivec_watch) m->ivec_broken = 1;
    if ((a >= 0x4012 && a <= 0x4014) || (a >= 0x0038 && a <= 0x003A)) m->ivec_broken = 0;
    if (m->map == 0) {
        if (a >= 0x4000) { if (a <= m->ramtop) m->mem[a] = v; return; }
        if (a >= 0x3C00) { if (m->vram[a - 0x3C00] != v) { m->vram[a - 0x3C00] = v; m->vram_dirty = 1; } return; }
        return;
    }
    if (m->map == 2 && a >= 0x3800 && a < 0x4000) {
        if (a >= 0x3C00) { int o = m->vpage * 1024 + (a - 0x3C00); if (m->vram[o] != v) { m->vram[o] = v; m->vram_dirty = 1; } }
        return;
    }
    m->mem[a] = v;
}

/* CP-500 M80: IN (F4) com o mapa no byte alto do endereco (registrador A).
 *  bits 2-4: 1C = RAM em 0000-FFFF (CP/M); 04 = teclado/video em 3800-3FFF
 *  bits 6-7: pagina de 1K da memoria de video (80x24) que aparece em 3C00 */
static void map_set(dgt_t *m, uint8_t v)
{
    int old80 = m->col80;
    /* Partida: o primeiro IN (F4) da ROM (em 3494h) troca 3000-37FF pela parte alta
     * da 27128 (programa de partida: logotipo, "BASIC (S ou N)", monitor), que se
     * copia para 4300h; o IN (F4) seguinte volta a ROM normal. */
    if (m->cpu.pc == 0x3496 && !m->hirom_done) { m->hirom = 1; m->hirom_done = 1; }
    else m->hirom = 0;
    m->map_reg = v;
    if (v & 0x20) m->map = 0;                    /* ROM + Model III */
    else if ((v & 0x18) == 0x18) m->map = 1;     /* so RAM */
    else m->map = 2;                             /* RAM + teclado/video em 3800-3FFF */
    m->vpage = (v >> 6) & 3;
    m->col80 = (v & 0x02) == 0;
    if (old80 != m->col80) m->vram_dirty = 1;
}

static uint8_t port_in(z80 *z, uint8_t p)
{
    dgt_t *m = (dgt_t *)z->userdata;
    uint8_t v = 0xFF;
    switch (p) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        v = ~(m->irq_latch & 0x04) & 0xFF;      /* b2 = relogio (0 = pendente) */
        break;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        v = 0xFF;
        if (m->fdc.intrq) v &= ~0x80;
        if (m->reset_btn) v &= ~0x20;
        break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        m->irq_latch &= ~0x04;                  /* limpa o relogio */
        v = 0xFF;
        break;
    case 0xF0: {
        /* nenhum disco montado: age como micro sem drives (a ROM entra no BASIC) */
        if (!m->drv[0] && !m->drv[1] && !m->drv[2] && !m->drv[3]) { v = 0xFF; break; }
        v = fdc_status(m);
        { static int lv = -1; if (v != lv) tr(m, "ST ", 0xF0, v); lv = v; }
        m->fdc.intrq = 0; nmi_check(m);
        break; }
    case 0xF1: v = m->fdc.track; break;
    case 0xF2: v = m->fdc.sector; break;
    case 0xF3:
        if (m->intrq_delay && m->fdc.phase == 0) { v = m->fdc.data; m->intrq_delay = 0; fdc_set_intrq(m); break; }
        v = fdc_read_data(m); break;
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: v = 0x30; break;    /* impressora pronta */
    case 0xFF: v = 0xFF; break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:
        tr(m, "MAP", z->port_hi, 0);
        map_set(m, z->port_hi);
        break;
    default: tr(m, "IN ", p, 0); break;
    }
    return v;
}

static void port_out(z80 *z, uint8_t p, uint8_t v)
{
    dgt_t *m = (dgt_t *)z->userdata;
    switch (p) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        m->irq_mask = v;
        m->irq_latch &= ~0x04;              /* (CP-500) escrever na mascara reconhece o relogio */
        break;
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: m->nmi_mask = v; nmi_check(m); break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        if ((m->port_ec ^ v) & 0x0C) m->vram_dirty = 1;
        m->port_ec = v;
        break;
    case 0xF0: tr(m, "CMD", m->fdc.sector, v); if (v < 0x80) tr(m, "TRK", m->fdc.track, m->fdc.data); fdc_command(m, v); nmi_check(m); break;
    case 0xF1: m->fdc.track = v; break;
    case 0xF2: m->fdc.sector = v; break;
    case 0xF3:
        if (m->intrq_delay && m->fdc.phase == 0) { m->intrq_delay = 0; fdc_set_intrq(m); break; }
        fdc_write_data(m, v); break;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: {
        int d = -1;
        if (v & 1) d = 0; else if (v & 2) d = 1; else if (v & 4) d = 2; else if (v & 8) d = 3;
        if (d >= 0) m->fdc.drive = d;
        m->fdc.side = (v >> 4) & 1;
        m->fdc.dden = (v >> 7) & 1;
        m->fdc.motor_off = m->cpu.cyc + 2 * m->clock_hz;
        { static int lv = -1; if (v != lv) tr(m, "SEL", 0xF4, v); lv = v; }
        break; }
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: break;   /* impressora */
    case 0xFF:
        if ((m->port_ff ^ v) & 0x03) {                    /* som pela saida do cassete */
            static const int lv[4] = { 0, 1, -1, 0 };
            unsigned long long hp = m->cpu.cyc - m->snd_last_cyc;
            m->snd_toggles++;
            m->snd_level = lv[v & 3];
            if (hp > 20 && hp < (unsigned long)(m->clock_hz / 20)) { m->snd_hp_sum += hp; m->snd_hp_n++; }
            m->snd_last_cyc = m->cpu.cyc;
        }
        m->port_ff = v;
        break;
    default: tr(m, "OUT", p, v); break;
    }
}

void dgt_key(dgt_t *m, int row, int bit, int down)
{
    if (down) m->kbd[row] |= 1 << bit; else m->kbd[row] &= ~(1 << bit);
}

void dgt_init(dgt_t *m)
{
    memset(m, 0, sizeof *m);
    m->clock_hz = 2000000;
    m->rtc_hz = 30;
    m->ramtop = 0xFFFF;
    m->fdc.drive = 0;
    m->fdc.dden = 1;
    m->fdc.dir = 1;
}

void dgt_reset(dgt_t *m)
{
    unsigned long long cyc = m->cpu.cyc;   /* o contador de ciclos continua (temporizadores) */
    z80_init(&m->cpu);
    m->cpu.cyc = cyc;
    m->fdc.motor_off = cyc;
    m->fdc.t_done = cyc;
    m->cpu.read_byte = mem_read;
    m->cpu.write_byte = mem_write;
    m->cpu.port_in = port_in;
    m->cpu.port_out = port_out;
    m->cpu.userdata = m;
    m->irq_latch = 0;
    m->rtc_next = cyc + m->clock_hz / m->rtc_hz;
    m->fdc.phase = 0; m->fdc.status = 0; m->fdc.cmd = 0; m->fdc.intrq = 0;
    memset(m->kbd, 0, sizeof m->kbd);
    m->vram_dirty = 1;
    m->map = 0; m->hirom = 0; m->hirom_done = 0; m->map_reg = 0xFF; m->col80 = 0; m->vpage = 0; m->irq_mask = 0; m->nmi_mask = 0; m->port_ec = 0; m->nmi_line = 0;
    m->ivec_watch = 0; m->ivec_broken = 0;
}

/* Programas convertidos de fita (ex.: LUNAR/CMD) se copiam por cima do DIGDOS,
 * inclusive da rotina de interrupcao, sem desligar as interrupcoes. Para nao
 * travar, a interrupcao do relogio so e entregue se o vetor (0038h, seguindo
 * os JP) leva a um codigo que parece o inicio de uma rotina de interrupcao. */
static int int_vector_ok(dgt_t *m)
{
    uint16_t a = 0x0038;
    int i;
    uint8_t op;
    for (i = 0; i < 4; i++) {
        op = mem_read(m, a);
        if (op != 0xC3) break;
        a = (uint16_t)(mem_read(m, (uint16_t)(a + 1)) | (mem_read(m, (uint16_t)(a + 2)) << 8));
    }
    if (m->ivec_broken) {
        /* volta a valer se a rotina original reaparecer (programa devolveu o DOS) */
        if (a == m->ivec_watch && mem_read(m, a) == m->ivec_orig) m->ivec_broken = 0;
        else return 0;
    }
    m->ivec_watch = a;           /* se o programa escrever aqui, a rotina foi destruida */
    m->ivec_orig = mem_read(m, a);
    op = mem_read(m, a);
    switch (op) {
    case 0xF5: case 0xC5: case 0xD5: case 0xE5:     /* PUSH */
    case 0xF3: case 0xFB: case 0xC9:                /* DI EI RET */
    case 0xDD: case 0xFD: case 0xED: case 0x08: case 0xD9:
    case 0x18: case 0x3A: case 0x2A: case 0x21: case 0x00: case 0xC3: case 0xCD:
        return 1;
    }
    return 0;
}

void dgt_inject(dgt_t *m, uint8_t ch)
{
    int n = (m->inj_h + 1) & 63;
    if (n != m->inj_t) { m->inj[m->inj_h] = ch; m->inj_h = n; }
}

/* Entrega um caractere injetado: no modo ROM (DIGBASIC/DIGDOS) pelo driver
 * apontado pelo DCB do teclado (4016h); no DGPM pelas entradas CONST/CONIN da BIOS. */
static void check_inject(dgt_t *m)
{
    z80 *z = &m->cpu;
    uint16_t pc = z->pc;
    if (m->map == 0) {
        uint16_t drv = m->mem[0x4016] | (m->mem[0x4017] << 8);
        if (pc != drv || drv < 0x0100) return;
        z->a = m->inj[m->inj_t];
        m->inj_t = (m->inj_t + 1) & 63;
    } else {
        uint16_t base = (uint16_t)((m->mem[1] | (m->mem[2] << 8)) - 3);
        uint16_t t6, t9;
        if (m->mem[base] != 0xC3) return;
        t6 = m->mem[base + 6] == 0xC3 ? (uint16_t)(m->mem[base + 7] | (m->mem[base + 8] << 8)) : 0;
        t9 = m->mem[base + 9] == 0xC3 ? (uint16_t)(m->mem[base + 10] | (m->mem[base + 11] << 8)) : 0;
        if (pc == base + 6 || (t6 && pc == t6)) z->a = 0xFF;          /* CONST: ha tecla */
        else if (pc == base + 9 || (t9 && pc == t9)) {               /* CONIN */
            z->a = m->inj[m->inj_t];
            m->inj_t = (m->inj_t + 1) & 63;
        } else return;
    }
    z->zf = z->a == 0; z->sf = (z->a & 0x80) != 0; z->cf = 0; z->nf = 0; z->hf = 0;
    z->pc = (uint16_t)(mem_read(m, z->sp) | (mem_read(m, (uint16_t)(z->sp + 1)) << 8));   /* RET */
    z->sp += 2;
}

/* CP/M (SO-08): a BIOS le o teclado pela interrupcao e traduz pela tabela em RAM
 * (64 entradas por bloco: normal, SHIFT, minusculas, ...). Para entregar um
 * caractere qualquer, a posicao de uma tecla que nao existe (linha 3, bit 3)
 * recebe o codigo nos 4 blocos e essa tecla e "apertada" por alguns quadros. */
static uint16_t find_keytab(dgt_t *m)
{
    static const char pat[] = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    unsigned a;
    if (m->keytab && !memcmp(m->mem + m->keytab, pat, 27) && m->mem[m->keytab + 128 + 1] == 'a') return m->keytab;
    m->keytab = 0;
    for (a = 0xC000; a < 0x10000 - 256; a++)
        if (m->mem[a] == '@' && !memcmp(m->mem + a, pat, 27) && m->mem[a + 27] == 0 && m->mem[a + 128 + 1] == 'a') {
            m->keytab = (uint16_t)a;
            break;
        }
    return m->keytab;
}

static void vkey_tick(dgt_t *m)
{
    uint16_t t;
    if (m->cpu.cyc < m->vkey_next) return;
    if (m->vkey_state == 1) {                    /* soltar */
        m->kbd_virt = 0;
        m->vkey_state = 2;
        m->vkey_next = m->cpu.cyc + m->clock_hz / 20;
        return;
    }
    m->vkey_state = 0;
    if (m->inj_h == m->inj_t || m->map == 0) return;
    t = find_keytab(m);
    if (!t) { m->vkey_next = m->cpu.cyc + m->clock_hz / 10; return; }
    {
        uint8_t ch = m->inj[m->inj_t]; int b;
        m->inj_t = (m->inj_t + 1) & 63;
        for (b = 0; b < 4; b++) m->mem[t + b * 64 + 27] = ch;
        m->kbd_virt = 1;
        m->vkey_state = 1;
        m->vkey_next = m->cpu.cyc + m->clock_hz / 12;
    }
}

void dgt_run(dgt_t *m, long cycles)
{
    unsigned long long end = m->cpu.cyc + cycles;
    while (m->cpu.cyc < end) {
        if (m->inj_h != m->inj_t && m->map == 0) check_inject(m);
        if (m->map != 0 && (m->inj_h != m->inj_t || m->vkey_state) ) vkey_tick(m);
        z80_step(&m->cpu);
        if (m->intrq_delay && m->cpu.cyc >= m->intrq_at) { m->intrq_delay = 0; fdc_set_intrq(m); }
        if (m->snd_rate && (unsigned long long)m->cpu.cyc * 256 >= m->snd_next_fp) {
            int n = (m->a_h + 1) & (AUD_N - 1);
            /* sem mudanca na saida ha 50 ms = silencio (o nivel parado da porta FF
             * viraria um degrau DC, que estala quando o audio esvazia) */
            int v = (m->cpu.cyc < m->mute_until || !m->snd_mach ||
                     m->cpu.cyc - m->snd_last_cyc > (unsigned long long)(m->clock_hz / 20)) ? 0 : m->snd_level * 40;
            int8_t smp;
            if (v > 127) v = 127;
            if (v < -127) v = -127;
            smp = (int8_t)v;
            m->snd_next_fp += (unsigned long long)((unsigned long long)m->clock_hz * 256 / m->snd_rate);
            if ((unsigned long long)m->cpu.cyc * 256 > m->snd_next_fp + (unsigned long long)m->clock_hz * 256 / 10)
                m->snd_next_fp = (unsigned long long)m->cpu.cyc * 256;   /* resincroniza */
            if (n != m->a_t) { m->audio[m->a_h] = smp; m->a_h = n; }
        }
        if (m->cpu.cyc >= m->rtc_next) {
            m->rtc_next += m->clock_hz / m->rtc_hz;
            m->irq_latch |= 0x04;
        }
        if (m->fdc.phase == 3 && m->cpu.cyc > m->fdc.t_done + (unsigned long)(m->clock_hz / 5)) {
            /* formatacao: termina no proximo pulso de indice */
            disk_format_track(cur_disk(m), m->fdc.head[m->fdc.drive], m->fdc.side,
                              m->fdc.fmtbuf, m->fdc.fmtlen, m->fdc.dden);
            fdc_end(m, 0);
        }
        if (m->fdc.phase == 0 && (m->fdc.status & 1) && m->cpu.cyc >= m->fdc.t_done) {
            m->fdc.status &= ~1;
            fdc_set_intrq(m);
        }
        m->cpu.int_pending = ((m->irq_latch & m->irq_mask & 0x04) && m->cpu.iff1 && int_vector_ok(m)) ? 1 : 0;
        m->cpu.int_data = 0xFF;
    }
    if (m->disk_activity) m->disk_activity--;
}

int dgt_mount(dgt_t *m, int drive, const char *path, char *err)
{
    disk_t *d = disk_open(path, err);
    if (!d) return -1;
    dgt_eject(m, drive);
    m->drv[drive] = d;
    return 0;
}

void dgt_insert(dgt_t *m, int drive, disk_t *d)
{
    dgt_eject(m, drive);
    m->drv[drive] = d;
}

void dgt_eject(dgt_t *m, int drive)
{
    if (m->drv[drive]) {
        disk_save(m->drv[drive]);
        disk_close(m->drv[drive]);
        m->drv[drive] = NULL;
    }
}

void dgt_flush(dgt_t *m)
{
    int i;
    for (i = 0; i < NDRIVES; i++) if (m->drv[i]) disk_save(m->drv[i]);
}
