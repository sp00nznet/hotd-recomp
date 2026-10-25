/* Online play: not yet (the menu's placeholder). */
#include "net.h"

int  net_active(void) { return 0; }
int  net_is_host(void) { return 0; }
void net_menu(HMENU m, UINT first_id) { (void)first_id; AppendMenuA(m, MF_STRING | MF_GRAYED, 0, "Not yet"); }
void net_command(int id) { (void)id; }
