#!/usr/bin/env bash
# One-command build of the native port on Linux or WSL (Ubuntu 24.04).
#
# Usage: scripts/build.sh <Disc 1 .cue or .bin> [work folder]   (default: ./work)
#   PE_TARGET=win32 (default) builds parasite-eve.exe for Windows,
#   PE_TARGET=linux the 32-bit Linux binary, PE_TARGET=both builds both.
#   PE_JOBS=N sets the number of parallel jobs (default: all cores).
#   PE_DOWNLOADS=<folder> takes the source code, compilers and Python packages
#   from a cache made by scripts/fetch-windows.py instead of the internet.
#
# Steps: source tree (scripts/setup-tree.sh), Python venv, MIPS tools, the
# decomp's PS1 build, function tables, overlays, PsyZ, host build and link.
# Each finished step leaves its output in place, so running the script again
# after a failure continues where it stopped. Logs go to <work>/build/logs/.
#
# Packages (Ubuntu 24.04):
#   sudo apt-get install git make curl python3-venv cmake ninja-build \
#     binutils-mipsel-linux-gnu gcc-mingw-w64-i686 g++-mingw-w64-i686 \
#     gcc-multilib g++-multilib
# The disc image is only read on your machine; nothing from it is uploaded.
set -euo pipefail

if [ $# -lt 1 ]; then
    sed -n '4,7p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 1
fi
DISC=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${2:-"$HERE/work"}
TARGET=${PE_TARGET:-win32}
JOBS=${PE_JOBS:-$(nproc)}

case "$TARGET" in
    win32) TARGETS=win32 ;;
    linux) TARGETS=linux ;;
    both) TARGETS="linux win32" ;;
    *) echo "error: PE_TARGET must be win32, linux or both" >&2; exit 1 ;;
esac

step() { printf '\n== %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

step "Checking tools"
missing=""
for t in git make curl python3 cmake ninja mipsel-linux-gnu-as; do
    command -v "$t" >/dev/null || missing="$missing $t"
done
case " $TARGETS " in *" win32 "*)
    command -v i686-w64-mingw32-gcc >/dev/null || missing="$missing i686-w64-mingw32-gcc" ;;
esac
case " $TARGETS " in *" linux "*)
    echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null \
        || missing="$missing gcc-multilib" ;;
esac
python3 -c 'import venv, ensurepip' 2>/dev/null || missing="$missing python3-venv"
[ -z "$missing" ] || die "missing:$missing (see the package list at the top of $0)"

PIP_SOURCE=()
if [ -n "${PE_DOWNLOADS:-}" ]; then
    [ -f "$PE_DOWNLOADS/git-urls.txt" ] || die "$PE_DOWNLOADS/git-urls.txt missing (run scripts/fetch-windows.py)"
    # Every git download (also pip's) comes from the cached repositories.
    n=0
    while read -r url name; do
        # file://, not a plain path: pip's clone uses --filter, which a
        # plain local path ignores.
        export "GIT_CONFIG_KEY_$n=url.file://$PE_DOWNLOADS/git/$name.git.insteadOf" "GIT_CONFIG_VALUE_$n=$url"
        n=$((n + 1))
    done < "$PE_DOWNLOADS/git-urls.txt"
    # The cache is on the Windows drive, owned by another user as git sees it.
    export "GIT_CONFIG_KEY_$n=safe.directory" "GIT_CONFIG_VALUE_$n=*"
    export GIT_CONFIG_COUNT=$((n + 1))
    PIP_SOURCE=(--no-index --find-links "$PE_DOWNLOADS/pip")
fi

if [ ! -d "$WORK" ]; then
    step "Source tree"
    # Built aside and moved in when complete, so an interrupted download
    # does not leave a half tree that the next run would take as done.
    rm -rf "$WORK.part"
    "$HERE/scripts/setup-tree.sh" "$WORK.part"
    mv "$WORK.part" "$WORK"
fi
cd "$WORK"
WORK=$(pwd)
LOGS="$WORK/build/logs"
mkdir -p "$LOGS"

step "Python venv"
if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
fi
.venv/bin/pip install -q "${PIP_SOURCE[@]}" -r requirements.txt pycdlib > "$LOGS/pip.log" 2>&1 \
    || { tail -20 "$LOGS/pip.log"; die "pip failed, see $LOGS/pip.log"; }

# The decomp calls the MIPS tools mipsel-none-elf-*; Ubuntu ships them as
# mipsel-linux-gnu-*.
mkdir -p .toolbin
for t in /usr/bin/mipsel-linux-gnu-*; do
    ln -sf "$t" ".toolbin/$(basename "$t" | sed s/mipsel-linux-gnu-/mipsel-none-elf-/)"
done
export PATH="$WORK/.toolbin:$WORK/.venv/bin:$PATH"
export PE_CPP="$WORK/tools/old-gcc/cpp"

