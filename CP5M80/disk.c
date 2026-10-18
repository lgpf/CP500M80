/* disk.c - leitura/gravacao de imagens DSK(JV1/JV3), IMG, IMD e HFE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disk.h"
#ifdef ARDUINO
#include "esp_heap_caps.h"
static void *ps_m(size_t n){void*p=heap_caps_malloc(n,MALLOC_CAP_SPIRAM);return p?p:malloc(n);}
static void *ps_c(size_t a,size_t b){void*p=heap_caps_calloc(a,b,MALLOC_CAP_SPIRAM);return p?p:calloc(a,b);}
#define malloc ps_m
#define calloc ps_c
/* SD: ler/gravar direto na PSRAM e lentissimo; passa por um buffer interno */
static uint8_t esp_bounce[2048];
static size_t esp_fread(void *p, size_t sz, size_t n, FILE *f)
{
    size_t tot = sz * n, done = 0;
    while (done < tot) {
        size_t c = tot - done > sizeof esp_bounce ? sizeof esp_bounce : tot - done;
        size_t r = fread(esp_bounce, 1, c, f);
        memcpy((uint8_t *)p + done, esp_bounce, r);
        done += r;
        if (r < c) break;
    }
    return sz ? done / sz : 0;
}
static size_t esp_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    size_t tot = sz * n, done = 0;
    while (done < tot) {
        size_t c = tot - done > sizeof esp_bounce ? sizeof esp_bounce : tot - done;
        size_t r;
        memcpy(esp_bounce, (const uint8_t *)p + done, c);
        r = fwrite(esp_bounce, 1, c, f);
        done += r;
        if (r < c) break;
    }
    return sz ? done / sz : 0;
}
static FILE *esp_fopen(const char *n, const char *m)
{
    FILE *f = fopen(n, m);
    if (f) setvbuf(f, NULL, _IOFBF, 4096);
    return f;
}
#define fread esp_fread
#define fwrite esp_fwrite
#define fopen esp_fopen
#endif

int img_geom_spt = 0, img_geom_ssz = 0, img_geom_first = -1, img_geom_sides = 0;

const char *disk_fmtname(int f)
{
    static const char *n[] = { "-", "JV1", "JV3", "IMG", "IMD", "HFE", "DMK" };
    return n[f];
}

static void add_sector(track_t *t, int c, int h, int r, int n, int dam, const uint8_t *data)
{
    sector_t *s;
    if (t->nsec >= MAXSEC) return;
    s = &t->s[t->nsec++];
    s->c = c; s->h = h; s->r = r; s->n = n & 3; s->dam = dam;
    s->size = 128 << (n & 3);
    s->data = malloc(s->size);
    if (data) memcpy(s->data, data, s->size); else memset(s->data, 0xE5, s->size);
}

static void free_track(track_t *t)
{
    int i;
    for (i = 0; i < t->nsec; i++) free(t->s[i].data);
    t->nsec = 0;
}

sector_t *disk_find(disk_t *d, int trk, int side, int sec)
{
    int i;
    track_t *t;
    if (!d || trk < 0 || trk >= MAXTRK || side > 1) return NULL;
    t = &d->t[trk][side];
    for (i = 0; i < t->nsec; i++)
        if (t->s[i].r == sec) return &t->s[i];
    return NULL;
}

static long fsize(FILE *f)
{
    long n;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    return n;
}

