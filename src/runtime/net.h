/* Online play for two players, lockstep over UDP (net.c). */
#pragma once
#include <windows.h>

int  net_active(void);                 /* a session is running */
int  net_is_host(void);
void net_menu(HMENU m, UINT first_id); /* the F10 menu's Online submenu */
void net_command(int id);              /* an item of it, numbered from 0 */
