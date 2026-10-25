/* The F10 menu: cheats and online play (menu.c). */
#pragma once
#include <windows.h>

void menu_init(int headless);
int  menu_arg(int argc, char** argv, int i);          /* --cmd NAME@T: entries consumed, or 0 */
int  menu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* r);   /* 1 = handled */
int  menu_cheats(void);                               /* the cheats on, a bit each */
void menu_set_cheats(int bits);                       /* online: the host's */
void menu_apply_cheats(void);                         /* hold them on now (online: once a frame) */
const char* menu_ini(void);                           /* hotd.ini, beside the host */
