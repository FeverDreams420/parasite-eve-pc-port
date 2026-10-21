#!/usr/bin/env bash
# Full host build: compile every game and overlay C file, then link.
# PE_TARGET=linux (default) or win32. Uses all CPU cores.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
TARGET="${PE_TARGET:-linux}"
case "$TARGET" in
    linux) HOST=build/pc-host ;;
    win32) HOST=build/pc-host-win32 ;;
esac
J="${PE_JOBS:-$(nproc)}"
LOG="build/pc-logs-$TARGET"
mkdir -p "$LOG"
fail=0
for set in main ovl; do
    if [ "$set" = main ]; then
        list=$(find src/main -name '*.c' -not -path 'src/main/psyq/*' | sort); out="$HOST"
    else
        list=$(find src/overlays -name '*.c' | sort); out="$HOST-ovl"
    fi
    printf '%s\n' $list | PE_TARGET="$TARGET" xargs -P"$J" -I{} sh -c \
        'tools/pc/host_compile.sh {} '"$out"'/{}.o > '"$LOG"'/$(echo {} | tr / _).txt 2>&1 || echo "FAIL {}"' \
        | tee "$LOG/failures-$set.txt"
    [ -s "$LOG/failures-$set.txt" ] && fail=1
done
[ "$fail" = 0 ] || { echo "compile failures, see $LOG" >&2; exit 1; }
rm -rf "build/pc-psyq" "build/pc-psyq-win32"
PE_TARGET="$TARGET" tools/pc/link_host.sh
