/*
 * Online play for two players: lockstep over UDP.
 *
 * Both PCs run the same game. Every frame of the game's logic, each PC
 * samples its own player's controls and sends them, and the frame runs only
 * when both players' controls for it are here (net_frame). Nothing else
 * crosses the wire, so the two games stay the same as long as the game is
 * deterministic given its inputs (docs/multiplayer.md: what had to be pinned).
 *
 *   seats     the host is player 1, the one who joins is player 2.
 *   delay     a PC's input for frame N is sampled at frame N-DELAY and sent,
 *             with the last REDUND frames repeated in every packet, so a lost
 *             packet costs nothing and a slow link costs a short stall.
 *   start     the joiner says HELLO with its build (the exe's PE stamp); the
 *             host answers WELCOME with the session (its cheats), and both
 *             start counting frames from the next game start (net_frame
 *             numbers them from the first call after the session begins).
 *   desync    every 60 frames each PC sends a hash of the game state
 *             (net_set_hash); a mismatch ends the session and says so.
 *   leaving   BYE, or 10 seconds of silence, ends the session; the game
 *             goes on with the other player's seat empty.
 *
 * Join codes are the host's IPv4 address and port in a form that survives a
 * chat window: HOTD-XXXXX-XXXXX (Crockford base 32), as Hover's.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "net.h"
#include "menu.h"

#define MAGIC    0x44544F48u           /* "HOTD" */
#define VERSION  1
#define DELAY    3                     /* frames between sampling an input and running it */
#define REDUND   8                     /* frames repeated in every packet */
#define RING     256                   /* frames kept, a power of two */
#define TIMEOUT  10000                 /* ms of silence that ends a session */
#define HASH_EVERY 60

enum { T_HELLO = 1, T_WELCOME, T_INPUT, T_HASH, T_BYE };

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, version; } hdr_t;
typedef struct { hdr_t h; uint32_t stamp; } hello_t;
typedef struct { hdr_t h; uint32_t stamp; int32_t cheats; } welcome_t;
typedef struct { hdr_t h; int32_t first; uint8_t n; net_input_t in[REDUND]; } input_t;
typedef struct { hdr_t h; int32_t frame; uint32_t hash; } hash_t;
#pragma pack(pop)

static CRITICAL_SECTION g_lock;
static HANDLE   g_arrived;             /* set when a remote input lands */
static SOCKET   g_sock = INVALID_SOCKET;
static struct sockaddr_in g_peer;      /* the other PC */
static volatile int g_have_peer, g_active, g_is_host, g_ended;
static int      g_port = 7797;
static uint32_t g_stamp;               /* this build: the exe's PE stamp */
static char     g_status[160] = "offline";
static volatile DWORD g_seen;          /* the last packet from the peer */
static HWND     g_frame_hwnd;
static int      g_headless;

/* The input ring: [seat][frame & (RING-1)], with the frame it holds. */
static net_input_t g_in[2][RING];
static int32_t  g_in_frame[2][RING];
static int32_t  g_frame;               /* the next frame net_frame runs */
static int32_t  g_sent_to;             /* our inputs are queued up to here */
static uint32_t g_hash[2][RING / HASH_EVERY + 1];
static int32_t  g_hash_frame[2][RING / HASH_EVERY + 1];

static void status(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_status, sizeof g_status - 1, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[net] %s\n", g_status);
}

static void hdr(void* p, int type) {
    hdr_t* h = (hdr_t*)p;
    h->magic = MAGIC, h->type = (uint8_t)type, h->version = VERSION;
}

static void send_peer(const void* p, int n) {
    if (g_have_peer) sendto(g_sock, (const char*)p, n, 0, (const struct sockaddr*)&g_peer, sizeof g_peer);
}

int  net_active(void) { return g_active; }
int  net_is_host(void) { return g_is_host; }
int  net_seat(void) { return g_is_host ? 0 : 1; }
const char* net_status(void) { return g_status; }

