/*
 * The F10 menu: cheats (and online play, net.c) around the game.
 *
 * The House of the Dead's cheats are its own. Holding Ctrl and typing SKIDMARX
 * on the main menu toggles the byte at 0x005CD390 (sub_0041DB90 compares the
 * last eight keys with the string at 0x005A1198); CREATURE toggles the
 * Creature Test at 0x005CE8B4. The game tests those bytes wherever a cheat
 * matters, so the menu holds them on instead of typing. Cheat mode is the
 * developers' own: invulnerability and no reloading in every mode, a Cheat
 * item on the main menu (stage and scene select, free play) and, while it is
 * on, Ctrl+R rapid fire, Ctrl+1/Ctrl+2 player 1's/2's invulnerability, and
 * Ctrl+3/Ctrl+4 to make a player die.
 *
 * The menu is a Win32 popup on the game's window, as in the other pcrecomp
 * ports. --cmd runs the same commands on a timeline, for headless checks.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "menu.h"
#include "net.h"

static const struct { uint32_t va; const char* name; const char* ini; } k_cheats[] = {
    { 0x005CD390u, "Cheat mode: invulnerable, no reloading, stage select (SKIDMARX)", "cheat_mode" },
    { 0x005CE8B4u, "Creature Test on the main menu (CREATURE)", "creature_test" },
};
#define NCHEATS (int)(sizeof k_cheats / sizeof k_cheats[0])

static int g_on[NCHEATS];
static volatile LONG g_clear;            /* bit i: clear cheat i once */
static int g_headless;
static char g_ini[MAX_PATH];

/* ---- cheats ----------------------------------------------------------------
 * Held on from a thread: the game toggles the bytes itself when the codes are
 * typed, so they are put back every 100 ms. Turning one off clears it once.
 * Online, the cheats are the host's, sent with the session (net.c), so every
 * PC plays the same game. */
static DWORD WINAPI cheat_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        LONG clear = InterlockedExchange(&g_clear, 0);
        for (int i = 0; i < NCHEATS; i++) {
            if (clear >> i & 1) MEM8(k_cheats[i].va) = 0;
            else if (g_on[i] && !MEM8(k_cheats[i].va)) MEM8(k_cheats[i].va) = 1;
        }
        Sleep(100);
    }
}

int menu_cheats(void) {
    int bits = 0;
    for (int i = 0; i < NCHEATS; i++) bits |= g_on[i] << i;
    return bits;
}

void menu_set_cheats(int bits) {
    for (int i = 0; i < NCHEATS; i++) {
        if (g_on[i] && !(bits >> i & 1)) InterlockedOr(&g_clear, 1 << i);
        g_on[i] = bits >> i & 1;
    }
}

static void toggle_cheat(int i) {
    if (net_active() && !net_is_host()) {
        fprintf(stderr, "[menu] online, the host sets the cheats\n");
        return;
    }
    g_on[i] = !g_on[i];
    if (!g_on[i]) InterlockedOr(&g_clear, 1 << i);
    fprintf(stderr, "[menu] %s %s\n", k_cheats[i].name, g_on[i] ? "on" : "off");
}

/* ---- settings (hotd.ini, beside the host) ----------------------------------- */
static void menu_save(void) {
    if (g_headless) return;                    /* a test run leaves the player's settings alone */
    for (int i = 0; i < NCHEATS; i++)
        WritePrivateProfileStringA("cheats", k_cheats[i].ini, g_on[i] ? "1" : "0", g_ini);
}

static void load(void) {
    GetModuleFileNameA(NULL, g_ini, sizeof g_ini);
    char* slash = strrchr(g_ini, '\\');
    strcpy(slash ? slash + 1 : g_ini, "hotd.ini");
    for (int i = 0; i < NCHEATS; i++)
        g_on[i] = !g_headless && GetPrivateProfileIntA("cheats", k_cheats[i].ini, 0, g_ini) != 0;
}

const char* menu_ini(void) { return g_ini; }

