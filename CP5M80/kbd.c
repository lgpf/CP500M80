/* kbd.c - layouts ABNT2/US e mapeamento simbolico para a matriz do DGT-100
 *
 * Matriz do DGT-100 (lida pela BIOS do DGPM):
 *   linha 0: @ A B C D E F G         linha 4: 0 1 2 3 4 5 6 7
 *   linha 1: H I J K L M N O         linha 5: 8 9 : ; , - . /
 *   linha 2: P Q R S T U V W         linha 6: ENTER CLEAR BREAK UP DOWN LEFT RIGHT ESPACO
 *   linha 3: X Y Z BS FUNC - CAPS DEL linha 7: SHIFT (bit0), CTRL (demais bits)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kbd.h"

int kbd_layout = LAYOUT_ABNT2;
uint16_t keymap[256][2];
int kbd_shift, kbd_ctrl, kbd_alt;
int kbd_ws_arrows;      /* 1 = setas do PC viram ^E ^X ^S ^D no CP/M */

#define K(r, b) ((uint16_t)(((r) << 3) | (b)))

/* layouts: [codigo] = {normal, shift} */
static const char us_n[128] =
    "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0*\0 ";
static const char us_s[128] =
    "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0*\0 ";

static int layout_char(int layout, int code, int sh)
{
    int c = 0;
    if (code < 0x3A) c = (unsigned char)(sh ? us_s[code] : us_n[code]);
    /* teclado numerico (sem E0) */
    switch (code) {
    case 0x47: c = '7'; break; case 0x48: c = '8'; break; case 0x49: c = '9'; break;
    case 0x4B: c = '4'; break; case 0x4C: c = '5'; break; case 0x4D: c = '6'; break;
    case 0x4F: c = '1'; break; case 0x50: c = '2'; break; case 0x51: c = '3'; break;
    case 0x52: c = '0'; break; case 0x53: c = '.'; break; case 0x4A: c = '-'; break;
    case 0x4E: c = '+'; break; case 0x37: c = '*'; break; case SC_E0(0x35): c = '/'; break;
    }
    if (layout == LAYOUT_ABNT2 || layout == LAYOUT_CUSTOM) {
        switch (code) {
        case 0x07: c = sh ? 0 : '6'; break;        /* trema: nao existe no DGT */
        case 0x1A: c = sh ? '`' : '\''; break;     /* acento agudo/crase */
        case 0x1B: c = sh ? '{' : '['; break;
        case 0x27: c = sh ? CH_CCED_UP : CH_CCED; break;   /* c cedilha */
        case 0x28: c = sh ? '^' : '~'; break;
        case 0x29: c = sh ? '"' : '\''; break;
        case 0x2B: c = sh ? '}' : ']'; break;
        case 0x35: c = sh ? ':' : ';'; break;
        case 0x56: c = sh ? '|' : '\\'; break;
        case 0x73: c = sh ? '?' : '/'; break;
        case 0x7E: c = '.'; break;
        }
    }
    return c;
}

int kbd_char(int code, int shift) { return layout_char(kbd_layout, code, shift); }

/* caractere -> tecla do CP-500 (matriz do TRS-80 Model III).
 * cpm=0: ROM/BASIC/DOS500 - teclas da matriz; o que nao existe no teclado e
 *        injetado na rotina de teclado (DCB em 4016h).
 * cpm=1: CP/M (SO-08) - tudo vai direto pela BIOS (CONST/CONIN), exato. */