static void end_session(const char* why) {
    if (!g_active && g_sock == INVALID_SOCKET) return;
    hdr_t b;
    hdr(&b, T_BYE);
    for (int k = 0; k < 3; k++) send_peer(&b, sizeof b);
    g_active = 0, g_ended = 1;
    status("%s", why);
    SetEvent(g_arrived);
    if (!g_headless && g_frame_hwnd) MessageBoxA(g_frame_hwnd, g_status, "The House of the Dead online", MB_OK | MB_ICONINFORMATION);
}

/* ---- receiving -------------------------------------------------------------- */
static void got_inputs(const input_t* p, int seat) {
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < p->n && i < REDUND; i++) {
        int32_t f = p->first + i;
        if (f < g_frame - RING / 2) continue;         /* long gone */
        g_in[seat][f & (RING - 1)] = p->in[i];
        g_in_frame[seat][f & (RING - 1)] = f;
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_arrived);
}

static void got_hash(const hash_t* p, int seat) {
    int k = (p->frame / HASH_EVERY) % (RING / HASH_EVERY + 1);
    EnterCriticalSection(&g_lock);
    g_hash[seat][k] = p->hash, g_hash_frame[seat][k] = p->frame;
    int mine = g_hash_frame[!seat][k] == p->frame, differ = mine && g_hash[!seat][k] != p->hash;
    LeaveCriticalSection(&g_lock);
    if (differ) {
        char why[96];
        _snprintf(why, sizeof why - 1, "the two games went apart at frame %d (desync): session ended", p->frame);
        why[sizeof why - 1] = 0;
        end_session(why);
    }
}

static DWORD WINAPI recv_thread(LPVOID unused) {
    (void)unused;
    char buf[1500];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(g_sock, buf, sizeof buf, 0, (struct sockaddr*)&from, &fl);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAECONNRESET) continue;   /* an ICMP for an old send */
            return 0;
        }
        const hdr_t* h = (const hdr_t*)buf;
        if (n < (int)sizeof *h || h->magic != MAGIC) continue;
        if (h->version != VERSION) { status("the other PC runs a different version of this port"); continue; }
        if (g_have_peer && (from.sin_addr.s_addr != g_peer.sin_addr.s_addr || from.sin_port != g_peer.sin_port)) {
            if (h->type == T_HELLO) {                /* a second player: the game has two seats */
                hdr_t b;
                hdr(&b, T_BYE);
                sendto(g_sock, (const char*)&b, sizeof b, 0, (struct sockaddr*)&from, sizeof from);
            }
            continue;
        }
        g_seen = GetTickCount();
        int remote = g_is_host ? 1 : 0;
        switch (h->type) {
        case T_HELLO:
            if (!g_is_host || n < (int)sizeof(hello_t)) break;
            if (((const hello_t*)buf)->stamp != g_stamp) {
                status("a player tried to join with a different thotd.exe");
                break;
            }
            if (!g_have_peer) {
                g_peer = from, g_have_peer = 1;
                char ip[64];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                status("player 2 joined from %s", ip);
            }
            {
                welcome_t w;
                hdr(&w, T_WELCOME);
                w.stamp = g_stamp, w.cheats = menu_cheats();
                send_peer(&w, sizeof w);
            }
            g_active = 1;
            break;
        case T_WELCOME:
            if (g_is_host || n < (int)sizeof(welcome_t)) break;
            if (!g_active) {
                if (((const welcome_t*)buf)->stamp != g_stamp) {
                    status("the host has a different thotd.exe: cannot play together");
                    break;
                }
                menu_set_cheats(((const welcome_t*)buf)->cheats);
                g_active = 1;
                status("joined: you are player 2");
            }
            break;
        case T_INPUT:
            if (n >= (int)offsetof(input_t, in)) got_inputs((const input_t*)buf, remote);
            break;
        case T_HASH:
            if (n >= (int)sizeof(hash_t)) got_hash((const hash_t*)buf, remote);
            break;
        case T_BYE:
            if (g_active) end_session(g_is_host ? "player 2 left" : "the host ended the game");
            break;
        }
    }
}

