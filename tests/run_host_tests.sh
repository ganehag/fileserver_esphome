#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

out_dir=/tmp/opencode/esphome-ext2-tests
mkdir -p "$out_dir/obj"
san_flags=()
if [[ -n "${SAN_FLAGS:-}" ]]; then
  read -r -a san_flags <<<"$SAN_FLAGS"
fi
buffer_flags=()
if [[ -n "${FILE_BUFFER_SIZE:-}" ]]; then
  buffer_flags+=("-DWEBDAV_FILING_FILE_BUFFER_SIZE=${FILE_BUFFER_SIZE}")
fi

# The ext2 implementation is C; compile it as such. The SD adapter
# is intentionally omitted here (host tests use a RAM block device).
for src in ext2_fs.c ext2_blockdev.c ext2_ram_blockdev.c; do
  gcc -std=c11 -Wall -Wextra -DWEBDAV_FILING_HOST_TEST "${san_flags[@]}" \
    -Icomponents/webdav_filing \
    -c "components/webdav_filing/$src" -o "$out_dir/obj/$(basename "$src" .c).o"
done

g++ -std=c++17 -Wall -Wextra -DWEBDAV_FILING_HOST_TEST "${san_flags[@]}" \
  "${buffer_flags[@]}" \
  -DWEBDAV_FILING_HTTPD_MAX_OPEN_SOCKETS=12 \
  -DWEBDAV_FILING_HTTPD_BACKLOG_CONN=16 \
  -DWEBDAV_FILING_HTTPD_MAX_URI_LEN=4096 \
  -DWEBDAV_FILING_HTTPD_MAX_REQ_HDR_LEN=8192 \
  -Icomponents/webdav_filing -Itests/stubs \
  tests/host_test.cpp components/webdav_filing/webdav_filing.cpp \
  components/webdav_filing/dav_xml.cpp components/webdav_filing/dav_props.cpp \
  components/webdav_filing/partition_scan.cpp components/webdav_filing/vfs_ext2.cpp \
  components/webdav_filing/vfs_router.cpp \
  "$out_dir/obj/ext2_fs.o" "$out_dir/obj/ext2_blockdev.o" "$out_dir/obj/ext2_ram_blockdev.o" \
  -o "$out_dir/host_test"

"$out_dir/host_test"
