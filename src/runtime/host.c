/*
 * The House of the Dead - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). Every import goes to the real DLL --
 * DirectDraw, DirectSound, DirectInput, winmm -- so what is here is only what
 * is specific to this game: where the image goes, the command line, the calls
 * that would reach outside the game's own window, the light gun, muting, and
 * the fault report. Taken from Twisted Metal's host (twistedmetal-recomp
 * src/runtime/host.c), itself from Messiah's.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's
 * 0x00400000..0x013D1600 is free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "ddraw_window.h"
#include "menu.h"
#include "net.h"
#include "net.h"

extern const uint32_t hotd_entry_va;  /* recomp_dispatch.c */

#define HOTD_IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_headless;
static HWND  g_game_hwnd;   /* the game's first top-level window: --pad posts keys to it */
static int   g_lockstep;    /* an online session runs in this game (hotd_net_gate) */

/* Seconds since the host started: the same clock as a --record video. */
static double run_seconds(void) {
    static DWORD t0;
    if (!t0) t0 = GetTickCount();
    return (GetTickCount() - t0) / 1000.0;
}
static int   g_fullscreen;   /* --fullscreen: let DirectDraw take the display */
static char  g_record[MAX_PATH];   /* --record out.mp4 */

/* ---- the process is the host, the module is the guest ----------------------
 * GetModuleHandleA(NULL) and GetModuleFileNameA(NULL) would name the host;
 * the game finds its own folder through them. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 512];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0))
                   : HOTD_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == HOTD_IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* ---- window calls ---------------------------------------------------------
 * A shim runs holding the machine, and a window call can wait on another
 * thread answering messages. So the call runs with the machine let go, as
 * native32's bridge does for every unshimmed import. Arguments are read
 * first; esp and the last error are put back. */
#define UNLOCKED(call) do { uint32_t esp_ = g_esp; mach_leave(); \
    call; DWORD err_ = GetLastError(); mach_enter(); \
    g_esp = esp_; SetLastError(err_); } while (0)

static WNDPROC g_game_proc;

static LRESULT CALLBACK host_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT r;
    if (menu_wndproc(h, m, w, l, &r)) return r;
    /* Online, a game the other PC can't see must not change: focus comes and
     * goes on one PC only, and the keyboard and mouse reach the game as the
     * frame's inputs (hotd_net_gate), never as messages. */
    if (g_lockstep) {
        switch (m) {
        case WM_ACTIVATEAPP: case WM_NCACTIVATE: w = TRUE; break;
        case WM_ACTIVATE: w = MAKEWPARAM(WA_ACTIVE, HIWORD(w)); break;
        case WM_KILLFOCUS: return 0;
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSCHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL:
            return DefWindowProcA(h, m, w, l);
        }
    }
    return CallWindowProcA(g_game_proc, h, m, w, l);
}

static void shim_CreateWindowExA(void) {
    uint32_t style = ARG(3);
    if (g_headless) style &= ~WS_VISIBLE;
    uint32_t a[12];
    for (int i = 0; i < 12; i++) a[i] = ARG(i);
    HWND h;
    UNLOCKED(h = CreateWindowExA(a[0], (LPCSTR)(uintptr_t)a[1], (LPCSTR)(uintptr_t)a[2], style,
                                 a[4], a[5], a[6], a[7], (HWND)(uintptr_t)a[8],
                                 (HMENU)(uintptr_t)a[9], (HINSTANCE)(uintptr_t)a[10], (LPVOID)(uintptr_t)a[11]));
    DWORD err = h ? 0 : GetLastError();
    if (h && !a[8] && !g_game_hwnd) {
        g_game_hwnd = h;
        /* The F10 menu's keys reach the host first (menu.c). The game's own
         * procedure is guest code: Windows' call to it faults on the
         * non-executable page and native32 runs the lifted body. */
        g_game_proc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)host_wndproc);
        net_set_window(h);
    }
    printf("[host] CreateWindowExA(\"%s\", %dx%d) -> %p (error %lu)\n",
           ARG(2) && HIWORD(ARG(2)) ? (const char*)(uintptr_t)ARG(2) : "", (int)ARG(6), (int)ARG(7), (void*)h, err);
    SetLastError(err);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ShowWindow(void) {
    HWND w = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    g_eax = 0;
    if (!g_headless) UNLOCKED(g_eax = ShowWindow(w, cmd));
    /* A hidden window is never activated; headless, it gets the activation
     * it would have had on screen. Posted, so nobody waits on anybody. */
    else if (cmd != SW_HIDE) PostMessageA(w, WM_ACTIVATEAPP, TRUE, 0);
    g_esp += 4 + 2 * 4;
}