static int open_socket(int port) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { status("no UDP socket"); return 0; }
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET, a.sin_port = htons((u_short)port), a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_sock, (struct sockaddr*)&a, sizeof a)) {
        status("UDP port %d is in use", port);
        closesocket(g_sock), g_sock = INVALID_SOCKET;
        return 0;
    }
    CloseHandle(CreateThread(NULL, 0, recv_thread, NULL, 0, NULL));
    return 1;
}

/* ---- join codes ------------------------------------------------------------- */
static const char k_b32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void net_encode(uint32_t ip_be, int port, char* out, size_t n) {
    uint64_t v = ((uint64_t)ntohl(ip_be) << 16) | (uint16_t)port;
    char c[10];
    for (int i = 9; i >= 0; i--) { c[i] = k_b32[v & 31]; v >>= 5; }
    _snprintf(out, n, "HOTD-%.5s-%.5s", c, c + 5);
}

int net_decode(const char* s, char* host, size_t n, int* port) {
    uint64_t v = 0;
    int k = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (_strnicmp(s, "HOTD-", 5)) return 0;
    for (s += 5; *s && k < 10; s++) {
        if (*s == '-') continue;
        const char* p = strchr(k_b32, toupper((unsigned char)*s));
        if (!p) return 0;
        v = (v << 5) | (uint64_t)(p - k_b32);
        k++;
    }
    if (k != 10) return 0;
    uint32_t ip = (uint32_t)(v >> 16);
    *port = (int)(v & 0xFFFF);
    _snprintf(host, n, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return 1;
}

/* The address to give out: a Tailscale one (100.64.0.0/10) if this PC has
 * one, since it works from anywhere, then a private LAN address. */
static uint32_t best_address(void) {
    char name[256];
    struct addrinfo hints = { 0 }, *res = NULL;
    uint32_t best = 0;
    int rank = 0;
    hints.ai_family = AF_INET;
    if (gethostname(name, sizeof name) || getaddrinfo(name, NULL, &hints, &res)) return 0;
    for (struct addrinfo* a = res; a; a = a->ai_next) {
        uint32_t ip = ((struct sockaddr_in*)a->ai_addr)->sin_addr.s_addr, h = ntohl(ip);
        int r = (h & 0xFFC00000u) == 0x64400000u ? 3
              : ((h >> 24) == 192 && ((h >> 16) & 255) == 168) || (h >> 24) == 10 || (h >> 20) == 0xAC1 ? 2
              : (h >> 24) != 127 ? 1 : 0;
        if (r > rank) rank = r, best = ip;
    }
    freeaddrinfo(res);
    return best;
}

/* ---- starting --------------------------------------------------------------- */
int net_host(int port) {
    if (g_sock != INVALID_SOCKET) return 0;
    if (port) g_port = port;
    g_is_host = 1;
    if (!open_socket(g_port)) return 0;
    char code[32];
    net_encode(best_address(), g_port, code, sizeof code);
    status("hosting on UDP port %d, join code %s: waiting for player 2", g_port, code);
    return 1;
}

static DWORD WINAPI hello_thread(LPVOID unused) {
    (void)unused;
    hello_t b;
    hdr(&b, T_HELLO);
    b.stamp = g_stamp;
    for (int k = 0; k < 40 && !g_active && !g_ended; k++) {   /* 20 s of asking */
        send_peer(&b, sizeof b);
        Sleep(500);
    }
    if (!g_active) status("no answer from the host");
    return 0;
}

int net_join(const char* addr) {
    char host[128];
    int port = g_port;
    struct addrinfo hints = { 0 }, *res = NULL;
    if (g_sock != INVALID_SOCKET) return 0;
    if (!net_decode(addr, host, sizeof host, &port)) {
        strncpy(host, addr, sizeof host - 1);
        host[sizeof host - 1] = 0;
        char* colon = strrchr(host, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
    }
    g_is_host = 0;
    if (!open_socket(0)) return 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) || !res) { status("cannot find %s", host); return 0; }
    g_peer = *(struct sockaddr_in*)res->ai_addr;
    g_peer.sin_port = htons((u_short)port);
    freeaddrinfo(res);
    g_have_peer = 1;
    status("joining %s:%d", host, port);
    CloseHandle(CreateThread(NULL, 0, hello_thread, NULL, 0, NULL));
    return 1;
}

