#!/bin/sh
# Static arm64 libntfs-3g + tools, no FUSE driver, from upstream source.
# Pinned: tuxera/ntfs-3g edge 2026.9.28, commit 7f0f841f.
#   Scripts/build-libntfs-3g.sh SRC_DIR BUILD_DIR PREFIX
set -eu
SRC=${1:?source checkout}; BLD=${2:?build dir}; PFX=${3:?prefix}
( cd "$SRC" && git rev-parse --short=8 HEAD && ./autogen.sh )
mkdir -p "$BLD" && cd "$BLD"
"$SRC/configure" --prefix="$PFX" --disable-ntfs-3g --enable-ntfsprogs \
  --disable-shared --enable-static --disable-crypto \
  CFLAGS="-O2 -g -mmacosx-version-min=14.0"
make -j8
make install rootlibdir="$PFX/lib" rootsbindir="$PFX/sbin" rootbindir="$PFX/bin"
