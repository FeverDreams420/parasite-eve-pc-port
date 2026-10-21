#!/usr/bin/env bash
# Link the 32-bit host program from the compiled game objects.
# Prerequisites: tools/pc/host_compile.sh for every game C file (build/pc-host),
# gen_func_table.py and gen_abs_syms.py outputs, and the i686 PsyZ build
# (build/pc-i686). Output: build/pc-link/parasite-eve
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT=build/pc-link
CC="${PE_HOST_CC:-gcc}"
CFLAGS="-m32 -fno-pie -fno-stack-protector -O1 -g -Wall -Wno-unused"
mkdir -p "$OUT/objs" "$OUT/rt"

"$CC" $CFLAGS -c pc/host/game_asm_host.c -o build/pc-host/game_asm_host.o
find build/pc-host/src -name '*.c.o' | sort > "$OUT/game_objs.txt"
echo build/pc-host/game_asm_host.o >> "$OUT/game_objs.txt"

# Shims: pc/host/psyq/*.c (PSY-Q functions implemented on the host).
: > "$OUT/shim_objs.txt"
for f in pc/host/psyq/*.c; do
    [ -f "$f" ] || continue
    o="$OUT/rt/shim_$(basename "$f" .c).o"
    "$CC" $CFLAGS -D__psyz -Ipc/host -Ithird_party/psyz/psyz/include -c "$f" -o "$o"
    echo "$o" >> "$OUT/shim_objs.txt"
done

# Decomp PSY-Q units compiled for the host (pc/host/psyq_units.txt).
: > "$OUT/psyq_objs.txt"
for f in $(grep -v '^#' pc/host/psyq_units.txt); do
    o="build/pc-psyq/$f.o"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
        tools/pc/host_compile.sh "$f" "$o" >/dev/null
    fi
    echo "$o" >> "$OUT/psyq_objs.txt"
done

python3 tools/pc/gen_link.py "$OUT" --game-objs "$OUT/game_objs.txt" \
    --psyq-objs "$OUT/psyq_objs.txt" --shim-objs "$OUT/shim_objs.txt"
{ cat "$OUT/psyq_rename.txt"; echo "main pe_game_main"; } > "$OUT/rename_all.txt"

: > "$OUT/link_objs.txt"
while read -r o; do
    d="$OUT/objs/${o#build/}"
    mkdir -p "$(dirname "$d")"
    objcopy --redefine-syms="$OUT/alias_rename.txt" "$o" "$d"
    objcopy --redefine-syms="$OUT/rename_all.txt" "$d"
    echo "$d" >> "$OUT/link_objs.txt"
done < <(cat "$OUT/game_objs.txt" "$OUT/psyq_objs.txt")

for f in pc/host/pe_host_mem.c pc/host/pe_host_main.c pc/host/pe_host_disc.c "$OUT/func_table.c" "$OUT/psyq_traps.c" "$OUT/psyq_stubs.c"; do
    "$CC" $CFLAGS -Ipc/host -c "$f" -o "$OUT/rt/$(basename "$f" .c).o"
done

"$CC" -m32 -no-pie -o "$OUT/parasite-eve" \
    $(cat "$OUT/link_objs.txt") $(cat "$OUT/shim_objs.txt") "$OUT"/rt/pe_host_*.o \
    "$OUT/rt/func_table.o" "$OUT/rt/psyq_traps.o" "$OUT/rt/psyq_stubs.o" \
    "$OUT/abs_syms.ld" \
    build/pc-i686/psyz/libpsyz.a build/pc-i686/psyz/sdl/libSDL3.a -lm -lpthread -ldl
echo "linked $OUT/parasite-eve"
