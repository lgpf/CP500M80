/* plat_esp32.cpp - TTGO VGA32 v1.4: VGA 640x400 (FabGL), PS/2, SD, audio */
#include "fabgl.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static bool sd_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = HSPI_HOST;
    host.max_freq_khz = 20000;
    spi_bus_config_t bus = { };
    bus.mosi_io_num = GPIO_NUM_12; bus.miso_io_num = GPIO_NUM_2; bus.sclk_io_num = GPIO_NUM_14;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 16384;
    esp_err_t r = spi_bus_initialize((spi_host_device_t)host.slot, &bus, 2);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) { printf("SD: spi %d\n", r); return false; }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = GPIO_NUM_13;
    dev.host_id = (spi_host_device_t)host.slot;
    esp_vfs_fat_sdmmc_mount_config_t mc = { };
    mc.format_if_mount_failed = false; mc.max_files = 8; mc.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card;
    r = esp_vfs_fat_sdspi_mount("/sd", &host, &dev, &mc, &card);
    if (r != ESP_OK) { printf("SD: erro %d\n", r); return false; }
    sdmmc_card_print_info(stdout, card);
    printf("SD: cartao suporta %u kHz\n", (unsigned)card->max_freq_khz);
    return true;
}
extern "C" {
#include "plat.h"
int dgt_main(int argc, char **argv);
void dgt_prealloc(void);
}

class DgtVGA : public fabgl::VGA4Controller { public: void relut() { updateRGB2PaletteLUT(); } };
static DgtVGA VGA;
static fabgl::PS2Controller PS2;
static fabgl::Canvas *cv;
static fabgl::SoundGenerator *snd;

static int in_menu;
#define OY 40   /* 640x480: centraliza 400 linhas */
static volatile unsigned long n_draw, n_glyph, n_key, n_present;
/* ---------------- tempo */
extern "C" unsigned long plat_ticks(void)
{
    uint64_t us = (uint64_t)esp_timer_get_time();
    return (unsigned long)(uint32_t)(us * 1193182ULL / 1000000ULL);
}
extern "C" void plat_idle(void) { vTaskDelay(1); }
extern "C" int plat_quit_requested(void) { return 0; }
extern "C" int plat_drives(char *list) { list[0] = 0; return 0; }
extern "C" int plat_init(void) { return 0; }

/* LEDs dos drives na porta PS/2 do mouse (nao usada): pino 1 (dados, IO27) = drives
 * 0 e 2; pino 5 (clock, IO26) = drives 1 e 3. LED + 330R para o GND (pino 3). */
static int led_state[2] = { -1, -1 };
extern "C" void plat_leds(int a, int b)
{
    if (a != led_state[0]) { led_state[0] = a; digitalWrite(27, a > 0 ? HIGH : LOW); }
    if (b != led_state[1]) { led_state[1] = b; digitalWrite(26, b > 0 ? HIGH : LOW); }
}
extern "C" void plat_shutdown(void) { delay(200); esp_restart(); }

/* ---------------- teclado: set 2 -> set 1 (o que o resto do programa espera) */
static const uint8_t s2to1[0x84] = {
 0xff,0x43,0x41,0x3f,0x3d,0x3b,0x3c,0x58,0x64,0x44,0x42,0x40,0x3e,0x0f,0x29,0x59,
 0x65,0x38,0x2a,0x70,0x1d,0x10,0x02,0x5a,0x66,0x71,0x2c,0x1f,0x1e,0x11,0x03,0x5b,
 0x67,0x2e,0x2d,0x20,0x12,0x05,0x04,0x5c,0x68,0x39,0x2f,0x21,0x14,0x13,0x06,0x5d,
 0x69,0x31,0x30,0x23,0x22,0x15,0x07,0x5e,0x6a,0x72,0x32,0x24,0x16,0x08,0x09,0x5f,
 0x6b,0x33,0x25,0x17,0x18,0x0b,0x0a,0x60,0x6c,0x34,0x35,0x26,0x27,0x19,0x0c,0x61,
 0x6d,0x73,0x28,0x74,0x1a,0x0d,0x62,0x6e,0x3a,0x36,0x1c,0x1b,0x75,0x2b,0x63,0x76,
 0x55,0x56,0x77,0x78,0x79,0x7a,0x0e,0x7b,0x7c,0x4f,0x7d,0x4b,0x47,0x7e,0x7f,0x6f,
 0x52,0x53,0x50,0x4c,0x4d,0x48,0x01,0x45,0x57,0x4e,0x51,0x4a,0x37,0x49,0x46,0x54,
 0x80,0x81,0x82,0x41 };
