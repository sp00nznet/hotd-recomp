/*
 * DirectDraw kept off the real screen. From Messiah's host (messiah-recomp
 * src/runtime/ddraw_window.c), which met it as DirectDraw 7.
 *
 * A fullscreen-exclusive DirectDraw game takes
 * the display (SetCooperativeLevel EXCLUSIVE|FULLSCREEN), changes its mode
 * (SetDisplayMode) and draws into a flipping primary chain. Each of those
 * reaches the player's real monitor -- under offstage it pulls the virtual
 * monitor out from under the recording, over RDP it lands on a phone -- so
 * by default none of them happen (REPO_RULES section 13):
 *
 *   SetCooperativeLevel  becomes DDSCL_NORMAL on the game's window.
 *   SetDisplayMode       is remembered, not done; GetDisplayMode answers it.
 *   the primary chain    becomes two offscreen surfaces of the remembered
 *                        mode and format, the "front" handed to the game and
 *                        the back buffer it renders into. Flip presents the
 *                        back buffer: GetDC on it (GDI converts any format),
 *                        into a 32-bit DIB, then to the window and/or ffmpeg.
 *
 * The hooks go into ddraw.dll's own vtables (shared by every object of the
 * class), set once from the DirectDrawCreateEx shim in host.c; a hook acts
 * only on the surfaces it made and passes everything else to the original.
 * --fullscreen leaves all of it alone. See docs/architecture.md.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "ddraw_window.h"

static int g_headless;
static const char* g_record;
static HWND g_hwnd;
static DWORD g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static IDirectDrawSurface7 *g_front, *g_back;
static volatile LONG g_frames;

static IDirectDraw7Vtbl g_dd;            /* the originals */
static IDirectDrawSurface7Vtbl g_surf;

/* ---- present: back buffer -> DIB -> window / ffmpeg ----------------------- */
static HDC g_dib_dc;
static HBITMAP g_dib;
static uint8_t* g_dib_bits;
static DWORD g_dib_w, g_dib_h;
static FILE* g_rec_pipe;
static SRWLOCK g_rec_lock = SRWLOCK_INIT;

static int dib_ensure(DWORD w, DWORD h) {
    if (g_dib && g_dib_w == w && g_dib_h == h) return 1;
    if (g_dib) DeleteObject(g_dib);
    if (!g_dib_dc) g_dib_dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB } };
    g_dib = CreateDIBSection(g_dib_dc, &bi, DIB_RGB_COLORS, (void**)&g_dib_bits, NULL, 0);
    if (!g_dib) return 0;
    SelectObject(g_dib_dc, g_dib);
    g_dib_w = w, g_dib_h = h;
    return 1;
}

/* ---- recording ------------------------------------------------------------------
 * A clock, not the game, sets the pace: a thread writes the last presented
 * frame 30 times a second, so a menu the game flips once still lasts as long
 * in the video as it did on screen. Every frame is scaled to one size, so the
 * game's mode changes (320x240 in play, 640x480 for movies) all land in it.
 * ponytail: 640x480 fixed, the largest mode Twisted Metal sets. */
#define REC_W 640
#define REC_H 480
static HDC g_rec_dc;
static uint8_t* g_rec_bits;

static DWORD WINAPI recorder(LPVOID unused) {
    (void)unused;
    DWORD next = GetTickCount();
    for (;;) {
        next += 33;
        int wait = (int)(next - GetTickCount());
        if (wait > 0) Sleep((DWORD)wait);
        AcquireSRWLockExclusive(&g_rec_lock);
        if (!g_rec_pipe) { ReleaseSRWLockExclusive(&g_rec_lock); return 0; }
        fwrite(g_rec_bits, 4, REC_W * REC_H, g_rec_pipe);
        ReleaseSRWLockExclusive(&g_rec_lock);
    }
}

/* An ffmpeg.exe next to the host wins over PATH: a test VM has none on PATH,
 * and the run folder is the one place a copy can be left. */
void ddw_ffmpeg(char* ff) {
    DWORD n = GetModuleFileNameA(NULL, ff, MAX_PATH);
    while (n && ff[n - 1] != '\\') n--;
    lstrcpynA(ff + n, "ffmpeg.exe", MAX_PATH - n);
    if (GetFileAttributesA(ff) == INVALID_FILE_ATTRIBUTES) lstrcpyA(ff, "ffmpeg");
}