static void shim_SetWindowPos(void) {
    UINT flags = ARG(6);
    if (g_headless) flags = (flags | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW;
    uint32_t a[6];
    for (int i = 0; i < 6; i++) a[i] = ARG(i);
    BOOL r;
    UNLOCKED(r = SetWindowPos((HWND)(uintptr_t)a[0], (HWND)(uintptr_t)a[1], (int)a[2], (int)a[3],
                              (int)a[4], (int)a[5], flags));
    g_eax = r;
    g_esp += 4 + 7 * 4;
}

static void shim_SetWindowLongA(void) {
    LONG v = (LONG)ARG(2);
    if (g_headless && (int)ARG(1) == GWL_STYLE) v &= ~WS_VISIBLE;
    if (g_headless && (int)ARG(1) == GWL_EXSTYLE) v &= ~WS_EX_TOPMOST;
    HWND w = (HWND)(uintptr_t)ARG(0);
    int idx = (int)ARG(1);
    LONG r;
    UNLOCKED(r = SetWindowLongA(w, idx, v));
    g_eax = (uint32_t)r;
    g_esp += 4 + 3 * 4;
}

static void shim_MessageBoxA(void) {
    const char* text = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
    const char* cap = ARG(2) ? (const char*)(uintptr_t)ARG(2) : "";
    printf("[host] MessageBoxA \"%s\": %s\n", cap, text);
    fflush(stdout);
    if (g_headless) {
        /* The answer that ends the question: retrying a CD prompt headless
         * would ask forever. */
        uint32_t b = ARG(3) & MB_TYPEMASK;
        g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO
              : (b == MB_RETRYCANCEL || b == MB_OKCANCEL) ? IDCANCEL
              : b == MB_ABORTRETRYIGNORE ? IDABORT : IDOK;
    } else {
        HWND w = (HWND)(uintptr_t)ARG(0);
        UINT type = ARG(3);
        int r;
        UNLOCKED(r = MessageBoxA(w, text, cap, type));
        g_eax = r;
    }
    g_esp += 4 + 4 * 4;
}

/* The game ends itself with ExitProcess, which skips the host's way out. */
static void shim_ExitProcess(void) {
    UINT code = ARG(0);
    printf("[host] ExitProcess(%u) from sub_%08X\n", code, g_cur_func);
    fflush(stdout);
    recomp_trace_flush();
    ddw_close();
    ExitProcess(code);
}

static void shim_TerminateProcess(void) {
    HANDLE h = (HANDLE)(uintptr_t)ARG(0);
    UINT code = ARG(1);
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        printf("[host] TerminateProcess(self, %u) from sub_%08X\n", code, g_cur_func);
        fflush(stdout);
        native32_dump_icalls(8);
        ddw_close();
    }
    g_eax = TerminateProcess(h, code);
    g_esp += 4 + 2 * 4;
}

/* ---- the CD ------------------------------------------------------------------
 * If the game looks for its disc by drive type, the host makes one free
 * letter a CD-ROM whose root is the game folder; paths on it are redirected
 * in every import that takes one. Real CD drives are hidden, so a disc left
 * in one is never preferred to the game folder. */
static char g_game_dir[MAX_PATH];
static char g_cd = 0;                    /* the virtual drive's letter */

/* "X:\rest" or "X:" on the virtual drive -> "<game dir>\rest"; else a copy. */
static const char* cd_path(const char* in, char* out, size_t n) {
    if (in && g_cd && (in[0] & ~0x20) == g_cd && in[1] == ':') {
        const char* rest = in + 2;
        while (*rest == '\\' || *rest == '/') rest++;
        _snprintf(out, n - 1, "%s\\%s", g_game_dir, rest);
        out[n - 1] = 0;
        return out;
    }
    return in;
}