static uint16_t char_to_dgt(int ch, int cpm)
{
    static const char r5[] = "89:;,-./";
    static const char sh4[] = "\0!\"#$%&'";
    static const char sh5[] = "()*+<=>?";
    int i;
    if (cpm) {
        if (ch == CH_CCED || ch == CH_CCED_UP) return KM_INJ | (ch == CH_CCED ? 'c' : 'C');
        if (ch == '\b') return KM_INJ | 0x08;
        if (ch > 0 && ch < 0x80) return KM_INJ | ch;
        return KM_NONE;
    }
    if (ch >= 'a' && ch <= 'z') return K((ch - 'a' + 1) / 8, (ch - 'a' + 1) % 8) | KM_PASS;
    if (ch >= 'A' && ch <= 'Z') return K((ch - 'A' + 1) / 8, (ch - 'A' + 1) % 8) | KM_PASS;
    if (ch == '@') return K(0, 0) | KM_NOSHIFT;
    if (ch >= '0' && ch <= '7') return K(4, ch - '0') | KM_NOSHIFT;
    for (i = 0; i < 8; i++) if (r5[i] == ch) return K(5, i) | KM_NOSHIFT;
    for (i = 1; i < 8; i++) if (sh4[i] == ch) return K(4, i) | KM_FSHIFT;
    for (i = 0; i < 8; i++) if (sh5[i] == ch) return K(5, i) | KM_FSHIFT;
    if (ch == ' ') return K(6, 7) | KM_PASS;
    switch (ch) {
    case '\b': return K(6, 5) | KM_NOSHIFT;           /* seta esquerda apaga */
    case '\t': return K(6, 6) | KM_NOSHIFT;           /* seta direita = TAB */
    case '[': return K(6, 3) | KM_NOSHIFT;            /* seta para cima = 5B */
    case CH_CCED: return KM_INJ | 'c';
    case CH_CCED_UP: return KM_INJ | 'C';
    }
    if (ch > 32 && ch < 0x7F) return KM_INJ | ch;     /* \ ] ^ _ ` { | } ~ */
    return KM_NONE;
}

/* legendas das teclas do DGT-100 (sem shift / com shift) -> posicao da matriz */
static const struct { int row, bit; int n, s; } dgt_legend[] = {
    {0,0,'@','\\'}, {4,0,'0',0}, {4,1,'1','!'}, {4,2,'2','"'}, {4,3,'3','#'}, {4,4,'4','$'},
    {4,5,'5','%'}, {4,6,'6','&'}, {4,7,'7','\''}, {5,0,'8','('}, {5,1,'9',')'}, {5,2,':','*'},
    {5,3,';','+'}, {5,4,',','<'}, {5,5,'-','='}, {5,6,'.','>'}, {5,7,'/','?'},
    {6,7,' ',' '}, {-1,0,0,0} };

/* Modo personalizado: cada tecla do PC aperta uma tecla FISICA do DGT (com ou sem
 * o SHIFT do DGT). Nada e traduzido: o resultado e o mesmo da maquina real. */
void kbd_custom_from_abnt2(void)
{
    int c, s, i;
    for (c = 0; c < 256; c++)
        for (s = 0; s < 2; s++) {
            int ch = layout_char(LAYOUT_ABNT2, c, s);
            uint16_t e = KM_NONE;
            if (ch >= 'a' && ch <= 'z') e = K((ch - 'a' + 1) / 8, (ch - 'a' + 1) % 8) | KM_NOSHIFT;
            else if (ch >= 'A' && ch <= 'Z') e = K((ch - 'A' + 1) / 8, (ch - 'A' + 1) % 8) | KM_FSHIFT;
            else if (ch)
                for (i = 0; dgt_legend[i].row >= 0; i++) {
                    if (dgt_legend[i].n == ch) { e = K(dgt_legend[i].row, dgt_legend[i].bit) | KM_NOSHIFT; break; }
                    if (dgt_legend[i].s == ch) { e = K(dgt_legend[i].row, dgt_legend[i].bit) | KM_FSHIFT; break; }
                }
            if (ch == ' ') e = K(6, 7) | KM_PASS;
            keymap[c][s] = e;
        }
    for (s = 0; s < 2; s++) {
        keymap[SC_ENTER][s] = keymap[SC_KENTER][s] = K(6, 0) | KM_PASS;
        keymap[SC_PGUP][s] = keymap[SC_PGDN][s] = keymap[SC_HOME][s] = K(6, 1) | KM_PASS;
        keymap[SC_ESC][s] = K(6, 2) | KM_PASS;
        keymap[SC_UP][s] = K(6, 3) | KM_PASS;
        keymap[SC_DOWN][s] = K(6, 4) | KM_PASS;
        keymap[SC_LEFT][s] = K(6, 5) | KM_PASS;
        keymap[SC_BKSP][s] = KM_CHAR | '\b';       /* seta esq. na ROM, BS no DGPM */
        keymap[SC_RIGHT][s] = keymap[SC_TAB][s] = K(6, 6) | KM_PASS;
        keymap[SC_CAPS][s] = K(4, 0) | KM_FSHIFT;        /* SHIFT + 0 = caps */
    }
}

