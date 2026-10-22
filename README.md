# Parasite Eve — native PC port (work in progress)

A native Windows/Linux build of **Parasite Eve** (PlayStation, North America,
`SLUS-00662`). The game's own code runs as compiled C on the PC CPU: no
emulator, no interpreter. The PlayStation hardware it talks to (graphics,
sound, CD drive, memory cards, controllers) is replaced by PC code.

> **Status: playable through Day 3 of Disc 1.** On one Windows PC it has been
> played from the start through the opera, the first battles, the sewers and
> their bosses, to the end of Day 3, with music and sound effects. Saves load
> and save through `.mcd` memory card files (an ePSXe card worked as is), and
> the in-game menu has a **Save Game** entry. Days 4 onward and Disc 2 have not
> been tested yet. See [ROADMAP.md](ROADMAP.md) for where it is going and
> [pc/PROGRESS.md](pc/PROGRESS.md) for the detailed technical log.

**You need your own copy of the game.** This repository contains no game data:
no disc images, no executable, no movies, no extracted assets. The program reads
`SLUS_006.62` and the rest straight from your Disc 1 image (`.cue`/`.bin`) and
checks the executable's SHA-1 (`452fb033f2eaa4b18aa20a5bca60b8125af3a37b`).
Please do not open issues or pull requests containing game files.

## Credits

This port stands on two projects:

