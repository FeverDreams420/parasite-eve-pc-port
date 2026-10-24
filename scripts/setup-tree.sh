#!/usr/bin/env bash
# Builds the working source tree for the native port:
#   khasinski/parasite-eve-pc at the version this port is built on, with this
#   repository's files (pc/, tools/pc/, ROADMAP.md) added on a branch "native",
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

git clone "$DECOMP_URL" "$DEST"
DEST=$(cd "$DEST" && pwd)
cd "$DEST"
git checkout -q -b native "$DECOMP_COMMIT"
# Only the submodules the PC build uses (PsyZ and its SDL3).
git submodule update --init third_party/psyz
git -C third_party/psyz submodule update --init external/SDL

mkdir -p pc tools/pc
cp -R "$HERE/pc/." pc/
cp -R "$HERE/tools/pc/." tools/pc/
cp "$HERE/ROADMAP.md" .

# PsyZ patches, in order (pc/psyz-patches/0001-..., 0002-..., ...).
for p in "$DEST"/pc/psyz-patches/[0-9][0-9][0-9][0-9]-*.patch; do
    git -C third_party/psyz apply "$p" || { echo "error: $p did not apply" >&2; exit 1; }
done

echo
echo "Source tree ready in $DEST (branch native)."
echo "Next: put your own SLUS_006.62 from Disc 1 at $DEST/assets/USA/main.exe"
echo "and follow the build steps in README.md."
