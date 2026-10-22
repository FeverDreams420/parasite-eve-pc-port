# Parasite Eve native PC port (work in progress)

This is my native PC port of **Parasite Eve** (PS1, North America, `SLUS-00662`).
The game's own code is compiled as C and runs directly on the PC. There's no
emulator. The PS1 hardware it talks to (graphics, sound, CD, memory cards,
controllers) is replaced with PC code.

**Where it's at:** I've played Disc 1 on my Windows PC from the start through
the end of Day 3: the opera, the first battles, the sewers and their bosses.
Music, sound effects and movie sound all work, and so does the play-time clock.
You can save from the in-game menu. Day 4 onward and Disc 2 haven't been tested
yet.

- [ROADMAP.md](ROADMAP.md): where this is going
- [pc/PROGRESS.md](pc/PROGRESS.md): technical notes and the bugs fixed so far

## You need your own game

This repo has **no game data**: no disc images, no executable, no movies, no
assets. The port reads everything from your own Disc 1 image (`.cue`/`.bin`)
and checks the executable's SHA-1 (`452fb033f2eaa4b18aa20a5bca60b8125af3a37b`).
Please don't post game files in issues or pull requests.

## Credits

- **[parasite-eve-pc](https://github.com/khasinski/parasite-eve-pc)** /
  **[parasite-eve-decomp](https://github.com/khasinski/parasite-eve-decomp)**
  by Chris Hasiński (khasinski). This is the decompilation of the whole game
  back into C. Every game function this port runs is his work, and none of
  this would exist without it. His code isn't in this repo:
  `scripts/setup-tree.sh` downloads it at the commit I build on (`f24c682`).
- **[PsyZ](https://github.com/Xeeynamo/psyz)** by Xeeynamo. It replaces Sony's
  PSY-Q libraries on SDL3. My changes to it are in `pc/psyz-patches/`.

This repo only holds the port layer (`pc/` and `tools/pc/`).

## How it works

- The decomp's C is compiled as a 32-bit x86 program.
- PS1 RAM is mapped at its real address (`0x80000000`), so every hard-coded
  address in the game still points to the right data.
- Every original function address gets a small jump to the compiled C, so
  function pointers stored in game data call PC code.
- Code the game loads from `PE.IMG` (overlays) is compiled in too. After each
  CD read, the port recognises what was loaded and switches those functions to C.
- Graphics and sound go through PsyZ (Direct3D 12 on my PC). The CD drive is
  your disc image, movies are decoded in software, and memory cards are
  `.mcd` files.

## Controls

| PS1 | Key | PS1 | Key |
| --- | --- | --- | --- |
| Start | Enter | Cross | X |
| Select | Backspace | Circle | D |
| D-pad | Arrows | Triangle | S |
| L1 / R1 | Q / R | Square | Z |
| L2 / R2 | W / E | | |

Xbox-style controllers work (I use an Xbox 360 pad).

| Extra | What it does |
| --- | --- |
| Triangle menu → **Save Game** | opens the save screen (last item in the left column) |
| F5 | same thing, as a shortcut |
| ` / ~ | skip the current movie |
| Tab (hold) | fast forward |
| F12 | snapshot for bug reports (drawing commands, picture, video memory) |

Memory cards go in `memcards/card1.mcd` and `card2.mcd` next to the exe. They're
128 KB raw images, the same format ePSXe uses, so emulator saves work as they
are. If there's no `card1.mcd`, you get a blank card that's written on your
first save. Close the game before swapping card files. After you load a save,
Aya starts at the room's entrance, because the game's save doesn't record
where she was standing.

## Building

The build isn't one-click yet, and I haven't tested it from a fresh setup. I've
only built it on Ubuntu 24.04 (32-bit GCC plus mingw-w64 i686, which also
builds the Windows exe), and only run the exe on my own PC.

```sh
git clone https://github.com/FeverDreams420/parasite-eve-pc-port
cd parasite-eve-pc-port
scripts/setup-tree.sh work        # needs git + bash (on Windows: Git Bash or WSL)
cd work
cp /path/to/SLUS_006.62 assets/USA/main.exe        # from your Disc 1
make build                                          # the decomp's own PS1 build
python3 tools/pc/gen_func_table.py build/pc-tables/func_table_all.c --json build/pc-tables/func_table_all.json
python3 tools/pc/gen_abs_syms.py build/pc-tables/pe_abs_syms.ld --json build/pc-tables/pe_abs_syms.json
cmake -S pc -B build/pc-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/pc/host/mingw-i686.cmake && cmake --build build/pc-win32
PE_TARGET=win32 tools/pc/build_all.sh
build/pc-link-win32/parasite-eve.exe "Parasite Eve (USA) (Disc 1).cue"
```

`setup-tree.sh` downloads the decomp, adds this repo's files on a `native`
branch and applies the PsyZ patches. The workarounds my build machine needed
and the Linux build flags are in [pc/PROGRESS.md](pc/PROGRESS.md).

If it crashes, the log gets a report and `crash-dump.bin` holds the game's
memory. That file contains game data, so please don't post it publicly.

## Known issues

- Only Disc 1, and only tested up to the end of Day 3.
- Movie lip sync hasn't been checked on a long cutscene yet, so the sound might
  drift.
- Some attack and ability sounds kept repeating. The latest fix is waiting to be
  tested in a battle.
- 32-bit build only for now.

## Help

Help is welcome, especially with PS1 audio, PS1 graphics and GTE precision
(PGXP-style), and testing further into the game with your own disc. Open an
issue describing what you see (no game files, please).

## License

I haven't picked a license for my code yet. The decomp isn't in this repo and
keeps its own terms. PsyZ keeps its MIT / MPL-2.0 terms, and my changes to its
files are in `pc/psyz-patches/`. The patches in `pc/host/patches/` change a few
lines of decomp files and quote a few lines around them as context.