- **[parasite-eve-pc](https://github.com/khasinski/parasite-eve-pc)** and the
  **[parasite-eve-decomp](https://github.com/khasinski/parasite-eve-decomp)**
  by Chris Hasiński (khasinski) — the decompilation of the whole game back
  into C: every game function this port runs is his reconstruction. Without it
  none of this would exist. His code is **not** included in this repository;
  `scripts/setup-tree.sh` downloads it from his repository at the version this
  port is built on (commit `f24c682`, 2026-10-09).
- **[PsyZ](https://github.com/Xeeynamo/psyz)** by Xeeynamo — a PC replacement
  for Sony's PSY-Q libraries on SDL3 (submodule `third_party/psyz`).

This repository holds only the port layer (`pc/` and `tools/pc/`), written for
this project.

## How it works (short version)

1. **The game is real C, compiled for x86.** The decomp's C is compiled with
   GCC/mingw as a 32-bit program.
2. **PS1 memory sits at the same addresses.** The program reserves
   `0x80000000` (2 MiB RAM, plus the three mirrors a retail console has) and the
   scratchpad, and loads your `SLUS_006.62` there, so every hard-coded address
   in the game still points at the right data.
3. **Function pointers are redirected.** At the original address of every
   function, a 5-byte x86 jump to the compiled C is written, so pointers stored
   in game data call PC code (2062 functions, main executable and the field
   engine).
4. **Overlays are recognised as they load.** All 190 code chunks the game loads
   from `PE.IMG` are compiled in; after each CD read the loaded chunk is checked
   against your disc and its functions are switched to the compiled C.
5. **PSY-Q is replaced.** Graphics and sound go through PsyZ (SDL3's GPU
   API; Direct3D 12 on the Windows PC it was tested on); the CD drive becomes your disc image; the movie
   decoder (MDEC) is done in software; memory cards are `.mcd` files.
6. **Hardware quirks the game relies on are reproduced** — RAM mirrors, reads
   through NULL pointers (the PS1 has its kernel RAM at address 0), and a few
   routines the decomp keeps as raw machine code, which are translated to C
   from your executable at build time.

## Controls (PsyZ defaults)

| PS1 | Keyboard | | PS1 | Keyboard |
| --- | --- | --- | --- | --- |
| Start | Enter | | Cross | X |
| Select | Backspace | | Circle | D |
| D-pad | Arrow keys | | Triangle | S |
| L1 / R1 | Q / R | | Square | Z |
| L2 / R2 | W / E | | | |

Xbox-style gamepads work through SDL (tested with an Xbox 360 controller).

Extra keys in this port:

| Key | What it does |
| --- | --- |
| Triangle menu → **Save Game** | opens the save screen anywhere the menu can open (last entry of the left column) |
| F5 | the same save screen, as a shortcut |
| ` / ~ (key under Escape) | skip the movie that is playing |
| Tab (hold) | fast forward |
| F12 | snapshot: the frame's drawing commands, picture and video memory, for bug reports |

Memory cards are `memcards/card1.mcd` and `card2.mcd` next to where the game
runs (128 KiB raw images, the format ePSXe and other emulators use). If
`card1.mcd` is missing, a blank formatted card is inserted and saved there on
the first save. Loading a save puts Aya at the room's start point: the
game's save does not record where she stood.

## Getting the source

```sh
git clone <this repo> parasite-eve-native
cd parasite-eve-native
scripts/setup-tree.sh work        # needs git + bash (Windows: Git Bash or WSL)
```

This downloads khasinski/parasite-eve-pc at commit `f24c682` into `work/`,
adds this repository's files on a branch `native` and applies the PsyZ patches.
All building happens inside `work/`.

## Building

The build is not one-click yet, and it has **not yet been tested from a fresh
setup**. So far it has only been built in one Linux environment (Ubuntu 24.04,
32-bit GCC and mingw-w64 i686), which also cross-compiles the Windows `.exe`;
that `.exe` has been run on one Windows PC (Direct3D 12). The setup
workarounds that environment needed are in [pc/PROGRESS.md](pc/PROGRESS.md)
("Cloud workspace setup"). The order of steps used there, inside `work/`:

```sh
cp /path/to/your/SLUS_006.62 assets/USA/main.exe   # from your Disc 1
make build                                          # the decomp's PS1 build (its own toolchain)
python3 tools/pc/gen_func_table.py build/pc-tables/func_table_all.c --json build/pc-tables/func_table_all.json
python3 tools/pc/gen_abs_syms.py build/pc-tables/pe_abs_syms.ld --json build/pc-tables/pe_abs_syms.json
# PsyZ + SDL3, Windows target (Linux: -B build/pc-i686 with -m32 flags, see PROGRESS.md)
cmake -S pc -B build/pc-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/pc/host/mingw-i686.cmake && cmake --build build/pc-win32
PE_TARGET=win32 tools/pc/build_all.sh               # game C + overlays + link
build/pc-link-win32/parasite-eve.exe "Parasite Eve (USA) (Disc 1).cue"
```

Debug options (environment variables): `PE_CAPTURE_DIR` + `PE_CAPTURE_EVERY`
(save frames), `PE_EXIT_AFTER`, `PE_PAD_SCRIPT` (scripted button presses),
`PE_KEY_SCRIPT` (scripted hotkeys), `PE_LOG_CD`, `PE_LOG_OVERLAYS`,
`PE_LOG_GPU` / `PE_DUMP_GPU`, `PE_LOG_PAD`, `PE_LOG_SPU`, `PE_LOG_VSYNC`,
`PE_SNAPSHOT_RAM` (F12 also saves PS1 RAM). On a crash the log gets a report
(call chain, last CD reads, overlays) and `crash-dump.bin` holds the PS1
memory; it contains game memory, so please do not post it publicly.

The shaders in `pc/psyz-patches/0004-*` were rebuilt with glslang and
spirv-cross (Ubuntu 24.04) and Microsoft's DXC v1.8.2505 for Linux.

## Known limits right now

- Only Disc 1, and only tested to the end of Day 3.
- No movie audio yet (XA audio sectors are skipped); music and sound effects
  play.
- Some ability and attack sound effects kept repeating; the latest fix (the
  sound sequencer's stop command) is waiting to be tested in a battle.
- Some values the game reads from PS1 kernel RAM through NULL pointers are not
  known; they read as 0 (see `pc/host/pe_host_lowmem.c`).
- 32-bit build only (the identity-mapped design needs it).

## Want to help?

Help is very welcome, especially from people who know:

- **PS1 audio** — XA/ADPCM movie audio, the SPU, the game's sound driver.
- **PS1 graphics** — the GPU, GTE precision (PGXP-style fixes), and later a
  native renderer.
- **Testing** — playing further with your own disc and reporting where it
  breaks. Frame captures (`PE_CAPTURE_DIR`) and the log help a lot.

Open an issue describing what you see (no game files, please).

## License

No license has been chosen yet for the port code in this repository.
The decomp is not part of this repository and keeps its own terms (its
LICENSE.md currently says no open-source license has been selected). PsyZ keeps
its MIT / MPL-2.0 terms; the changes to its MPL-2.0 files are in
`pc/psyz-patches/`. The host patches in `pc/host/patches/` change a few lines
of decomp files and quote a few surrounding lines as context.
