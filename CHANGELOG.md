# Changelog

## [0.1.0] - 2026-10-25

The first release: SEGA's The House of the Dead (1998, Windows) recompiled to
native C on pcrecomp's native32 host. Early alpha.

### Added
- The whole game lifted (11,709 functions, 0 errors): the attract demo, the
  menus and Arcade mode play, in a window, with DirectDraw kept off the
  display.
- Setup.cmd and the recomp-launcher installer: from your installed copy (or a
  .zip of it) to a built game in one step.
- F10 menu, Cheats: the game's own cheat mode (the `SKIDMARX` code:
  invulnerable, no reloading, stage and scene select, free play, Edit Char
  Stats) and Creature Test, held on for you.
- F10 menu, Online: two players over LAN or internet, lockstep over UDP, with
  join codes and a desync check. Tested across two PCs through a NAT
  ([docs/multiplayer.md](docs/multiplayer.md)).
- A PC with no audio device plays silent instead of crashing.

### Known
- Not yet played start to finish by hand; sound not yet checked by ear.
- No light gun: the mouse aims.
- An online session starts with the game: Host and Join restart it.