static uint8_t kq[64]; static int kqh, kqt;
static int k_rel, k_e0, k_skip;
static void kpush(int v) { int n = (kqh + 1) & 63; if (n != kqt) { kq[kqh] = v; kqh = n; } }
extern "C" int plat_scancode(void)
{
    static int64_t lasty;
    int64_t t = esp_timer_get_time();
    if (t - lasty > 100000) { lasty = t; vTaskDelay(1); }
    { static int64_t lastp; if (t - lastp > 3000000) { lastp = t;
        Serial.printf("vivo: telas %lu glifos %lu quadros %lu teclas %lu menu %d\n", n_draw, n_glyph, n_present, n_key, in_menu); } }   /* deixa o nucleo 0 respirar (watchdog) */
    fabgl::Keyboard *kb = PS2.keyboard();
    while (kqh == kqt && kb && kb->scancodeAvailable()) {
        int c = kb->getNextScancode(0);
        if (c < 0) break;
        if (k_skip) { k_skip--; continue; }
        if (c == 0xE1) { k_skip = 7; continue; }      /* Pause */
        if (c == 0xE0) { k_e0 = 1; continue; }
        if (c == 0xF0) { k_rel = 1; continue; }
        if (c == 0xFA || c == 0xAA || c == 0xFE) { k_rel = k_e0 = 0; continue; }
        if (c < 0x84) {
            int s1 = s2to1[c];
            if (!(k_e0 && (s1 == 0x2A || s1 == 0x36)) && s1 != 0xff) {   /* shift falso */
                if (k_e0) kpush(0xE0);
                kpush(s1 | (k_rel ? 0x80 : 0));
            }
        }
        k_rel = k_e0 = 0;
    }
    if (kqh == kqt) return -1;
    n_key++;
    int v = kq[kqt]; kqt = (kqt + 1) & 63; return v;
}

/* ---------------- video */
static const RGB888 phos[4] = { RGB888(40,255,96), RGB888(255,255,255), RGB888(176,176,176), RGB888(255,180,0) };
static RGB888 pal[4];
static uint8_t (*efont)[24];
static uint16_t shadow[80 * 25];    /* RAM interna: comparado a cada quadro */
static uint16_t *screen;            /* menus (PSRAM) */


static void setpal(int i, RGB888 c) {  pal[i] = c; VGA.setPaletteItem(i, c); }
static void glyph(int x, int y, int h, const uint8_t *data, int fg, int bg)
{
    /* escreve direto no quadro (2 bits por ponto, 4 pontos por byte) */
    uint8_t lut[16];
    for (int n = 0; n < 16; n++) {
        uint8_t b = 0;
        for (int k = 0; k < 4; k++) b |= ((n & (8 >> k)) ? fg : bg) << (6 - 2 * k);
        lut[n] = b;
    }
    for (int r = 0; r < h; r++) {
        uint8_t *row = (uint8_t *)fabgl::VGA4Controller::sgetScanline(y + OY + r) + (x >> 2);
        uint8_t d = data[r];
        row[0] = lut[d >> 4]; row[1] = lut[d & 15];
    }
    n_glyph++;
}
static void clearfb(int idx)
{
    uint8_t b = idx * 0x55;
    for (int y = 0; y < 480; y++) memset(fabgl::VGA4Controller::sgetScanline(y), b, 160);
}

static int emode;
extern "C" void plat_emu_mode(uint8_t font[256][24], int color, int mode)
{
    in_menu = 0; efont = font; emode = mode;
    setpal(0, RGB888(0,0,0)); setpal(1, phos[color & 3]); setpal(2, RGB888(0,0,0)); setpal(3, RGB888(0,0,0)); VGA.relut();
    clearfb(0);
    for (int i = 0; i < 80 * 25; i++) shadow[i] = 0xFFFF;
}
extern "C" void plat_emu_draw(const uint8_t *vram, int mode, int wide)
{
    n_draw++;
    if (mode) {                                  /* 80x24, glifos 8x16 */
        for (int i = 0; i < 1920; i++) {
            int g = vram[i];
            if (shadow[i] == g) continue;
            shadow[i] = g;
            glyph((i % 80) * 8, 8 + (i / 80) * 16, 16, efont[g], 1, 0);
        }
        return;
    }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 64; c++) {
            int g = vram[r * 64 + c];
            if (wide && (c & 1)) g = ' ';
            if (shadow[r * 64 + c] == g) continue;
            shadow[r * 64 + c] = g;
            glyph(64 + c * 8, 8 + r * 24, 24, efont[g], 1, 0);
        }
}

