# Native port for Parasite Eve PC *WORK IN PROGRESS* 
still need disc 2 tested and implemented into the build

## What you need first

- Your own Parasite Eve **Disc 1** image (`.cue` + `.bin`). Nothing from the
  game is included or downloaded.
- Windows 10 or 11 with **WSL**: in PowerShell as administrator run
  `wsl --install`, restart, then open Ubuntu once to create your user.
- [Git for Windows](https://git-scm.com/download/win) and
  [Python 3](https://www.python.org/downloads/windows/) (3.8 or newer).
  If either is missing, `build-windows.bat` offers to install it for you
  with winget.

On Linux you need none of the Windows items; see
[Linux or WSL](#linux-or-wsl-ubuntu-2404) below.

## Controls

| PS1 | Key | PS1 | Key |
| --- | --- | --- | --- |
| Start | Enter | Cross | X |
| Select | Backspace | Circle | D |
| D-pad | Arrows | Triangle | S |
| L1 / R1 | Q / R | Square | Z |
| L2 / R2 | W / E | | |

Xbox-style controllers work.

| Key | What it does |
| --- | --- |
| F5 | save screen |
| ` / ~ | skip movie |
| Tab (hold) | fast forward |
| F4 | fullscreen on / off |
| Shift+F4 | borderless window (fallback if fullscreen stutters) |
| F12 | snapshot for bug reports |

## Building

### Windows

1. Install everything under [What you need first](#what-you-need-first).
2. Download this repository (`git clone` or Code > Download ZIP) and unzip it.
3. Drop your Disc 1 `.cue` onto `build-windows.bat` (or double-click it and
   paste the path).
4. When it says `Done`, put `parasite-eve.exe` in a folder with your Disc 1
   `.cue`/`.bin` (or set `disc=` in `settings.ini`) and double-click it.

It downloads the source code, compilers and packages on Windows into a
`downloads` folder next to the `.bat` (WSL's own connection can be much
slower), installs the Ubuntu packages it needs inside WSL, builds in your
Ubuntu home folder (`~/parasite-eve-build`) and writes `parasite-eve.exe` next
to the `.bat`. The first build takes a while; running it again continues or
updates. Ubuntu's package lists are still downloaded inside WSL when packages
are missing.

### Linux or WSL (Ubuntu 24.04)

```sh
sudo apt-get install git make curl python3-venv cmake ninja-build \
    binutils-mipsel-linux-gnu gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-multilib g++-multilib
git clone https://github.com/FeverDreams420/parasite-eve-pc-port
cd parasite-eve-pc-port
scripts/build.sh "/path/to/Parasite Eve (USA) (Disc 1).cue"
```

Output: `work/dist/parasite-eve.exe`. Start it with your Disc 1 `.cue` as the
argument, or set `disc=` in `settings.ini`. Logs are in `work/build/logs/`.

## Credits

Decomp by [khasinski](https://github.com/khasinski/parasite-eve-pc), PsyZ by
[Xeeynamo](https://github.com/Xeeynamo/psyz).