static const char *ext(const char *p)
{
    const char *e = strrchr(p, '.');
    return e ? e + 1 : "";
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) { if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

/* ---------------- CRC CCITT ---------------- */
static uint16_t crc16(uint16_t crc, const uint8_t *p, int n)
{
    int i;
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

/* ---------------- JV1 ---------------- */
static int load_jv1(disk_t *d, uint8_t *buf, long len)
{
    int trk, s, nt = len / 2560;
    if (len % 2560 && len % 2560 != 0) nt++;
    d->nsides = 1;
    d->ntracks = nt;
    for (trk = 0; trk < nt && trk < MAXTRK; trk++)
        for (s = 0; s < 10; s++) {
            long off = (long)(trk * 10 + s) * 256;
            uint8_t tmp[256];
            memset(tmp, 0xE5, 256);
            if (off < len) memcpy(tmp, buf + off, (len - off) >= 256 ? 256 : len - off);
            /* diretorio do TRSDOS (trilha 17) usa marca deletada */
            add_sector(&d->t[trk][0], trk, 0, s, 1, trk == 17 ? 0xF8 : 0xFB, tmp);
        }
    return 0;
}

static int save_jv1(disk_t *d, FILE *f)
{
    int trk, s;
    uint8_t blank[256];
    memset(blank, 0xE5, 256);
    for (trk = 0; trk < d->ntracks; trk++)
        for (s = 0; s < 10; s++) {
            sector_t *x = disk_find(d, trk, 0, s);
            fwrite(x && x->size == 256 ? x->data : blank, 1, 256, f);
        }
    return 0;
}

/* ---------------- JV3 ---------------- */
#define JV3_ENTRIES 2901
static int is_jv3(uint8_t *buf, long len)
{
    int i, ok = 0;
    if (len < JV3_ENTRIES * 3 + 1) return 0;
    for (i = 0; i < JV3_ENTRIES; i++) {
        uint8_t t = buf[i * 3], s = buf[i * 3 + 1];
        if (t == 0xFF && s == 0xFF) break;
        if (t > 90) return 0;
        ok++;
    }
    return ok > 0 && (buf[JV3_ENTRIES * 3] == 0xFF || buf[JV3_ENTRIES * 3] == 0x00);
}

static int load_jv3(disk_t *d, uint8_t *buf, long len)
{
    long off = JV3_ENTRIES * 3 + 1;
    int i;
    d->ntracks = 0; d->nsides = 1;
    d->wp = buf[JV3_ENTRIES * 3] == 0;
    for (i = 0; i < JV3_ENTRIES; i++) {
        uint8_t t = buf[i * 3], s = buf[i * 3 + 1], fl = buf[i * 3 + 2];
        int side = (fl >> 4) & 1, dd = (fl >> 7) & 1, szc = fl & 3, size, n, dam;
        static const int sz[4] = { 256, 128, 1024, 512 };
        if (t == 0xFF) break;
        size = sz[szc];
        n = size == 128 ? 0 : size == 256 ? 1 : size == 512 ? 2 : 3;
        dam = dd ? (((fl >> 5) & 3) == 1 ? 0xF8 : 0xFB) : ((((fl >> 5) & 3) != 0) ? 0xF8 : 0xFB);
        if (off + size > len) break;
        if (t < MAXTRK) {
            add_sector(&d->t[t][side], t, side, s, n, dam, buf + off);
            d->t[t][side].mfm = dd;
            if (t + 1 > d->ntracks) d->ntracks = t + 1;
            if (side) d->nsides = 2;
        }
        off += size;
    }
    return 0;
}

static int save_jv3(disk_t *d, FILE *f)
{
    uint8_t hdr[JV3_ENTRIES * 3 + 1];
    int t, h, i, k = 0;
    memset(hdr, 0xFF, sizeof hdr);
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < d->nsides; h++)
            for (i = 0; i < d->t[t][h].nsec && k < JV3_ENTRIES; i++, k++) {
                sector_t *s = &d->t[t][h].s[i];
                int mfm = d->t[t][h].mfm, szc;
                uint8_t fl = 0;
                szc = s->size == 256 ? 0 : s->size == 128 ? 1 : s->size == 1024 ? 2 : 3;
                fl = szc | (h << 4) | (mfm << 7);
                if (s->dam == 0xF8) fl |= mfm ? 0x20 : 0x60;
                hdr[k * 3] = t; hdr[k * 3 + 1] = s->r; hdr[k * 3 + 2] = fl;
            }
    hdr[JV3_ENTRIES * 3] = d->wp ? 0 : 0xFF;
    fwrite(hdr, 1, sizeof hdr, f);
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < d->nsides; h++)
            for (i = 0; i < d->t[t][h].nsec; i++)
                fwrite(d->t[t][h].s[i].data, 1, d->t[t][h].s[i].size, f);
    return 0;
}

/* ---------------- IMG cru ---------------- */
static void img_geom(long len, int *spt, int *ssz, int *first, int *sides)
{
    *spt = 18; *ssz = 256; *first = 0; *sides = 1;
    if (len == 40L * 10 * 256 || len == 35L * 10 * 256) { *spt = 10; *ssz = 256; }
    else if (len == 40L * 18 * 256 * 2 || len == 80L * 18 * 256 * 2) { *sides = 2; }
    else if (len == 40L * 10 * 512) { *spt = 10; *ssz = 512; *first = 1; }
    if (img_geom_spt) *spt = img_geom_spt;
    if (img_geom_ssz) *ssz = img_geom_ssz;
    if (img_geom_first >= 0) *first = img_geom_first;
    if (img_geom_sides) *sides = img_geom_sides;
}

static int load_img(disk_t *d, uint8_t *buf, long len)
{
    int spt, ssz, first, sides, t, h, s, n;
    long off = 0, trklen;
    img_geom(len, &spt, &ssz, &first, &sides);
    n = ssz == 128 ? 0 : ssz == 256 ? 1 : ssz == 512 ? 2 : 3;
    trklen = (long)spt * ssz;
    d->nsides = sides;
    d->ntracks = (int)((len + trklen * sides - 1) / (trklen * sides));
    if (d->ntracks > MAXTRK) d->ntracks = MAXTRK;
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < sides; h++) {
            d->t[t][h].mfm = !(spt == 10 && ssz == 256);
            for (s = 0; s < spt; s++) {
                uint8_t tmp[1024];
                memset(tmp, 0xE5, ssz);
                if (off < len) memcpy(tmp, buf + off, len - off >= ssz ? ssz : len - off);
                /* DGPM (CP/M do DGT): 10x512, trilha 0 numera de 0 a 9, as demais de 1 a 10 */
                add_sector(&d->t[t][h], t, h, (spt == 10 && ssz == 512 && t == 0 && img_geom_first < 0) ? s : first + s, n,
                           /* DIGDOS dupla densidade: diretorio na trilha 20 com marca deletada */
                           (spt == 18 && ssz == 256 && t == 20) ? 0xF8 : 0xFB, tmp);
                off += ssz;
            }
        }
    return 0;
}