void net_leave(void) { end_session("left the online game"); }

/* ---- the lockstep ----------------------------------------------------------- */
static void send_inputs(void) {
    input_t p;
    int seat = net_seat();
    hdr(&p, T_INPUT);
    EnterCriticalSection(&g_lock);
    p.first = g_sent_to - REDUND + 1;
    if (p.first < 0) p.first = 0;
    p.n = (uint8_t)(g_sent_to - p.first + 1);
    for (int i = 0; i < p.n; i++) p.in[i] = g_in[seat][(p.first + i) & (RING - 1)];
    LeaveCriticalSection(&g_lock);
    send_peer(&p, (int)(offsetof(input_t, in) + p.n * sizeof(net_input_t)));
}

int net_frame(const net_input_t* mine, net_input_t both[2]) {
    if (!g_active) return 0;
    int seat = net_seat(), other = !seat;
    int32_t f = g_frame;
    EnterCriticalSection(&g_lock);
    if (f == 0) {                              /* the first DELAY frames: nobody's input yet */
        for (int32_t k = 0; k < DELAY; k++) {
            memset(&g_in[seat][k], 0, sizeof g_in[seat][k]);
            g_in_frame[seat][k] = k;
        }
    }
    g_in[seat][(f + DELAY) & (RING - 1)] = *mine;
    g_in_frame[seat][(f + DELAY) & (RING - 1)] = f + DELAY;
    g_sent_to = f + DELAY;
    LeaveCriticalSection(&g_lock);
    send_inputs();

    /* Stall, never guess: the frame runs when the other player's input is here.
     * A peer that stays silent ends the session; the game goes on alone. */
    DWORD t0 = GetTickCount();
    for (;;) {
        EnterCriticalSection(&g_lock);
        int here = (f < DELAY && g_in_frame[other][f & (RING - 1)] != f) ? 1
                 : g_in_frame[other][f & (RING - 1)] == f;
        LeaveCriticalSection(&g_lock);
        if (here || !g_active) break;
        if (WaitForSingleObject(g_arrived, 50) == WAIT_TIMEOUT) send_inputs();   /* resend while waiting */
        if (GetTickCount() - g_seen > TIMEOUT && GetTickCount() - t0 > TIMEOUT) {
            end_session(g_is_host ? "player 2 stopped answering" : "the host stopped answering");
            break;
        }
    }
    EnterCriticalSection(&g_lock);
    both[seat] = g_in[seat][f & (RING - 1)];
    if (g_active && g_in_frame[other][f & (RING - 1)] == f) both[other] = g_in[other][f & (RING - 1)];
    else memset(&both[other], 0, sizeof both[other]);
    LeaveCriticalSection(&g_lock);
    g_frame = f + 1;
    return 1;
}

void net_set_hash(uint32_t hash) {
    int32_t f = g_frame - 1;
    if (!g_active || f % HASH_EVERY) return;
    hash_t p;
    hdr(&p, T_HASH);
    p.frame = f, p.hash = hash;
    send_peer(&p, sizeof p);
    got_hash(&p, net_seat());                  /* compare with the other's, if it is here */
}

int32_t net_frame_no(void) { return g_frame; }

/* ---- the F10 menu ----------------------------------------------------------- */
enum { ID_HOST, ID_CODE, ID_JOIN, ID_LEAVE };

void net_menu(HMENU m, UINT first) {
    char t[200];
    _snprintf(t, sizeof t - 1, "Status: %s", g_status);
    t[sizeof t - 1] = 0;
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, t);
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    UINT idle = g_sock == INVALID_SOCKET ? 0 : MF_GRAYED;
    _snprintf(t, sizeof t - 1, "&Host a game (you are player 1, UDP port %d)", g_port);
    t[sizeof t - 1] = 0;
    AppendMenuA(m, MF_STRING | idle, first + ID_HOST, t);
    AppendMenuA(m, MF_STRING | (g_is_host && g_sock != INVALID_SOCKET ? 0 : MF_GRAYED), first + ID_CODE,
                "&Copy the join code");
    AppendMenuA(m, MF_STRING | idle, first + ID_JOIN, "&Join a game (the join code or address on the clipboard)");
    AppendMenuA(m, MF_STRING | (g_active ? 0 : MF_GRAYED), first + ID_LEAVE, "&Leave the online game");
}

