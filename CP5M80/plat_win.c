#ifndef ARDUINO   /* no Arduino/ESP32 este arquivo fica de fora */
/* plat_win.c - Windows (Win32 puro, sem DLLs extras)
 * Janela com a tela 640x400 ampliada, teclado por scancode, som por waveOut.
 * Alt+Enter alterna tela cheia. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <stdio.h>
#include "plat.h"

#define W 640
#define H 400

static HWND hwnd;
static HDC memdc;
static HBITMAP dib;
static uint32_t *pix;
static HFONT menufont;
static int in_menu, quit_req, fullscreen;
static int led_st[2];
static uint8_t (*efont)[24];
static uint32_t fg_color = 0x33FF66;
static uint16_t screen[80 * 25];
static WINDOWPLACEMENT wp_prev = { sizeof(wp_prev) };

#define KQ 1024
static int kq[KQ], kh, kt;
static unsigned char down[256];

static const uint32_t cga[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF };
static const uint32_t phosphor[4] = { 0x40FF60, 0xFFFFFF, 0xB0B0B0, 0xFFB400 };

static void kpush(int v) { int n = (kh + 1) % KQ; if (n != kt) { kq[kh] = v; kh = n; } }

static void push_key(int code, int isdown)
{
    if (code & 0x80) kpush(0xE0);
    kpush((code & 0x7F) | (isdown ? 0 : 0x80));
    down[code & 0xFF] = (unsigned char)isdown;
}

static void release_all(void)
{
    int c;
    for (c = 0; c < 256; c++) if (down[c]) push_key(c, 0);
}

static void toggle_fullscreen(void)
{
    DWORD style = GetWindowLong(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hwnd, &wp_prev) && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        fullscreen = 1;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &wp_prev);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = 0;
    }
}

static void paint(HDC dc)
{
    RECT rc;
    int cw, ch, w, h, x, y;
    GetClientRect(hwnd, &rc);
    cw = rc.right; ch = rc.bottom;
    /* mantem a proporcao 4:3 (640x400 com pixels altos, como no monitor) */
    w = cw; h = cw * 3 / 4;
    if (h > ch) { h = ch; w = ch * 4 / 3; }
    x = (cw - w) / 2; y = (ch - h) / 2;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, x, y, w, h, memdc, 0, 0, W, H, SRCCOPY);
    if (x > 0) { RECT r = { 0, 0, x, ch }; FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); r.left = x + w; r.right = cw; FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); }
    if (y > 0) { RECT r = { 0, 0, cw, y }; FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); r.top = y + h; r.bottom = ch; FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE: quit_req = 1; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_KILLFOCUS: release_all(); return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int isdown = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        int sc = (lp >> 16) & 0xFF, ext = (lp >> 24) & 1;
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) return DefWindowProc(h, msg, wp, lp);  /* Alt+F4 */
        if (isdown && wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) { if (!(lp & (1 << 30))) toggle_fullscreen(); return 0; }
        if (sc == 0x45) ext = 0;            /* NumLock */
        if (sc) push_key(sc | (ext ? 0x80 : 0), isdown);
        return 0; }
    case WM_SYSCHAR: case WM_CHAR: return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;   /* Alt nao abre menu */
        break;
    case WM_DROPFILES: break;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* ---------------- som: onda quadrada continua por waveOut ---------------- */
#define SR 22050
#define NB 4
#define BS 441
static HWAVEOUT wo;
static WAVEHDR wh[NB];
static unsigned char wbuf[NB][BS];
static volatile int snd_hz;
static double phase;
static int (*pcm_pull)(signed char *dst, int n);

int plat_audio_rate(void) { return SR; }
void plat_audio_source(int (*pull)(signed char *dst, int n)) { pcm_pull = pull; }