static int save_img(disk_t *d, FILE *f)
{
    int t, h, s;
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < d->nsides; h++) {
            /* ordena por numero de setor */
            track_t *tr = &d->t[t][h];
            int lo = 255, hi = -1, i;
            for (i = 0; i < tr->nsec; i++) { if (tr->s[i].r < lo) lo = tr->s[i].r; if (tr->s[i].r > hi) hi = tr->s[i].r; }
            for (s = lo; s <= hi; s++) {
                sector_t *x = disk_find(d, t, h, s);
                if (x) fwrite(x->data, 1, x->size, f);
            }
        }
    return 0;
}

/* ---------------- IMD ---------------- */
static int load_imd(disk_t *d, uint8_t *buf, long len)
{
    long p = 0;
    while (p < len && buf[p] != 0x1A) p++;
    d->imd_comment_len = (int)p;
    d->imd_comment = malloc(p + 1);
    memcpy(d->imd_comment, buf, p);
    p++;
    d->ntracks = 0; d->nsides = 1;
    while (p + 5 <= len) {
        int mode = buf[p], cyl = buf[p + 1], head = buf[p + 2], nsec = buf[p + 3], szc = buf[p + 4];
        int side = head & 1, i, size = 128 << szc;
        uint8_t *smap, *cmap = NULL, *hmap = NULL;
        p += 5;
        smap = buf + p; p += nsec;
        if (head & 0x80) { cmap = buf + p; p += nsec; }
        if (head & 0x40) { hmap = buf + p; p += nsec; }
        if (cyl >= MAXTRK) break;
        d->t[cyl][side].mfm = mode >= 3;
        d->imd_mode[cyl][side] = mode;
        if (cyl + 1 > d->ntracks) d->ntracks = cyl + 1;
        if (side) d->nsides = 2;
        for (i = 0; i < nsec; i++) {
            uint8_t typ = buf[p++], tmp[8192];
            int dam = 0xFB;
            if (size > 8192) return -1;
            if (typ == 0) memset(tmp, 0xE5, size);
            else if ((typ - 1) & 1) { memset(tmp, buf[p], size); p++; }
            else { memcpy(tmp, buf + p, size); p += size; }
            if (typ == 3 || typ == 4 || typ == 7 || typ == 8) dam = 0xF8;
            add_sector(&d->t[cyl][side], cmap ? cmap[i] : cyl, hmap ? hmap[i] : side, smap[i], szc, dam, tmp);
        }
    }
    return 0;
}

static int save_imd(disk_t *d, FILE *f)
{
    int t, h, i;
    if (d->imd_comment) fwrite(d->imd_comment, 1, d->imd_comment_len, f);
    else fputs("IMD 1.18: DGT-100 emulador\r\n", f);
    fputc(0x1A, f);
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < d->nsides; h++) {
            track_t *tr = &d->t[t][h];
            int szc = tr->nsec ? tr->s[0].n : 1, mode = d->imd_mode[t][h];
            int needc = 0, needh = 0;
            if (!tr->nsec) continue;
            if (!mode) mode = tr->mfm ? 5 : 2;
            for (i = 0; i < tr->nsec; i++) { if (tr->s[i].c != t) needc = 1; if (tr->s[i].h != h) needh = 1; }
            fputc(mode, f); fputc(t, f); fputc(h | (needc ? 0x80 : 0) | (needh ? 0x40 : 0), f);
            fputc(tr->nsec, f); fputc(szc, f);
            for (i = 0; i < tr->nsec; i++) fputc(tr->s[i].r, f);
            if (needc) for (i = 0; i < tr->nsec; i++) fputc(tr->s[i].c, f);
            if (needh) for (i = 0; i < tr->nsec; i++) fputc(tr->s[i].h, f);
            for (i = 0; i < tr->nsec; i++) {
                sector_t *s = &tr->s[i];
                int j, same = 1, del = s->dam == 0xF8;
                for (j = 1; j < s->size; j++) if (s->data[j] != s->data[0]) { same = 0; break; }
                if (same) { fputc(del ? 4 : 2, f); fputc(s->data[0], f); }
                else { fputc(del ? 3 : 1, f); fwrite(s->data, 1, s->size, f); }
            }
        }
    return 0;
}

/* ---------------- HFE ---------------- */
/* fluxo de bits: HFE guarda os bits LSB primeiro; cada bit = uma celula */
typedef struct { uint8_t *b; long n; } bits_t;

static int getbit(bits_t *bs, long i) { return (bs->b[i >> 3] >> (i & 7)) & 1; }

static uint16_t rd16cells(bits_t *bs, long p)
{
    uint16_t v = 0; int i;
    for (i = 0; i < 16; i++) v = (v << 1) | getbit(bs, p + i);
    return v;
}

static uint8_t rdbyte(bits_t *bs, long p) /* le 8 bits de dados (celulas impares) */
{
    uint8_t v = 0; int i;
    for (i = 0; i < 8; i++) v = (v << 1) | getbit(bs, p + 2 * i + 1);
    return v;
}

