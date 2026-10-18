/* main.c - emulador do CP-500 M80 (Prologica): laco principal e menus
 *
 * F1 ajuda   F5 configuracao   F9 discos   F10 reset   Shift+F10 sair
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dgt.h"
#include "common.h"
#include "kbd.h"
#include "plat.h"
#ifdef ARDUINO
#include "esp_fs.h"
#include "esp_heap_caps.h"
#define main dgt_main
#endif

#define VERSAO "1.0"

#ifdef ARDUINO
static uint8_t esp_mem[65536];          /* .bss na RAM interna: a memoria do Z80 tem de ser rapida */
static dgt_t *Mp;
#define M (*Mp)
void dgt_prealloc(void) { Mp = (dgt_t *)heap_caps_calloc(1, sizeof(dgt_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
#else
static dgt_t M;
#endif
#ifdef ARDUINO
static uint8_t (*font)[24], (*font80)[24];     /* na PSRAM: a RAM interna vai para o Z80 */
#else
static uint8_t font[256][24], font80[256][24];
#endif
static int cur_mode = -1, font_kana = 0;     /* 0 = 64x16, 1 = 80x24 */
static char basedir[260];
static char cfgpath[300], keypath[300];

/* configuracao */
static struct {
    int color;      /* 0 verde, 1 branco, 2 cinza, 3 ambar */
    int memk;       /* 16 32 48 64 */
    int sound;
    int layout;
    int click;      /* (nao usado no CP-500) */
    int wsarrows;   /* setas no CP/M: 0 = originais do CP-500, 1 = WordStar */
    int asciifont;  /* 1 = mostra [ \\ ] ^ _ no lugar das setas e do Ç */
    int leds;       /* LEDs dos drives (tela/pinos) */
    int cursor;     /* caractere do cursor em todos os sistemas (0 = original de cada um) */
} cfg = { 0, 48, 1, LAYOUT_ABNT2, 1, 0, 1, 1, 0 };   /* setas originais, LEDs ligados */

static const char *color_names[] = { "Verde", "Branco", "Cinza", "Ambar" };
static char lastdir[260];

/* ------------------------------------------------------------ util */
static void join(char *out, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\')) sprintf(out, "%s%s", dir, name);
    else sprintf(out, "%s/%s", dir, name);
}

static void cfg_load(void)
{
    FILE *f = fopen(cfgpath, "r");
    char line[400], key[64], val[300];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63[^=]=%299[^\r\n]", key, val) != 2) continue;
        if (!strcmp(key, "cor")) cfg.color = atoi(val) & 3;
        else if (!strcmp(key, "memoria")) cfg.memk = atoi(val);
        else if (!strcmp(key, "som")) cfg.sound = atoi(val);
        else if (!strcmp(key, "teclado")) { cfg.layout = atoi(val); if (cfg.layout < 0 || cfg.layout > 2) cfg.layout = 0; }
        else if (!strcmp(key, "cursor")) cfg.cursor = atoi(val) & 0xFF;
        else if (!strcmp(key, "leds")) cfg.leds = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "clique")) cfg.click = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "setaswordstar")) cfg.wsarrows = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "glifos")) cfg.asciifont = atoi(val) ? 1 : 0;
        else if (!strcmp(key, "pasta")) strncpy(lastdir, val, sizeof lastdir - 1);
    }
    fclose(f);
    if (cfg.memk != 16 && cfg.memk != 32 && cfg.memk != 48) cfg.memk = 64;
}

static void cfg_save(void)
{
    FILE *f = fopen(cfgpath, "w");
    if (!f) return;
    fprintf(f, "cor=%d\nmemoria=%d\nsom=%d\ncursor=%d\nclique=%d\nteclado=%d\nglifos=%d\nsetaswordstar=%d\nleds=%d\npasta=%s\n",
            cfg.color, cfg.memk, cfg.sound, cfg.cursor, cfg.click, cfg.layout, cfg.asciifont, cfg.wsarrows, cfg.leds, lastdir);
    fclose(f);
}

static void apply_memory(void)
{
    M.ramtop = 0xFFFF;                       /* 48K no BASIC/DOS500 (4000-FFFF); o CP/M usa os 64K */
}



static void setup_keyboard(void)
{
    kbd_default_map(cfg.layout);
    if (cfg.layout == LAYOUT_CUSTOM) kbd_load_map(keypath);
}

/* ------------------------------------------------------------ teclas nos menus */
enum { K_NONE = 0, K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END,
       K_ENTER, K_ESC, K_BKSP, K_DEL, K_TAB, K_F1, K_F2, K_F3, K_F5, K_F9, K_F10, K_INS };

static int menu_shift;

/* espera uma tecla; devolve codigo K_* ou caractere. raw recebe o scancode */
static int getkey_raw(int *raw)
{
    kev_t ev;
    for (;;) {
        int r;
        if (plat_quit_requested()) return K_ESC;
        r = plat_scancode();
        if (r < 0) { plat_idle(); continue; }
        if (!kbd_decode(r, &ev)) continue;
        if (ev.code == SC_LSHIFT || ev.code == SC_RSHIFT) { menu_shift = ev.down; continue; }
        if (!ev.down) continue;
        if (raw) *raw = ev.code;
        switch (ev.code) {
        case SC_UP: case 0x48: return K_UP;
        case SC_DOWN: case 0x50: return K_DOWN;
        case SC_LEFT: case 0x4B: return K_LEFT;
        case SC_RIGHT: case 0x4D: return K_RIGHT;
        case SC_PGUP: case 0x49: return K_PGUP;
        case SC_PGDN: case 0x51: return K_PGDN;
        case SC_HOME: case 0x47: return K_HOME;
        case SC_END: case 0x4F: return K_END;
        case SC_ENTER: case SC_KENTER: return K_ENTER;
        case SC_ESC: return K_ESC;
        case SC_BKSP: return K_BKSP;
        case SC_DEL: case 0x53: return K_DEL;
        case SC_INS: case 0x52: return K_INS;
        case SC_TAB: return K_TAB;
        case SC_F1: return K_F1;
        case SC_F2: return K_F2;
        case SC_F3: return K_F3;
        case SC_F5: return K_F5;
        case SC_F9: return K_F9;
        case SC_F10: return K_F10;
        }
        {
            int c = kbd_char(ev.code, menu_shift);
            if (c) return c;
        }
    }
}

static int getkey(void) { return getkey_raw(NULL); }

