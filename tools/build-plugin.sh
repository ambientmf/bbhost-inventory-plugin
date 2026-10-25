#!/bin/sh
# Builds plugin/inv_order.c into a Windows DLL with clang + mingw-w64.
#   tools/build-plugin.sh [BBHOST_SRC] [OUT_DIR]
# BBHOST_SRC: a bbhost checkout (for include/bbhost_plugin.h), default /mnt/c/bb/bbhost-dev
# OUT_DIR:    where the DLL goes, default build/
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
bbhost=${1:-/mnt/c/bb/bbhost-dev}
outdir=${2:-$here/build}
mkdir -p "$outdir"
prlimit --as=2000000000 clang --target=x86_64-w64-mingw32 -O2 -Wall -Wextra -shared \
    -I"$bbhost/include" -o "$outdir/inv_order.dll" "$here/plugin/inv_order.c"
echo "built $outdir/inv_order.dll"