static void decode_mfm(track_t *t, bits_t *bs)
{
    long p, end = bs->n - 64;
    int have_id = 0;
    uint8_t id[6];
    for (p = 0; p < end; p++) {
        if (rd16cells(bs, p) != 0x4489) continue;
        if (rd16cells(bs, p + 16) != 0x4489 || rd16cells(bs, p + 32) != 0x4489) continue;
        {
            long q = p + 48;
            uint8_t m = rdbyte(bs, q);
            if (m == 0xFE) {
                int i;
                for (i = 0; i < 6; i++) id[i] = rdbyte(bs, q + 16 + 16 * i);
                have_id = 1;
                p = q + 16 * 7 - 1;
            } else if ((m == 0xFB || m == 0xF8) && have_id) {
                int n = id[3] & 3, size = 128 << n, i;
                uint8_t tmp[1024];
                if (q + 16 + 16L * size > bs->n) break;
                for (i = 0; i < size; i++) tmp[i] = rdbyte(bs, q + 16 + 16 * i);
                add_sector(t, id[0], id[1], id[2], n, m, tmp);
                have_id = 0;
                p = q + 16L * (size + 1) - 1;
            }
        }
    }
    t->mfm = 1;
}

/* FM: marca de endereco com relogio 0xC7 */
static uint16_t fm_mark(uint8_t data, uint8_t clk)
{
    uint16_t v = 0; int i;
    for (i = 7; i >= 0; i--) v = (v << 2) | (((clk >> i) & 1) << 1) | ((data >> i) & 1);
    return v;
}

static void decode_fm(track_t *t, bits_t *bs)
{
    long p, end = bs->n - 64;
    int have_id = 0, i;
    uint8_t id[6];
    uint16_t mi = fm_mark(0xFE, 0xC7), md = fm_mark(0xFB, 0xC7), mdd = fm_mark(0xF8, 0xC7);
    uint16_t md2 = fm_mark(0xFA, 0xC7), md3 = fm_mark(0xF9, 0xC7);
    for (p = 0; p < end; p++) {
        uint16_t w = rd16cells(bs, p);
        if (w == mi) {
            for (i = 0; i < 6; i++) id[i] = rdbyte(bs, p + 16 + 16 * i);
            have_id = 1; p += 16 * 7 - 1;
        } else if ((w == md || w == mdd || w == md2 || w == md3) && have_id) {
            int n = id[3] & 3, size = 128 << n;
            uint8_t tmp[1024];
            if (p + 16 + 16L * size > bs->n) break;
            for (i = 0; i < size; i++) tmp[i] = rdbyte(bs, p + 16 + 16 * i);
            add_sector(t, id[0], id[1], id[2], n, (w == md || w == md2) ? 0xFB : 0xF8, tmp);
            have_id = 0; p += 16L * (size + 1) - 1;
        }
    }
    t->mfm = 0;
}

static int load_hfe(disk_t *d, uint8_t *buf, long len)
{
    int nt = buf[9], ns = buf[10], t, h;
    long lut = (long)(buf[18] | (buf[19] << 8)) * 512;
    memcpy(d->hfe_hdr, buf, 512);
    d->ntracks = nt > MAXTRK ? MAXTRK : nt;
    d->nsides = ns > 2 ? 2 : ns;
    for (t = 0; t < d->ntracks; t++) {
        long off = (long)(buf[lut + 4 * t] | (buf[lut + 4 * t + 1] << 8)) * 512;
        int tl = buf[lut + 4 * t + 2] | (buf[lut + 4 * t + 3] << 8);
        d->hfe_len[t] = tl;
        for (h = 0; h < d->nsides; h++) {
            bits_t bs;
            long i, k = 0;
            bs.n = (long)(tl / 2) * 8;
            bs.b = malloc(tl / 2 + 16);
            for (i = 0; i < tl; i += 512) {
                long n = (tl - i) / 2; if (n > 256) n = 256;
                if (off + i + h * 256 + n <= len) memcpy(bs.b + k, buf + off + i + h * 256, n);
                k += n;
            }
            decode_mfm(&d->t[t][h], &bs);
            if (d->t[t][h].nsec == 0) decode_fm(&d->t[t][h], &bs);
            free(bs.b);
        }
    }
    return 0;
}

/* codificador para regravar trilhas HFE */
typedef struct { uint8_t *b; long n, max; int last; } enc_t;

static void putcell(enc_t *e, int v)
{
    if (e->n >= e->max) return;
    if (v) e->b[e->n >> 3] |= 1 << (e->n & 7);
    e->n++;
}

static void mfm_byte(enc_t *e, uint8_t v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        int b = (v >> i) & 1;
        putcell(e, (!b && !e->last) ? 1 : 0);
        putcell(e, b);
        e->last = b;
    }
}

static void mfm_sync(enc_t *e) /* A1 com relogio faltando = 0x4489 */
{
    int i;
    for (i = 15; i >= 0; i--) putcell(e, (0x4489 >> i) & 1);
    e->last = 1;
}

static void fm_byte(enc_t *e, uint8_t v, uint8_t clk)
{
    int i;
    for (i = 7; i >= 0; i--) { putcell(e, (clk >> i) & 1); putcell(e, (v >> i) & 1); }
}

