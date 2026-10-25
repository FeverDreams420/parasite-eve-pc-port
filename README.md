# Parasite Eve native PC port (playable)

Disc 2 has been tested: you can play the whole game to the end, including the
end-game content. No need to build anything: just download the release and
point it to your disc files.

This port adds an HD option, and you can switch back to the original graphics
whenever you want. Nothing is emulated: it is a fully native x86 program.

## Features added

- Widescreen support and 8x foreground models.
- Save option in the Triangle menu during normal play (you can't save in
  battles).
- Switch back to the original graphics at any time with F7, or with the new
  Graphics option in the Triangle menu's User Preferences.

## What you need

- Your own Parasite Eve (USA) **Disc 1** image (`.cue` + `.bin`). Nothing from
  the game is included or downloaded. For the second half of the game you
  also need **Disc 2**: keep it next to Disc 1 with "Disc 2" in its name, or
  pick it when the game asks (saved as `disc2=` in `settings.ini`). The game
  changes to it on its own disc screen.

## Playing on Windows

1. Download `parasite-eve-pc-port-v0.5-windows.zip` from
   [Releases](https://github.com/FeverDreams420/parasite-eve-pc-port/releases)
   and unzip it.
2. Double-click `parasite-eve.exe`. The first time, it asks for your Disc 1
   `.cue` (or finds it in the same folder), and for Disc 2 if it isn't next
   to Disc 1. Both are remembered in `settings.ini`.

Saves go to `memcards` next to the exe. Nothing else needs to be installed
(Windows 10 or 11).

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
| F5 | Open the save screen |
| ` / ~ | Skip the movie |
| Tab (hold) | Fast forward |
| F4 | Fullscreen on/off |
| Shift+F4 | Borderless window (if fullscreen stutters) |
| F7 | Switch between HD and Original graphics |
| F12 | Snapshot for bug reports |

## Building it yourself (optional)

Only needed to build the exe from source. Besides Disc 1 (the build reads
it), on Windows you need:

- **WSL**: in PowerShell as administrator run `wsl --install`, restart, then
  open Ubuntu once to create your user.
- [Git for Windows](https://git-scm.com/download/win) and
  [Python 3](https://www.python.org/downloads/windows/) (3.8 or newer).
  If either is missing, `build-windows.bat` offers to install it for you
  with winget.

On Linux you need none of the Windows items; see
[Linux or WSL](#linux-or-wsl-ubuntu-2404) below.

### Windows

1. Install everything above.
2. Download this repository (`git clone` or Code > Download ZIP) and unzip it.
3. Drop your Disc 1 `.cue` (and Disc 2 `.cue`) onto `build-windows.bat`, or
   double-click it and paste the paths. Disc 2 next to Disc 1 is found on
   its own; press Enter at the Disc 2 question if you don't have it.
4. When it says `Done`, double-click `parasite-eve.exe` in the same folder.
   The `settings.ini` written next to it names both discs (`disc=`,
   `disc2=`); copy it along if you move the exe.

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
argument, or set `disc=` in `settings.ini`. Disc 2 is found next to Disc 1, or
set `disc2=`. Logs are in `work/build/logs/`.

## Credits

Decomp by [khasinski](https://github.com/khasinski/parasite-eve-pc), PsyZ by
[Xeeynamo](https://github.com/Xeeynamo/psyz).

The release exe also contains PsyZ (MIT and MPL 2.0; source: this repository's
`pc/psyz-patches/` on top of Xeeynamo/psyz 6d9c573) and SDL 3 (zlib license);
see `THIRD_PARTY_NOTICES.txt` in the zip.

Unofficial fan project, not affiliated with Square Enix or Sony. Parasite Eve
belongs to Square Enix.
