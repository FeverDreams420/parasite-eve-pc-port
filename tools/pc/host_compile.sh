#!/usr/bin/env bash
# Compile one decomp C file for the 32-bit x86 host.
# Usage: tools/pc/host_compile.sh <src.c> <out.o> [extra cflags]
# Writes <out>.i (preprocessed), <out>.host.c (asm rewritten) and
# <out>.asm.json (transform report) next to the object.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IN="$1"; OUT="$2"; shift 2
CC="${PE_HOST_CC:-gcc}"
ARCH="${PE_HOST_ARCH:--m32}"
mkdir -p "$(dirname "$OUT")"
# Host-only source fixes live in pc/host/patches/<path>.patch. The patched
# copy is compiled from the build tree; the source tree is never modified.
REL="${IN#$ROOT/}"
PATCH="$ROOT/pc/host/patches/$REL.patch"
SRCDIR="$(cd "$(dirname "$IN")" && pwd)"
if [ -f "$PATCH" ]; then
    cp "$IN" "$OUT.patched.c"
    patch -s -p1 -d "$(dirname "$OUT")" "$(basename "$OUT").patched.c" < "$PATCH" 2>/dev/null \
        || patch -s "$OUT.patched.c" < "$PATCH"
    IN="$OUT.patched.c"
fi
COMMON="$ARCH -fcommon -funsigned-char -fno-strict-aliasing -fwrapv -D__PE_HOST__=1"
"$CC" $COMMON -E -I"$SRCDIR" -I"$ROOT/pc/host/include" -I"$ROOT/include" -I"$ROOT/tools/m2c" "$@" "$IN" -o "$OUT.i"
python3 "$ROOT/tools/pc/prep_host_sources.py" "$OUT.i" "$OUT.host.c" --report "$OUT.asm.json" || true
"$CC" $COMMON -std=gnu11 -O1 -g -Wuninitialized -Wmaybe-uninitialized \
    -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
    -Wno-error=incompatible-pointer-types \
    -include "$ROOT/pc/host/pe_host_prelude.h" -c "$OUT.host.c" -o "$OUT" 2> "$OUT.warn" || { cat "$OUT.warn" >&2; exit 1; }