static void encode_track(track_t *t, uint8_t *out, long cells)
{
    enc_t e;
    int i, j;
    e.b = out; e.n = 0; e.max = cells; e.last = 0;
    memset(out, 0, (cells + 7) / 8);
    if (t->mfm) {
        /* gap3 calculado para caber na trilha (ex.: DIGDOS 18x256) */
        long avail = cells / 16, fixed = 32;
        int gap3;
        for (i = 0; i < t->nsec; i++) fixed += 62 + t->s[i].size;
        gap3 = t->nsec ? (int)((avail - fixed - 16) / t->nsec) : 24;
        if (gap3 > 54) gap3 = 54;
        if (gap3 < 4) gap3 = 4;
        for (j = 0; j < 32; j++) mfm_byte(&e, 0x4E);
        for (i = 0; i < t->nsec; i++) {
            sector_t *s = &t->s[i];
            uint8_t hdr[8]; uint16_t c;
            for (j = 0; j < 12; j++) mfm_byte(&e, 0x00);
            for (j = 0; j < 3; j++) mfm_sync(&e);
            hdr[0] = 0xA1; hdr[1] = 0xA1; hdr[2] = 0xA1; hdr[3] = 0xFE;
            hdr[4] = s->c; hdr[5] = s->h; hdr[6] = s->r; hdr[7] = s->n;
            c = crc16(0xFFFF, hdr, 8);
            for (j = 3; j < 8; j++) mfm_byte(&e, hdr[j]);
            mfm_byte(&e, c >> 8); mfm_byte(&e, c & 0xFF);
            for (j = 0; j < 22; j++) mfm_byte(&e, 0x4E);
            for (j = 0; j < 12; j++) mfm_byte(&e, 0x00);
            for (j = 0; j < 3; j++) mfm_sync(&e);
            hdr[3] = s->dam;
            c = crc16(0xFFFF, hdr, 4);
            c = crc16(c, s->data, s->size);
            mfm_byte(&e, s->dam);
            for (j = 0; j < s->size; j++) mfm_byte(&e, s->data[j]);
            mfm_byte(&e, c >> 8); mfm_byte(&e, c & 0xFF);
            for (j = 0; j < gap3; j++) mfm_byte(&e, 0x4E);
        }
        while (e.n < e.max) mfm_byte(&e, 0x4E);
    } else {
        for (j = 0; j < 40; j++) fm_byte(&e, 0xFF, 0xFF);
        for (j = 0; j < 6; j++) fm_byte(&e, 0x00, 0xFF);
        fm_byte(&e, 0xFC, 0xD7);
        for (j = 0; j < 26; j++) fm_byte(&e, 0xFF, 0xFF);
        for (i = 0; i < t->nsec; i++) {
            sector_t *s = &t->s[i];
            uint8_t hdr[5]; uint16_t c;
            for (j = 0; j < 6; j++) fm_byte(&e, 0x00, 0xFF);
            hdr[0] = 0xFE; hdr[1] = s->c; hdr[2] = s->h; hdr[3] = s->r; hdr[4] = s->n;
            c = crc16(0xFFFF, hdr, 5);
            fm_byte(&e, 0xFE, 0xC7);
            for (j = 1; j < 5; j++) fm_byte(&e, hdr[j], 0xFF);
            fm_byte(&e, c >> 8, 0xFF); fm_byte(&e, c & 0xFF, 0xFF);
            for (j = 0; j < 11; j++) fm_byte(&e, 0xFF, 0xFF);
            for (j = 0; j < 6; j++) fm_byte(&e, 0x00, 0xFF);
            hdr[0] = s->dam;
            c = crc16(0xFFFF, hdr, 1);
            c = crc16(c, s->data, s->size);
            fm_byte(&e, s->dam, 0xC7);
            for (j = 0; j < s->size; j++) fm_byte(&e, s->data[j], 0xFF);
            fm_byte(&e, c >> 8, 0xFF); fm_byte(&e, c & 0xFF, 0xFF);
            for (j = 0; j < 27; j++) fm_byte(&e, 0xFF, 0xFF);
        }
        while (e.n < e.max) fm_byte(&e, 0xFF, 0xFF);
    }
}

static int save_hfe(disk_t *d, FILE *f)
{
    int t, h;
    long pos;
    uint8_t lut[512];
    uint8_t hdr[512];
    memcpy(hdr, d->hfe_hdr, 512);
    if (memcmp(hdr, "HXCPICFE", 8)) {
        memset(hdr, 0xFF, 512);
        memcpy(hdr, "HXCPICFE", 8);
        hdr[8] = 0; hdr[11] = 0; hdr[12] = 250; hdr[13] = 0; hdr[14] = 0; hdr[15] = 0;
        hdr[16] = 7; hdr[17] = 1;
    }
    hdr[9] = d->ntracks; hdr[10] = d->nsides; hdr[18] = 1; hdr[19] = 0;
    fwrite(hdr, 1, 512, f);
    memset(lut, 0xFF, 512);
    pos = 2;
    for (t = 0; t < d->ntracks; t++) {
        int tl = d->hfe_len[t] ? d->hfe_len[t] : 0x61B0;
        lut[t * 4] = pos & 0xFF; lut[t * 4 + 1] = pos >> 8;
        lut[t * 4 + 2] = tl & 0xFF; lut[t * 4 + 3] = tl >> 8;
        pos += (tl + 511) / 512;
    }
    fwrite(lut, 1, 512, f);
    for (t = 0; t < d->ntracks; t++) {
        int tl = d->hfe_len[t] ? d->hfe_len[t] : 0x61B0;
        long half = tl / 2, i, blocks = (tl + 511) / 512;
        uint8_t *sb[2], *out;
        for (h = 0; h < 2; h++) {
            sb[h] = calloc(half + 8, 1);
            if (h < d->nsides) encode_track(&d->t[t][h], sb[h], half * 8);
        }
        out = malloc(blocks * 512);
        memset(out, 0, blocks * 512);
        for (i = 0; i < blocks; i++) {
            long n = half - i * 256; if (n > 256) n = 256; if (n < 0) n = 0;
            memcpy(out + i * 512, sb[0] + i * 256, n);
            memcpy(out + i * 512 + 256, sb[1] + i * 256, n);
        }
        fwrite(out, 1, blocks * 512, f);
        free(out); free(sb[0]); free(sb[1]);
    }
    return 0;
}

