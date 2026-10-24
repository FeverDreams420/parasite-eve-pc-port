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
| F4 | fullscreen on / off |
| Shift+F4 | borderless window one line shorter than the screen (fallback if fullscreen stutters) |
| F12 | snapshot for bug reports (drawing commands, picture, video memory) |

Memory cards go in `memcards/card1.mcd` and `card2.mcd` next to the exe. They're
128 KB raw images, the same format ePSXe uses, so emulator saves work as they
are. If there's no `card1.mcd`, you get a blank card that's written on your
first save. Close the game before swapping card files. After you load a save,
Aya starts at the room's entrance, because the game's save doesn't record
where she was standing.

## Building

Linux or WSL (Ubuntu 24.04). The build makes the Windows exe with mingw-w64,
and can also make a 32-bit Linux binary. It doesn't work on macOS: the port is
a 32-bit x86 program. The script is new and hasn't had a full run on a clean
machine yet; if a step fails, the error names the log that says why.

```sh
sudo apt-get install git make curl python3-venv cmake ninja-build \
    binutils-mipsel-linux-gnu gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-multilib g++-multilib
git clone https://github.com/FeverDreams420/parasite-eve-pc-port
cd parasite-eve-pc-port
scripts/build.sh "/path/to/Parasite Eve (USA) (Disc 1).cue"
```

The result is `work/dist/parasite-eve.exe`. Put it anywhere and start it with
your Disc 1 `.cue` as the argument, or set `disc=` in `settings.ini`.

What `scripts/build.sh` does, in order (logs in `work/build/logs/`):

1. `scripts/setup-tree.sh work`: downloads the decomp at `f24c682`, adds this
   repo's files on a `native` branch, gets PsyZ and SDL3 and applies
   `pc/psyz-patches/` in order.
2. Python venv with `requirements.txt` and `pycdlib`. Ubuntu's
   `mipsel-linux-gnu-*` tools are linked as `mipsel-none-elf-*` in
   `work/.toolbin`.
3. `tools/scripts/setup_stock_cc1.sh`, `setup_stock_cc281.sh` and
   `setup_maspsx.sh` (old-gcc 0.17 from GitHub releases, maspsx from GitHub).
4. `tools/pc/extract_disc.py` copies `SLUS_006.62` to `assets/USA/main.exe` and
   `PE.IMG` to `disc/extracted/PE.IMG` from your image and checks the exe's
   SHA-1. Nothing from the disc leaves your machine.
5. `make build`, the decomp's own PS1 build. Linking `main.elf` fails because
   five PSY-Q objects need old GAS; that's expected, since the PC build
   replaces PSY-Q. The script checks that every game object was built.
6. `tools/pc/gen_func_table.py` and `gen_abs_syms.py`, then
   `make overlay-build-all` and every `src/overlays/**/*.c` through
   `tools/scripts/cc.sh`.
7. PsyZ with CMake (`pc/host/mingw-i686.cmake` for Windows), then
   `tools/pc/build_all.sh`, then a copy without debug info in `work/dist/`.

Options: `PE_TARGET=linux` or `PE_TARGET=both` for the Linux binary
(`work/dist/parasite-eve`), `PE_JOBS=N` for the number of jobs, a second
argument for another work folder. Running the script again continues where it
stopped. On a headless machine, build the Linux PsyZ with
`PE_CMAKE_LINUX_FLAGS="-DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_UNIX_CONSOLE_BUILD=ON"`.
I haven't tried the Linux binary on a desktop, so I don't know which 32-bit
development packages SDL3 needs there.

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