/* ------------------------------------------------------------ desenho */
#define A_TITLE 0x1F
#define A_BACK  0x17
#define A_SEL   0x70
#define A_HINT  0x30
#define A_BOX   0x1E
#define A_ERR   0x4F

static void putf(int x, int y, int attr, const char *fmt, ...);
#include <stdarg.h>
static void putf(int x, int y, int attr, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    plat_put(x, y, attr, buf);
}

static void bar(int y, int attr, const char *s)
{
    plat_fill(0, y, 80, 1, attr, ' ');
    plat_put(1, y, attr, s);
}

static void message(const char *msg, int attr)
{
    int w = (int)strlen(msg) + 4, x = (80 - w) / 2;
    plat_fill(x, 11, w, 3, attr, ' ');
    plat_put(x + 2, 12, attr, msg);
    plat_present();
    getkey();
}

/* linha de entrada de texto */
static int input_line(int x, int y, int w, char *buf, int max)
{
    int len = (int)strlen(buf);
    for (;;) {
        plat_fill(x, y, w, 1, A_SEL, ' ');
        plat_put(x, y, A_SEL, len > w - 1 ? buf + len - (w - 1) : buf);
        plat_put(x + (len > w - 1 ? w - 1 : len), y, 0x0F, " ");
        plat_present();
        {
            int k = getkey();
            if (k == K_ENTER) return 1;
            if (k == K_ESC) return 0;
            if (k == K_BKSP && len) buf[--len] = 0;
            else if (k >= 32 && k < 127 && len < max - 1) { buf[len++] = (char)k; buf[len] = 0; }
        }
    }
}

/* ------------------------------------------------------------ ajuda (F1) */
static const char *help_text[] = {
    "CP-500 M80 - Emulador  versao " VERSAO,
    "",
    "Emula o microcomputador CP-500 M80 da Prologica, compativel com o",
    "TRS-80 Model III: Z80 a 2 MHz, video 64x16 (BASIC e DOS500) e 80x24 no",
    "CP/M (SO-08), 64K de RAM e ate 4 drives de disquete.",
    "",
    "TECLAS DO EMULADOR",
    "  F1          Esta ajuda (Esc ou F1 volta)",
    "  F5          Configuracao: cor, memoria, som, teclado e mapa de teclas",
    "  F9          Discos: escolher as imagens dos drives 0-3 (A-D)",
    "  F10         Reset. Com disco no drive 0 da boot pelo disco; sem",
    "              nenhum disco aparece o logotipo da Prologica e a pergunta",
    "              BASIC (S ou N): S = BASIC residente (Cass?), N = monitor",
    "              da ROM (MONITOR VERSAO 1.1).",
    "  Shift+F10   Sai do emulador (os discos alterados sao gravados)",
    "  Alt+Enter   Tela cheia / janela (so na versao Windows)",
    "",
    "TECLADO DO CP-500",
    "  Esc         BREAK (no CP/M gera ESC)",
    "  PgUp/PgDn   CLEAR      (Home tambem)",
    "  Setas       setas do CP-500 (no CP/M a BIOS manda TAB, BS, ^ e `).",
    "              F5 > Setas no CP/M = WordStar: viram ^E ^X ^S ^D e",
    "              PgUp/PgDn = ^R/^C. Cuidado: no prompt ^C reinicia o CP/M.",
    "  Backspace   seta esquerda (apaga); no CP/M gera BS",
    "  Tab         seta direita (TAB)",
    "  CapsLock    SHIFT + 0 do CP-500 (alterna maiusculas/minusculas)",
    "  Ctrl+letra  no CP/M gera o caractere de controle (^C, ^S...). No",
    "              CP-500 real o CTRL e SHIFT + seta para baixo.",
    "",
    "  No BASIC e no DOS500 as letras saem maiusculas como no CP-500 real",
    "  (SHIFT + 0 alterna maiusculas/minusculas). Os simbolos que o",
    "  teclado do CP-500 nao tem ( \\ ] ^ _ { | } ~ ) sao entregues direto.",
    "  No CP/M o emulador entrega exatamente o caractere digitado no PC.",
    "  Em F5 > Teclado ha tres opcoes:",
    "    ABNT2 / US     o emulador traduz os simbolos do PC.",
    "    Personalizado  cada tecla do PC aperta uma tecla FISICA do CP-500.",
    "                   Em F5 > Editar teclado personalizado aparece o",
    "                   teclado: escolha a tecla com as setas, Enter, e",
    "                   aperte no PC a combinacao (sem e com o SHIFT).",
    "                   Del limpa, F3 volta ao padrao. Fica em CP5M80.KEY.",
    "  As teclas F1, F5, F9 e F10 sao sempre do emulador.",
    "",
    "DISCOS (F9)",
    "  Setas escolhem o drive.",
    "  Enter       abre a lista de arquivos para escolher a imagem",
    "  E           coloca um disco VAZIO (sem formatacao) no drive. Pede o",
    "              nome do arquivo; a extensao define o formato. Formate-o",
    "              no DOS500 (FORMAT) ou no CP/M antes de usar.",
    "  Del         ejeta o disco do drive",
    "  P           liga/desliga a protecao contra gravacao",
    "  X           exporta o disco do drive para outro formato (ex.: DMK",
    "              para HFE). Pede o nome; a extensao define o formato.",
    "  Esc         volta ao emulador SEM reset. Se trocou de sistema, use",
    "              F10 para dar boot.",
    "",
    "  Na lista de arquivos: setas/PgUp/PgDn/Home/End movem, Enter escolhe",
    "  (ou entra na pasta), Backspace sobe uma pasta, Tab mostra todos os",
    "  arquivos ou so imagens de disco, uma letra pula para o primeiro",
    "  arquivo com aquela inicial, Esc cancela.",
    "",
    "FORMATOS DE IMAGEM",
    "  DMK   David Keil (trilhas completas, FM/MFM, 1 ou 2 faces). E o",
    "        formato dos discos do SO-08 (CP/M). Lido e gravado.",
    "  DSK   JV1 ou JV3 (detectado automaticamente; DMK tambem e aceito",
    "        com extensao .DSK). Os discos do DOS500 sao JV3.",
    "  HFE   HxC (Gotek). FM ou MFM, lido e gravado.",
    "  IMD   ImageDisk. Qualquer geometria, lido e gravado.",
    "  JV1, JV3, IMG  setores crus (IMG: /G:setores,tamanho,primeiro).",
    "",
    "  As alteracoes sao gravadas no arquivo ao ejetar, ao trocar de disco",
    "  e ao sair do emulador.",
    "",
    "LEDS DOS DRIVES (F5 > LEDs dos drives: Ligados/Desligados)",
    "  Imitam a luz do drive: o da esquerda e dos drives 0 e 2, o da direita",
    "  dos drives 1 e 3. Acendem a cada acesso e apagam 0,5 s depois.",
    "  Windows: dois LEDs vermelhos no canto superior direito da tela.",
    "  DOS: dois quadradinhos vermelhos no canto da tela (em cima a direita",
    "  no modo de 64 colunas, embaixo a direita no de 80 colunas).",
    "  ESP32: LEDs reais na porta PS/2 do MOUSE, cada um com resistor de",
    "  330 ohms e catodo no pino 3 (GND): pino 1 (IO27) = drives 0 e 2,",
    "  pino 5 (IO26) = drives 1 e 3.",
    "",
    "CONFIGURACAO (F5)",
    "  Cor da tela   verde, branco, cinza ou ambar",
    "  (memoria fixa: 48K no BASIC e no DOS500, 64K no CP/M)",
    "  Som           saida do cassete (porta FF): placa de som no Windows,",
    "                alto-falante do PC no DOS",
    "  Cursor        troca o cursor do BASIC/DOS500 (64 colunas) por outro",
    "                caractere; Enter mostra o mapa (Del = original)",
    "  Teclado       ABNT2, US ou personalizado",
    "  A configuracao fica em CP5M80.CFG e o mapa em CP5M80.KEY.",
    "",
    "COMO O CP-500 M80 FUNCIONA (descoberto na ROM e na BIOS do SO-08)",
    "  - Base TRS-80 Model III: portas E0 (interrupcoes, relogio 30 Hz),",
    "    E4 (NMI do disco), EC, F0-F3 (controladora WD1793), F4 (drive),",
    "    FF (cassete/som). A leitura de setor usa wait states + NMI.",
    "  - IN (F4) com o mapa no registrador A: bit 5 = ROM e Model III;",
    "    bits 3-4 = so RAM (CP/M 64K); bits 6-7 = pagina de 1K do video",
    "    de 80 colunas em 3C00h (24 linhas de 128 bytes, 80 usados).",
    "  - Partida: o primeiro IN (F4) da ROM troca 3000-37FF pela parte alta",
    "    da 27128, um programa que se copia para 4300h (logotipo, BASIC S/N,",
    "    monitor) e da o boot pelo disco se houver.",
    "  - Gerador de caracteres 2732: dois jogos de 256 caracteres 8x8; o",
    "    segundo (com video inverso e semigraficos 2x2) e o das 80 colunas.",
    "",
    "ROMS: ja estao embutidas no executavel. Para usar outras, coloque na",
    "mesma pasta (tem prioridade): CP500M80_27128.bin e CP500M80_2732.bin",
    "",
    "LINHA DE COMANDO",
    "  CP5M80 [disco0 [disco1 [disco2 [disco3]]]] [/G:spt,tam,primeiro]",
    "  Ex.: CP5M80 SO08UTL1.DSK JOGOS01.DSK",
    "",
    "RetroLabs - JL Martins e Luiz Pacheco - 2026",
    NULL
};