/* ---------------- formatacao (Write Track) ---------------- */
void disk_format_track(disk_t *d, int trk, int side, const uint8_t *buf, int len, int mfm)
{
    track_t *t;
    int i = 0;
    if (!d || trk >= MAXTRK) return;
    t = &d->t[trk][side];
    free_track(t);
    t->mfm = mfm;
    while (i < len) {
        if (buf[i] == 0xFE && i + 4 < len) {
            int c = buf[i + 1], h = buf[i + 2], r = buf[i + 3], n = buf[i + 4] & 3, size = 128 << n, j;
            i += 5;
            for (j = i; j < len && j < i + 60; j++)
                if (buf[j] == 0xFB || buf[j] == 0xF8 || buf[j] == 0xFA || buf[j] == 0xF9) break;
            if (j < len && j < i + 60 && j + 1 + size <= len) {
                add_sector(t, c, h, r, n, (buf[j] == 0xF8 || buf[j] == 0xF9) ? 0xF8 : 0xFB, buf + j + 1);
                i = j + 1 + size;
            } else {
                add_sector(t, c, h, r, n, 0xFB, NULL);
            }
        } else i++;
    }
    if (trk + 1 > d->ntracks) d->ntracks = trk + 1;
    if (side + 1 > d->nsides) d->nsides = side + 1;
    d->dirty = 1;
}


/* ---------------- DMK (David Keil): trilhas cruas com tabela de IDAMs ---------------- */
static int is_dmk(const uint8_t *buf, long len)
{
    int nt, tl, ns;
    if (len < 16 + 128) return 0;
    if (buf[0] != 0x00 && buf[0] != 0xFF) return 0;
    nt = buf[1]; tl = buf[2] | (buf[3] << 8); ns = (buf[4] & 0x10) ? 1 : 2;
    if (nt < 1 || nt > 96 || tl < 0x0A00 || tl > 0x4000) return 0;
    return len >= 16 + (long)nt * ns * tl && len <= 16 + (long)nt * ns * tl + 512;
}

static int load_dmk(disk_t *d, uint8_t *buf, long len)
{
    int nt = buf[1], tl = buf[2] | (buf[3] << 8), fl = buf[4], ns = (fl & 0x10) ? 1 : 2, t, h, i;
    (void)len;
    d->wp = d->wp || buf[0] == 0xFF;
    d->ntracks = nt > MAXTRK ? MAXTRK : nt;
    d->nsides = ns;
    d->dmk_tl = tl; d->dmk_flags = fl;
    for (t = 0; t < d->ntracks; t++)
        for (h = 0; h < ns; h++) {
            const uint8_t *tr = buf + 16 + ((long)t * ns + h) * tl;
            track_t *trk = &d->t[t][h];
            for (i = 0; i < 64; i++) {
                int p = tr[2 * i] | (tr[2 * i + 1] << 8), dd, step, q, k, n, size, dam;
                uint8_t data[1024];
                if (!p) break;
                dd = (p & 0x8000) != 0; p &= 0x3FFF;
                step = (dd || (fl & 0x40)) ? 1 : 2;
                if (p + 7 * step >= tl || tr[p] != 0xFE) continue;
                trk->mfm = dd;
                n = tr[p + 4 * step] & 3; size = 128 << n;
                q = p + 7 * step;
                for (k = 0; k < 60 && q < tl; k++, q += step)
                    if (tr[q] >= 0xF8 && tr[q] <= 0xFB) break;
                if (k == 60 || q + (size + 1) * step > tl) { add_sector(trk, tr[p + step], tr[p + 2 * step], tr[p + 3 * step], n, 0xFB, NULL); continue; }
                dam = (tr[q] == 0xF8 || tr[q] == 0xF9) ? 0xF8 : 0xFB;
                for (k = 0; k < size; k++) data[k] = tr[q + (k + 1) * step];
                add_sector(trk, tr[p + step], tr[p + 2 * step], tr[p + 3 * step], n, dam, data);
            }
        }
    return 0;
}

typedef struct { uint8_t *b; int n, max, step; } dmkw_t;
static void dput(dmkw_t *w, uint8_t v, int cnt)
{
    while (cnt-- > 0) { int k; for (k = 0; k < w->step; k++) if (w->n < w->max) w->b[w->n++] = v; }
}