/* Every "X:\" in an MCI command string, rewritten the same way. */
static const char* cd_mci_string(const char* in, char* out, size_t n) {
    size_t o = 0, gl = strlen(g_game_dir);
    for (const char* p = in; *p && o + 1 < n; p++) {
        int word = p == in || p[-1] == ' ' || p[-1] == '"';
        int on_cd = word && g_cd && (p[0] & ~0x20) == g_cd && p[1] == ':';
        if (on_cd && o + gl + 1 < n) {   /* no room: left as it was */
            memcpy(out + o, g_game_dir, gl);
            o += gl;
            p++;                         /* "X:" dropped; the '\' after it is kept */
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
    return out;
}

static void cd_pick(const char* game_dir) {
    lstrcpynA(g_game_dir, game_dir, MAX_PATH);
    DWORD used = GetLogicalDrives();
    for (char c = 'D'; c <= 'Z' && !g_cd; c++)
        if (!(used & (1u << (c - 'A')))) g_cd = c;
    printf("  CD drive: %c: -> %s\n", g_cd ? g_cd : '?', g_game_dir);
}

static void shim_GetDriveTypeA(void) {
    const char* root = (const char*)(uintptr_t)ARG(0);
    UINT t = GetDriveTypeA(root);
    if (root && g_cd && (root[0] & ~0x20) == g_cd) t = DRIVE_CDROM;
    else if (t == DRIVE_CDROM) t = DRIVE_NO_ROOT_DIR;
    g_eax = t;
    g_esp += 4 + 1 * 4;
}

/* The game's file opens, logged: they are the milestones a run is read by. */
static void shim_CreateFileA(void) {
    char buf[MAX_PATH * 2];
    const char* in = (const char*)(uintptr_t)ARG(0);
    /* The CD check (sub_0041196C) takes the first CD-ROM drive whose root has
     * no thotd.exe, the retail disc's layout; the drive that has one is the
     * hard disk install. The virtual CD is the game folder, so its root
     * answers that it has none. */
    if (in && g_cd && (in[0] & ~0x20) == g_cd && !_stricmp(in + 2, "\\thotd.exe")) {
        printf("[host] %6.1fs open %s -> hidden (the CD check)\n", run_seconds(), in);
        SetLastError(ERROR_FILE_NOT_FOUND);
        g_eax = (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE;
        g_esp += 4 + 7 * 4;
        return;
    }
    /* Online, player 2 plays with the host's settings (net.c sends them):
     * its own files are left alone and it reads a copy of the host's. */
    int len;
    const char* base = in ? max(strrchr(in, '\\'), strrchr(in, '/')) : NULL;
    base = base ? base + 1 : in;
    const uint8_t* session = g_lockstep && !net_is_host() && base ? net_session_file(base, &len) : NULL;
    if (session) {
        _snprintf(buf, sizeof buf - 1, "%s\\%s.online", g_game_dir, base);
        buf[sizeof buf - 1] = 0;
        static int written[2];
        int k = !_stricmp(base, "cstats.cfg");
        if (!written[k]) {
            FILE* f = fopen(buf, "wb");
            if (f) fwrite(session, 1, len, f), fclose(f);
            written[k] = 1;
        }
        in = buf;
    }
    const char* name = cd_path(in, buf, sizeof buf);
    g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    printf("[host] %6.1fs open %s -> %s\n", run_seconds(), name ? name : "(null)",
           g_eax == (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE ? "missing" : "ok");
    g_esp += 4 + 7 * 4;
}

static void shim_DeleteFileA(void) {
    char buf[MAX_PATH * 2];
    g_eax = DeleteFileA(cd_path((const char*)(uintptr_t)ARG(0), buf, sizeof buf));
    g_esp += 4 + 1 * 4;
}

/* MCI can wait for a device or post to the game's window, so it runs unlocked. */
static void shim_mciSendStringA(void) {
    char buf[1024];
    const char* cmd = cd_mci_string((const char*)(uintptr_t)ARG(0), buf, sizeof buf);
    char* ret = (char*)(uintptr_t)ARG(1);
    UINT n = ARG(2);
    HWND cb = (HWND)(uintptr_t)ARG(3);
    MCIERROR r;
    UNLOCKED(r = mciSendStringA(cmd, ret, n, cb));
    printf("[mci] \"%s\" -> %lu%s%s\n", cmd, (unsigned long)r, ret && n && ret[0] ? ": " : "", ret && n ? ret : "");
    g_eax = r;
    g_esp += 4 + 4 * 4;
}

/* ---- gamebios.dll: Sega's light gun ----------------------------------------
 * The game imports four calls from Sega's gun driver, all cdecl. There is no
 * gun: gbInitDeviceDriver answers -1, as the driver does when it finds none,
 * and the others answer 0. */
static void shim_gbInitDeviceDriver(void) {
    printf("[gun] gbInitDeviceDriver -> no gun\n");
    g_eax = 0xFFFFFFFFu;
    g_esp += 4;
}
static void shim_gbl_zero(void) { g_eax = 0; g_esp += 4; }

/* ---- DirectInput --------------------------------------------------------------
 * DirectInputCreateA checks that its HINSTANCE is a module Windows loaded;
 * the game's is the guest image's base, so it gets the host's (as Messiah's,
 * Shadows' and Bunghole's hosts do).
 *
 * The game reads the keyboard through DirectInput, so the input mailbox's keys
 * are merged into what its devices return (from Bunghole's host). Headless,
 * the window is hidden and never in the foreground, so a device asks to read
 * in the background, and only the mailbox's keys are seen. */
static volatile uint8_t g_pad_down[256]; /* the keys the mailbox holds now (input mailbox below) */
static volatile LONG g_cur_x = 320, g_cur_y = 240;   /* the mailbox's cursor, in the picture */
static volatile LONG g_mouse_step;                   /* 2: a move to g_cur_x/y is waiting */

typedef HRESULT (WINAPI *di_createdev_t)(void*, const GUID*, void**, void*);
typedef HRESULT (WINAPI *did_coop_t)(void*, HWND, DWORD);
typedef HRESULT (WINAPI *did_getstate_t)(void*, DWORD, void*);
typedef HRESULT (WINAPI *did_getdata_t)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
static di_createdev_t g_di_createdev;
static did_coop_t g_did_coop;
static did_getstate_t g_did_getstate;
static did_getdata_t g_did_getdata;

static void patch_slot(void* obj, int slot, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[slot] == hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

/* A virtual-key code's DirectInput key (DIK_*), which is its scan code. */
static int dik_of(int vk) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC_EX);
    return (sc & 0xFF) | ((sc & 0xFF00) == 0xE000 ? 0x80 : 0);
}

static HRESULT WINAPI hk_did_coop(void* self, HWND w, DWORD flags) {
    if (g_headless) flags = (flags & ~(DISCL_FOREGROUND | DISCL_EXCLUSIVE)) | DISCL_BACKGROUND | DISCL_NONEXCLUSIVE;
    return g_did_coop(self, w, flags);
}

/* The mailbox's mouse: "m X Y" is two polls, far up-left into the corner,
 * then X, Y from there, since the game moves its cursor by relative motion.
 * ponytail: assumes one count of motion is one pixel; scale here if not. */
static void mailbox_mouse(DIMOUSESTATE* m) {
    LONG step = InterlockedExchange(&g_mouse_step, g_mouse_step ? g_mouse_step - 1 : 0);
    if (step == 2) m->lX = -4000, m->lY = -4000;
    if (step == 1) m->lX = g_cur_x, m->lY = g_cur_y;
    m->rgbButtons[0] |= g_pad_down[VK_LBUTTON] ? 0x80 : 0;
    m->rgbButtons[1] |= g_pad_down[VK_RBUTTON] ? 0x80 : 0;
}

static void mailbox_keys(uint8_t* kb) {
    for (int vk = 3; vk < 256; vk++)                 /* past the mouse buttons */
        if (g_pad_down[vk]) kb[dik_of(vk)] = 0x80;
}

/* ---- online play: the lockstep's side in the game ------------------------------
 * net.c moves the inputs; this is where the game meets them (research and
 * reasoning in docs/multiplayer.md):
 *
 *   the gate    run_lift.py patches the top of the input poller, sub_00578100,
 *               which runs once per frame after the game's logic: hotd_net_gate
 *               samples this PC's mouse and keyboard, and net_frame returns once
 *               both players' inputs for the frame are here.
 *   the poller  then reads DirectInput as always, and the hooks answer with
 *               the frame's inputs: the keys either player holds, and player
 *               1's mouse. So both PCs' games compute the same state from it.
 *   player 2    has a mouse of its own: a shadow of the poller's mouse block
 *               (0x012FDB80, eight dwords), kept by the same rules, swapped in
 *               around player 2's reader (sub_0043591C from 0x00436678) and its
 *               start check (0x0041914C). Both players are set to Mouse.
 *   the clock   timeGetTime is the real one until the game's frame timer first
 *               asks (sub_00578930, after the boot's display benchmark), then a
 *               shared one from the host's base: 1 ms a read, and at least
 *               17 ms a frame. */
static void* g_kbd_dev, *g_mouse_dev;    /* the game's devices, for sampling */
static net_input_t g_both[2];           /* the frame's inputs */
static uint32_t g_p2m[8];               /* player 2's mouse block */
static uint32_t g_p1m_saved[8];
static int g_p2_swapped;
#define MOUSE_BLOCK 0x012FDB80u
#define P2_RECORD   (0x00D9F058u + 0xCCu)

static int g_vclock_on;                 /* the shared clock has begun */
static uint32_t g_vclock, g_vclock_f0;

static uint32_t now_ms(int tick_count) {
    if (!g_lockstep) return tick_count ? GetTickCount() : timeGetTime();
    if (!g_vclock_on && g_cur_func == 0x00578930u) {
        g_vclock_on = 1;
        g_vclock = net_clock_base();
        fprintf(stderr, "[net] the shared clock starts at %u\n", g_vclock);
    }
    return g_vclock_on ? ++g_vclock : (tick_count ? GetTickCount() : timeGetTime());
}

static void shim_timeGetTime(void) { g_eax = now_ms(0); g_esp += 4; }
static void shim_GetTickCount(void) { g_eax = now_ms(1); g_esp += 4; }

static void pack_input(net_input_t* in, const uint8_t* kb, const DIMOUSESTATE* m) {
    memset(in, 0, sizeof *in);
    in->dx = (int16_t)max(-32768, min(32767, m->lX));
    in->dy = (int16_t)max(-32768, min(32767, m->lY));
    for (int b = 0; b < 3; b++) in->buttons |= (m->rgbButtons[b] & 0x80) ? 1 << b : 0;
    for (int k = 1; k < 256 && in->nkeys < NET_KEYS; k++)
        if (kb[k] & 0x80) in->keys[in->nkeys++] = (uint8_t)k;
}

/* The poller's mouse rules (sub_00578100 from 0x0057827D), for player 2's block. */
static void mouse_block_step(uint32_t* b, const net_input_t* in) {
    b[3] = b[0];                                            /* previous buttons */
    b[0] = in->buttons;
    b[6] = (uint32_t)(int32_t)in->dx, b[7] = (uint32_t)(int32_t)in->dy;
    int32_t x = (int32_t)b[4] + in->dx, y = (int32_t)b[5] + in->dy;
    int32_t w = (int32_t)MEM32(0x00C64A3Cu), h = (int32_t)MEM32(0x00C64A40u);
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    b[4] = (uint32_t)x, b[5] = (uint32_t)y;
    uint32_t changed = b[0] ^ b[3];
    b[1] = changed & b[0], b[2] = changed & b[3];         /* presses, releases */
}

void hotd_net_gate(void) {
    if (!g_lockstep) return;
    uint8_t kb[256] = { 0 };
    DIMOUSESTATE m = { 0 };
    if (!g_headless && GetForegroundWindow() == g_game_hwnd) {   /* only a player at this window plays */
        if (g_kbd_dev && FAILED(g_did_getstate(g_kbd_dev, sizeof kb, kb))) memset(kb, 0, sizeof kb);
        if (g_mouse_dev && FAILED(g_did_getstate(g_mouse_dev, sizeof m, &m))) memset(&m, 0, sizeof m);
    }
    mailbox_keys(kb);
    mailbox_mouse(&m);
    net_input_t mine;
    pack_input(&mine, kb, &m);
    int32_t f = net_frame_no();
    if (!net_frame(&mine, g_both)) {                       /* the session ended: play on alone */
        memset(g_both, 0, sizeof g_both);
        g_both[net_seat()] = mine;
    }
    if (f == 0) memcpy(g_p2m, (void*)(uintptr_t)MOUSE_BLOCK, sizeof g_p2m);   /* player 2 starts where 1 does */
    mouse_block_step(g_p2m, &g_both[1]);
    MEM32(0x00D5B200u) = 0, MEM32(0x00D5B204u) = 0;         /* both players: Mouse */
    menu_apply_cheats();                                    /* the host's, on the same frame on both */
    if (g_vclock_on) {
        if (f == 0) g_vclock_f0 = g_vclock;
        if (g_vclock < g_vclock_f0 + 17u * (uint32_t)f) g_vclock = g_vclock_f0 + 17u * (uint32_t)f;
    }
    /* The desync check: what both games' logic computed from the same inputs,
     * the two players' input records and player records (aim, life, ammo,
     * score) and the game's own random seed. */
    uint32_t hsh = 2166136261u;
    for (uint32_t a = 0x00D9F5E0u; a < 0x00D9F5E0u + 2 * 0x1C; a++) hsh = (hsh ^ MEM8(a)) * 16777619u;
    for (uint32_t a = 0x00D9F058u; a < 0x00D9F058u + 2 * 0xCC; a++) {
        uint32_t o = (a - 0x00D9F058u) % 0xCC;      /* +0xA8, +0xC0: pointers into each PC's own heap */
        if ((o >= 0xA8 && o < 0xAC) || (o >= 0xC0 && o < 0xC4)) continue;
        hsh = (hsh ^ MEM8(a)) * 16777619u;
    }
    hsh = (hsh ^ MEM32(0x00DA10F0u)) * 16777619u;
    net_set_hash(hsh);
}

/* Around player 2's reader and start check (run_lift.py PATCHES). */
void hotd_p2_in(uint32_t record) {
    if (!g_lockstep || record != P2_RECORD || g_p2_swapped) return;
    memcpy(g_p1m_saved, (void*)(uintptr_t)MOUSE_BLOCK, sizeof g_p1m_saved);
    memcpy((void*)(uintptr_t)MOUSE_BLOCK, g_p2m, sizeof g_p2m);
    g_p2_swapped = 1;
}

void hotd_p2_out(void) {
    if (!g_p2_swapped) return;
    memcpy(g_p2m, (void*)(uintptr_t)MOUSE_BLOCK, sizeof g_p2m);   /* what the reader left, kept */
    memcpy((void*)(uintptr_t)MOUSE_BLOCK, g_p1m_saved, sizeof g_p1m_saved);
    g_p2_swapped = 0;
}

static HRESULT WINAPI hk_did_getstate(void* self, DWORD cb, void* data) {
    static DWORD sizes[4];
    for (int i = 0; i < 4 && sizes[i] != cb; i++)
        if (!sizes[i]) { sizes[i] = cb; printf("[input] the game polls GetDeviceState (%lu bytes)\n", cb); break; }
    if (g_lockstep) {                                       /* the frame's inputs, the same on both PCs */
        memset(data, 0, cb);
        if (cb == 256)
            for (int p = 0; p < 2; p++)
                for (int k = 0; k < g_both[p].nkeys; k++) ((uint8_t*)data)[g_both[p].keys[k]] = 0x80;
        if (cb == sizeof(DIMOUSESTATE) || cb == 20) {
            DIMOUSESTATE* m = (DIMOUSESTATE*)data;
            m->lX = g_both[0].dx, m->lY = g_both[0].dy;
            for (int b = 0; b < 3; b++) m->rgbButtons[b] = (g_both[0].buttons >> b & 1) ? 0x80 : 0;
        }
        return DI_OK;
    }
    HRESULT hr = g_did_getstate(self, cb, data);
    if (g_headless || FAILED(hr)) memset(data, 0, cb), hr = DI_OK;   /* the real devices stay out */
    if (cb == sizeof(DIMOUSESTATE) || cb == 20) mailbox_mouse((DIMOUSESTATE*)data);
    if (cb == 256) mailbox_keys((uint8_t*)data);
    return hr;
}

/* Buffered reads: a mailbox key's press and release, as the records a real
 * keyboard would have queued. Online the game gets none: every input is the
 * frame's (above). */
static HRESULT WINAPI hk_did_getdata(void* self, DWORD cb, DIDEVICEOBJECTDATA* d, DWORD* n, DWORD f) {
    static uint8_t seen[256];
    static void* devs[4];
    for (int i = 0; i < 4 && devs[i] != self; i++)
        if (!devs[i]) { devs[i] = self; printf("[input] device %p polls GetDeviceData (%lu-byte records)\n", self, cb); break; }
    if (g_lockstep) { if (n) *n = 0; return DI_OK; }
    DWORD room = n ? *n : 0;
    HRESULT hr = g_did_getdata(self, cb, d, n, f);
    if ((g_headless || FAILED(hr)) && n) *n = 0, hr = DI_OK;
    if (!d || !n || (f & DIGDD_PEEK)) return hr;
    for (int vk = 3; vk < 256 && *n < room; vk++) {
        if (seen[vk] == g_pad_down[vk]) continue;
        seen[vk] = g_pad_down[vk];
        DIDEVICEOBJECTDATA* r = (DIDEVICEOBJECTDATA*)((uint8_t*)d + *n * cb);
        memset(r, 0, cb);
        r->dwOfs = dik_of(vk);
        r->dwData = seen[vk] ? 0x80 : 0;
        r->dwTimeStamp = GetTickCount();
        (*n)++;
    }
    return hr;
}

static HRESULT WINAPI hk_di_createdev(void* self, const GUID* g, void** dev, void* outer) {
    HRESULT hr = g_di_createdev(self, g, dev, outer);
    if (hr == 0 && *dev) {
        if (IsEqualGUID(g, &GUID_SysKeyboard)) g_kbd_dev = *dev;
        if (IsEqualGUID(g, &GUID_SysMouse)) g_mouse_dev = *dev;
        patch_slot(*dev, 13, (void*)hk_did_coop, (void**)&g_did_coop);
        patch_slot(*dev, 9, (void*)hk_did_getstate, (void**)&g_did_getstate);
        patch_slot(*dev, 10, (void*)hk_did_getdata, (void**)&g_did_getdata);
    }
    return hr;
}

static void shim_DirectInputCreateA(void) {
    typedef HRESULT (WINAPI *dicreate_t)(HINSTANCE, DWORD, void**, void*);
    static dicreate_t real;
    if (!real) real = (dicreate_t)GetProcAddress(LoadLibraryA("dinput.dll"), "DirectInputCreateA");
    HINSTANCE h = native32_in_guest(ARG(0)) ? GetModuleHandleA(NULL) : (HINSTANCE)(uintptr_t)ARG(0);
    void** out = (void**)(uintptr_t)ARG(2);
    g_eax = (uint32_t)real(h, ARG(1), out, (void*)(uintptr_t)ARG(3));
    if (g_eax == 0 && out && *out) patch_slot(*out, 3, (void*)hk_di_createdev, (void**)&g_di_createdev);
    printf("[host] DirectInputCreateA(version 0x%X) -> 0x%08X\n", ARG(1), g_eax);
    g_esp += 4 + 4 * 4;
}

/* The game's one-instance check is the global atom "TheHouseOfTheDead"
 * (sub_0041859C): found means another copy runs, so it quits. A global atom
 * outlives its process, and a run that ends in a fault never deletes it: every
 * later run quit at once. The process's own atom table answers instead. */
static void shim_GlobalAddAtomA(void) {
    g_eax = AddAtomA((LPCSTR)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}
static void shim_GlobalFindAtomA(void) {
    g_eax = FindAtomA((LPCSTR)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}
static void shim_GlobalDeleteAtom(void) {
    g_eax = DeleteAtom((ATOM)ARG(0));
    g_esp += 4 + 1 * 4;
}

/* Its other one-instance checks: FindWindowA for another copy's window (found,
 * it quits at once) and a named mutex. Two copies on one PC are how online
 * play is tried out, so each sees only its own windows and its own mutexes. */
static void shim_FindWindowA(void) {
    HWND w = FindWindowA((LPCSTR)(uintptr_t)ARG(0), (LPCSTR)(uintptr_t)ARG(1));
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    g_eax = pid == GetCurrentProcessId() ? (uint32_t)(uintptr_t)w : 0;
    g_esp += 4 + 2 * 4;
}

static void shim_CreateMutexA(void) {
    const char* name = (const char*)(uintptr_t)ARG(2);
    char own[MAX_PATH];
    if (name) {
        _snprintf(own, sizeof own - 1, "%s.%lu", name, GetCurrentProcessId());
        own[sizeof own - 1] = 0;
        name = own;
    }
    g_eax = (uint32_t)(uintptr_t)CreateMutexA((LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(0), (BOOL)ARG(1), name);
    g_esp += 4 + 3 * 4;
}

/* ---- Watcom's longjmp --------------------------------------------------------
 * The game runs its tasks under a setjmp in sub_0043E354; a task ends with
 * longjmp(buf, 3), or 2 to run again. run_lift.py patches the first
 * instruction of the guest's longjmp (0x00579783) to come here, with eax the
 * jmp_buf and edx the value. Watcom's buffer: ebx ecx edx esi edi ebp at
 * 0..0x14, the return address at 0x18 (a lifted call pushes a dummy, so the
 * landing pad's own address is used), esp at 0x1C. */
extern const uint32_t hotd_setjmp_resume[][2];   /* recomp_dispatch.c */

void hotd_longjmp(void) {
    uint32_t jb = g_eax, val = g_edx ? g_edx : 1;
    uint32_t esp = MEM32(jb + 0x1C);
    recomp_ehframe_t* f = recomp_eh_owner(esp);
    uint32_t resume = 0;
    for (int i = 0; f && hotd_setjmp_resume[i][0]; i++)
        if (hotd_setjmp_resume[i][0] == f->func) resume = hotd_setjmp_resume[i][1];
    if (!resume) {
        fprintf(stderr, "[longjmp] buffer 0x%08X (esp 0x%08X): no lifted setjmp caller owns it\n", jb, esp);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    g_ebx = MEM32(jb), g_ecx = MEM32(jb + 4), g_edx = MEM32(jb + 8);
    g_esi = MEM32(jb + 0xC), g_edi = MEM32(jb + 0x10), g_ebp = MEM32(jb + 0x14);
    g_esp = esp;
    g_eax = val;
    recomp_eh_resume(f, resume);
}

/* ---- input mailbox (--pad FILE) -------------------------------------------------
 * netlab's pad step: the file holds "<key> [ms]", a Windows virtual-key code
 * and how long to hold it (default 150 ms). The host presses it, empties the
 * file so the driver knows it landed, and lets go after the time;
 * "<key>+<key>" holds several together. The game sees the press both through
 * GetAsyncKeyState and WM_KEYDOWN/WM_KEYUP to its window. Headless, the real
 * keyboard is not read at all, so a run is the same on any machine.
 *
 * The mouse: "m <x> <y>" puts the cursor at a point of the game's 640x480
 * picture, and keys 0x01/0x02 are its buttons. Headless, the cursor is the
 * host's own (GetCursorPos/SetCursorPos/ClipCursor below): the real one is
 * never moved or confined. */
static char g_pad[MAX_PATH];

static LPARAM key_lparam(unsigned vk, int up) {
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC_EX);
    LPARAM l = 1 | (sc & 0xFF) << 16 | ((sc & 0xFF00) == 0xE000) << 24;
    return up ? l | 3u << 30 : l;
}

static DWORD WINAPI pad_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(20);
        char buf[128] = "";
        HANDLE f = CreateFileA(g_pad, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD n = 0;
        ReadFile(f, buf, sizeof buf - 1, &n, NULL);
        if (n) { SetFilePointer(f, 0, NULL, FILE_BEGIN); SetEndOfFile(f); }
        CloseHandle(f);
        buf[n] = 0;
        if (buf[0] == 'm') {
            char* q = buf + 1;
            g_cur_x = strtol(q, &q, 0);
            g_cur_y = strtol(q, &q, 0);
            g_mouse_step = 2;
            printf("[pad] %6.1fs mouse to %ld,%ld\n", run_seconds(), g_cur_x, g_cur_y);
            if (g_game_hwnd) PostMessageA(g_game_hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(g_cur_x, g_cur_y));
            continue;
        }
        unsigned keys[8], nk = 0;
        char* p = buf;
        while (nk < 8) {
            unsigned vk = strtoul(p, &p, 0);
            if (!vk || vk > 0xFE) break;
            keys[nk++] = vk;
            if (*p != '+') break;
            p++;
        }
        if (!nk) continue;
        unsigned ms = strtoul(p, NULL, 0);
        if (!ms) ms = 150;
        printf("[pad] %6.1fs press", run_seconds());
        for (unsigned i = 0; i < nk; i++) printf(" 0x%02X", keys[i]);
        printf(" for %u ms\n", ms);
        for (unsigned i = 0; i < nk; i++) {
            g_pad_down[keys[i]] = 1;
            if (g_game_hwnd) PostMessageA(g_game_hwnd, keys[i] == VK_LBUTTON ? WM_LBUTTONDOWN : keys[i] == VK_RBUTTON
                                          ? WM_RBUTTONDOWN : WM_KEYDOWN, keys[i] == VK_LBUTTON ? MK_LBUTTON
                                          : keys[i] == VK_RBUTTON ? MK_RBUTTON : keys[i],
                                          keys[i] <= VK_RBUTTON ? MAKELPARAM(g_cur_x, g_cur_y) : key_lparam(keys[i], 0));
        }
        Sleep(ms);
        for (unsigned i = 0; i < nk; i++) {
            g_pad_down[keys[i]] = 0;
            if (g_game_hwnd) PostMessageA(g_game_hwnd, keys[i] == VK_LBUTTON ? WM_LBUTTONUP : keys[i] == VK_RBUTTON
                                          ? WM_RBUTTONUP : WM_KEYUP, keys[i] <= VK_RBUTTON ? 0 : keys[i],
                                          keys[i] <= VK_RBUTTON ? MAKELPARAM(g_cur_x, g_cur_y) : key_lparam(keys[i], 1));
        }
    }
}

static void shim_GetAsyncKeyState(void) {
    int vk = (int)ARG(0) & 0xFF;
    static uint8_t seen[256];             /* each key the game polls, once: what it reads */
    if (!seen[vk]) seen[vk] = 1, printf("[pad] the game polls key 0x%02X\n", vk);
    SHORT s = g_headless || g_lockstep ? 0 : GetAsyncKeyState(vk);   /* online: only the frame's inputs */
    if (g_lockstep) { g_eax = 0; g_esp += 4 + 1 * 4; return; }
    if (g_pad_down[vk]) s |= (SHORT)0x8000;
    g_eax = (uint16_t)s;
    g_esp += 4 + 1 * 4;
}

static void shim_GetCursorPos(void) {
    POINT* pt = (POINT*)(uintptr_t)ARG(0);
    static int once;
    if (!once++) printf("[input] the game reads GetCursorPos\n");
    if (!g_headless) g_eax = GetCursorPos(pt);
    else {
        pt->x = g_cur_x, pt->y = g_cur_y;
        if (g_game_hwnd) ClientToScreen(g_game_hwnd, pt);
        g_eax = TRUE;
    }
    g_esp += 4 + 1 * 4;
}

static void shim_SetCursorPos(void) {
    if (!g_headless) g_eax = SetCursorPos((int)ARG(0), (int)ARG(1));
    else {
        POINT pt = { (LONG)ARG(0), (LONG)ARG(1) };
        if (g_game_hwnd) ScreenToClient(g_game_hwnd, &pt);
        g_cur_x = pt.x, g_cur_y = pt.y;
        g_eax = TRUE;
    }
    g_esp += 4 + 2 * 4;
}

static void shim_ClipCursor(void) {
    g_eax = g_headless ? TRUE : ClipCursor((const RECT*)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static native32_shim_t g_shims[] = {
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetCursorPos", shim_GetCursorPos },
    { "timeGetTime", shim_timeGetTime },
    { "GetTickCount", shim_GetTickCount },
    { "SetCursorPos", shim_SetCursorPos },
    { "ClipCursor", shim_ClipCursor },
    { "gbInitDeviceDriver", shim_gbInitDeviceDriver },
    { "gblGetButtonStatus", shim_gbl_zero },
    { "gblGetCursorPosition", shim_gbl_zero },
    { "gblSetScreenMode", shim_gbl_zero },
    { "DirectInputCreateA", shim_DirectInputCreateA },
    { "GlobalAddAtomA", shim_GlobalAddAtomA },
    { "GlobalFindAtomA", shim_GlobalFindAtomA },
    { "GlobalDeleteAtom", shim_GlobalDeleteAtom },
    { "FindWindowA", shim_FindWindowA },
    { "CreateMutexA", shim_CreateMutexA },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
    { "SetWindowPos", shim_SetWindowPos },
    { "SetWindowLongA", shim_SetWindowLongA },
    { "MessageBoxA", shim_MessageBoxA },
    { "ExitProcess", shim_ExitProcess },
    { "TerminateProcess", shim_TerminateProcess },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "CreateFileA", shim_CreateFileA },
    { "DeleteFileA", shim_DeleteFileA },
    { "GetDriveTypeA", shim_GetDriveTypeA },
    { "mciSendStringA", shim_mciSendStringA },
};

/* ---- muted by default -------------------------------------------------------
 * Every run is silent until sound is being tested (--sound): the process's own
 * audio session is muted, which catches DirectSound and winmm alike without
 * touching the system volume. */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

/* 1: muted; 2: there is no audio device; 0: failed. */
static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) { IMMDeviceEnumerator_Release(en); return 2; }
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* ---- reports ---------------------------------------------------------------- */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and only
 * real faults get here. Written with WriteFile from a static buffer: a fault
 * while another thread holds the CRT's stderr lock would otherwise print
 * nothing. */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, ddw_frames());
    native32_dump_icalls(8);
    ddw_close();
    recomp_trace_flush();
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* exe = "work\\thotd.exe";
    const char* game = "game";
    char exe_full[MAX_PATH], game_full[MAX_PATH], guest_args[512] = "";
    int run = 0, sound = 0, net_host_port = 0, net_test = 0;
    const char* net_join_addr = NULL;
    /* A fault ends the run with TerminateProcess: nothing buffered may be lost. */
    setvbuf(stdout, NULL, _IONBF, 0);
    run_seconds();                      /* the clock starts with the host */
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (!n) n = menu_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_record, NULL);
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) _snprintf(guest_args, sizeof guest_args - 1, " %s", argv[++i]);
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) GetFullPathNameA(argv[++i], MAX_PATH, g_pad, NULL);
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else if (!strcmp(argv[i], "--host")) net_host_port = i + 1 < argc && atoi(argv[i + 1]) ? atoi(argv[++i]) : -1;
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) net_join_addr = argv[++i];
        else if (!strcmp(argv[i], "--net-selftest")) net_test = 1;
        else if (!strcmp(argv[i], "--desync-test") && i + 1 < argc) g_net_desync_test = atoi(argv[++i]);
        else {
            printf("usage: hotd [--run] [--headless] [--record out.mp4] [--fullscreen] [--sound] [--exe work\\thotd.exe]\n"
                   "            [--game game] [--args \"<game switches>\"] [--watchdog S] [--pad FILE] [--callbacks]\n"
                   "            [--host [PORT]] [--join CODE|ADDR[:PORT]] [--net-selftest] [--cmd NAME@SECONDS]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    cd_pick(game_full);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\thotd.exe", game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"%s", g_guest_exe, guest_args);

    /* Before any game code: the guard watches every monitor's mode, and the
     * DirectDraw hooks keep the game's mode changes off them (ddraw_window.c).
     * A run that cannot have both does not start. */
    ddw_init(g_headless, g_record[0] ? g_record : NULL);
    if (!g_fullscreen) ddw_guard_start();
    if (!ddw_install(g_fullscreen)) return 1;
    menu_init(g_headless);              /* hotd.ini: the cheats */
    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("House of the Dead recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, HOTD_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, HOTD_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, HOTD_IMAGE_BASE, HOTD_IMAGE_BASE + span);
    if (native32_bind(HOTD_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
    printf("  guest command line %s\n", g_guest_cmdline);
    /* Online play: the build is the exe's PE stamp, so both PCs run the same game. */
    net_init(NULL, MEM32(HOTD_IMAGE_BASE + MEM32(HOTD_IMAGE_BASE + 0x3C) + 8), g_headless);
    if (net_test) return net_selftest();
    if (net_host_port) net_host(net_host_port > 0 ? net_host_port : 0);
    if (net_join_addr) net_join(net_join_addr);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", hotd_entry_va);
        return 0;
    }
    /* The game opens its data relative to its working directory. */
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    CoInitialize(NULL);                 /* the game's own CoInitialize then answers S_FALSE */
    int m = sound ? 1 : mute_process();
    if (!m) {                           /* fail closed: a run is silent or it does not start */
        fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
        return 1;
    }
    printf("  audio: %s\n", sound ? "on (--sound)" : m == 2 ? "muted (no audio device)" : "muted (--sound to hear it)");
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    if (g_pad[0]) {
        printf("  input mailbox: %s\n", g_pad);
        CloseHandle(CreateThread(NULL, 0, pad_thread, NULL, 0, NULL));
    }
    printf("  entering 0x%08X\n\n", hotd_entry_va);
    fflush(stdout);
    /* Online: the game starts once the other PC is here, so both start from
     * the same first frame. Given up, it starts alone. */
    if (net_host_port || net_join_addr) {
        g_lockstep = net_wait();
        printf("  online: %s\n", g_lockstep ? net_status() : "no session, playing alone");
    }
    native32_call_guest(hotd_entry_va, 0, NULL);
    ddw_close();
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
