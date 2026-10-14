#!/usr/bin/env bash
# Build the 3DS port. Run from anywhere:
#   port/3ds/build.sh
#
# Must run inside devkitPro's own MSYS2 (its CMake refuses to run otherwise,
# and only that environment has a writable TMP for the compiler's temp files).
# This script re-execs itself there when needed.
#
# The build directory is always recreated: reconfiguring an existing one makes
# GNU Make choke on Windows drive-letter colons in the generated dependency
# files ("multiple target patterns"). Installing ninja into devkitPro's MSYS2
# (pacman -S ninja) would avoid this and allow incremental builds.
set -e

# devkitPro's MSYS2 and a generic MSYS2/Git-Bash mount the same drive at
# different roots, so hand the re-exec a Windows path and convert it there.
DKP_BASH=/c/devkitPro/msys2/usr/bin/bash.exe
if [ -z "$EB_3DS_IN_DKP" ] && [ -x "$DKP_BASH" ]; then
    SELF_WIN="$(cygpath -w "$(cd "$(dirname "$0")" && pwd)/$(basename "$0")")"
    exec "$DKP_BASH" -lc "EB_3DS_IN_DKP=1 bash \"\$(cygpath -u '$SELF_WIN')\""
fi

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

PORT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$PORT_DIR/../.." && pwd)"
EBTOOLS="$REPO_ROOT/.venv/Scripts/ebtools.exe"

if [ ! -x "$EBTOOLS" ]; then
    echo "ebtools not found at $EBTOOLS" >&2
    echo "Create it with:  python -m venv .venv && .venv/Scripts/python -m pip install -e ." >&2
    exit 1
fi

rm -rf "$PORT_DIR/build"
cmake -S "$PORT_DIR" -B "$PORT_DIR/build" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
    -DEBTOOLS_EXE="$EBTOOLS"
cmake --build "$PORT_DIR/build" -j4

echo
echo "Built: $PORT_DIR/build/earthbound_3ds.3dsx"
