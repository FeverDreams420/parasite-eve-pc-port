#!/usr/bin/env bash
# Builds the working source tree for the native port:
#   khasinski/parasite-eve-pc at the version this port is built on, with this
#   repository's files (pc/, tools/pc/) added on a branch "native",
#   and the PsyZ patches applied.
#
# Usage: scripts/setup-tree.sh [destination folder]   (default: ./work)
# Needs git and bash (on Windows: Git Bash or WSL).
# It downloads only source code. It does not download or contain any game data.
set -euo pipefail

DECOMP_URL=https://github.com/khasinski/parasite-eve-pc.git
DECOMP_COMMIT=f24c6824961123aa4b414425574776413b1fdd14   # 2026-10-09, what the port is built and tested on

HERE=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:-"$HERE/work"}

if [ -e "$DEST" ]; then
    echo "error: $DEST already exists" >&2
    exit 1
fi

# Only the pinned commits, without history: a much smaller download than a
# full clone. Network steps are retried, as large GitHub fetches can drop.
retry() {
    local n
    for n in 1 2 3; do
        "$@" && return 0
        echo "(attempt $n failed, retrying)" >&2
        sleep 5
    done
    return 1
}
# HTTP/1.1: long HTTP/2 transfers from GitHub can break off ("curl 92").
git() { command git -c http.version=HTTP/1.1 "$@"; }
# fetch_commit <dir> <url> <commit>: one commit, no history.
fetch_commit() {
    git init -q "$1"
    git -C "$1" remote add origin "$2" 2>/dev/null || true
    retry git -C "$1" fetch --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
}
fetch_commit "$DEST" "$DECOMP_URL" "$DECOMP_COMMIT"
DEST=$(cd "$DEST" && pwd)
cd "$DEST"
git checkout -q -b native
# Only the submodules the PC build uses (PsyZ and its SDL3), each at the
# commit its parent pins.
gitlink() { git -C "$1" ls-tree HEAD "$2" | awk '{ print $3 }'; }
git submodule init third_party/psyz
fetch_commit third_party/psyz "$(git config submodule.third_party/psyz.url)" \
    "$(gitlink . third_party/psyz)"
git -C third_party/psyz submodule init external/SDL
fetch_commit third_party/psyz/external/SDL \
    "$(git -C third_party/psyz config submodule.external/SDL.url)" \
    "$(gitlink third_party/psyz external/SDL)"

mkdir -p pc tools/pc
cp -R "$HERE/pc/." pc/
cp -R "$HERE/tools/pc/." tools/pc/

# PsyZ patches, in order (pc/psyz-patches/0001-..., 0002-..., ...).
for p in "$DEST"/pc/psyz-patches/[0-9][0-9][0-9][0-9]-*.patch; do
    git -C third_party/psyz apply "$p" || { echo "error: $p did not apply" >&2; exit 1; }
done

echo
echo "Source tree ready in $DEST (branch native)."
echo "Next: put your own SLUS_006.62 from Disc 1 at $DEST/assets/USA/main.exe"
echo "and follow the build steps in README.md."