static void help(void)
{
    int n = 0, top = 0, rows = 23, i;
    while (help_text[n]) n++;
    plat_menu_mode();
    for (;;) {
        bar(0, A_TITLE, "CP-500 M80  -  Ajuda");
        for (i = 0; i < rows; i++) {
            plat_fill(0, 1 + i, 80, 1, A_BACK, ' ');
            if (top + i < n) plat_put(1, 1 + i, A_BACK, help_text[top + i]);
        }
        {
            char b[80];
            sprintf(b, "Setas/PgUp/PgDn/Home/End = navegar   Esc ou F1 = voltar     linha %d/%d", top + 1, n);
            bar(24, A_HINT, b);
        }
        plat_present();
        switch (getkey()) {
        case K_UP: if (top > 0) top--; break;
        case K_DOWN: if (top < n - rows) top++; break;
        case K_PGUP: top -= rows; if (top < 0) top = 0; break;
        case K_PGDN: top += rows; if (top > n - rows) top = n - rows; if (top < 0) top = 0; break;
        case K_HOME: top = 0; break;
        case K_END: top = n - rows > 0 ? n - rows : 0; break;
        case K_ESC: case K_F1: return;
        }
    }
}

/* ------------------------------------------------------------ seletor de arquivos (estilo LIST) */
typedef struct { char name[64]; int isdir; long size; } fent_t;
static fent_t *fl;
static int fln, flcap;

