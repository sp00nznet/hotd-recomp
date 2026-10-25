/* Online play for two players, lockstep over UDP (net.c). */
#pragma once
#include <windows.h>
#include <stdint.h>

/* One player's controls for one frame of the game's logic. */
#pragma pack(push, 1)
typedef struct { int16_t x, y; uint16_t buttons; uint16_t extra; } net_input_t;
#pragma pack(pop)

void net_init(HWND frame, uint32_t exe_stamp, int headless);
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
void net_set_hash(uint32_t hash);       /* the game state after the frame: the desync check */

void net_menu(HMENU m, UINT first_id);  /* the F10 menu's Online submenu */
void net_command(int id);               /* an item of it, numbered from 0 */
int  net_selftest(void);
void net_encode(uint32_t ip_be, int port, char* out, size_t n);
int  net_decode(const char* s, char* host, size_t n, int* port);