/* menus: 4 cores. Converte as cores CGA dos atributos */
static int cga2(int c)
{
    switch (c & 15) {
    case 0: return 0;
    case 1: case 9: return 1;
    case 2: case 3: case 10: case 11: case 4: case 12: case 5: case 13: case 6: return 2;
    default: return 3;
    }
}
extern "C" void plat_menu_mode(void)
{
    in_menu = 1;
    setpal(0, RGB888(0,0,0)); setpal(1, RGB888(0,0,170)); setpal(2, RGB888(0,170,170)); setpal(3, RGB888(255,255,255)); VGA.relut();
    clearfb(0);
    memset(screen, 0, 80 * 25 * 2);
    for (int i = 0; i < 80 * 25; i++) shadow[i] = 0xFFFF;
}
extern "C" void plat_put(int x, int y, int attr, const char *s)
{
    if (y < 0 || y >= 25) return;
    for (; *s && x < 80; s++, x++)
        if (x >= 0) screen[y * 80 + x] = (uint16_t)((attr << 8) | (unsigned char)*s);
}
extern "C" void plat_fill(int x, int y, int w, int h, int attr, int ch)
{
    for (int j = y; j < y + h && j < 25; j++)
        for (int i = x; i < x + w && i < 80; i++)
            if (i >= 0 && j >= 0) screen[j * 80 + i] = (uint16_t)((attr << 8) | ch);
}
extern "C" void plat_present(void)
{
    n_present++;
    if (!in_menu) return;
    const fabgl::FontInfo *f = &fabgl::FONT_8x16;
    for (int i = 0; i < 80 * 25; i++) {
        uint16_t v = screen[i];
        if (v == shadow[i]) continue;
        shadow[i] = v;
        int a = v >> 8, ch = v & 255;
        int fg = cga2(a & 15), bg = cga2(a >> 4);
        if (fg == bg) fg = bg == 3 ? 0 : 3;
        glyph((i % 80) * 8, (i / 80) * 16, 16, f->data + ch * 16, fg, bg);
    }
}

/* ---------------- som (DAC GPIO25) */
#define SRATE 16000
static int (*pcm_pull)(signed char *dst, int n);
class DgtWave : public fabgl::WaveformGenerator {
public:
    void setFrequency(int) { }
    int getSample() {
        signed char s = 0;
        if (pcm_pull) pcm_pull(&s, 1);
        return s;
    }
};
static DgtWave wave;
extern "C" void plat_sound(int hz) { (void)hz; }
extern "C" int plat_audio_rate(void) { return SRATE; }
extern "C" void plat_audio_source(int (*pull)(signed char *dst, int n)) { pcm_pull = pull; }

/* ---------------- inicio */
static void emu_task(void *)
{
    static char a0[] = "/sd/CP5M80";
    static char *argv[] = { a0, NULL };
    /* monta o SD aqui: a interrupcao do SPI fica no nucleo 0, longe do video */
    if (!sd_mount())
        Serial.println("SD nao montado (sem cartao?)");
    Serial.println("emulador iniciando");
    dgt_main(1, argv);
    esp_restart();
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.printf("\nCP-500 M80 ESP32  PSRAM: %u bytes  heap interno: %u\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    screen = (uint16_t *)heap_caps_calloc(80 * 25, 2, MALLOC_CAP_SPIRAM);
    pinMode(27, OUTPUT); digitalWrite(27, LOW);
    pinMode(26, OUTPUT); digitalWrite(26, LOW);
    PS2.begin(PS2Preset::KeyboardPort0, KbdMode::NoVirtualKeys);
    VGA.begin();
    VGA.setResolution(VGA_640x480_60Hz);
    cv = new fabgl::Canvas(&VGA);
    Serial.printf("video %dx%d\n", VGA.getScreenWidth(), VGA.getScreenHeight());
    setpal(0, RGB888(0,0,0)); setpal(1, RGB888(0,0,170)); setpal(2, RGB888(0,170,170)); setpal(3, RGB888(255,255,255)); VGA.relut();
    clearfb(1);
    for (const char *p = "CP-500 M80 iniciando..."; *p; p++) glyph(240 + (p - "CP-500 M80 iniciando...") * 8, 200, 16, fabgl::FONT_8x16.data + *p * 16, 3, 1);
    Serial.println("tela de teste desenhada");
    delay(1500);
    snd = new fabgl::SoundGenerator(SRATE);
    wave.setSampleRate(SRATE);
    wave.setVolume(100);
    wave.enable(true);
    snd->attach(&wave);
    snd->play(true);
    Serial.printf("heap interno livre: %u  maior bloco: %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    dgt_prealloc();
    Serial.printf("depois da maquina: livre %u maior bloco %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    static StackType_t stk[16384];
    static StaticTask_t tcb;
    if (!xTaskCreateStaticPinnedToCore(emu_task, "dgt", sizeof stk, NULL, 3, stk, &tcb, 0))
        Serial.println("ERRO: nao consegui criar a tarefa do emulador");   /* nucleo 1 fica com o video */
}

void loop() { vTaskDelay(1000); }