static int is_image(const char *n)
{
    const char *e = strrchr(n, '.');
    static const char *ok[] = { "HFE", "IMD", "DSK", "DMK", "JV1", "JV3", "IMG", NULL };
    int i;
    if (!e) return 0;
    for (i = 0; ok[i]; i++) {
        const char *a = e + 1, *b = ok[i];
        while (*a && *b && toupper((unsigned char)*a) == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

static int fcmp(const void *a, const void *b)
{
    const fent_t *x = a, *y = b;
    static const int ord[3] = { 1, 0, 2 };     /* pastas, arquivos, unidades */
    if (x->isdir != y->isdir) return ord[x->isdir] - ord[y->isdir];
    if (x->isdir == 2) return strcmp(x->name, y->name);
    if (!strcmp(x->name, "..")) return -1;
    if (!strcmp(y->name, "..")) return 1;
    return strcasecmp(x->name, y->name);
}

static void fadd(const char *n, int isdir, long size)
{
    if (fln >= flcap) { flcap = flcap ? flcap * 2 : 256; fl = realloc(fl, flcap * sizeof(fent_t)); }
    strncpy(fl[fln].name, n, 63); fl[fln].name[63] = 0;
    fl[fln].isdir = isdir; fl[fln].size = size;
    fln++;
}

static void read_dir(int all)
{
    DIR *d = opendir(".");
    struct dirent *e;
    char drives[32];
    int i, nd;
    fln = 0;
    if (d) {
        while ((e = readdir(d)) != NULL) {
            struct stat st;
            if (!strcmp(e->d_name, ".")) continue;
            if (stat(e->d_name, &st)) continue;
            if (S_ISDIR(st.st_mode)) fadd(e->d_name, 1, 0);
            else if (all || is_image(e->d_name)) fadd(e->d_name, 0, (long)st.st_size);
        }
        closedir(d);
    }
    nd = plat_drives(drives);
    for (i = 0; i < nd; i++) { char b[8]; sprintf(b, "[%c:]", drives[i]); fadd(b, 2, 0); }
    qsort(fl, fln, sizeof(fent_t), fcmp);
}

/* devolve 1 e o caminho completo em out se escolheu */
static int file_picker(const char *title, char *out)
{
    int all = 0, sel = 0, top = 0, cols = 5, rows = 21, cw = 16;
    char cwd[260], olddir[260];
    if (!getcwd(olddir, sizeof olddir)) olddir[0] = 0;
    if (lastdir[0] && chdir(lastdir) != 0) lastdir[0] = 0;
    read_dir(all);
    for (;;) {
        int i, k;
        char b[400];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
        if (sel >= fln) sel = fln - 1;
        if (sel < 0) sel = 0;
        /* rolagem por colunas, como no LIST */
        while (sel < top) top -= rows;
        while (sel >= top + rows * cols) top += rows;
        if (top < 0) top = 0;
        plat_fill(0, 0, 80, 25, A_BACK, ' ');
        sprintf(b, "%s   %d de %d   Pasta: %.40s%s", title, fln ? sel + 1 : 0, fln, cwd, all ? "\\*.*" : "\\(imagens)");
        bar(0, A_TITLE, b);
        for (i = 0; i < rows * cols && top + i < fln; i++) {
            fent_t *f = &fl[top + i];
            int x = 1 + (i / rows) * cw, y = 2 + i % rows;
            char nm[32];
            if (f->isdir == 1) snprintf(nm, sizeof nm, "%-.10s <DIR>", f->name);
            else snprintf(nm, sizeof nm, "%-.15s", f->name);
            plat_fill(x - 1, y, cw, 1, top + i == sel ? A_SEL : A_BACK, ' ');
            plat_put(x, y, top + i == sel ? A_SEL : (f->isdir ? 0x1B : A_BACK), nm);
        }
        if (fln && !fl[sel].isdir) {
            sprintf(b, "%s   %ld bytes", fl[sel].name, fl[sel].size);
            plat_put(1, 23, 0x1E, b);
        }
        bar(24, A_HINT, "Enter=escolher  Bksp=sobe pasta  Tab=todos/imagens  Letra=pular  Esc=cancelar");
        plat_present();
        k = getkey();
        switch (k) {
        case K_UP: sel--; break;
        case K_DOWN: sel++; break;
        case K_LEFT: if (sel - rows >= 0) sel -= rows; else sel = 0; break;
        case K_RIGHT: if (sel + rows < fln) sel += rows; else sel = fln - 1; break;
        case K_PGUP: sel -= rows * cols; if (sel < 0) sel = 0; break;
        case K_PGDN: sel += rows * cols; if (sel >= fln) sel = fln - 1; break;
        case K_HOME: sel = 0; break;
        case K_END: sel = fln - 1; break;
        case K_TAB: all = !all; read_dir(all); sel = top = 0; break;
        case K_BKSP: if (chdir("..") == 0) { read_dir(all); sel = top = 0; } break;
        case K_ESC:
            if (getcwd(cwd, sizeof cwd)) strcpy(lastdir, cwd);
            if (olddir[0] && chdir(olddir)) {}
            return 0;
        case K_ENTER:
            if (!fln) break;
            if (fl[sel].isdir == 2) {
                char dr[4] = { fl[sel].name[1], ':', 0 };
                char cur[260];
                if (getcwd(cur, sizeof cur) && chdir(dr) != 0) { message("Unidade nao disponivel", A_ERR); break; }
                read_dir(all); sel = top = 0;
            } else if (fl[sel].isdir) {
                if (chdir(fl[sel].name) == 0) { read_dir(all); sel = top = 0; }
            } else {
                if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
                join(out, cwd, fl[sel].name);
                strcpy(lastdir, cwd);
                if (olddir[0] && chdir(olddir)) {}
                return 1;
            }
            break;
        default:
            if (k > 32 && k < 127) {
                int j;
                for (j = 1; j <= fln; j++) {
                    int idx = (sel + j) % fln;
                    if (toupper((unsigned char)fl[idx].name[0]) == toupper(k)) { sel = idx; break; }
                }
            }
        }
    }
}

/* ------------------------------------------------------------ discos (F9) */
static void disks_menu(void)
{
    int sel = 0;
    char err[300], path[300];
    plat_menu_mode();
    for (;;) {
        int i, k;
        plat_fill(0, 0, 80, 25, A_BACK, ' ');
        bar(0, A_TITLE, "CP-500 M80  -  Unidades de disco");
        putf(2, 2, 0x1E, "Drive          Arquivo                              Formato Trilhas Estado");
        for (i = 0; i < NDRIVES; i++) {
            disk_t *d = M.drv[i];
            int a = i == sel ? A_SEL : A_BACK;
            char nm[40];
            plat_fill(1, 4 + i * 2, 78, 1, a, ' ');
            if (d) {
                const char *p = d->path;
                if (strlen(p) > 36) p += strlen(p) - 36;
                snprintf(nm, sizeof nm, "%s", p);
                putf(2, 4 + i * 2, a, "%d  /  %c:     %-36s %-7s %3d     %s%s", i, 'A' + i, nm,
                     disk_fmtname(d->fmt), d->ntracks, d->wp ? "protegido" : "gravavel", d->dirty ? " *" : "");
            } else {
                putf(2, 4 + i * 2, a, "%d  /  %c:     (vazio)", i, 'A' + i);
            }
        }
        putf(2, 14, A_BACK, "O DOS500 chama os drives de 0 a 3; o CP/M (SO-08) de A: a D:.");
        putf(2, 15, A_BACK, "* = alteracoes ainda nao gravadas (sao gravadas ao ejetar ou sair).");
        bar(24, A_HINT, "Enter=escolher  E=disco vazio  X=exportar  Del=ejetar  P=protecao  Esc=voltar");
        plat_present();
        k = getkey();
        switch (k) {
        case K_UP: sel = (sel + NDRIVES - 1) % NDRIVES; break;
        case K_DOWN: sel = (sel + 1) % NDRIVES; break;
        case K_ESC: case K_F9: return;
        case K_ENTER: {
            char t[40];
            sprintf(t, "Drive %d (%c:)", sel, 'A' + sel);
            if (file_picker(t, path)) {
                if (dgt_mount(&M, sel, path, err)) message(err, A_ERR);
            }
            break; }
        case K_DEL:
            dgt_eject(&M, sel);
            break;
        case 'e': case 'E': {
            disk_t *d;
            char cwd[260];
            plat_fill(2, 18, 76, 4, A_BOX, ' ');
            putf(3, 18, A_BOX, "Disco vazio no drive %d. Nome do arquivo (extensao = formato:", sel);
            putf(3, 19, A_BOX, ".DMK .HFE .IMD .DSK .JV3). Formate no DOS500 ou no CP/M.");
            if (lastdir[0]) snprintf(path, sizeof path, "%s/NOVO%d.DMK", lastdir, sel);
            else if (getcwd(cwd, sizeof cwd)) snprintf(path, sizeof path, "%s/NOVO%d.DMK", cwd, sel);
            else sprintf(path, "NOVO%d.DMK", sel);
            if (input_line(3, 21, 74, path, sizeof path)) {
                d = disk_new(path, err);
                if (!d) message(err, A_ERR);
                else dgt_insert(&M, sel, d);
            }
            break; }
        case 'p': case 'P':
            if (M.drv[sel]) M.drv[sel]->wp = !M.drv[sel]->wp;
            break;
        case 'x': case 'X': {
            disk_t *d = M.drv[sel];
            char *dot;
            FILE *f;
            if (!d) break;
            plat_fill(2, 18, 76, 4, A_BOX, ' ');
            putf(3, 18, A_BOX, "Exportar o disco do drive %d. Nome do arquivo (extensao = formato:", sel);
            putf(3, 19, A_BOX, ".HFE .IMD .DSK .JV3 .IMG). O disco montado nao muda.");
            strcpy(path, d->path);
            dot = strrchr(path, '.');
            if (dot && !strchr(dot, '/') && !strchr(dot, '\\')) *dot = 0;
            strcat(path, d->fmt == FMT_HFE ? ".IMD" : ".HFE");
            if (!input_line(3, 21, 74, path, sizeof path)) break;
            f = fopen(path, "rb");
            if (f) {
                int k2;
                fclose(f);
                plat_fill(2, 18, 76, 4, A_BOX, ' ');
                putf(3, 19, A_BOX, "%.50s ja existe. Sobrescrever (S/N)?", path);
                plat_present();
                k2 = getkey();
                if (k2 != 's' && k2 != 'S') break;
            }
            if (disk_export(d, path, err)) message(err, A_ERR);
            else message(err[0] ? err : "Disco exportado.", A_TITLE);
            break; }
        }
    }
}

/* ------------------------------------------------------------ editor visual do teclado */
/* Teclado fisico do CP-500. sh=1: a tecla tem legenda com SHIFT */
typedef struct { const char *lbl; int row, bit, sh; int x, y, w; } dkey_t;
static dkey_t dk[80];
static int ndk;

static void dk_add(const char *lbl, int row, int bit, int sh, int *x, int y, int w)
{
    dk[ndk].lbl = lbl; dk[ndk].row = row; dk[ndk].bit = bit; dk[ndk].sh = sh;
    dk[ndk].x = *x; dk[ndk].y = y; dk[ndk].w = w;
    ndk++; *x += w + 1;
}

static void dk_build(void)
{
    /* teclado do CP-500 (placa PIL-03 da Prologica) */
    static const char *row1[] = { "1 !", "2 \"", "3 #", "4 $", "5 %", "6 &", "7 '", "8 (", "9 )", " 0 ", ": *", "- =" };
    static const int r1r[] = { 4,4,4,4,4,4,4,5,5,4,5,5 }, r1b[] = { 1,2,3,4,5,6,7,0,1,0,2,5 };
    static const char *let1 = "QWERTYUIOP", *let2 = "ASDFGHJKL", *let3 = "ZXCVBNM";
    static char lbl[26][4];
    int i, x, y;
    ndk = 0;
    for (i = 0; i < 26; i++) { lbl[i][0] = ' '; lbl[i][1] = (char)('A' + i); lbl[i][2] = ' '; lbl[i][3] = 0; }
#define LET(c) lbl[(c) - 'A'], ((c) - 'A' + 1) / 8, ((c) - 'A' + 1) % 8, 1
    x = 7; y = 4;
    for (i = 0; i < 12; i++) dk_add(row1[i], r1r[i], r1b[i], 1, &x, y, 3);
    dk_add("BREAK", 6, 2, 0, &x, y, 5);
    x = 1; y = 7;
    dk_add("CIMA", 6, 3, 0, &x, y, 5);
    for (i = 0; let1[i]; i++) dk_add(LET(let1[i]), &x, y, 3);
    dk_add(" @ ", 0, 0, 1, &x, y, 3);
    dk_add("ESQ", 6, 5, 0, &x, y, 3);
    dk_add("CLEAR", 6, 1, 0, &x, y, 5);
    x = 3; y = 10;
    dk_add("BAIXO", 6, 4, 0, &x, y, 5);
    for (i = 0; let2[i]; i++) dk_add(LET(let2[i]), &x, y, 3);
    dk_add("; +", 5, 3, 1, &x, y, 3);
    dk_add("DIR", 6, 6, 0, &x, y, 3);
    dk_add("ENTER", 6, 0, 0, &x, y, 5);
    x = 5; y = 13;
    dk_add("SHIFT", -1, 0, 0, &x, y, 5);
    for (i = 0; let3[i]; i++) dk_add(LET(let3[i]), &x, y, 3);
    dk_add(", <", 5, 4, 1, &x, y, 3);
    dk_add(". >", 5, 6, 1, &x, y, 3);
    dk_add("/ ?", 5, 7, 1, &x, y, 3);
    dk_add("SHIFT", -1, 0, 0, &x, y, 5);
    x = 14; y = 16;
    dk_add("          ESPACO          ", 6, 7, 0, &x, y, 26);
#undef LET
}

/* nome legivel de uma combinacao do PC */
static const char *pc_combo(int code, int sh)
{
    static char b[40];
    sprintf(b, "%s%s", sh ? "Shift+" : "", kbd_pc_name(code));
    return b;
}

/* espera uma tecla do PC para mapear; -1 = Esc (pular) */
static int capture(int *code, int *sh)
{
    for (;;) {
        int raw = 0, k = getkey_raw(&raw);
        if (k == K_ESC) return -1;
        if (k == K_F1 || k == K_F5 || k == K_F9 || k == K_F10) continue;
        if (raw == SC_LCTRL || raw == SC_RCTRL || raw == SC_ALT || raw == SC_RALT) continue;
        *code = raw; *sh = menu_shift;
        return 0;
    }
}

static void keymap_editor(void)
{
    int sel = 1;
    char msg[160] = "";
    if (cfg.layout != LAYOUT_CUSTOM) {
        /* comeca do mapa fisico equivalente ao ABNT2 */
        cfg.layout = LAYOUT_CUSTOM;
        kbd_default_map(LAYOUT_CUSTOM);
        kbd_load_map(keypath);
    }
    dk_build();
    for (;;) {
        int i, k, c, s, n;
        dkey_t *d = &dk[sel];
        plat_fill(0, 0, 80, 25, A_BACK, ' ');
        bar(0, A_TITLE, "Teclado personalizado do CP-500  (vale para o BASIC e o DOS500)");
        putf(1, 2, A_BACK, "Setas escolhem a tecla do CP-500. Enter grava as teclas do PC que vao aperta-la.");
        for (i = 0; i < ndk; i++) {
            int a = i == sel ? A_SEL : (dk[i].row < 0 ? 0x18 : 0x70 - 0x40);
            char t[20];
            snprintf(t, sizeof t, "%-*.*s", dk[i].w, dk[i].w, dk[i].lbl);
            plat_fill(dk[i].x, dk[i].y - 1, dk[i].w, 3, a, ' ');
            plat_put(dk[i].x, dk[i].y, a, t);
        }
        /* combinacoes do PC ligadas a tecla escolhida */
        plat_fill(1, 18, 78, 4, A_BACK, ' ');
        if (d->row < 0) putf(1, 18, 0x1E, "%s: fixa (Shift do PC).", d->lbl);
        else {
            char list[300] = "";
            n = 0;
            for (c = 0; c < 256; c++)
                for (s = 0; s < 2; s++) {
                    uint16_t e = keymap[c][s];
                    if (e == KM_NONE || (e & (KM_CHAR | KM_INJ))) continue;
                    if (((e >> 3) & 7) != d->row || (e & 7) != d->bit) continue;
                    if (strlen(list) < 230) {
                        strcat(list, n ? ", " : "");
                        strcat(list, pc_combo(c, s));
                        strcat(list, (e & KM_FSHIFT) ? " (com SHIFT)" : "");
                    }
                    n++;
                }
            putf(1, 18, 0x1E, "Tecla %s (matriz %d.%d)  <-  %s", d->lbl, d->row, d->bit, n ? "" : "nenhuma tecla do PC");
            if (n) { plat_put(3, 19, A_BACK, list); if (strlen(list) > 75) plat_put(3, 20, A_BACK, list + 75); }
        }
        putf(1, 22, 0x1B, "%s", msg);
        bar(24, A_HINT, "Enter=gravar teclas  Del=limpar tecla  F3=padrao (ABNT2)  Esc=gravar e sair");
        plat_present();
        k = getkey();
        msg[0] = 0;
        if (k == K_ESC) { kbd_save_map(keypath); return; }
        if (k == K_LEFT && sel > 0 && dk[sel - 1].y == d->y) sel--;
        else if (k == K_RIGHT && sel < ndk - 1 && dk[sel + 1].y == d->y) sel++;
        else if (k == K_UP || k == K_DOWN) {
            int ty = d->y + (k == K_UP ? -3 : 3), best = -1, bd = 999, cx = d->x + d->w / 2;
            for (i = 0; i < ndk; i++)
                if (dk[i].y == ty) {
                    int dd = abs(dk[i].x + dk[i].w / 2 - cx);
                    if (dd < bd) { bd = dd; best = i; }
                }
            if (best >= 0) sel = best;
        } else if (k == K_F3) {
            kbd_custom_from_abnt2();
            strcpy(msg, "Mapa padrao restaurado.");
        } else if (k == K_DEL && d->row >= 0) {
            for (c = 0; c < 256; c++)
                for (s = 0; s < 2; s++) {
                    uint16_t e = keymap[c][s];
                    if (e != KM_NONE && !(e & (KM_CHAR | KM_INJ)) && ((e >> 3) & 7) == d->row && (e & 7) == d->bit)
                        keymap[c][s] = KM_NONE;
                }
        } else if (k == K_ENTER && d->row >= 0) {
            int code, sh, pass;
            uint16_t base = (uint16_t)((d->row << 3) | d->bit);
            for (pass = 0; pass < (d->sh ? 2 : 1); pass++) {
                plat_fill(1, 18, 78, 5, 0x4F, ' ');
                if (!d->sh) putf(2, 19, 0x4F, "Aperte no PC a tecla (ou combinacao) para %s.   Esc = cancelar", d->lbl);
                else if (!pass) putf(2, 19, 0x4F, "Aperte no PC a combinacao para %s SEM o shift do CP-500.   Esc = pular", d->lbl);
                else putf(2, 19, 0x4F, "Agora a combinacao para SHIFT + %s.   Esc = pular", d->lbl);
                plat_present();
                if (capture(&code, &sh)) continue;
                keymap[code][sh] = (uint16_t)(base | (!d->sh ? KM_PASS : pass ? KM_FSHIFT : KM_NOSHIFT));
                snprintf(msg, sizeof msg, "%s -> %s%s", pc_combo(code, sh), pass ? "SHIFT + " : "", d->lbl);
            }
        }
    }
}

/* ------------------------------------------------------------ escolha do cursor */
static void vputs(uint8_t *v, int r, int c, const char *s)
{
    for (; *s && c < 64; s++, c++) v[r * 64 + c] = (uint8_t)*s;
}

/* mostra os caracteres do DGT (com a fonte do DGT) para escolher o cursor */
static void cursor_chooser(void)
{
    static uint8_t v[1024];
    int sel = cfg.cursor > 0 && cfg.cursor <= 0xBF ? cfg.cursor : 0x0D;   /* 00..BF */
    plat_emu_mode(font, cfg.color, 0);
    for (;;) {
        int i, k;
        char b[64];
        memset(v, ' ', sizeof v);
        vputs(v, 0, 1, "ESCOLHA O CURSOR   SETAS, ENTER=OK, DEL=ORIGINAL, ESC=VOLTA");
        for (i = 0; i < 192; i++) {
            int r = 1 + i / 16, c = 4 + (i % 16) * 3 + 2;
            v[r * 64 + c] = (uint8_t)i;
            if (i == sel) { v[r * 64 + c - 1] = '>'; v[r * 64 + c + 1] = '<'; }
        }
        sprintf(b, "CODIGO %02XH", sel);
        vputs(v, 13, 4, b);
        vputs(v, 15, 4, "READY");
        v[15 * 64 + 10] = (uint8_t)sel;
        plat_emu_draw(v, 0, 0);
        plat_present();
        k = getkey();
        if (k == K_LEFT && sel > 0) sel--;
        else if (k == K_RIGHT && sel < 191) sel++;
        else if (k == K_UP && sel >= 16) sel -= 16;
        else if (k == K_DOWN && sel < 176) sel += 16;
        else if (k == K_ENTER) { cfg.cursor = sel ? sel : 0x0D; break; }
        else if (k == K_DEL) { cfg.cursor = 0; break; }
        else if (k == K_ESC) break;
    }
    plat_menu_mode();
}

/* ------------------------------------------------------------ configuracao (F5) */
static void config_menu(void)
{
    int sel = 0, n = 8, oldlayout = cfg.layout;
    plat_menu_mode();
    for (;;) {
        int i, k;
        char v[10][64];
        sprintf(v[0], "%s", color_names[cfg.color]);
        sprintf(v[1], "%dK", cfg.memk);
        sprintf(v[2], "%s", cfg.sound ? "Ligado" : "Desligado");
        sprintf(v[3], "%s", cfg.wsarrows ? "WordStar (^E ^X ^S ^D, PgUp/PgDn = ^R ^C)" : "Originais do CP-500");
        if (cfg.cursor) sprintf(v[8], "%02XH  (Enter = escolher no mapa)", cfg.cursor);
        else sprintf(v[8], "original de cada sistema (Enter = escolher)");
        sprintf(v[4], "%s", cfg.layout == LAYOUT_US ? "US" : cfg.layout == LAYOUT_CUSTOM ? "Personalizado (teclas do CP-500)" : "ABNT2");
        v[5][0] = 0;
        sprintf(v[6], "(Enter)");
        sprintf(v[7], "(Enter)");
        sprintf(v[9], "%s", cfg.leds ? "Ligados" : "Desligados");
        {
            static const char *lbl[10] = { "Cor da tela", "Memoria (BASIC/DOS500)", "Som", "Setas no CP/M", "Teclado", "", "Editar teclado personalizado", "Voltar ao emulador", "Cursor (64 colunas)", "LEDs dos drives" };
            static const int ord[8] = { 0, 2, 3, 8, 4, 9, 6, 7 };   /* ordem na tela */
            plat_fill(0, 0, 80, 25, A_BACK, ' ');
            bar(0, A_TITLE, "CP-500 M80  -  Configuracao");
            for (i = 0; i < n; i++) {
                int a = i == sel ? A_SEL : A_BACK;
                plat_fill(2, 2 + i * 2, 70, 1, a, ' ');
                putf(3, 2 + i * 2, a, "%-29s %s", lbl[ord[i]], v[ord[i]]);
            }
        }
        putf(3, 20, A_BACK, "Memoria: 48K no BASIC e no DOS500, 64K no CP/M (como no CP-500 M80).");
        bar(24, A_HINT, "Setas=escolher  Esquerda/Direita/Enter=mudar  Esc=gravar e voltar");
        plat_present();
        k = getkey();
        if (k == K_ESC || k == K_F5 || (k == K_ENTER && sel == 7)) break;
        if (k == K_UP) sel = (sel + n - 1) % n;
        else if (k == K_DOWN) sel = (sel + 1) % n;
        else if (k == K_LEFT || k == K_RIGHT || k == K_ENTER) {
            int d = k == K_LEFT ? -1 : 1;
            static const int ord2[8] = { 0, 2, 3, 8, 4, 9, 6, 7 };
            switch (ord2[sel]) {
            case 8: if (k == K_ENTER) cursor_chooser(); break;
            case 0: cfg.color = (cfg.color + 4 + d) & 3; break;
            case 1: cfg.memk += d * 16; if (cfg.memk > 48) cfg.memk = 16; if (cfg.memk < 16) cfg.memk = 48; break;
            case 9: cfg.leds = !cfg.leds; break;
            case 2: cfg.sound = !cfg.sound; M.snd_mach = cfg.sound; break;
            case 3: cfg.wsarrows = !cfg.wsarrows; kbd_ws_arrows = cfg.wsarrows; break;
            case 4: cfg.layout = (cfg.layout + 3 + d) % 3; setup_keyboard(); build_font(&M, font, font_kana); build_font80(&M, font80); break;
            case 5: cfg.asciifont = !cfg.asciifont; build_font(&M, font, font_kana); build_font80(&M, font80); break;
            case 6: if (k == K_ENTER) { keymap_editor(); build_font(&M, font, font_kana); build_font80(&M, font80); } break;
            }
        }
    }
    if (oldlayout != cfg.layout) setup_keyboard();
    cfg_save();
}

/* ------------------------------------------------------------ principal */
static int audio_pull(signed char *dst, int n)
{
    static int primed;
    int k = 0, avail = (M.a_h - M.a_t) & (AUD_N - 1);
    /* mantem a latencia baixa: descarta excesso (> ~250 ms) */
    if (avail > M.snd_rate / 4) { M.a_t = (M.a_h - M.snd_rate / 12) & (AUD_N - 1); avail = M.snd_rate / 12; }
    /* depois de esvaziar, espera juntar ~40 ms antes de voltar a tocar (evita picotar) */
    if (!primed) { if (avail < M.snd_rate / 25) return 0; primed = 1; }
    if (avail < n) primed = 0;
    while (k < n && M.a_t != M.a_h) { dst[k++] = M.audio[M.a_t]; M.a_t = (M.a_t + 1) & (AUD_N - 1); }
    return k;
}

/* cursor: o mesmo caractere em todos os sistemas (BASIC/DIGDOS: posicao em 4020h;
 * DGPM: posicao em FB82h). So troca se ali estiver o cursor original do sistema. */
static void draw_screen(void)
{
    static uint8_t v[2048];
    int pos = -1, r;
    if (M.col80) {
        /* 80x24: cada linha ocupa 128 bytes (3 paginas de 1K com 8 linhas) */
        for (r = 0; r < 24; r++) memcpy(v + r * 80, M.vram + r * 128, 80);
        pos = M.mem[0xEBAC] | (M.mem[0xEBAD] << 8);              /* cursor da BIOS do SO-08 */
        pos = (pos >> 7) * 80 + (pos & 0x7F);
        if (pos >= 0 && pos < 1920 && v[pos] != 0x5F) pos = -1;
        if (cfg.cursor && pos >= 0 && pos < 1920) v[pos] = (uint8_t)cfg.cursor;
        plat_emu_draw(v, 1, 0);
        return;
    }
    memcpy(v, M.vram, 1024);
    if (M.map == 0) {
        pos = (M.mem[0x4020] | (M.mem[0x4021] << 8)) - 0x3C00;
        if (pos >= 0 && pos < 1024 && v[pos] != 0xB0 && v[pos] != 0x5F) pos = -1;
    }
    if (cfg.cursor && pos >= 0 && pos < 1024) v[pos] = (uint8_t)cfg.cursor;
    plat_emu_draw(v, 0, (M.port_ec & 0x04) != 0);
}

static void enter_emu(void)
{
    kbd_release_all(&M);
    cur_mode = M.col80;
    plat_emu_mode(cur_mode ? font80 : font, cfg.color, cur_mode);
    M.vram_dirty = 1;
}

int main(int argc, char **argv)
{
    char err[300];
    int i, nd = 0, quit = 0;
    unsigned long last, acc_ticks = 0;
    unsigned long frame_ticks = PLAT_HZ / 50;      /* 20 ms */
    unsigned long since_frame = 0;
    unsigned long snd_last = 0;

    /* pasta do executavel */
    strncpy(basedir, argv[0], sizeof basedir - 1);
    {
        char *p = strrchr(basedir, '/'), *q = strrchr(basedir, '\\');
        if (q > p) p = q;
        if (p) *p = 0; else strcpy(basedir, ".");
    }
    join(cfgpath, basedir, "CP5M80.CFG");
    join(keypath, basedir, "CP5M80.KEY");
    cfg_load();

#ifdef ARDUINO
    if (!Mp) Mp = (dgt_t *)heap_caps_calloc(1, sizeof(dgt_t), MALLOC_CAP_SPIRAM);
    printf("CP5M80: maquina %u bytes em %p\n", (unsigned)sizeof(dgt_t), (void*)Mp);
#endif
    dgt_init(&M);
#ifdef ARDUINO
    M.mem = esp_mem;
    font = (uint8_t (*)[24])heap_caps_calloc(256, 24, MALLOC_CAP_SPIRAM);
    font80 = (uint8_t (*)[24])heap_caps_calloc(256, 24, MALLOC_CAP_SPIRAM);
#endif
    if (load_roms(&M, basedir, err)) { printf("CP5M80: %s\n", err); return 1; }
    build_font(&M, font, font_kana); build_font80(&M, font80);
    apply_memory();
    setup_keyboard();

    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' || argv[i][0] == '-') && (argv[i][1] == 'G' || argv[i][1] == 'g') && argv[i][2] == ':') {
            sscanf(argv[i] + 3, "%d,%d,%d", &img_geom_spt, &img_geom_ssz, &img_geom_first);
        } else if (nd < NDRIVES) {
            if (dgt_mount(&M, nd, argv[i], err)) { printf("CP5M80: %s\n", err); return 1; }
            nd++;
        }
    }

    if (plat_init()) { printf("CP5M80: falha ao iniciar o video/teclado\n"); return 1; }
    dgt_reset(&M);
    M.snd_rate = plat_audio_rate();
    M.click_off = 0;                         /* o CP-500 nao tem clique de teclado */
    M.snd_mach = cfg.sound;
    kbd_ws_arrows = cfg.wsarrows;
    if (M.snd_rate) plat_audio_source(audio_pull);
    enter_emu();

    last = plat_ticks();
    while (!quit) {
        int r;
        kev_t ev;
        unsigned long now, dt;
        while ((r = plat_scancode()) >= 0) {
            if (!kbd_decode(r, &ev)) continue;
            if (ev.down) {
                int menu = 1;
                switch (ev.code) {
                case SC_F1: plat_sound(0); help(); break;
                case SC_F5: plat_sound(0); config_menu(); break;
                case SC_F9: plat_sound(0); disks_menu(); dgt_flush(&M); break;
                case SC_F10:
                    if (kbd_shift) { quit = 1; break; }
                    apply_memory();
                    dgt_reset(&M);
                    kbd_release_all(&M);
                    menu = 0;
                    break;
                default: menu = 0;
                }
                if (menu) { enter_emu(); last = plat_ticks(); acc_ticks = 0; continue; }
                if (ev.code == SC_F10) continue;
            }
            kbd_event(&M, &ev);
        }
        if (plat_quit_requested()) quit = 1;
        if (quit) break;

#ifdef ARDUINO
        {   static unsigned long t0; static unsigned long long c0;
            unsigned long tn = plat_ticks();
            if (tn - t0 > 5 * PLAT_HZ) { printf("Z80: %lu kHz\n", (unsigned long)((M.cpu.cyc - c0) * PLAT_HZ / (tn - t0) / 1000)); t0 = tn; c0 = M.cpu.cyc; }
        }
#endif
        now = plat_ticks();
        dt = now - last;
        last = now;
        if (dt > frame_ticks) dt = frame_ticks;   /* atrasado: nao acumula; quadro e teclado seguem a 50 Hz */
        acc_ticks += dt;
        since_frame += dt;
        if (acc_ticks >= PLAT_HZ / 1000) {
            long cyc = (long)((double)acc_ticks * M.clock_hz / PLAT_HZ);
            acc_ticks = 0;
            dgt_run(&M, cyc);
        } else plat_idle();

        if (since_frame >= frame_ticks) {
            since_frame = 0;
            kbd_tick(&M);
            plat_leds(cfg.leds ? M.cpu.cyc < M.led_until[0] : -1, cfg.leds ? M.cpu.cyc < M.led_until[1] : -1);
            if (M.col80 != cur_mode || ((M.port_ec >> 3) & 1) != font_kana) {
                font_kana = (M.port_ec >> 3) & 1;
                build_font(&M, font, font_kana);
                enter_emu();
            }
            if (M.vram_dirty) { M.vram_dirty = 0; draw_screen(); }
            if (!M.snd_rate) {
                /* alto-falante do PC: frequencia pelo meio-periodo medio do quadro */
                long hz = 0;
                if (cfg.sound && M.snd_hp_n >= 2 && M.cpu.cyc >= M.mute_until) hz = (long)((double)M.clock_hz * M.snd_hp_n / (2.0 * M.snd_hp_sum));
                if (M.cpu.cyc - M.snd_last_cyc > (unsigned long long)M.clock_hz / 25) hz = 0;
                M.snd_hp_sum = 0; M.snd_hp_n = 0;
                (void)snd_last;
                plat_sound(hz >= 30 && hz <= 12000 ? (int)hz : 0);
            }
            plat_present();
        }
    }
    plat_sound(0);
    dgt_flush(&M);
    for (i = 0; i < NDRIVES; i++) dgt_eject(&M, i);
    cfg_save();
    plat_shutdown();
    printf("CP-500 M80 emulador " VERSAO " - ate logo!\n");
    return 0;
}