static void record_start(void) {
    char ff[MAX_PATH], cmd[2 * MAX_PATH + 256];
    ddw_ffmpeg(ff);
    _snprintf(cmd, sizeof cmd - 1,
              "\"\"%s\" -v error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 30 -i - "
              "-pix_fmt yuv420p \"%s\"\"", ff, REC_W, REC_H, g_record);
    cmd[sizeof cmd - 1] = 0;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), REC_W, -REC_H, 1, 32, BI_RGB } };
    g_rec_dc = CreateCompatibleDC(NULL);
    HBITMAP b = CreateDIBSection(g_rec_dc, &bi, DIB_RGB_COLORS, (void**)&g_rec_bits, NULL, 0);
    if (b) SelectObject(g_rec_dc, b);
    g_rec_pipe = b ? _popen(cmd, "wb") : NULL;
    if (!g_rec_pipe) { fprintf(stderr, "[record] cannot start ffmpeg\n"); g_record = NULL; return; }
    printf("[record] %dx%d at 30 fps -> %s\n", REC_W, REC_H, g_record);
    CloseHandle(CreateThread(NULL, 0, recorder, NULL, 0, NULL));
}

/* The newly presented frame becomes the one the recorder writes. */
static void record_frame(void) {
    AcquireSRWLockExclusive(&g_rec_lock);
    if (g_rec_pipe) {
        SetStretchBltMode(g_rec_dc, COLORONCOLOR);   /* whole pixels: 320 -> 640 is exact */
        StretchBlt(g_rec_dc, 0, 0, REC_W, REC_H, g_dib_dc, 0, 0, g_dib_w, g_dib_h, SRCCOPY);
        GdiFlush();
    }
    ReleaseSRWLockExclusive(&g_rec_lock);
}

void ddw_close(void) {
    AcquireSRWLockExclusive(&g_rec_lock);
    if (g_rec_pipe) { _pclose(g_rec_pipe); g_rec_pipe = NULL; }
    g_record = NULL;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

/* While a movie plays (movie.c) its frames are the picture; the game's own
 * presents, a blank screen behind the movie window, are not shown. One lock
 * keeps the two from sharing the DIB at once. */
static volatile LONG g_movie;
static SRWLOCK g_present_lock = SRWLOCK_INIT;

/* The DIB holds a finished frame: to ffmpeg, and to the window. */
static void show(void) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented\n", n);
    if (g_record) record_frame();
    if (!g_headless && g_hwnd && IsWindowVisible(g_hwnd)) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        HDC wdc = GetDC(g_hwnd);
        SetStretchBltMode(wdc, HALFTONE);
        StretchBlt(wdc, 0, 0, rc.right, rc.bottom, g_dib_dc, 0, 0, g_dib_w, g_dib_h, SRCCOPY);
        ReleaseDC(g_hwnd, wdc);
    }
}

static void present(IDirectDrawSurface7* src) {
    if (g_movie) return;
    DDSURFACEDESC2 d = { sizeof d };
    if (FAILED(g_surf.GetSurfaceDesc(src, &d)) || !dib_ensure(d.dwWidth, d.dwHeight)) return;
    HDC sdc;
    if (FAILED(g_surf.GetDC(src, &sdc))) return;
    BitBlt(g_dib_dc, 0, 0, d.dwWidth, d.dwHeight, sdc, 0, 0, SRCCOPY);
    g_surf.ReleaseDC(src, sdc);
    show();
}

long ddw_frames(void) { return g_frames; }

void ddw_movie(int on) { InterlockedExchange(&g_movie, on); }

void ddw_movie_frame(const uint32_t* bgra, int w, int h) {
    AcquireSRWLockExclusive(&g_present_lock);
    if (dib_ensure(w, h)) {
        memcpy(g_dib_bits, bgra, (size_t)w * h * 4);
        show();
    }
    ReleaseSRWLockExclusive(&g_present_lock);
}

/* ---- every IDirectDraw version ---------------------------------------------
 * IDirectDraw, 2, 4 and 7 each have their own vtable, and the game reaches
 * more than one: DirectDrawCreate (v1, through GetProcAddress) to probe the
 * devices, DirectDrawCreateEx (v7) to render. All four share these slots, so
 * the same hooks go into each, with the originals kept per vtable. */
enum { SLOT_CREATE_SURFACE = 6, SLOT_ENUM_MODES = 8, SLOT_GET_MODE = 12, SLOT_RESTORE_MODE = 19,
       SLOT_COOP = 20, SLOT_SET_MODE = 21 };
#define VT(o) (*(void***)(o))
typedef HRESULT (STDMETHODCALLTYPE *coop_t)(void*, HWND, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *getmode_t)(void*, DDSURFACEDESC2*);
static struct { void** vt; coop_t coop; getmode_t getmode; } g_orig[8];
static int g_norig;

static int orig_of(void* o) {
    for (int i = 0; i < g_norig; i++) if (g_orig[i].vt == VT(o)) return i;
    return -1;
}

