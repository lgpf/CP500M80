/* plat.h - camada de plataforma (MS-DOS real ou Linux para testes) */
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>

int  plat_init(void);
void plat_shutdown(void);
unsigned long plat_ticks(void);          /* tiques de 1/1193182 s */
#define PLAT_HZ 1193182UL
int  plat_scancode(void);                /* proximo byte do teclado ou -1 */
void plat_idle(void);
int  plat_quit_requested(void);          /* janela fechada (Windows) */

/* modo emulador. mode 0: 64x16 com glifos 8x24; mode 1: 80x24 com glifos 8x16
 * (linhas 0-15 de cada glifo) */
void plat_emu_mode(uint8_t font[256][24], int color, int mode);
void plat_emu_draw(const uint8_t *cells, int mode, int wide);
/* modo menu: texto 80x25 padrao */
void plat_menu_mode(void);
void plat_put(int x, int y, int attr, const char *s);
void plat_fill(int x, int y, int w, int h, int attr, int ch);
void plat_present(void);

void plat_sound(int hz);                 /* 0 = silencio (alto-falante do PC) */
int  plat_audio_rate(void);              /* >0: a plataforma toca PCM nessa taxa */
void plat_audio_source(int (*pull)(signed char *dst, int n));

/* LEDs dos drives (0/2 e 1/3): Windows desenha no canto da tela, ESP32 em pinos */
void plat_leds(int a, int b);

/* diretorios/arquivos */
int  plat_drives(char *list);            /* letras validas, ex "ACD" */
#endif
