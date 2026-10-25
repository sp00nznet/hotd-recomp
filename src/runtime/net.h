/* Online play for two players, lockstep over UDP (net.c). */
#pragma once
#include <windows.h>
#include <stdint.h>

/* One player's controls for one frame of the game's logic: the mouse's motion
 * and buttons (bit 0 left, 1 right, 2 middle) and the keys held (DIK codes). */
#define NET_KEYS 8
#pragma pack(push, 1)
typedef struct { int16_t dx, dy; uint8_t buttons, nkeys; uint8_t keys[NET_KEYS]; } net_input_t;
#pragma pack(pop)

void net_init(HWND frame, uint32_t exe_stamp, int headless);
/* Before the game starts, with --host or --join: wait until the other PC is
 * here (a small window says so). 0 = given up (closed, or no answer). */
int  net_wait(void);
/* The session's settings, from the host: the joiner plays with them. */
const uint8_t* net_session_file(const char* name, int* len);   /* "thotd.cfg", "cstats.cfg" */
void net_set_window(HWND frame);
int  net_host(int port);                /* 0: the ini's port */
int  net_join(const char* addr);        /* "host[:port]" or a join code */
void net_leave(void);
int  net_active(void);                  /* a session is running */
int  net_is_host(void);
int  net_seat(void);                    /* 0 the host (player 1), 1 the joiner (player 2) */
const char* net_status(void);

/* The lockstep: called once per frame of the game's logic with this PC's
 * player's controls; returns when both players' controls for the frame are
 * here, in both[seat]. 0 when no session runs (both is untouched). */
int  net_frame(const net_input_t* mine, net_input_t both[2]);
int32_t net_frame_no(void);             /* the frame net_frame runs next */
uint32_t net_clock_base(void);          /* where the shared clock starts, the same on both PCs */
void net_set_hash(uint32_t hash);       /* the game state after the frame: the desync check */

void net_menu(HMENU m, UINT first_id);  /* the F10 menu's Online submenu */
void net_command(int id);               /* an item of it, numbered from 0 */
int  net_selftest(void);
extern int32_t g_net_desync_test;       /* --desync-test FRAME: the check's own test */
void net_encode(uint32_t ip_be, int port, char* out, size_t n);
int  net_decode(const char* s, char* host, size_t n, int* port);
