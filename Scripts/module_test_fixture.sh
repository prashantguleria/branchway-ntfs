#!/bin/sh
# Builds the module test fixtures (images only, never a device):
#   fixture.img  128 MiB NTFS, label MODTEST, /$BwJournal (16 MiB), /hello.txt (resident),
#                /big.bin (1 MiB, non-resident), /dir/
#   dirty.img    fixture with VOLUME_IS_DIRTY set   (mounts read-only)
#   hiber.img    fixture with an in-use hiberfil.sys (mounts read-only)
# Usage: Scripts/module_test_fixture.sh [DIR]   (default /Volumes/Media/spikes/ntfs/s6work/ntfs2/module)
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:-/Volumes/Media/spikes/ntfs/s6work/ntfs2/module}
MKNTFS=$REPO/build/libntfs-3g/sbin/mkntfs
H=$REPO/build/ntfsharness
case "$DIR" in /dev/*) echo "refusing a device path" >&2; exit 2;; esac
test -x "$MKNTFS" || { echo "run 'make libntfs' first" >&2; exit 1; }
test -x "$H" || { echo "run 'make' first (build/ntfsharness)" >&2; exit 1; }
mkdir -p "$DIR"
IMG=$DIR/fixture.img
rm -f "$IMG" "$DIR/dirty.img" "$DIR/hiber.img"
SIZE=$((128 * 1024 * 1024))
/usr/bin/truncate -s $SIZE "$IMG" 2>/dev/null || dd if=/dev/zero of="$IMG" bs=1 count=0 seek=$SIZE 2>/dev/null
"$MKNTFS" -Q -F -c 4096 -s 512 -p 2048 -H 255 -S 63 -L MODTEST "$IMG" $((SIZE / 512)) >/dev/null
"$H" jinit --image "$IMG" --size 16 >/dev/null
T=$(mktemp -d "${TMPDIR:-/tmp}/bwfix.XXXXXX")
printf 'hello, ntfs\n' > "$T/hello.txt"
/usr/bin/perl -e 'print chr($_ % 251) for 0 .. 1048575' > "$T/big.bin"
"$H" write --image "$IMG" --rw "$T/hello.txt" /hello.txt >/dev/null
"$H" write --image "$IMG" --rw "$T/big.bin" /big.bin >/dev/null
"$H" mkdir --image "$IMG" --rw /dir >/dev/null
rm -rf "$T"
cp "$IMG" "$DIR/dirty.img"; "$H" setflag --image "$DIR/dirty.img" --rw dirty >/dev/null
cp "$IMG" "$DIR/hiber.img"; "$H" setflag --image "$DIR/hiber.img" --rw hiberfile >/dev/null
"$H" fsck --image "$IMG" | tail -1
ls -l "$DIR"