static int dmk_build(track_t *t, uint8_t *out, int tl, int fl)
{
    dmkw_t w;
    int i, need, gap3, mfm = t->mfm, step = (mfm || (fl & 0x40)) ? 1 : 2;
    memset(out, 0, tl);
    w.b = out + 128; w.n = 0; w.max = tl - 128; w.step = step;
    /* espaco de cada setor (bytes logicos) sem o gap3 */
    need = mfm ? 80 + 12 + 4 + 50 : 40 + 6 + 1 + 26;
    for (i = 0; i < t->nsec; i++) need += mfm ? 12 + 4 + 4 + 2 + 22 + 12 + 4 + t->s[i].size + 2 : 6 + 1 + 4 + 2 + 11 + 6 + 1 + t->s[i].size + 2;
    gap3 = t->nsec ? ((tl - 128) / step - need) / t->nsec : 0;
    if (gap3 > 54) gap3 = 54;
    if (gap3 < 4) gap3 = 4;
    if (mfm) { dput(&w, 0x4E, 80); dput(&w, 0, 12); dput(&w, 0xC2, 3); dput(&w, 0xFC, 1); dput(&w, 0x4E, 50); }
    else { dput(&w, 0xFF, 40); dput(&w, 0, 6); dput(&w, 0xFC, 1); dput(&w, 0xFF, 26); }
    for (i = 0; i < t->nsec && i < 64; i++) {
        sector_t *s = &t->s[i];
        uint8_t id[8], hd[4]; uint16_t crc; int pos, k;
        if (mfm) { dput(&w, 0, 12); dput(&w, 0xA1, 3); }
        else dput(&w, 0, 6);
        pos = 128 + w.n;
        out[2 * i] = pos & 0xFF; out[2 * i + 1] = (pos >> 8) | (mfm ? 0x80 : 0);
        id[0] = 0xA1; id[1] = 0xA1; id[2] = 0xA1; id[3] = 0xFE; id[4] = s->c; id[5] = s->h; id[6] = s->r; id[7] = s->n;
        crc = mfm ? crc16(0xFFFF, id, 8) : crc16(0xFFFF, id + 3, 5);
        for (k = 3; k < 8; k++) dput(&w, id[k], 1);
        dput(&w, crc >> 8, 1); dput(&w, crc & 0xFF, 1);
        if (mfm) { dput(&w, 0x4E, 22); dput(&w, 0, 12); dput(&w, 0xA1, 3); }
        else { dput(&w, 0xFF, 11); dput(&w, 0, 6); }
        hd[0] = hd[1] = hd[2] = 0xA1; hd[3] = s->dam;
        crc = mfm ? crc16(0xFFFF, hd, 4) : crc16(0xFFFF, hd + 3, 1);
        crc = crc16(crc, s->data, s->size);
        dput(&w, s->dam, 1);
        for (k = 0; k < s->size; k++) dput(&w, s->data[k], 1);
        dput(&w, crc >> 8, 1); dput(&w, crc & 0xFF, 1);
        dput(&w, mfm ? 0x4E : 0xFF, gap3);
    }
    while (w.n < w.max) w.b[w.n++] = mfm ? 0x4E : 0xFF;
    return 0;
}

static int save_dmk(disk_t *d, FILE *f)
{
    uint8_t hdr[16], *tb;
    int t, h, ns = d->nsides > 1 ? 2 : 1, tl = d->dmk_tl ? d->dmk_tl : 0x1900, fl = d->dmk_flags & ~0x10;
    int nt = d->ntracks ? d->ntracks : 40;
    /* trilha grande o bastante para o maior conteudo */
    for (t = 0; t < nt; t++) for (h = 0; h < ns; h++) {
        track_t *tr = &d->t[t][h]; int i, need = 128 + 300, step = (tr->mfm || (fl & 0x40)) ? 1 : 2;
        for (i = 0; i < tr->nsec; i++) need += (tr->s[i].size + 70) * step;
        if (need > tl) tl = need;
    }
    if (ns == 1) fl |= 0x10;
    memset(hdr, 0, 16);
    hdr[0] = 0; hdr[1] = nt; hdr[2] = tl & 0xFF; hdr[3] = tl >> 8; hdr[4] = fl;
    fwrite(hdr, 1, 16, f);
    tb = malloc(tl);
    for (t = 0; t < nt; t++)
        for (h = 0; h < ns; h++) { dmk_build(&d->t[t][h], tb, tl, fl); fwrite(tb, 1, tl, f); }
    free(tb);
    return 0;
}

/* ---------------- abrir/gravar ---------------- */
disk_t *disk_open(const char *path, char *err)
{
    FILE *f = fopen(path, "rb");
    disk_t *d;
    uint8_t *buf;
    long len;
    const char *e = ext(path);
    int r = -1;
    if (!f) { sprintf(err, "nao consegui abrir %s", path); return NULL; }
    len = fsize(f);
    buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != (size_t)len) { fclose(f); free(buf); sprintf(err, "erro lendo %s", path); return NULL; }
    fclose(f);
    d = calloc(1, sizeof(disk_t));
    strncpy(d->path, path, sizeof d->path - 1);
    f = fopen(path, "r+b");
    if (!f) d->wp = 1; else fclose(f);

    if (len >= 512 && !memcmp(buf, "HXCPICFE", 8)) { d->fmt = FMT_HFE; r = load_hfe(d, buf, len); }
    else if (len >= 4 && !memcmp(buf, "IMD ", 4)) { d->fmt = FMT_IMD; r = load_imd(d, buf, len); }
    else if (ieq(e, "IMG")) { d->fmt = FMT_IMG; r = load_img(d, buf, len); }
    else if (is_dmk(buf, len)) { d->fmt = FMT_DMK; r = load_dmk(d, buf, len); }
    else if (is_jv3(buf, len)) { d->fmt = FMT_JV3; r = load_jv3(d, buf, len); }
    else if (ieq(e, "DSK") || ieq(e, "JV1") || len % 2560 == 0) { d->fmt = FMT_JV1; r = load_jv1(d, buf, len); }
    else { d->fmt = FMT_IMG; r = load_img(d, buf, len); }
    free(buf);
    if (r) { sprintf(err, "formato invalido: %s", path); disk_close(d); return NULL; }
    return d;
}