void kbd_default_map(int layout)
{
    int c, s;
    kbd_layout = layout;
    if (layout == LAYOUT_CUSTOM) { kbd_custom_from_abnt2(); return; }
    for (c = 0; c < 256; c++)
        for (s = 0; s < 2; s++) {
            int ch = layout_char(layout, c, s);
            keymap[c][s] = ch ? (uint16_t)(KM_CHAR | ch) : KM_NONE;
        }
    for (s = 0; s < 2; s++) {
        keymap[SC_ENTER][s] = keymap[SC_KENTER][s] = K(6, 0) | KM_PASS;
        keymap[SC_PGUP][s] = keymap[SC_PGDN][s] = K(6, 1) | KM_PASS;   /* CLEAR */
        keymap[SC_ESC][s] = K(6, 2) | KM_PASS;                         /* BREAK */
        keymap[SC_UP][s] = K(6, 3) | KM_PASS;
        keymap[SC_DOWN][s] = K(6, 4) | KM_PASS;
        keymap[SC_LEFT][s] = K(6, 5) | KM_PASS;
        keymap[SC_RIGHT][s] = K(6, 6) | KM_PASS;
        keymap[SC_BKSP][s] = KM_CHAR | '\b';                          /* BS (depende do sistema) */
        keymap[SC_DEL][s] = KM_CHAR | 0x7F;                            /* DEL no DGPM */
        keymap[SC_CAPS][s] = K(4, 0) | KM_FSHIFT;                      /* CAPS = SHIFT + 0 */
        keymap[SC_HOME][s] = K(6, 1) | KM_PASS;
    }
    keymap[SC_TAB][0] = keymap[SC_TAB][1] = KM_CHAR | '\t';
}

int kbd_load_map(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char hdr[4];
    int c, s;
    if (!f) return -1;
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "DGK", 3)) { fclose(f); return -1; }
    if (hdr[3] != kbd_layout) { fclose(f); return -1; }
    for (c = 0; c < 256; c++)
        for (s = 0; s < 2; s++) {
            unsigned char b[2];
            if (fread(b, 1, 2, f) != 2) { fclose(f); return -1; }
            keymap[c][s] = b[0] | (b[1] << 8);
        }
    fclose(f);
    /* CapsLock e sempre o SHIFT + 0 do CP-500 (mapas antigos tinham outra tecla) */
    keymap[SC_CAPS][0] = keymap[SC_CAPS][1] = K(4, 0) | KM_FSHIFT;
    return 0;
}

int kbd_save_map(const char *path)
{
    FILE *f = fopen(path, "wb");
    int c, s;
    if (!f) return -1;
    fwrite("DGK", 1, 3, f);
    fputc(kbd_layout, f);
    for (c = 0; c < 256; c++)
        for (s = 0; s < 2; s++) { fputc(keymap[c][s] & 0xFF, f); fputc(keymap[c][s] >> 8, f); }
    fclose(f);
    return 0;
}

int kbd_decode(int raw, kev_t *ev)
{
    static int e0 = 0, skip = 0;
    if (skip) { skip--; return 0; }
    if (raw == 0xE0) { e0 = 1; return 0; }
    if (raw == 0xE1) { skip = 5; return 0; }  /* Pause */
    ev->down = !(raw & 0x80);
    ev->code = (raw & 0x7F) | (e0 ? 0x80 : 0);
    e0 = 0;
    if (ev->code == SC_E0(0x2A) || ev->code == SC_E0(0x36)) return 0;  /* shift falso */
    return 1;
}

/* teclas pressionadas: codigo -> entrada resolvida */
static uint16_t held[256];

/* chamado a cada quadro: desfaz a alteracao da tabela pouco depois de soltar a tecla */
static unsigned char isheld[256];
static int caps_seq, cpm_lower;     /* CapsLock: SHIFT, depois SHIFT+0, depois solta (quadros de 20 ms) */
static void rebuild(dgt_t *m);

void kbd_tick(dgt_t *m)
{
    if (caps_seq) {
        if (++caps_seq > 10) caps_seq = 0;
        rebuild(m);
    }
}

static unsigned short press_seq, pressed_at[256];   /* ordem em que as teclas desceram */