static void snd_fill(int i)
{
    int k;
    if (pcm_pull) {                      /* amostras geradas pelo emulador (onda real) */
        static signed char tmp[BS];
        static signed char last;
        int n = pcm_pull(tmp, BS);
        for (k = 0; k < BS; k++) {
            signed char v = k < n ? tmp[k] : 0;
            if (k < n) last = v;
            wbuf[i][k] = (unsigned char)(128 + v);
        }
        (void)last;
        return;
    }
    for (k = 0; k < BS; k++) {
        if (snd_hz > 0) {
            phase += (double)snd_hz / SR;
            if (phase >= 1.0) phase -= 1.0;
            wbuf[i][k] = phase < 0.5 ? 128 + 40 : 128 - 40;
        } else wbuf[i][k] = 128;
    }
}

static void snd_pump(void)
{
    int i;
    if (!wo) return;
    for (i = 0; i < NB; i++)
        if (wh[i].dwFlags & WHDR_DONE) {
            snd_fill(i);
            wh[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &wh[i], sizeof(WAVEHDR));
        }
}

static void snd_init(void)
{
    WAVEFORMATEX f;
    int i;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = SR;
    f.wBitsPerSample = 8; f.nBlockAlign = 1; f.nAvgBytesPerSec = SR;
    if (waveOutOpen(&wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = 0; return; }
    for (i = 0; i < NB; i++) {
        memset(&wh[i], 0, sizeof(WAVEHDR));
        wh[i].lpData = (LPSTR)wbuf[i]; wh[i].dwBufferLength = BS;
        waveOutPrepareHeader(wo, &wh[i], sizeof(WAVEHDR));
        snd_fill(i);
        waveOutWrite(wo, &wh[i], sizeof(WAVEHDR));
    }
}

void plat_sound(int hz) { snd_hz = hz; }

/* ---------------- basico ---------------- */
static void pump(void)
{
    MSG m;
    while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    snd_pump();
}

int plat_init(void)
{
    WNDCLASSA wc;
    RECT r = { 0, 0, 960, 720 };
    BITMAPINFO bi;
    HDC dc;
    HINSTANCE inst = GetModuleHandle(NULL);
    timeBeginPeriod(1);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "CP5M80";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("CP5M80", "CP-500 M80   (F1 ajuda  F5 config  F9 discos  F10 reset  Alt+Enter tela cheia)",
                         WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!hwnd) return -1;
    dc = GetDC(hwnd);
    memdc = CreateCompatibleDC(dc);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&pix, NULL, 0);
    SelectObject(memdc, dib);
    ReleaseDC(hwnd, dc);
    menufont = CreateFontA(16, 8, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(memdc, menufont);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    snd_init();
    return 0;
}

void plat_shutdown(void)
{
    if (wo) { waveOutReset(wo); waveOutClose(wo); wo = 0; }
    timeEndPeriod(1);
    if (hwnd) DestroyWindow(hwnd);
}

unsigned long plat_ticks(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    {
        static LARGE_INTEGER t0;
        unsigned long long d;
        if (!t0.QuadPart) t0 = c;
        d = (unsigned long long)(c.QuadPart - t0.QuadPart);
        return (unsigned long)((d / f.QuadPart) * PLAT_HZ + (d % f.QuadPart) * PLAT_HZ / f.QuadPart);
    }
}

int plat_scancode(void)
{
    int v;
    if (kh == kt) pump();
    if (kh == kt) return -1;
    v = kq[kt];
    kt = (kt + 1) % KQ;
    return v;
}

void plat_idle(void)
{
    pump();
    Sleep(1);
}

int plat_quit_requested(void) { return quit_req; }

/* ---------------- video ---------------- */
static int emode;
void plat_emu_mode(uint8_t font[256][24], int color, int mode)
{
    int i;
    efont = font;
    emode = mode;
    fg_color = phosphor[color & 3];
    in_menu = 0;
    for (i = 0; i < W * H; i++) pix[i] = 0;
    led_st[0] = led_st[1] = -1;          /* redesenha os LEDs */
    InvalidateRect(hwnd, NULL, FALSE);
}

void plat_emu_draw(const uint8_t *vram, int mode, int wide)
{
    int r, c, y, b;
    if (mode) {                             /* 80x24, glifos 8x16 */
        for (r = 0; r < 24; r++)
            for (c = 0; c < 80; c++) {
                int g = vram[r * 80 + c];
                for (y = 0; y < 16; y++) {
                    uint8_t bits = efont[g][y];
                    uint32_t *p = pix + (8 + r * 16 + y) * W + c * 8;
                    for (b = 0; b < 8; b++) p[b] = (bits & (0x80 >> b)) ? fg_color : 0;
                }
            }
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    for (r = 0; r < 16; r++)
        for (c = 0; c < 64; c++) {
            int g = vram[r * 64 + c];
            if (wide) {                     /* modo 32 colunas: caracteres em dobro */
                if (c & 1) continue;
                for (y = 0; y < 24; y++) {
                    uint8_t bits = efont[g][y];
                    uint32_t *p = pix + (8 + r * 24 + y) * W + 64 + c * 8;
                    for (b = 0; b < 16; b++) p[b] = (bits & (0x80 >> (b / 2))) ? fg_color : 0;
                }
                continue;
            }
            for (y = 0; y < 24; y++) {
                uint8_t bits = efont[g][y];
                uint32_t *p = pix + (8 + r * 24 + y) * W + 64 + c * 8;
                for (b = 0; b < 8; b++) p[b] = (bits & (0x80 >> b)) ? fg_color : 0;
            }
        }
    InvalidateRect(hwnd, NULL, FALSE);
}

void plat_menu_mode(void)
{
    in_menu = 1;
    memset(screen, 0, sizeof screen);
}

void plat_put(int x, int y, int attr, const char *s)
{
    if (y < 0 || y >= 25) return;
    for (; *s && x < 80; s++, x++)
        if (x >= 0) screen[y * 80 + x] = (uint16_t)((attr << 8) | (unsigned char)*s);
}

void plat_fill(int x, int y, int w, int h, int attr, int ch)
{
    int i, j;
    for (j = y; j < y + h && j < 25; j++)
        for (i = x; i < x + w && i < 80; i++)
            if (i >= 0 && j >= 0) screen[j * 80 + i] = (uint16_t)((attr << 8) | ch);
}

static COLORREF rgb(uint32_t c) { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

void plat_present(void)
{
    if (in_menu) {
        int x, y;
        GdiFlush();
        for (y = 0; y < 25; y++)
            for (x = 0; x < 80; x++) {
                uint16_t v = screen[y * 80 + x];
                RECT rc = { x * 8, y * 16, x * 8 + 8, y * 16 + 16 };
                char ch = (char)(v & 0xFF);
                if (!ch) ch = ' ';
                SetBkColor(memdc, rgb(cga[(v >> 12) & 7]));
                SetTextColor(memdc, rgb(cga[(v >> 8) & 15]));
                ExtTextOutA(memdc, x * 8, y * 16, ETO_OPAQUE | ETO_CLIPPED, &rc, &ch, 1, NULL);
            }
        GdiFlush();
        InvalidateRect(hwnd, NULL, FALSE);
    }
    pump();
}

int plat_drives(char *list)
{
    DWORD m = GetLogicalDrives();
    int i, n = 0;
    for (i = 0; i < 26; i++) if (m & (1u << i)) list[n++] = (char)('A' + i);
    list[n] = 0;
    return n;
}

/* LEDs dos drives no canto superior direito (faixa preta acima da tela):
 * vermelho escuro apagado, vermelho vivo no acesso */
static void led_draw(int i, int on)
{
    int cx = 604 + i * 18, cy = 4, x, y;
    uint32_t c = on < 0 ? 0 : on ? 0xFF3030 : 0x501010, rim = on < 0 ? 0 : on ? 0xFF9090 : 0x301010;
    for (y = -3; y <= 3; y++)
        for (x = -4; x <= 4; x++) {
            int r2 = x * x + y * y * 2;
            if (r2 > 17) continue;
            pix[(cy + y) * W + cx + x] = (r2 > 11) ? rim : c;
        }
}
void plat_leds(int a, int b)
{
    RECT r;
    if (in_menu) { led_st[0] = led_st[1] = -1; return; }
    if (a == led_st[0] && b == led_st[1]) return;
    led_st[0] = a; led_st[1] = b;
    led_draw(0, a); led_draw(1, b);
    GetClientRect(hwnd, &r);
    InvalidateRect(hwnd, NULL, FALSE);
}
#endif