disk_t *disk_new(const char *path, char *err)
{
    disk_t *d;
    FILE *f;
    const char *e = ext(path);
    f = fopen(path, "rb");
    if (f) { fclose(f); sprintf(err, "%s ja existe", path); return NULL; }
    f = fopen(path, "wb");
    if (!f) { sprintf(err, "nao consegui criar %s", path); return NULL; }
    fclose(f);
    d = calloc(1, sizeof(disk_t));
    strncpy(d->path, path, sizeof d->path - 1);
    if (ieq(e, "HFE")) d->fmt = FMT_HFE;
    else if (ieq(e, "IMD")) d->fmt = FMT_IMD;
    else if (ieq(e, "IMG")) d->fmt = FMT_IMG;
    else if (ieq(e, "JV3")) d->fmt = FMT_JV3;
    else if (ieq(e, "DMK")) d->fmt = FMT_DMK;
    else d->fmt = FMT_JV1;
    d->ntracks = 40; d->nsides = 1;
    {
        int t;
        static const uint8_t tpl[32] = { 'H','X','C','P','I','C','F','E', 0, 40, 1, 0, 250, 0, 0, 0, 7, 1, 1, 0 };
        memset(d->hfe_hdr, 0xFF, 512);
        memcpy(d->hfe_hdr, tpl, 20);
        for (t = 0; t < MAXTRK; t++) { d->hfe_len[t] = 0x61B0; d->t[t][0].mfm = d->t[t][1].mfm = 1; }
    }
    d->dirty = 1;
    return d;
}

int disk_save(disk_t *d)
{
    FILE *f;
    if (!d || !d->dirty || d->wp) return 0;
    f = fopen(d->path, "wb");
    if (!f) return -1;
    switch (d->fmt) {
    case FMT_JV1: save_jv1(d, f); break;
    case FMT_JV3: save_jv3(d, f); break;
    case FMT_IMG: save_img(d, f); break;
    case FMT_IMD: save_imd(d, f); break;
    case FMT_HFE: save_hfe(d, f); break;
    case FMT_DMK: save_dmk(d, f); break;
    }
    fclose(f);
    d->dirty = 0;
    return 0;
}

/* exporta a imagem para outro arquivo; a extensao escolhe o formato */
int disk_export(disk_t *d, const char *path, char *err)
{
    int fmt, oldfmt = d->fmt, olddirty = d->dirty, oldwp = d->wp, t, h, i, r;
    char oldpath[260];
    const char *e = ext(path);
    if (ieq(e, "HFE")) fmt = FMT_HFE;
    else if (ieq(e, "IMD")) fmt = FMT_IMD;
    else if (ieq(e, "IMG")) fmt = FMT_IMG;
    else if (ieq(e, "JV3")) fmt = FMT_JV3;
    else if (ieq(e, "DMK")) fmt = FMT_DMK;
    else if (ieq(e, "DSK") || ieq(e, "JV1")) fmt = FMT_JV1;
    else { sprintf(err, "extensao desconhecida (use HFE IMD DSK DMK JV1 JV3 IMG)"); return -1; }
    if (fmt == FMT_JV1) {
        /* JV1 so guarda 10 setores de 256 em densidade simples: senao usa JV3 */
        int ok = 1;
        for (t = 0; t < d->ntracks && ok; t++)
            for (h = 0; h < d->nsides && ok; h++) {
                track_t *tr = &d->t[t][h];
                if (h > 0 && tr->nsec) ok = 0;
                if (tr->mfm && tr->nsec) ok = 0;
                for (i = 0; i < tr->nsec; i++) if (tr->s[i].size != 256 || tr->s[i].r > 9) ok = 0;
            }
        if (!ok) fmt = FMT_JV3;
    }
    strcpy(oldpath, d->path);
    strncpy(d->path, path, sizeof d->path - 1);
    d->fmt = fmt; d->dirty = 1; d->wp = 0;
    r = disk_save(d);
    strcpy(d->path, oldpath);
    d->fmt = oldfmt; d->dirty = olddirty; d->wp = oldwp;
    if (r) { sprintf(err, "nao consegui gravar %s", path); return -1; }
    if (fmt != FMT_JV1 && (ieq(e, "DSK") || ieq(e, "JV1"))) sprintf(err, "Gravado em formato JV3 (disco em dupla densidade nao cabe em JV1)");
    else err[0] = 0;
    return 0;
}

void disk_close(disk_t *d)
{
    int t, h;
    if (!d) return;
    for (t = 0; t < MAXTRK; t++) for (h = 0; h < 2; h++) free_track(&d->t[t][h]);
    free(d->imd_comment);
    free(d);
}