static void rebuild(dgt_t *m)
{
    int c, fs = -1, fc = 0, last = -1;
    unsigned short best = 0;
    memset(m->kbd, 0, sizeof m->kbd);
    /* a tecla mais recente decide o SHIFT forcado (ex.: "=" ainda apertado quando
     * o "1" desce: sem isso o "1" sairia "!") */
    for (c = 0; c < 256; c++) {
        uint16_t e;
        if (!isheld[c] || held[c] == KM_NONE) continue;
        e = held[c];
        if ((e & (KM_FSHIFT | KM_NOSHIFT)) && pressed_at[c] >= best) { best = pressed_at[c]; last = c; }
    }
    if (last >= 0) fs = (held[last] & KM_FSHIFT) ? 1 : 0;
    for (c = 0; c < 256; c++) {
        uint16_t e;
        if (!isheld[c]) continue;
        e = held[c];
        if (e == KM_NONE) continue;
        /* tecla antiga que pede o SHIFT ao contrario: sai da matriz */
        if (fs == 1 && (e & KM_NOSHIFT)) continue;
        if (fs == 0 && (e & KM_FSHIFT)) continue;
        m->kbd[(e >> 3) & 7] |= 1 << (e & 7);
        if (e & KM_FCTRL) fc = 1;
    }
    if (fs == -1) fs = kbd_shift;
    if (fs) m->kbd[7] |= 0x01;
    if (caps_seq && caps_seq <= 8) m->kbd[7] |= 0x01;
    if (caps_seq >= 3 && caps_seq <= 6) m->kbd[4] |= 0x01;
    (void)fc;
}

static unsigned char isinj[256];

void kbd_event(dgt_t *m, kev_t *ev)
{
    int c = ev->code;
    switch (c) {
    case SC_LSHIFT: case SC_RSHIFT: kbd_shift = ev->down; rebuild(m); return;
    case SC_LCTRL: case SC_RCTRL: kbd_ctrl = ev->down; rebuild(m); return;
    case SC_ALT: case SC_RALT: kbd_alt = ev->down; rebuild(m); return;
    }
    if (c == SC_CAPS) {
        /* ROM/DOS500: alterna o indicador de maiusculas da ROM (4019h), que e o que
         * o SHIFT+0 faz; CP/M: a BIOS do SO-08 alterna com o codigo 80h da tabela */
        if (ev->down && !isheld[c]) {
            if (m->map == 0) m->mem[0x4019] ^= 1;
            else { cpm_lower = !cpm_lower; dgt_inject(m, 0x80); }   /* 80h: a BIOS alterna o CAPS dela */
        }
        isheld[c] = ev->down;
        return;
    }
    if (ev->down) {
        uint16_t e;
        if (isheld[c] && !isinj[c]) return;    /* repeticao do PC: so para caracteres entregues */
        isheld[c] = 1;
        pressed_at[c] = ++press_seq;
        if (m->click_off) m->mute_until = m->cpu.cyc + (unsigned long long)m->clock_hz * 8 / 100;
        e = keymap[c][kbd_shift ? 1 : 0];
        if (e != KM_NONE && (e & KM_CHAR)) e = char_to_dgt(e & 0xFF, !(m->map == 0));
        if (m->map != 0 && c == SC_ESC) e = KM_INJ | 0x1B;     /* ESC no CP/M */
        /* setas do PC no CP/M: comandos de cursor do WordStar (^E ^X ^S ^D); as setas do
         * CP-500 dariam ^ ` BS TAB, que o WordStar nao usa para mover o cursor */
        if (m->map != 0 && kbd_ws_arrows) {
            if (c == SC_UP) e = KM_INJ | 0x05;
            else if (c == SC_DOWN) e = KM_INJ | 0x18;
            else if (c == SC_LEFT) e = KM_INJ | 0x13;
            else if (c == SC_RIGHT) e = KM_INJ | 0x04;
            else if (c == SC_PGUP) e = KM_INJ | 0x12;          /* ^R pagina acima */
            else if (c == SC_PGDN) e = KM_INJ | 0x03;          /* ^C pagina abaixo */
        }
        /* Ctrl+letra no CP/M: caractere de controle */
        if (m->map != 0 && kbd_ctrl) {
            int ch = kbd_char(c, kbd_shift);
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            if (ch >= '@' && ch <= '_') e = KM_INJ | (ch & 0x1F);
        }
        /* CP/M com caracteres na fila: a tecla da matriz entra na fila tambem (pela
         * tabela da BIOS), para nao passar na frente do que foi digitado antes */
        if (m->map != 0 && e != KM_NONE && !(e & KM_INJ) && m->keytab && (m->inj_h != m->inj_t || m->vkey_state)) {
            int fs = (e & KM_FSHIFT) ? 1 : (e & KM_NOSHIFT) ? 0 : kbd_shift;
            uint8_t code = m->mem[m->keytab + fs * 64 + ((e >> 3) & 7) * 8 + (e & 7)];
            if (code && code < 0x80) e = KM_INJ | code;
        }
        /* CP/M: letras seguem o CAPS do CP-500 (maiusculas ate o CapsLock) */
        if (m->map != 0 && e != KM_NONE && (e & KM_INJ)) {
            int ch = e & 0xFF;
            if (ch >= 'a' && ch <= 'z' && !cpm_lower) e = KM_INJ | (ch - 32);
            else if (ch >= 'A' && ch <= 'Z' && !cpm_lower && !kbd_shift) e = KM_INJ | ch;
        }
        isinj[c] = e != KM_NONE && (e & KM_INJ);
        if (isinj[c]) { dgt_inject(m, e & 0xFF); e = KM_NONE; }
        held[c] = e;
    } else {
        isheld[c] = 0; isinj[c] = 0;
    }
    rebuild(m);
}

