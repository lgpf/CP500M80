#ifndef COMMON_H
#define COMMON_H
#include "dgt.h"
int load_roms(dgt_t *m, const char *dir, char *err);
void build_font(dgt_t *m, uint8_t font[256][24], int kana);
void build_font80(dgt_t *m, uint8_t font[256][24]);
int glyph_of(uint8_t v);
#endif
