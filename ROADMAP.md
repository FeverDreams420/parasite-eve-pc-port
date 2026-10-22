# Roadmap

What I want in the end: Parasite Eve as a standalone, fully native PC game.
No emulation, no memory card files, every option in the game's own settings
menu, widescreen, and saving from the menu whenever you want. It should stay as
close to the original as possible, so no replaced textures or backgrounds.

✅ done · 🔧 in progress · ⬜ not started

## Before this

I first got the game running on PC as a static recompilation with PSXRecomp,
in a separate project that isn't in this repo. That version played past the
switch to Disc 2 and had my first mod: a seamless disc switch that goes from
Disc 1 to Disc 2 on its own, with no "insert disc 2" prompt. This native port
is a fresh start to get rid of the emulation layer completely.

## 1. Every function as C, boot to the title ✅

- ✅ All of the decomp's game C compiled for 32-bit x86 (main exe and all 190
  overlays), with the hand-written MIPS routines translated to C
- ✅ PS1 memory at its real addresses, jumps at every original function,
  overlays checked against the disc
- ✅ PSY-Q replaced: graphics, sound, CD, controllers, memory cards, movie
  decoding
- ✅ Windows build, running on my PC with an Xbox 360 pad

## 2. Fully playable Disc 1 🔧

- ✅ Start of the game through the end of Day 3
- ✅ Saving and loading (`.mcd` files, emulator cards work)
- ✅ Play-time clock
- ✅ Character shadows without coloured streaks
- 🔧 Movie sound works, lip sync still needs checking on a long cutscene
- 🔧 Sound effects that kept repeating: fix done, needs a battle test
- 🔧 Day 4 to the end of Disc 1

## 3. Disc 2 without a disc change ⬜

- ⬜ Read both disc images and send each read to the right one
- ⬜ Skip the "insert disc 2" screen entirely (the native version of my
  PSXRecomp mod)
- ⬜ Play through Disc 2

## 4. A PC game, not a PS1 game ⬜

- 🔧 Save anywhere: **Save Game** in the menu (and F5) works. Next is putting
  Aya back exactly where she was when you load.
- ⬜ Native saves with no memory card files
- ⬜ In-game settings menu for every mod and graphics option
- ⬜ Controller remapping, Xbox button icons, keyboard and mouse
- ⬜ Brightness and contrast
- ⬜ One-click build, and a release that only needs your own disc

## 5. Graphics, staying faithful ⬜

In this order:

1. ⬜ Higher internal resolution for 3D, text and effects. The pre-rendered
   backgrounds stay as they are.
2. ⬜ Smooth geometry so models stop wobbling (like PGXP in emulators).
3. ⬜ Widescreen. The 3D can be widened, but the backgrounds are 4:3 with
   nothing past the edges, so I still need to figure out how to fill the sides
   without replacing any art.
4. ⬜ Maybe a fully native renderer, only if 1 to 3 aren't enough.