static void clip_put(const char* s) {
    size_t n = strlen(s) + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), s, n);
    GlobalUnlock(g);
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
        CloseClipboard();
    } else GlobalFree(g);
}

static void show(const char* msg) {
    if (!g_headless) MessageBoxA(g_frame_hwnd, msg, "The House of the Dead online", MB_OK | MB_ICONINFORMATION);
}

void net_command(int id) {
    const char* ini = menu_ini();
    if (id == ID_HOST) {
        if (net_host(GetPrivateProfileIntA("net", "port", 7797, ini))) net_command(ID_CODE);
        else show(g_status);
    } else if (id == ID_CODE) {
        char code[32], msg[400], ip[64] = "?";
        uint32_t a = best_address();
        inet_ntop(AF_INET, &a, ip, sizeof ip);
        net_encode(a, g_port, code, sizeof code);
        clip_put(code);
        _snprintf(msg, sizeof msg - 1,
                  "Join code %s (%s, UDP port %d) is on the clipboard.\n\n"
                  "Player 2 copies it and chooses F10, Online, Join. Over the internet, forward UDP port %d "
                  "to this PC (or both use Tailscale).\n\nThen start a game: player 2 presses Start to join in.",
                  code, ip, g_port, g_port);
        msg[sizeof msg - 1] = 0;
        show(msg);
    } else if (id == ID_JOIN) {
        char a[96] = "";
        if (OpenClipboard(NULL)) {                /* a code or address someone pasted wins over the ini */
            HANDLE h = GetClipboardData(CF_TEXT);
            const char* p = h ? (const char*)GlobalLock(h) : NULL;
            if (p && strlen(p) < sizeof a) {
                strcpy(a, p);
                for (char* e = a + strlen(a); e > a && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ');) *--e = 0;
                if (strchr(a, ' ') || !(strchr(a, '.') || strchr(a, ':') || !_strnicmp(a, "HOTD-", 5))) a[0] = 0;
            }
            if (p) GlobalUnlock(h);
            CloseClipboard();
        }
        if (!a[0]) GetPrivateProfileStringA("net", "join", "", a, sizeof a, ini);
        if (!a[0]) { show("Copy the host's join code (HOTD-XXXXX-XXXXX) or address, then choose Join again."); return; }
        WritePrivateProfileStringA("net", "join", a, ini);
        if (!net_join(a)) show(g_status);
    } else if (id == ID_LEAVE) {
        net_leave();
    }
}

/* ---- setting up ------------------------------------------------------------- */
void net_init(HWND frame, uint32_t exe_stamp, int headless) {
    g_frame_hwnd = frame;
    g_stamp = exe_stamp;
    g_headless = headless;
    InitializeCriticalSection(&g_lock);
    g_arrived = CreateEventA(NULL, FALSE, FALSE, NULL);
    memset(g_in_frame, 0xFF, sizeof g_in_frame);      /* an empty slot holds no frame (-1) */
    memset(g_hash_frame, 0xFF, sizeof g_hash_frame);
    g_port = GetPrivateProfileIntA("net", "port", 7797, menu_ini());
    g_seen = GetTickCount();
}

void net_set_window(HWND frame) { g_frame_hwnd = frame; }

/* --net-selftest: the join codes round-trip and reject what isn't one. */
int net_selftest(void) {
    char code[32], host[64];
    int port = 0, fails = 0;
    struct in_addr a;
    inet_pton(AF_INET, "100.101.102.103", &a);
    net_encode(a.s_addr, 7797, code, sizeof code);
    if (!net_decode(code, host, sizeof host, &port) || strcmp(host, "100.101.102.103") || port != 7797) fails++;
    if (net_decode("HOTD-12345", host, sizeof host, &port) || net_decode("100.1.2.3", host, sizeof host, &port)) fails++;
    fprintf(stderr, "[net] selftest %s (%s)\n", fails ? "FAILED" : "ok", code);
    return fails;
}
