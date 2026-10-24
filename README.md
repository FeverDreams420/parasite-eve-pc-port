# Native port for Parasite Eve PC

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

Linux or WSL (Ubuntu 24.04). You need your own Disc 1 image (`.cue`/`.bin`).

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