static HRESULT STDMETHODCALLTYPE dd_SetCooperativeLevel(void* dd, HWND w, DWORD flags) {
    int i = orig_of(dd);
    printf("[ddraw] SetCooperativeLevel(%p, 0x%lX) -> normal\n", (void*)w, flags);
    if (w) g_hwnd = w;
    DWORD keep = flags & (DDSCL_FPUSETUP | DDSCL_FPUPRESERVE | DDSCL_MULTITHREADED);
    return i < 0 ? DDERR_GENERIC : g_orig[i].coop(dd, w, DDSCL_NORMAL | keep);
}

static void remember_mode(DWORD w, DWORD h, DWORD bpp) {
    printf("[ddraw] SetDisplayMode %lux%lu %lubpp: kept off the display\n", w, h, bpp);
    g_mode_w = w, g_mode_h = h, g_mode_bpp = bpp;
    if (g_hwnd && !g_headless) {
        RECT r = { 0, 0, (LONG)w, (LONG)h };
        AdjustWindowRectEx(&r, GetWindowLongA(g_hwnd, GWL_STYLE), FALSE, GetWindowLongA(g_hwnd, GWL_EXSTYLE));
        SetWindowPos(g_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* IDirectDraw's takes three arguments, the later versions five. */
static HRESULT STDMETHODCALLTYPE dd1_SetDisplayMode(void* dd, DWORD w, DWORD h, DWORD bpp) {
    (void)dd;
    remember_mode(w, h, bpp);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE dd_SetDisplayMode(void* dd, DWORD w, DWORD h, DWORD bpp,
                                                   DWORD rate, DWORD flags) {
    (void)dd, (void)rate, (void)flags;
    remember_mode(w, h, bpp);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE dd_RestoreDisplayMode(void* dd) { (void)dd; return DD_OK; }

static void format_of(DDPIXELFORMAT* pf, DWORD bpp) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB | (bpp == 8 ? DDPF_PALETTEINDEXED8 : 0);
    pf->dwRGBBitCount = bpp;
    if (bpp == 16) pf->dwRBitMask = 0xF800, pf->dwGBitMask = 0x07E0, pf->dwBBitMask = 0x001F;
    else if (bpp > 16) pf->dwRBitMask = 0xFF0000, pf->dwGBitMask = 0x00FF00, pf->dwBBitMask = 0x0000FF;
}

static void mode_format(DDPIXELFORMAT* pf) { format_of(pf, g_mode_bpp); }

/* The modes a 1990s card offered. Windows 11 lists none below 640x480 and no
 * 8-bit ones, and a game that checks the list before SetDisplayMode (Twisted
 * Metal wants 320x240x8) gives up. No mode is ever set on the display here,
 * so every one of these is honest: the game gets a surface of that size and
 * format, and the host presents it. IDirectDraw and IDirectDraw2 only: their
 * callback takes a DDSURFACEDESC; 4 and 7 keep the real list. */
typedef HRESULT (WINAPI *enum_cb_t)(DDSURFACEDESC*, void*);
static HRESULT STDMETHODCALLTYPE dd1_EnumDisplayModes(void* dd, DWORD flags, DDSURFACEDESC* want,
                                                      void* ctx, enum_cb_t cb) {
    static const WORD sizes[][2] = { { 320, 200 }, { 320, 240 }, { 512, 384 }, { 640, 400 },
                                     { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    static const BYTE depths[] = { 8, 16, 32 };
    (void)dd, (void)flags;
    printf("[ddraw] EnumDisplayModes: the classic modes, 320x200 to 1024x768 at 8/16/32 bpp\n");
    for (int i = 0; i < (int)(sizeof sizes / sizeof sizes[0]); i++)
        for (int k = 0; k < (int)sizeof depths; k++) {
            DDSURFACEDESC d = { sizeof d };
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = sizes[i][0], d.dwHeight = sizes[i][1];
            d.lPitch = (LONG)(d.dwWidth * depths[k] / 8);
            d.dwRefreshRate = 60;
            format_of(&d.ddpfPixelFormat, depths[k]);
            if (want && (((want->dwFlags & DDSD_WIDTH) && want->dwWidth != d.dwWidth) ||
                         ((want->dwFlags & DDSD_HEIGHT) && want->dwHeight != d.dwHeight) ||
                         ((want->dwFlags & DDSD_PIXELFORMAT) &&
                          want->ddpfPixelFormat.dwRGBBitCount != depths[k])))
                continue;
            if (cb(&d, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    return DD_OK;
}

/* DDSURFACEDESC and DDSURFACEDESC2 agree up to the pixel format (0x48), which
 * is as far as this writes. */
static HRESULT STDMETHODCALLTYPE dd_GetDisplayMode(void* dd, DDSURFACEDESC2* d) {
    int i = orig_of(dd);
    HRESULT hr = i < 0 ? DDERR_GENERIC : g_orig[i].getmode(dd, d);
    if (SUCCEEDED(hr)) {
        d->dwWidth = g_mode_w, d->dwHeight = g_mode_h;
        d->lPitch = (LONG)(g_mode_w * (g_mode_bpp / 8));
        mode_format(&d->ddpfPixelFormat);
    }
    return hr;
}

static HRESULT offscreen(IDirectDraw7* dd, DWORD caps, IDirectDrawSurface7** out) {
    DDSURFACEDESC2 d = { sizeof d };
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = g_mode_w, d.dwHeight = g_mode_h;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | (caps & DDSCAPS_3DDEVICE);
    mode_format(&d.ddpfPixelFormat);
    return g_dd.CreateSurface(dd, &d, out, NULL);
}

static HRESULT STDMETHODCALLTYPE dd_CreateSurface(IDirectDraw7* dd, DDSURFACEDESC2* d,
                                                  IDirectDrawSurface7** out, IUnknown* outer) {
    if (!(d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) return g_dd.CreateSurface(dd, d, out, outer);
    DWORD caps = d->ddsCaps.dwCaps, backs = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0;
    HRESULT hr = offscreen(dd, caps | (backs ? 0 : DDSCAPS_3DDEVICE), out);
    if (SUCCEEDED(hr) && backs) hr = offscreen(dd, caps | DDSCAPS_3DDEVICE, &g_back);
    printf("[ddraw] primary (caps 0x%lX, %lu back buffers) -> offscreen %lux%lu %lubpp: 0x%08lX\n",
           caps, backs, g_mode_w, g_mode_h, g_mode_bpp, (unsigned long)hr);
    if (SUCCEEDED(hr)) g_front = *out;
    if (FAILED(hr) && *out) { g_surf.Release(*out); *out = NULL; }
    return hr;
}

/* ---- IDirectDrawSurface7 --------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE s_Flip(IDirectDrawSurface7* s, IDirectDrawSurface7* to, DWORD flags) {
    if (s != g_front) return g_surf.Flip(s, to, flags);
    present(g_back ? g_back : g_front);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE s_GetAttachedSurface(IDirectDrawSurface7* s, DDSCAPS2* caps,
                                                      IDirectDrawSurface7** out) {
    if (s == g_front && g_back && (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_3DDEVICE))) {
        g_surf.AddRef(g_back);
        *out = g_back;
        return DD_OK;
    }
    return g_surf.GetAttachedSurface(s, caps, out);
}

/* Without a back buffer the game draws straight onto the "primary" (movies,
 * 2D screens): each finished write to it is shown. */
static HRESULT STDMETHODCALLTYPE s_Blt(IDirectDrawSurface7* s, RECT* r, IDirectDrawSurface7* src,
                                       RECT* sr, DWORD flags, DDBLTFX* fx) {
    HRESULT hr = g_surf.Blt(s, r, src, sr, flags, fx);
    if (s == g_front && !g_back) present(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE s_BltFast(IDirectDrawSurface7* s, DWORD x, DWORD y,
                                           IDirectDrawSurface7* src, RECT* sr, DWORD flags) {
    HRESULT hr = g_surf.BltFast(s, x, y, src, sr, flags);
    if (s == g_front && !g_back) present(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE s_Unlock(IDirectDrawSurface7* s, RECT* r) {
    HRESULT hr = g_surf.Unlock(s, r);
    if (s == g_front && !g_back) present(s);
    return hr;
}

static ULONG STDMETHODCALLTYPE s_Release(IDirectDrawSurface7* s) {
    if (s == g_front && g_back) {
        ULONG n = g_surf.Release(s);
        if (!n) { g_surf.Release(g_back); g_back = NULL; g_front = NULL; }
        return n;
    }
    ULONG n = g_surf.Release(s);
    if (!n && s == g_front) g_front = NULL;
    return n;
}

/* ---- IDirectDraw (v1) surfaces ------------------------------------------------
 * The same stand-in for a DirectDraw 1 game (Twisted Metal): its primary
 * becomes offscreen surfaces of the remembered mode, in system memory because
 * an 8-bit surface is not something a modern card keeps in video memory.
 * GDI cannot be trusted to convert an 8-bit surface (it would use whatever
 * palette it finds), so the present reads the pixels and maps them through
 * the palette the game set on its primary, itself.
 *
 * Every finished write to the front is presented, flip chain or not: Twisted
 * Metal's menus fill the back buffer, Lock it to draw, and then copy it with
 * Blt(front <- back) instead of flipping. Presenting only on Flip showed 105
 * black frames in 40 s (docs/display.md). */
typedef HRESULT (STDMETHODCALLTYPE *create1_t)(IDirectDraw*, DDSURFACEDESC*, IDirectDrawSurface**, IUnknown*);
static create1_t g_create1;
static IDirectDrawSurfaceVtbl g_surf1;   /* the originals */
static IDirectDrawSurface *g_front1, *g_back1;
static IDirectDrawPalette* g_pal1;        /* what the game set on either */

static void present1_locked(IDirectDrawSurface* src);
static void present1(IDirectDrawSurface* src) {
    if (g_movie) return;
    AcquireSRWLockExclusive(&g_present_lock);
    present1_locked(src);
    ReleaseSRWLockExclusive(&g_present_lock);
}

static void present1_locked(IDirectDrawSurface* src) {
    DDSURFACEDESC d = { sizeof d };
    if (FAILED(g_surf1.Lock(src, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL))) return;
    if (dib_ensure(d.dwWidth, d.dwHeight)) {
        if (d.ddpfPixelFormat.dwRGBBitCount == 8) {
            PALETTEENTRY pe[256] = { 0 };
            uint32_t lut[256];
            if (g_pal1) IDirectDrawPalette_GetEntries(g_pal1, 0, 0, 256, pe);
            for (int i = 0; i < 256; i++) lut[i] = pe[i].peRed << 16 | pe[i].peGreen << 8 | pe[i].peBlue;
            DWORD lit = 0;
            for (DWORD y = 0; y < d.dwHeight; y++) {
                const uint8_t* in = (const uint8_t*)d.lpSurface + (size_t)y * d.lPitch;
                uint32_t* out = (uint32_t*)g_dib_bits + (size_t)y * g_dib_w;
                for (DWORD x = 0; x < d.dwWidth; x++) out[x] = lut[in[x]], lit += in[x] != 0;
            }
            if (g_frames < 3 || g_frames % 1000 == 0)    /* blank frames are told apart from none */
                printf("[ddraw] present %s: palette %p, %lu of %lu pixels non-zero\n",
                       src == g_back1 ? "back" : "front", (void*)g_pal1, lit, d.dwWidth * d.dwHeight);
            g_surf1.Unlock(src, NULL);
        } else {
            g_surf1.Unlock(src, NULL);
            HDC sdc;
            if (FAILED(g_surf1.GetDC(src, &sdc))) return;
            BitBlt(g_dib_dc, 0, 0, d.dwWidth, d.dwHeight, sdc, 0, 0, SRCCOPY);
            g_surf1.ReleaseDC(src, sdc);
        }
        show();
    } else {
        g_surf1.Unlock(src, NULL);
    }
}

static int ours1(IDirectDrawSurface* s) { return s && (s == g_front1 || s == g_back1); }

/* A real flip waits for the vertical blank, and the game is paced by it: its
 * menus count frames, not time. Without the wait it ran at about 660 frames a
 * second and the title menu timed out into the attract loop in 0.4 s. So a
 * frame the game finishes waits for the next 60 Hz tick, as on the monitor
 * it was made for. */
static void vblank(void) {
    static LARGE_INTEGER f, next;
    LARGE_INTEGER now;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    LONGLONG tick = f.QuadPart / 60;
    if (next.QuadPart < now.QuadPart - tick) next.QuadPart = now.QuadPart;   /* fell behind: no catching up */
    next.QuadPart += tick;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = next.QuadPart - now.QuadPart;
        if (left <= 0) break;
        Sleep(left * 1000 / f.QuadPart > 1 ? 1 : 0);
    }
}

static HRESULT STDMETHODCALLTYPE s1_Flip(IDirectDrawSurface* s, IDirectDrawSurface* to, DWORD flags) {
    if (s != g_front1) return g_surf1.Flip(s, to, flags);
    present1(g_back1 ? g_back1 : g_front1);
    vblank();
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE s1_GetAttachedSurface(IDirectDrawSurface* s, DDSCAPS* caps,
                                                       IDirectDrawSurface** out) {
    if (s == g_front1 && g_back1 && (caps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP))) {
        g_surf1.AddRef(g_back1);
        *out = g_back1;
        return DD_OK;
    }
    return g_surf1.GetAttachedSurface(s, caps, out);
}

static HRESULT STDMETHODCALLTYPE s1_Blt(IDirectDrawSurface* s, RECT* r, IDirectDrawSurface* src,
                                        RECT* sr, DWORD flags, DDBLTFX* fx) {
    HRESULT hr = g_surf1.Blt(s, r, src, sr, flags, fx);
    if (s == g_front1) present1(s);
    if (s == g_front1 && src && src == g_back1) vblank();   /* the menus' way of flipping */
    return hr;
}

static HRESULT STDMETHODCALLTYPE s1_BltFast(IDirectDrawSurface* s, DWORD x, DWORD y,
                                            IDirectDrawSurface* src, RECT* sr, DWORD flags) {
    HRESULT hr = g_surf1.BltFast(s, x, y, src, sr, flags);
    if (s == g_front1) present1(s);
    return hr;
}

static HRESULT STDMETHODCALLTYPE s1_Unlock(IDirectDrawSurface* s, void* p) {
    HRESULT hr = g_surf1.Unlock(s, p);
    if (s == g_front1) present1(s);
    return hr;
}

/* Kept by the host as well as passed on: an offscreen surface may refuse a
 * palette, and the present needs it either way. */
static HRESULT STDMETHODCALLTYPE s1_SetPalette(IDirectDrawSurface* s, IDirectDrawPalette* pal) {
    printf("[ddraw] SetPalette(%s %p, %p)\n", s == g_front1 ? "front" : s == g_back1 ? "back" : "other",
           (void*)s, (void*)pal);
    if (!ours1(s)) return g_surf1.SetPalette(s, pal);
    if (pal) IDirectDrawPalette_AddRef(pal);
    if (g_pal1) IDirectDrawPalette_Release(g_pal1);
    g_pal1 = pal;
    g_surf1.SetPalette(s, pal);
    return DD_OK;
}

static HRESULT STDMETHODCALLTYPE s1_GetPalette(IDirectDrawSurface* s, IDirectDrawPalette** out) {
    if (!ours1(s) || !g_pal1) return g_surf1.GetPalette(s, out);
    IDirectDrawPalette_AddRef(g_pal1);
    *out = g_pal1;
    return DD_OK;
}

static ULONG STDMETHODCALLTYPE s1_Release(IDirectDrawSurface* s) {
    ULONG n = g_surf1.Release(s);
    if (!n && s == g_front1) {
        if (g_back1) g_surf1.Release(g_back1);
        g_front1 = g_back1 = NULL;
    }
    return n;
}

static void patch_surface1_vtbl(IDirectDrawSurface* s) {
    static int done;
    if (done) return;
    IDirectDrawSurfaceVtbl* vt = s->lpVtbl;
    DWORD old;
    VirtualProtect(vt, sizeof *vt, PAGE_READWRITE, &old);
    g_surf1 = *vt;
    vt->Flip = s1_Flip;
    vt->GetAttachedSurface = s1_GetAttachedSurface;
    vt->Blt = s1_Blt;
    vt->BltFast = s1_BltFast;
    vt->Unlock = s1_Unlock;
    vt->SetPalette = s1_SetPalette;
    vt->GetPalette = s1_GetPalette;
    vt->Release = s1_Release;
    VirtualProtect(vt, sizeof *vt, old, &old);
    done = 1;
}

static HRESULT offscreen1(IDirectDraw* dd, IDirectDrawSurface** out) {
    DDSURFACEDESC d = { sizeof d };
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = g_mode_w, d.dwHeight = g_mode_h;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    mode_format(&d.ddpfPixelFormat);
    return g_create1(dd, &d, out, NULL);
}

static HRESULT STDMETHODCALLTYPE dd1_CreateSurface(IDirectDraw* dd, DDSURFACEDESC* d,
                                                   IDirectDrawSurface** out, IUnknown* outer) {
    if (!(d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) {
        /* A surface made without a pixel format takes the display's, and the
         * display never left the desktop's 32 bits: the game's 8-bit sprites
         * would be 32-bit surfaces, and a blit between them and the 8-bit
         * stand-ins fails. So it gets the mode the game asked for, in system
         * memory if the card will not hold that format. */
        DDSURFACEDESC c = *d;
        if (!(c.dwFlags & DDSD_PIXELFORMAT)) {
            c.dwFlags |= DDSD_PIXELFORMAT;
            mode_format(&c.ddpfPixelFormat);
        }
        HRESULT hr = g_create1(dd, &c, out, outer);
        if (FAILED(hr) && !(c.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            c.ddsCaps.dwCaps = (c.ddsCaps.dwCaps & ~DDSCAPS_VIDEOMEMORY) | DDSCAPS_SYSTEMMEMORY;
            hr = g_create1(dd, &c, out, outer);
        }
        static int logged;
        if (logged++ < 40 || FAILED(hr))
            printf("[ddraw] v1 surface %lux%lu caps 0x%lX%s -> %lu bpp, caps 0x%lX: 0x%08lX\n",
                   c.dwWidth, c.dwHeight, d->ddsCaps.dwCaps, (d->dwFlags & DDSD_PIXELFORMAT) ? "" : " (no format)",
                   c.ddpfPixelFormat.dwRGBBitCount, c.ddsCaps.dwCaps, (unsigned long)hr);
        return hr;
    }
    DWORD backs = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0;
    HRESULT hr = offscreen1(dd, out);
    if (SUCCEEDED(hr)) patch_surface1_vtbl(*out);
    if (SUCCEEDED(hr) && backs) hr = offscreen1(dd, &g_back1);
    printf("[ddraw] v1 primary (caps 0x%lX, %lu back buffers) -> offscreen %lux%lu %lubpp: 0x%08lX\n",
           d->ddsCaps.dwCaps, backs, g_mode_w, g_mode_h, g_mode_bpp, (unsigned long)hr);
    if (SUCCEEDED(hr)) g_front1 = *out;
    else if (*out) { IDirectDrawSurface_Release(*out); *out = NULL; }
    return hr;
}

/* ---- install --------------------------------------------------------------- */

static void patch_surface_vtbl(IDirectDrawSurface7* s) {
    static int done;
    if (done) return;
    IDirectDrawSurface7Vtbl* vt = s->lpVtbl;
    DWORD old;
    VirtualProtect(vt, sizeof *vt, PAGE_READWRITE, &old);
    g_surf = *vt;
    vt->Flip = s_Flip;
    vt->GetAttachedSurface = s_GetAttachedSurface;
    vt->Blt = s_Blt;
    vt->BltFast = s_BltFast;
    vt->Unlock = s_Unlock;
    vt->Release = s_Release;
    VirtualProtect(vt, sizeof *vt, old, &old);
    done = 1;
}

static void hook_dd_vtbl(void* obj, int v1, int v12, int v7) {
    void** vt = VT(obj);
    if (orig_of(obj) >= 0 || g_norig == 8) return;
    DWORD old;
    VirtualProtect(vt, 32 * sizeof(void*), PAGE_READWRITE, &old);
    g_orig[g_norig].vt = vt;
    g_orig[g_norig].coop = (coop_t)vt[SLOT_COOP];
    g_orig[g_norig].getmode = (getmode_t)vt[SLOT_GET_MODE];
    g_norig++;
    if (v7) {
        g_dd = *(IDirectDraw7Vtbl*)vt;
        vt[SLOT_CREATE_SURFACE] = (void*)dd_CreateSurface;
    }
    vt[SLOT_COOP] = (void*)dd_SetCooperativeLevel;
    vt[SLOT_SET_MODE] = v1 ? (void*)dd1_SetDisplayMode : (void*)dd_SetDisplayMode;
    vt[SLOT_RESTORE_MODE] = (void*)dd_RestoreDisplayMode;
    vt[SLOT_GET_MODE] = (void*)dd_GetDisplayMode;
    if (v12) vt[SLOT_ENUM_MODES] = (void*)dd1_EnumDisplayModes;
    if (v1) {
        g_create1 = (create1_t)vt[SLOT_CREATE_SURFACE];
        vt[SLOT_CREATE_SURFACE] = (void*)dd1_CreateSurface;
    }
    VirtualProtect(vt, 32 * sizeof(void*), old, &old);
}

/* A compatibility layer applied to this exe: DWM8And16BitMitigation, which
 * Windows adds by itself (HKLM, marked "$") to an exe that once asked
 * DirectDraw for a 16-bit mode, switched the mode of every monitor on each
 * launch. apphelp.dll sits in front of DirectDraw in every process, watching
 * for that request; a layer is what turns the mitigation on. */
static int layer_applied(char* what, size_t n) {
    char exe[MAX_PATH], env[512];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    if (GetEnvironmentVariableA("__COMPAT_LAYER", env, sizeof env) && env[0]) {
        _snprintf(what, n - 1, "__COMPAT_LAYER=%s", env);
        return 1;
    }
    static const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    for (int i = 0; i < 2; i++) {
        char val[512];
        DWORD len = sizeof val;
        if (RegGetValueA(roots[i], "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                         exe, RRF_RT_REG_SZ, NULL, val, &len) == ERROR_SUCCESS) {
            _snprintf(what, n - 1, "%s ...\\AppCompatFlags\\Layers: %s", i ? "HKLM" : "HKCU", val);
            return 1;
        }
    }
    return 0;
}

int ddw_install(int fullscreen) {
    char what[600] = "";
    if (layer_applied(what, sizeof what)) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        fprintf(stderr,
            "[ddraw] a compatibility layer is applied to %s\n"
            "        (%s). It changes every monitor's display mode; not running.\n"
            "        Delete that value (docs/display.md).\n", exe, what);
        return 0;
    }
    /* Through GetProcAddress, as the game gets them, so the objects made here
     * are the kind the game gets, apphelp's entry points included. */
    HMODULE dll = LoadLibraryA("ddraw.dll");
    typedef HRESULT (WINAPI *create_t)(GUID*, IDirectDraw**, IUnknown*);
    typedef HRESULT (WINAPI *create_ex_t)(GUID*, void**, REFIID, IUnknown*);
    create_t create = dll ? (create_t)GetProcAddress(dll, "DirectDrawCreate") : NULL;
    create_ex_t create_ex = dll ? (create_ex_t)GetProcAddress(dll, "DirectDrawCreateEx") : NULL;
    if (!create || !create_ex) { fprintf(stderr, "[ddraw] no DirectDraw\n"); return 0; }
    if (fullscreen) return 1;
    IDirectDraw* dd1 = NULL;
    if (FAILED(create(NULL, &dd1, NULL))) { fprintf(stderr, "[ddraw] DirectDrawCreate failed\n"); return 0; }
    static const IID* iids[] = { &IID_IDirectDraw2, &IID_IDirectDraw4, &IID_IDirectDraw7 };
    hook_dd_vtbl(dd1, 1, 1, 0);
    for (int i = 0; i < 3; i++) {
        void* o = NULL;
        if (FAILED(IDirectDraw_QueryInterface(dd1, iids[i], &o))) continue;
        hook_dd_vtbl(o, 0, i == 0, i == 2);
        if (i == 2) {               /* the surface vtable, through a throwaway surface */
            IDirectDrawSurface7* probe = NULL;
            DDSURFACEDESC2 d = { sizeof d };
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.dwWidth = d.dwHeight = 8;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            if (SUCCEEDED(g_dd.CreateSurface((IDirectDraw7*)o, &d, &probe, NULL))) {
                patch_surface_vtbl(probe);
                g_surf.Release(probe);
            }
        }
        IUnknown_Release((IUnknown*)o);
    }
    IDirectDraw_Release(dd1);
    void* dd7 = NULL;                   /* DirectDrawCreateEx's object, in case it differs */
    if (SUCCEEDED(create_ex(NULL, &dd7, &IID_IDirectDraw7, NULL))) {
        hook_dd_vtbl(dd7, 0, 0, 0);
        IUnknown_Release((IUnknown*)dd7);
    }
    printf("[ddraw] %d DirectDraw interfaces hooked: display mode and exclusive mode stay off the screen\n", g_norig);
    if (g_norig < 4) fprintf(stderr, "[ddraw] expected at least 4 interface vtables; not running\n");
    return g_norig >= 4;
}

/* ---- the backstop -------------------------------------------------------------
 * Every monitor's mode, read at start and every 20 ms after: if any changes,
 * the run ends there (Windows puts a mode back when the process that set it
 * exits). */
#define MAX_MON 16
static struct { char dev[32]; DWORD w, h, bpp, hz; } g_mon[MAX_MON];
static int g_nmon;

static int read_modes(int compare) {
    DISPLAY_DEVICEA dd = { sizeof dd };
    int n = 0;
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0) && n < MAX_MON; i++, dd.cb = sizeof dd) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEA m = { 0 };
        m.dmSize = sizeof m;
        if (!EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &m)) continue;
        if (compare) {
            if (n >= g_nmon || strcmp(g_mon[n].dev, dd.DeviceName) || g_mon[n].w != m.dmPelsWidth ||
                g_mon[n].h != m.dmPelsHeight || g_mon[n].bpp != m.dmBitsPerPel ||
                g_mon[n].hz != m.dmDisplayFrequency) {
                fprintf(stderr, "\n[guard] %s is now %lux%lu %lubpp %luHz: ending the run\n",
                        dd.DeviceName, m.dmPelsWidth, m.dmPelsHeight, m.dmBitsPerPel, m.dmDisplayFrequency);
                return 0;
            }
        } else {
            strncpy(g_mon[n].dev, dd.DeviceName, sizeof g_mon[n].dev - 1);
            g_mon[n].w = m.dmPelsWidth, g_mon[n].h = m.dmPelsHeight;
            g_mon[n].bpp = m.dmBitsPerPel, g_mon[n].hz = m.dmDisplayFrequency;
        }
        n++;
    }
    if (!compare) g_nmon = n;
    return !compare || n == g_nmon;
}

static DWORD WINAPI guard(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(20);
        if (!read_modes(1)) {
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 5);
        }
    }
}

void ddw_guard_start(void) {
    read_modes(0);
    printf("[guard] %d monitors watched: any mode change ends the run\n", g_nmon);
    CloseHandle(CreateThread(NULL, 0, guard, NULL, 0, NULL));
}

void ddw_init(int headless, const char* record) {
    g_headless = headless;
    g_record = record;
    if (g_record) record_start();
}
