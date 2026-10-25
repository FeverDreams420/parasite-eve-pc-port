#!/usr/bin/env bash
# Called by build-windows.bat inside WSL; runs scripts/build.sh with the work
# folder in the Linux home (much faster than the Windows drive) and copies
# parasite-eve.exe back to Windows. Downloads are made on Windows by
# scripts/fetch-windows.py into a cache folder (<cache> below), as WSL
# networking can be much slower than Windows on the same connection.
#
#   scripts/build-wsl.sh prepare <cache>       (as root) lists what fetch-windows.py must get
#   scripts/build-wsl.sh packages <cache>      (as root) installs the Ubuntu packages
#   scripts/build-wsl.sh build <disc> <outdir> <cache>
# All paths are Windows paths.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
PACKAGES="git make curl python3-venv cmake ninja-build binutils-mipsel-linux-gnu
    gcc-mingw-w64-i686 g++-mingw-w64-i686 gcc-multilib g++-multilib"

missing_packages() {
    local p
    for p in $PACKAGES; do
        [ "$(dpkg-query -W -f='${Status}' "$p" 2>/dev/null)" = "install ok installed" ] \
            || echo "$p"
    done
}

case "${1:-}" in
prepare)
    [ $# -eq 2 ] || { echo "usage: $0 prepare <cache>" >&2; exit 1; }
    CACHE=$(wslpath -a "$2")
    mkdir -p "$CACHE"
    python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])' > "$CACHE/wsl-python.txt"
    missing=$(missing_packages)
    if [ -z "$missing" ]; then
        echo "Ubuntu packages already installed."
        : > "$CACHE/apt-uris.txt"
        exit 0
    fi
    # The package lists come from WSL itself; the packages are then
    # downloaded on Windows.
    echo "Missing:" $missing
    apt-get update -q
    apt-get install --print-uris -qq -y $missing > "$CACHE/apt-uris.txt"
    ;;
packages)
    [ $# -eq 2 ] || { echo "usage: $0 packages <cache>" >&2; exit 1; }
    CACHE=$(wslpath -a "$2")
    missing=$(missing_packages)
    if [ -z "$missing" ]; then
        echo "Ubuntu packages already installed."
        exit 0
    fi
    echo "Installing:" $missing
    # apt checks each cached .deb against its package list before using it.
    if compgen -G "$CACHE/apt/*.deb" > /dev/null; then
        cp "$CACHE"/apt/*.deb /var/cache/apt/archives/
    fi
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q $missing
    ;;
build)
    [ $# -eq 4 ] || { echo "usage: $0 build <disc> <outdir> <cache>" >&2; exit 1; }
    DISC=$(wslpath -a "$2")
    OUT=$(wslpath -a "$3")
    export PE_DOWNLOADS=$(wslpath -a "$4")
    [ -f "$DISC" ] || { echo "error: $DISC not found" >&2; exit 1; }
    WORK="$HOME/parasite-eve-build"

    # The work folder holds a copy of pc/ and tools/pc/; start it over when
    # this checkout's files have changed (after a git pull). An unchanged
    # checkout keeps it, so a failed build continues where it stopped.
    STAMP=$(cd "$HERE" && find pc tools scripts -type f -print0 | sort -z \
        | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
    if [ -d "$WORK" ] && [ "$(cat "$WORK.stamp" 2>/dev/null)" != "$STAMP" ]; then
        echo "Port files changed since the last build: starting the work folder over."
        rm -rf "$WORK"
    fi
    echo "$STAMP" > "$WORK.stamp"
    PE_TARGET=win32 "$HERE/scripts/build.sh" "$DISC" "$WORK"
    cp "$WORK/dist/parasite-eve.exe" "$OUT/parasite-eve.exe"
    echo "Copied to $3\\parasite-eve.exe"
    ;;
*)
    sed -n '8,10p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 1
    ;;
esac
