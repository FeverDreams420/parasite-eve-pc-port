# Parasite Eve — native PC port (work in progress)

A native Windows/Linux build of **Parasite Eve** (PlayStation, North America,
`SLUS-00662`). The game's own code runs as compiled C on the PC CPU: no
emulator, no interpreter. The PlayStation hardware it talks to (graphics,
sound, CD drive, memory cards, controllers) is replaced by PC code.

> **Status: boots and plays the opening.** On a real disc image it reaches the
> publisher screen, the opening movie, the title screen, the New Game / Tutorial
> menu, the New Game movie, the first scene outside Carnegie Hall (Aya, her date
> and the doorman, with dialogue) and the name-entry screen. Beyond that point
> has not been tested yet. See [ROADMAP.md](ROADMAP.md) for where it is going
> and [pc/PROGRESS.md](pc/PROGRESS.md) for the detailed technical log.

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
   in game data call PC code (1718 functions in the main executable).
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
`PE_LOG_CD`, `PE_LOG_OVERLAYS`, `PE_LOG_GPU` / `PE_DUMP_GPU`, `PE_LOG_PAD`.

## Known limits right now

- Only Disc 1, and only the opening has been played.
- No movie audio yet (XA audio sectors are skipped).
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
