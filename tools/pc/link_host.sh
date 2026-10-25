#!/usr/bin/env bash
# Link the 32-bit host program from the compiled game objects.
# PE_TARGET=linux (default) -> build/pc-link/parasite-eve
# PE_TARGET=win32           -> build/pc-link-win32/parasite-eve.exe
# Prerequisites: tools/pc/host_compile.sh for every game C file (into
# build/pc-host or build/pc-host-win32), build/pc-tables/* from
# gen_func_table.py and gen_abs_syms.py, and the PsyZ build for the target
# (build/pc-i686 or build/pc-win32).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TARGET="${PE_TARGET:-linux}"
case "$TARGET" in
    linux)
        CC=gcc; NM=nm; OBJCOPY=objcopy; PREFIX=""
        CFLAGS="-m32 -fno-pie -fno-stack-protector -O1 -g -Wall -Wno-unused"
        HOST=build/pc-host; PSYQ=build/pc-psyq; OUT=build/pc-link; PSYZ=build/pc-i686
        EXE="$OUT/parasite-eve"
        LDFLAGS="-m32 -no-pie"
        LIBS="-lm -lpthread -ldl" ;;
    win32)
        CC=i686-w64-mingw32-gcc; NM=i686-w64-mingw32-nm; OBJCOPY=i686-w64-mingw32-objcopy; PREFIX="_"
        CFLAGS="-fno-stack-protector -O1 -g -Wall -Wno-unused"
        HOST=build/pc-host-win32; PSYQ=build/pc-psyq-win32; OUT=build/pc-link-win32; PSYZ=build/pc-win32
        EXE="$OUT/parasite-eve.exe"
        LDFLAGS="-Wl,--large-address-aware,--disable-dynamicbase -static"
        LIBS="-lm -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8 -lws2_32 -lwinspool -lcomdlg32" ;;
esac
mkdir -p "$OUT/objs" "$OUT/rt"

find "$HOST/src" -name '*.c.o' | sort > "$OUT/game_objs.txt"

# Shims: pc/host/psyq/*.c (PSY-Q functions implemented on the host).
: > "$OUT/shim_objs.txt"
for f in pc/host/psyq/*.c; do
    o="$OUT/rt/shim_$(basename "$f" .c).o"
    "$CC" $CFLAGS -D__psyz -Ipc/host -Ithird_party/psyz/psyz/include -c "$f" -o "$o"
    echo "$o" >> "$OUT/shim_objs.txt"
done

# Decomp PSY-Q units compiled for the host (pc/host/psyq_units.txt).
: > "$OUT/psyq_objs.txt"
for f in $(grep -v '^#' pc/host/psyq_units.txt); do
    o="$PSYQ/$f.o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        PE_TARGET="$TARGET" tools/pc/host_compile.sh "$f" "$o" >/dev/null
    fi
    echo "$o" >> "$OUT/psyq_objs.txt"
done

# Overlays: host objects of every overlay C source, then per-overlay names.
OVL_HOST="$HOST-ovl"
while read -r f; do
    o="$OVL_HOST/$f.o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ "pc/host/patches/$f.patch" -nt "$o" ]; then
        PE_TARGET="$TARGET" tools/pc/host_compile.sh "$f" "$o" >/dev/null
    fi
done < <(find src/overlays -name '*.c' | sort)
python3 tools/pc/gen_overlays.py "$OUT/ovl" --host-objs "$OVL_HOST" \
    --nm "$NM" --sym-prefix "$PREFIX"

python3 tools/pc/gen_link.py "$OUT" --game-objs "$OUT/game_objs.txt" \
    --psyq-objs "$OUT/psyq_objs.txt" --shim-objs "$OUT/shim_objs.txt" \
    --extra-refs "$OUT/ovl/overlay_global_refs.txt" \
    --nm "$NM" --sym-prefix "$PREFIX"

: > "$OUT/link_objs.txt"
while read -r o; do
    d="$OUT/objs/${o#build/}"
    mkdir -p "$(dirname "$d")"
    "$OBJCOPY" --redefine-syms="$OUT/alias_rename.txt" "$o" "$d"
    "$OBJCOPY" --redefine-syms="$OUT/psyq_rename.txt" "$d"
    echo "$d" >> "$OUT/link_objs.txt"
done < <(cat "$OUT/game_objs.txt" "$OUT/psyq_objs.txt")

# Per-overlay copies of the overlay objects.
: > "$OUT/ovl_link_objs.txt"
for rf in "$OUT"/ovl/rename_*.txt; do
    ov="$(basename "$rf" .txt)"; ov="${ov#rename_}"
    while read -r o; do
        d="$OUT/ovl/objs/$ov/$(basename "$o")"
        mkdir -p "$(dirname "$d")"
        "$OBJCOPY" --redefine-syms="$OUT/alias_rename.txt" "$o" "$d"
        "$OBJCOPY" --redefine-syms="$rf" "$d"
        "$OBJCOPY" --redefine-syms="$OUT/psyq_rename.txt" "$d"
        echo "$d" >> "$OUT/ovl_link_objs.txt"
    done < "$OUT/ovl/objs_$ov.txt"
done

for f in pc/host/pe_host_mem.c pc/host/pe_host_main.c pc/host/pe_host_disc.c pc/host/pe_host_sha1.c pc/host/pe_host_gte.c pc/host/pe_host_overlays.c pc/host/pe_host_lowmem.c pc/host/pe_host_datacode.c "$OUT/ovl/overlay_table.c" "$OUT/func_table.c" "$OUT/psyq_traps.c" "$OUT/psyq_stubs.c"; do
    "$CC" $CFLAGS -Ipc/host -c "$f" -o "$OUT/rt/$(basename "$f" .c).o"
done

"$CC" $LDFLAGS -o "$EXE" \
    $(cat "$OUT/link_objs.txt") $(cat "$OUT/shim_objs.txt") "$OUT"/rt/pe_host_*.o \
    "$OUT/rt/func_table.o" "$OUT/rt/psyq_traps.o" "$OUT/rt/psyq_stubs.o" \
    "$OUT/rt/overlay_table.o" $(cat "$OUT/ovl_link_objs.txt") \
    "$OUT/abs_syms.ld" "$OUT/ovl/overlay_abs.ld" \
    "$PSYZ/psyz/libpsyz.a" "$PSYZ/psyz/sdl/libSDL3.a" $LIBS
echo "linked $EXE"
