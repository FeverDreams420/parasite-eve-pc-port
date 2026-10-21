# Roadmap

The goal: Parasite Eve as a **standalone, fully native PC game**. No emulation
and no memory cards. Every mod and graphics option lives in the game's own
settings menu, it plays in widescreen, and you can save from the menu whenever
you want. It must stay as faithful to the original as possible: no replaced
textures or backgrounds.

Status key: ✅ done · 🔧 in progress · ⬜ not started

## How we got here

Before this native port, the game was brought up as a static recompilation
with PSXRecomp in a separate project (not in this repository). That work got
the game playing on PC up to and past the Disc 1 to Disc 2 change, and gave the
first mod: a **seamless disc switch** that changes from Disc 1 to Disc 2
by itself, with the "insert disc 2" screen kept black and no on-screen prompt.
The native port is a fresh start meant to remove the emulation layer
completely.

## Phase 1 — every function as C, boot to the title ✅

- ✅ All game C from the decomp compiled for 32-bit x86 (main executable and all
  190 overlays), with every hand-written MIPS routine translated to C.
- ✅ PS1 memory at its original addresses, jumps at every original function
  address, overlays recognised and checked against the disc (SHA-1).
- ✅ PSY-Q replaced: graphics, SPU, CD (reading `.cue`/`.bin`), controllers,
  memory cards (`.mcd`), software movie decoder (MDEC), CD streaming.
- ✅ Publisher screen, opening movie, title screen, title menu.
- ✅ Windows build (mingw-w64), tested on one real PC with an Xbox 360 pad.

## Phase 2 — fully playable Disc 1 🔧

- ✅ New Game movie, first scene outside Carnegie Hall, name entry.
- ⬜ Play on through the opera house, the first battles and the rest of Disc 1,
  fixing whatever breaks.
- ⬜ Movie audio (XA ADPCM sectors, currently skipped).
- ⬜ Check music and sound effects against the original.
- ⬜ Saving and loading through the memory-card path, as a stepping stone.
- ⬜ Values the game reads from PS1 kernel RAM through NULL pointers: find out
  what the real console holds there, if it matters anywhere.

## Phase 3 — Disc 2 and no disc change ⬜

- ⬜ Read both disc images and route each read to the right one.
- ⬜ Find how the game decides which disc is in, and answer it without the
  "insert disc 2" screen ever appearing (the native version of the PSXRecomp
  mod).
- ⬜ Play through Disc 2.

## Phase 4 — a PC game, not a PS1 game ⬜

- ⬜ Native saves: save from the menu at any time, no memory card files.
- ⬜ In-game settings menu holding every mod and graphics option.
- ⬜ Controller remapping, Xbox button icons, keyboard and mouse.
- ⬜ Brightness and contrast settings.
- ⬜ A one-click build, and a release that only needs the player's own disc.

## Phase 5 — graphics, staying faithful ⬜

In this order:

1. ⬜ Higher internal resolution for the 3D, text and effects (PsyZ already has
   a setting for it). Pre-rendered backgrounds stay as they are.
2. ⬜ Smooth geometry: keep the extra precision from the geometry (GTE) math
   so models stop wobbling (like PGXP in emulators). This may also fix limbs
   that don't line up.
3. ⬜ Widescreen. The 3D can be widened; the pre-rendered backgrounds are 4:3
   and have nothing past their edges, so how the sides are filled, without
   replacing any art, is still open.
4. ⬜ Possibly, a fully native renderer drawing the game's models directly,
   replacing the PS1 graphics-command layer. Last, and only if 1–3 aren't
   enough.

## Help wanted

PS1 audio (XA/SPU), PS1 graphics and GTE precision, and play-testing with your
own disc. See the README.
