#!/usr/bin/env bash
# Compile one decomp C file for the 32-bit x86 host.
# Usage: tools/pc/host_compile.sh <src.c> <out.o> [extra cflags]
# PE_TARGET=linux (default, gcc -m32) or win32 (i686-w64-mingw32-gcc).
# Writes <out>.i (preprocessed), <out>.host.c (asm rewritten),
# <out>.asm.json (transform report) and <out>.warn next to the object.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IN="$1"; OUT="$2"; shift 2
case "${PE_TARGET:-linux}" in
    linux) CC="${PE_HOST_CC:-gcc}"; ARCH="-m32 -fno-pie" ;;
    win32) CC="${PE_HOST_CC:-i686-w64-mingw32-gcc}"; ARCH="" ;;
    *) echo "unknown PE_TARGET" >&2; exit 2 ;;
esac
mkdir -p "$(dirname "$OUT")"
# Host-only source fixes live in pc/host/patches/<path>.patch. The patched
# copy is compiled from the build tree; the source tree is never modified.
REL="${IN#$ROOT/}"
PATCH="$ROOT/pc/host/patches/$REL.patch"
SRCDIR="$(cd "$(dirname "$IN")" && pwd)"
if [ -f "$PATCH" ]; then
    cp "$IN" "$OUT.patched.c"
    patch -s "$OUT.patched.c" < "$PATCH"
    IN="$OUT.patched.c"
fi
# Patched copies of headers (pc/host/patches/include/...) go first on the
# include path.
PINC="$ROOT/build/pc-host-include"
for hp in $(cd "$ROOT/pc/host/patches" && find include -name '*.h.patch' 2>/dev/null); do
    h="${hp%.patch}"
    dst="$PINC/${h#include/}"
    if [ ! -f "$dst" ] || [ "$ROOT/pc/host/patches/$hp" -nt "$dst" ] || [ "$ROOT/$h" -nt "$dst" ]; then
        mkdir -p "$(dirname "$dst")"
        cp "$ROOT/$h" "$dst.tmp.$$"
        patch -s "$dst.tmp.$$" < "$ROOT/pc/host/patches/$hp"
        mv "$dst.tmp.$$" "$dst"
    fi
done
COMMON="$ARCH -fno-stack-protector -fcommon -funsigned-char -fno-strict-aliasing -fwrapv -D__PE_HOST__=1"
"$CC" $COMMON -E -I"$SRCDIR" -I"$PINC" -I"$ROOT/pc/host/include" -I"$ROOT/include" -I"$ROOT/tools/m2c" "$@" "$IN" -o "$OUT.i"
python3 "$ROOT/tools/pc/prep_host_sources.py" "$OUT.i" "$OUT.host.c" --report "$OUT.asm.json" || true
"$CC" $COMMON -std=gnu11 ${PE_HOST_OPT:--O0} -g -Wuninitialized -Wmaybe-uninitialized \
    -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
    -Wno-error=incompatible-pointer-types \
    -include "$ROOT/pc/host/pe_host_prelude.h" -c "$OUT.host.c" -o "$OUT" 2> "$OUT.warn" || { cat "$OUT.warn" >&2; exit 1; }