/* ---- the popup ------------------------------------------------------------- */
enum { ID_CHEAT = 100, ID_NET = 300 };

static void add(HMENU m, UINT id, const char* text, int checked, int enabled) {
    AppendMenuA(m, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, text);
}

static void run(int cmd) {
    if (cmd >= ID_CHEAT && cmd < ID_CHEAT + NCHEATS) toggle_cheat(cmd - ID_CHEAT);
    else if (cmd >= ID_NET) net_command(cmd - ID_NET);
    if (cmd) menu_save();
}

static void show(HWND h) {
    HMENU m = CreatePopupMenu(), ch = CreatePopupMenu(), nm = CreatePopupMenu();
    int locked = net_active() && !net_is_host();
    for (int i = 0; i < NCHEATS; i++) add(ch, ID_CHEAT + i, k_cheats[i].name, g_on[i], !locked);
    AppendMenuA(ch, MF_SEPARATOR, 0, NULL);
    add(ch, 0, "With cheat mode: Ctrl+R rapid fire, Ctrl+1/2 invulnerability", 0, 0);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)ch, "Cheats");
    net_menu(nm, ID_NET);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)nm, "Online (2 players)");

    /* the game hides the cursor: shown for the menu */
    int shown = 0;
    while (ShowCursor(TRUE) < 0) shown++;
    shown++;
    ClipCursor(NULL);
    POINT at;
    GetCursorPos(&at);
    SetForegroundWindow(h);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, h, NULL);
    PostMessageA(h, WM_NULL, 0, 0);
    DestroyMenu(m);
    while (shown--) ShowCursor(FALSE);
    /* the key or click that chose stays down a moment: not the game's */
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 500;) {
        if (!((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
               GetAsyncKeyState(VK_ESCAPE)) & 0x8000)) break;
        Sleep(10);
    }
    run(cmd);
}

int menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r) {
    (void)l;
    *r = 0;
    if (g_headless) return 0;
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F10) { show(h); return 1; }                 /* F10 arrives as a system key */
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        return w == VK_F10;                                     /* not the window menu */
    }
    return 0;
}

/* ---- --cmd NAME@T: the menu's commands on a timeline ----------------------- */
#define MAX_CMDS 32
static struct { int id; double t; } g_script[MAX_CMDS];
static int g_nscript;

static DWORD WINAPI script_thread(LPVOID unused) {
    (void)unused;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < g_nscript; i++) {
        while ((GetTickCount() - t0) / 1000.0 < g_script[i].t) Sleep(20);
        run(g_script[i].id);
    }
    return 0;
}

int menu_arg(int argc, char** argv, int i) {
    static const struct { const char* name; int id; } names[] = {
        { "cheat", ID_CHEAT + 0 }, { "creature", ID_CHEAT + 1 },
    };
    if (strcmp(argv[i], "--cmd") || i + 1 >= argc) return 0;
    char name[32];
    const char* at = strchr(argv[i + 1], '@');
    int id = -1, n = at ? (int)(at - argv[i + 1]) : 0;
    if (!at || n >= (int)sizeof name || g_nscript == MAX_CMDS) { fprintf(stderr, "--cmd NAME@SECONDS\n"); exit(1); }
    memcpy(name, argv[i + 1], n), name[n] = 0;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
        if (!strcmp(name, names[k].name)) id = names[k].id;
    if (id < 0) { fprintf(stderr, "--cmd %s: unknown (cheat, creature)\n", name); exit(1); }
    g_script[g_nscript].id = id, g_script[g_nscript++].t = atof(at + 1);
    return 2;
}

void menu_init(int headless) {
    g_headless = headless;
    load();
    fprintf(stderr, "[menu] F10; cheats 0x%X (%s)\n", menu_cheats(), g_ini);
    CloseHandle(CreateThread(NULL, 0, cheat_thread, NULL, 0, NULL));
    if (g_nscript) CloseHandle(CreateThread(NULL, 0, script_thread, NULL, 0, NULL));
}
