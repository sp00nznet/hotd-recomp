# Online play

The House of the Dead is a two-player game: player 2 joins by pressing their
Shoot button, with their own crosshair and HUD. On one PC that needs two input
devices (the game's Options assign Mouse, Keyboard 1/2, Joystick 1/2 or
Lightgun 1/2 to each player). The port adds the same game over the network:
**lockstep over UDP** (`src/runtime/net.c` moves the inputs, `hotd_net_gate`
and its neighbours in `src/runtime/host.c` meet the game). Addresses are
`thotd.exe` VAs.

## Playing online

- **Host:** F10, *Online*, *Host a game*. The game starts again and waits for
  player 2, with a join code (`HOTD-XXXXX-XXXXX`, the host's address and port)
  on the clipboard. Player 2 needs to reach UDP port 7797: on a LAN it just
  works; over the internet, forward the port on the host's router, or both
  use Tailscale (the code then carries the Tailscale address).
- **Join:** copy the code, then F10, *Online*, *Join a game*. The game starts
  again and connects.
- In the game, player 1 is the host, player 2 the one who joined: start a
  game as usual, and player 2 clicks to join in. Both play with the mouse.
- The same from the command line: `hotd.exe --run --host [PORT]` and
  `hotd.exe --run --join CODE` (or `ADDR[:PORT]`). The port is `[net] port=`
  in `hotd.ini`.
- The host's settings (`thotd.cfg`: difficulty, life, continues; and
  `cstats.cfg`) and cheats are the session's. Player 2's own files are left
  alone; it plays from copies (`*.online` in its game folder).

## How it works

**Both PCs run the whole game.** Each frame, each PC samples its own player's
mouse and keys and sends them; the frame runs when both players' inputs for it
are here (3 frames of delay, the last 8 frames repeated in every packet, so a
lost packet costs nothing and a slow link stalls rather than skips). Nothing
else crosses the wire, so it works only because the game is deterministic
given its inputs, which it is once these are pinned:

| What | Where | What the port does |
|---|---|---|
| The frame | the message pump calls `sub_00562E80` once per idle pass: the frame timer `sub_00578930`, the logic (the task scheduler `sub_0043E354` with the players' input reads), then the input poller `sub_00578100`, then the render | the gate is the poller's first instruction (a `run_lift.py` patch): the poll's snapshot is what the next frame's logic sees, so both PCs' frames read the same input |
| The keyboard and mouse | the poller reads DirectInput (`GetDeviceState`, 256 bytes and `DIMOUSESTATE`) into the keyboard buffers at 0x012FD980 and the mouse block at 0x012FDB80 (buttons, presses, releases, previous, position, deltas) | the hooks answer with the frame's inputs: the keys either player holds, and player 1's mouse |
| Player 2's mouse | `sub_00436614` fills each player's input record (0x00D9F5E0 + p×0x1C) through the reader `sub_0043591C`, by the player's device ([0x00D5B200]/[0x00D5B204]); the start check is `sub_00436480`, called for each player at 0x00419142/0x0041914C | both players are set to Mouse; player 2's mouse block is a shadow kept by the poller's rules and swapped in around its reader (0x00436676/0x00436678) and start check |
| The clock | `timeGetTime` at 70 sites: the fire/reload gate (0x00436419), the menus' and title's timers, `srand(timeGetTime())` at 0x00435A44, and a display benchmark at boot | real until the frame timer first asks, then a shared clock from a base the host picks past both PCs' clocks: 1 ms a read, at least 17 ms a frame |
| Random numbers | the CRT's `rand` (seeded from the clock, or 123456 at 0x0043B6C1) and the game's own at 0x00471878 (seed [0x00DA10F0]) | deterministic once the clock is shared |
| Messages | focus changes, and keyboard and mouse messages, happen on one PC only | not passed to the game in a session |
| Cheats | held on by the menu | applied by the gate, the same frame on both; fixed for the session |
| One-instance checks | a global atom, `FindWindowA` and a named mutex | per process, so two copies can run on one PC (how this was tested) |

**The desync check.** Every 60 frames each PC hashes both players' input
records and player records (but +0xA8 and +0xC0, pointers into each PC's own
heap) and the game's random seed; a mismatch ends the session on both PCs and
says so. `--desync-test FRAME` makes one PC's hash lie from that frame on: the
check's own test.

**Tested:**

- **Two PCs, through a NAT** (netlab, 2026-10-25): a Windows workstation
  hosting on the LAN, a Windows VM behind a home-router-style NAT joining, both
  headless and scripted (the host playing Arcade, player 2 joining in and
  shooting through its input mailbox). Twice for 300 s: every check in
  agreement through frame 2,400, player 2's HUD and crosshair on the host's
  picture. The VM has no audio device and no GPU: alone it runs the game at
  about 15 fps, and the pair ran at about 11, the slower PC's pace.
- **Two copies on one PC** over localhost, each with its own game folder
  (`tools\drive.ps1` with `tools\scripts\arcade-shoot.txt` on the host and
  `p2-long.txt` on the joiner): 5,400 frames in agreement with both shooting
  and cheat mode on; the deliberate desync caught at its frame on both.
- Not yet: a session over the internet, or a long one.

**Limits.** Two players (the game has two seats). A session starts at the
game's first frame on both PCs; joining a game already running would need the
running game's state sent across, so Host and Join restart the game. If the
other PC goes quiet for 10 seconds, the session ends and the game plays on
alone.
