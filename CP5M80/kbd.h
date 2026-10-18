/* kbd.h - teclado do PC -> matriz do DGT-100 */
#ifndef KBD_H
#define KBD_H
#include <stdint.h>
#include "dgt.h"

/* entrada da tabela: bits 0-2 = bit, 3-5 = linha da matriz */
#define KM_NONE     0xFFFF
#define KM_FSHIFT   0x0040   /* forca SHIFT ligado */
#define KM_NOSHIFT  0x0080   /* forca SHIFT desligado */
#define KM_FCTRL    0x0100   /* forca CTRL */
#define KM_PASS     0x0200   /* repassa SHIFT/CTRL fisicos (letras) */
#define KM_INJ      0x4000   /* injeta o codigo (byte baixo) na rotina de teclado */
#define KM_CHAR     0x8000   /* caractere: traduzido conforme o sistema (ROM/DIGDOS ou DGPM) */
#define CH_CCED     0x87     /* c cedilha (codigos internos) */
#define CH_CCED_UP  0x80

enum { LAYOUT_ABNT2, LAYOUT_US, LAYOUT_CUSTOM };   /* CUSTOM: teclas fisicas do DGT, base ABNT2 */

/* codigos de tecla: scancode set 1; com prefixo E0 soma 0x80 */
#define SC_E0(x) ((x) | 0x80)
#define SC_ESC   0x01
#define SC_BKSP  0x0E
#define SC_TAB   0x0F
#define SC_ENTER 0x1C
#define SC_LCTRL 0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_ALT   0x38
#define SC_SPACE 0x39
#define SC_CAPS  0x3A
#define SC_F1    0x3B
#define SC_F2    0x3C
#define SC_F3    0x3D
#define SC_F4    0x3E
#define SC_F5    0x3F
#define SC_F9    0x43
#define SC_F10   0x44
#define SC_UP    SC_E0(0x48)
#define SC_DOWN  SC_E0(0x50)
#define SC_LEFT  SC_E0(0x4B)
#define SC_RIGHT SC_E0(0x4D)
#define SC_HOME  SC_E0(0x47)
#define SC_END   SC_E0(0x4F)
#define SC_PGUP  SC_E0(0x49)
#define SC_PGDN  SC_E0(0x51)
#define SC_INS   SC_E0(0x52)
#define SC_DEL   SC_E0(0x53)
#define SC_KENTER SC_E0(0x1C)
#define SC_RCTRL SC_E0(0x1D)
#define SC_RALT  SC_E0(0x38)

/* evento decodificado */
typedef struct { int code; int down; } kev_t;

extern int kbd_layout;
extern uint16_t keymap[256][2];      /* [codigo][shift] */
extern int kbd_shift, kbd_ctrl, kbd_alt;
extern int kbd_ws_arrows;

void kbd_default_map(int layout);
void kbd_custom_from_abnt2(void);              /* mapa fisico inicial do modo personalizado */
int  kbd_load_map(const char *path);
int  kbd_save_map(const char *path);
int  kbd_decode(int raw, kev_t *ev);           /* bytes crus -> evento; 1 se completo */
void kbd_event(dgt_t *m, kev_t *ev);           /* aplica no DGT */
void kbd_release_all(dgt_t *m);
void kbd_tick(dgt_t *m);                       /* chamar a cada quadro (20 ms) */
int  kbd_char(int code, int shift);            /* caractere do layout (menus) */
const char *kbd_dgt_name(int row, int bit);
const char *kbd_entry_name(uint16_t e);
uint16_t kbd_resolve(uint16_t e, int cpm);    /* KM_CHAR -> tecla da matriz */
const char *kbd_pc_name(int code);
#endif
