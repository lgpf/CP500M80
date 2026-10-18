/* disk.h - imagens de disquete para o emulador DGT-100 */
#ifndef DISK_H
#define DISK_H
#include <stdint.h>

#define MAXTRK 86
#define MAXSEC 40

enum { FMT_NONE, FMT_JV1, FMT_JV3, FMT_IMG, FMT_IMD, FMT_HFE, FMT_DMK };

typedef struct {
    uint8_t c, h, r, n;     /* ID: trilha, lado, setor, codigo de tamanho */
    uint8_t dam;            /* 0xFB normal, 0xF8 deletado */
    uint16_t size;
    uint8_t *data;
} sector_t;

typedef struct {
    int nsec;
    int mfm;                /* 1 = dupla densidade */
    sector_t s[MAXSEC];
} track_t;

typedef struct {
    int fmt;
    int ntracks, nsides;
    int wp, dirty;
    track_t t[MAXTRK][2];
    char path[260];
    /* HFE: cabecalho original e tamanho de cada trilha (para regravar) */
    uint8_t hfe_hdr[512];
    int hfe_len[MAXTRK];
    /* IMD: comentario original */
    char *imd_comment; int imd_comment_len;
    uint8_t imd_mode[MAXTRK][2];
    int dmk_tl, dmk_flags;
} disk_t;

/* geometria para IMG cru (0 = automatico) */
extern int img_geom_spt, img_geom_ssz, img_geom_first, img_geom_sides;

disk_t *disk_open(const char *path, char *err);
disk_t *disk_new(const char *path, char *err);  /* disco vazio (sem formatar) */
int  disk_save(disk_t *d);           /* grava se dirty; 0 = ok */
void disk_close(disk_t *d);
int  disk_export(disk_t *d, const char *path, char *err);  /* converte p/ outro formato */
sector_t *disk_find(disk_t *d, int trk, int side, int sec);
void disk_format_track(disk_t *d, int trk, int side, const uint8_t *buf, int len, int mfm);
const char *disk_fmtname(int f);

#endif