void kbd_release_all(dgt_t *m)
{
    memset(isheld, 0, sizeof isheld);
    kbd_shift = kbd_ctrl = kbd_alt = 0;
    memset(m->kbd, 0, sizeof m->kbd);
}

const char *kbd_dgt_name(int row, int bit)
{
    static const char *n[8][8] = {
        { "@", "A", "B", "C", "D", "E", "F", "G" },
        { "H", "I", "J", "K", "L", "M", "N", "O" },
        { "P", "Q", "R", "S", "T", "U", "V", "W" },
        { "X", "Y", "Z", "(3.3)", "(3.4)", "(3.5)", "(3.6)", "(3.7)" },
        { "0", "1", "2", "3", "4", "5", "6", "7" },
        { "8", "9", ":", ";", ",", "-", ".", "/" },
        { "ENTER", "CLEAR", "BREAK", "CIMA", "BAIXO", "ESQ", "DIR", "ESPACO" },
        { "SHIFT", "SHIFT D", "", "", "", "", "", "" } };
    return n[row & 7][bit & 7];
}

const char *kbd_pc_name(int code)
{
    static char buf[16];
    int ch;
    switch (code) {
    case SC_ESC: return "Esc"; case SC_BKSP: return "Backspace"; case SC_TAB: return "Tab";
    case SC_ENTER: return "Enter"; case SC_KENTER: return "Enter(num)"; case SC_SPACE: return "Espaco";
    case SC_CAPS: return "CapsLock"; case SC_UP: return "Seta cima"; case SC_DOWN: return "Seta baixo";
    case SC_LEFT: return "Seta esq"; case SC_RIGHT: return "Seta dir"; case SC_HOME: return "Home";
    case SC_END: return "End"; case SC_PGUP: return "PgUp"; case SC_PGDN: return "PgDn";
    case SC_INS: return "Insert"; case SC_DEL: return "Delete";
    }
    if (code >= 0x3B && code <= 0x44) { sprintf(buf, "F%d", code - 0x3A); return buf; }
    if ((code >= 0x47 && code <= 0x53) || code == 0x37) { sprintf(buf, "Num %c", kbd_char(code, 0)); return buf; }
    if (code == SC_E0(0x35)) return "Num /";
    if (code == 0x27 && kbd_layout != LAYOUT_US) return "C cedilha";
    if (code == 0x73) return "'/' (ABNT2)";
    ch = kbd_char(code, 0);
    if (ch > 32 && ch < 127) { sprintf(buf, "'%c'", ch); return buf; }
    sprintf(buf, "sc %02X", code);
    return buf;
}

uint16_t kbd_resolve(uint16_t e, int cpm)
{
    if (e != KM_NONE && (e & KM_CHAR)) return char_to_dgt(e & 0xFF, cpm);
    return e;
}

const char *kbd_entry_name(uint16_t e)
{
    static char buf[64];
    int ch;
    if (e == KM_NONE) return "(nenhuma)";
    if (!(e & KM_CHAR)) return kbd_dgt_name((e >> 3) & 7, e & 7);
    ch = e & 0xFF;
    if (ch == '\b') return "automatico: seta esq (BASIC) / BS (CP/M)";
    if (ch == '\t') return "automatico: TAB";
    if (ch == 0x7F) return "automatico: DEL (CP/M)";
    if (ch == 0x1B) return "automatico: ESC (CP/M) / BREAK";
    sprintf(buf, "automatico: caractere '%c'", ch);
    return buf;
}
