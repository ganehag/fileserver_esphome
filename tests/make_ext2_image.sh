#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ $# != 2 ]]; then
  echo "usage: $0 IMAGE SIZE (for example 16M)" >&2
  exit 2
fi

image=$1
size=$2
out_dir=/tmp/opencode/esphome-ext2-tests
mkdir -p "$out_dir/obj"

# Keep this formatter separate from the firmware: WebDAV never formats a card.
for src in ext2_fs.c ext2_blockdev.c ext2_ram_blockdev.c; do
  gcc -std=c11 -Wall -Wextra \
    -Icomponents/webdav_filing \
    -c "components/webdav_filing/$src" -o "$out_dir/obj/$(basename "$src" .c).o"
done
gcc -std=c11 -Wall -Wextra \
  -Icomponents/webdav_filing \
  tests/make_ext2_image.c \
  "$out_dir/obj/ext2_fs.o" "$out_dir/obj/ext2_blockdev.o" \
  "$out_dir/obj/ext2_ram_blockdev.o" \
  -o "$out_dir/make_ext2_image"

mkdir -p "$(dirname "$image")"
truncate -s "$size" "$image"
"$out_dir/make_ext2_image" "$image" "$(stat -c %s "$image")"