step "PS1 compilers (old-gcc 2.7.2 / 2.8.1, maspsx)"
# setup_compiler <script>: with PE_DOWNLOADS, the script runs with its
# release URL pointed at the cached copy.
setup_compiler() {
    if [ -n "${PE_DOWNLOADS:-}" ]; then
        local local_script="tools/scripts/.$1.local.sh"
        # Percent-encoded (curl decodes file:// paths): sed would read a & or
        # # in the folder name (C:\Users\A&B\...) as its own.
        local url="file://$PE_DOWNLOADS/files"
        url=${url//\%/%25}; url=${url// /%20}; url=${url//\&/%26}; url=${url//\#/%23}
        sed "s#^REL=.*#REL=\"$url\"#" "tools/scripts/$1.sh" > "$local_script"
        bash "$local_script"
        rm -f "$local_script"
    else
        bash "tools/scripts/$1.sh"
    fi
}
[ -x tools/old-gcc/cc1 ] && [ -x tools/old-gcc/cpp ] || setup_compiler setup_stock_cc1
[ -x tools/old-gcc-281/cc1 ] || setup_compiler setup_stock_cc281
[ -f tools/maspsx/maspsx.py ] || bash tools/scripts/setup_maspsx.sh

step "Files from Disc 1"
if [ -f assets/USA/main.exe ] && [ -f disc/extracted/PE.IMG ]; then
    echo "already there"
else
    python3 tools/pc/extract_disc.py "$DISC" .
fi

step "Decomp PS1 build (make build)"
# Linking main.elf needs the PSY-Q libraries' old GAS and fails; the PC build
# only needs the game objects, which are checked below.
make -k -j"$JOBS" build > "$LOGS/make-build.log" 2>&1 || true
missing=0
while read -r src; do
    [ -f "build/USA/$src.o" ] || { echo "missing build/USA/$src.o"; missing=1; }
done < <(find src/main -name '*.c' -not -path 'src/main/psyq/*' | sort)
[ "$missing" = 0 ] || die "game objects missing, see $LOGS/make-build.log"

step "Function tables"
mkdir -p build/pc-tables
python3 tools/pc/gen_func_table.py build/pc-tables/func_table_all.c --json build/pc-tables/func_table_all.json
python3 tools/pc/gen_abs_syms.py build/pc-tables/pe_abs_syms.ld --json build/pc-tables/pe_abs_syms.json

step "Overlays (make overlay-build-all)"
make overlay-build-all > "$LOGS/overlay-build.log" 2>&1 \
    || { tail -20 "$LOGS/overlay-build.log"; die "overlay build failed, see $LOGS/overlay-build.log"; }

step "Overlay objects"
find src/overlays -name '*.c' | sort | xargs -P"$JOBS" -I{} sh -c \
    'mkdir -p build/USA-ovl-objs/$(dirname {}) && tools/scripts/cc.sh {} build/USA-ovl-objs/{}.o > /dev/null 2>&1 || echo "FAIL {}"' \
    | tee "$LOGS/overlay-objs-failures.txt"
[ ! -s "$LOGS/overlay-objs-failures.txt" ] || die "overlay objects failed (list in $LOGS/overlay-objs-failures.txt)"

for t in $TARGETS; do
    step "PsyZ ($t)"
    if [ "$t" = win32 ]; then
        cmake -S pc -B build/pc-win32 -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_TOOLCHAIN_FILE="$WORK/pc/host/mingw-i686.cmake" > "$LOGS/cmake-win32.log" 2>&1 \
            && cmake --build build/pc-win32 >> "$LOGS/cmake-win32.log" 2>&1 \
            || { tail -20 "$LOGS/cmake-win32.log"; die "PsyZ build failed, see $LOGS/cmake-win32.log"; }
    else
        cmake -S pc -B build/pc-i686 -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_EXE_LINKER_FLAGS=-m32 \
            ${PE_CMAKE_LINUX_FLAGS:-} > "$LOGS/cmake-linux.log" 2>&1 \
            && cmake --build build/pc-i686 >> "$LOGS/cmake-linux.log" 2>&1 \
            || { tail -20 "$LOGS/cmake-linux.log"; die "PsyZ build failed, see $LOGS/cmake-linux.log"; }
    fi

    step "Host build and link ($t)"
    PE_TARGET=$t PE_JOBS=$JOBS tools/pc/build_all.sh > "$LOGS/link-$t.log" 2>&1 \
        || { tail -20 "$LOGS/link-$t.log"; die "host build failed, see $LOGS/link-$t.log"; }
done

step "Release copy"
mkdir -p dist
for t in $TARGETS; do
    if [ "$t" = win32 ]; then
        i686-w64-mingw32-strip --strip-debug -o dist/parasite-eve.exe build/pc-link-win32/parasite-eve.exe
        out=dist/parasite-eve.exe
    else
        strip --strip-debug -o dist/parasite-eve build/pc-link/parasite-eve
        out=dist/parasite-eve
    fi
    # Debug info holds build paths; the stripped copy must not.
    if grep -q -a -F "$WORK" "$out"; then
        echo "warning: $out still contains the build path $WORK" >&2
    fi
    echo "$out  $(sha256sum "$out" | cut -d' ' -f1)"
done
echo
echo "Done. Run it with your Disc 1 next to it, or pass the .cue:"
echo "  parasite-eve.exe \"Parasite Eve (USA) (Disc 1).cue\""
