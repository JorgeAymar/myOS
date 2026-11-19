#!/usr/bin/env bash
# Builds a FAT32 disk image (superfloppy, no partition table) with sample files.
# Usage: tools/mkdisk.sh <output.img> [size-MiB]
# Needs mtools (brew install mtools); nothing is mounted on the host.
set -euo pipefail
OUT="$1"
SIZE_MB="${2:-64}"

for tool in mformat mcopy mmd; do
    command -v "$tool" >/dev/null || { echo "mkdisk: '$tool' not found (brew install mtools)" >&2; exit 1; }
done

TMP="$OUT.tmp"
rm -f "$TMP"
dd if=/dev/zero of="$TMP" bs=1048576 count="$SIZE_MB" status=none
# -F: FAT32, 512-byte sectors, 1 sector per cluster (forces > 65525 clusters at 64 MiB)
mformat -i "$TMP" -F -c 1 -v MYOSDISK -T $((SIZE_MB * 2048)) -h 64 -s 32 ::

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
printf 'Hello from the myOS FAT32 disk!\n' > "$STAGE/HELLO.TXT"
printf 'Long file names work.\n' > "$STAGE/A long file name.txt"
printf 'Files on this disk are mounted at /disk in myOS.\n' > "$STAGE/readme.txt"

export MTOOLS_NO_VFAT=0 MTOOLS_SKIP_CHECK=1
mcopy -i "$TMP" "$STAGE/HELLO.TXT" ::/HELLO.TXT
mmd -i "$TMP" ::/docs
mcopy -i "$TMP" "$STAGE/A long file name.txt" "::/docs/A long file name.txt"
mcopy -i "$TMP" "$STAGE/readme.txt" ::/docs/readme.txt
mv "$TMP" "$OUT"
echo "mkdisk: $OUT (${SIZE_MB} MiB FAT32)"
