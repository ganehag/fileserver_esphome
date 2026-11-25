#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

config=tests/auto-volume.yaml
build=davbox-auto-volume
base=/tmp/opencode/esphome-ext2-tests/card-ext2.img
work=$(mktemp -d)
emu=
log=

stop_emulator() {
  if [[ -n "$emu" ]]; then
    kill "$emu" 2>/dev/null || true
    wait "$emu" 2>/dev/null || true
    emu=
  fi
}

trap 'stop_emulator; rm -rf "$work"' EXIT

printf 'Preparing a host-formatted base ext2 image...\n'
tests/make_ext2_image.sh "$base" 16M
[[ -s "$base" ]] || { echo "base ext2 image missing: $base" >&2; exit 1; }

.venv/bin/esphome compile "$config" > "$work/build.log" 2>&1 || {
  cat "$work/build.log" >&2
  exit 1
}
# A config under tests/ builds into tests/.esphome; run_emulator.sh looks in the
# project root, so expose the same build directory there.
mkdir -p .esphome/build
ln -sfn "$PWD/tests/.esphome/build/$build" ".esphome/build/$build"

python3 - "$base" "$work/card-auto-mbr.img" "$work/card-auto-gpt.img" <<'PY'
import sys
import zlib
from pathlib import Path

sys.path.insert(0, "tests")
from fat16_image import build_fat16

base, mbr_path, gpt_path = sys.argv[1:]
ext2 = Path(base).read_bytes()
sector_size = 512
fat_start = 2048
fat_sectors = 4096  # 2 MiB FAT16 partition
fat2_start = 8192
ext2_start = 16384
ext2_sectors = 32768  # 16 MiB ext2 image prepared above
ext2_start2 = 49152
total = 131072  # 64 MiB card


def put_le(buf, offset, value, size):
    buf[offset:offset + size] = value.to_bytes(size, "little")


def make_fat16():
    return build_fat16(fat_sectors)


def new_card():
    card = bytearray(total * sector_size)
    fat = make_fat16()
    for start in (fat_start, fat2_start):
        card[start * sector_size:start * sector_size + len(fat)] = fat
    for start in (ext2_start, ext2_start2):
        card[start * sector_size:start * sector_size + len(ext2)] = ext2
    return card


def mbr_entry(card, index, partition_type, lba, count):
    p = 446 + index * 16
    card[p + 4] = partition_type
    card[p + 8:p + 12] = lba.to_bytes(4, "little")
    card[p + 12:p + 16] = count.to_bytes(4, "little")


mbr = new_card()
mbr_entry(mbr, 0, 0x0C, fat_start, fat_sectors)
mbr_entry(mbr, 1, 0x0C, fat2_start, fat_sectors)
mbr_entry(mbr, 2, 0x83, ext2_start, ext2_sectors)
mbr_entry(mbr, 3, 0x83, ext2_start2, ext2_sectors)
mbr[510:512] = b"\x55\xaa"
Path(mbr_path).write_bytes(mbr)

gpt = new_card()
mbr_entry(gpt, 0, 0xEE, 1, total - 1)
gpt[510:512] = b"\x55\xaa"

entries_lba = 2
entry_count = 128
entry_size = 128
header = gpt[512:1024]
header[0:8] = b"EFI PART"
put_le(header, 8, 0x00010000, 4)
put_le(header, 12, 92, 4)
put_le(header, 16, 0, 4)
put_le(header, 24, 1, 8)
put_le(header, 32, total - 1, 8)
put_le(header, 40, 34, 8)
put_le(header, 48, total - 34, 8)
put_le(header, 72, entries_lba, 8)
put_le(header, 80, entry_count, 4)
put_le(header, 84, entry_size, 4)
gpt[512:1024] = header


def gpt_entry(card, index, guid, first, last, name):
    p = 2 * 512 + index * 128
    card[p:p + 16] = guid
    put_le(card, p + 32, first, 8)
    put_le(card, p + 40, last, 8)
    raw = name.encode("utf-16-le")[:72]
    card[p + 56:p + 56 + len(raw)] = raw


gpt_entry(gpt, 0, bytes([1]) + bytes(15), fat_start, fat_start + fat_sectors - 1, "fatdata")
gpt_entry(gpt, 1, bytes([2]) + bytes(15), fat2_start, fat2_start + fat_sectors - 1, "fatdata2")
linux_guid = bytes([0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4])
gpt_entry(gpt, 2, linux_guid, ext2_start, ext2_start + ext2_sectors - 1, "davdata")
gpt_entry(gpt, 3, linux_guid, ext2_start2, ext2_start2 + ext2_sectors - 1, "davdata2")

entry_bytes = entry_count * entry_size
put_le(gpt, 512 + 88, zlib.crc32(gpt[entries_lba * 512:entries_lba * 512 + entry_bytes]) & 0xFFFFFFFF, 4)
put_le(gpt, 512 + 16, 0, 4)
put_le(gpt, 512 + 16, zlib.crc32(gpt[512:512 + 92]) & 0xFFFFFFFF, 4)
Path(gpt_path).write_bytes(gpt)
PY

wait_ready() {
  local url=$1 ready=0
  for _ in $(seq 1 100); do
    if curl -fsS -m 1 -X OPTIONS "$url" > /dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.5
  done
  if [[ "$ready" != 1 ]]; then
    echo "emulator did not become ready at $url" >&2
    tail -80 "$log" >&2
    exit 1
  fi
}

status() {
  curl -sS -m 15 -o "$work/body" -w '%{http_code}' "$@"
}

expect_status() {
  local expected=$1
  shift
  local actual
  actual=$(status "$@")
  if [[ "$actual" != "$expected" ]]; then
    echo "expected HTTP $expected, got $actual" >&2
    cat "$work/body" >&2
    tail -80 "$log" >&2
    exit 1
  fi
}

run_case() {
  local image=$1 port=$2 table=$3
  shift 3
  local names=("$@")
  log="$work/emulator-$table.log"
  : > "$log"

  timeout 90s env \
    WIFI_SSID=auto-smoke \
    WIFI_PASSWORD=auto-smoke-password \
    DAVBOX_CONFIG="$config" \
    DAVBOX_BUILD="$build" \
    DAVBOX_CARD="$image" \
    DAVBOX_PORT="$port" \
    tests/run_emulator.sh --timeout 80s > "$log" 2>&1 &
  emu=$!
  wait_ready "http://localhost:${port}/dav/"

  local url="http://localhost:${port}/dav/"
  expect_status 207 -X PROPFIND -H 'Depth: 1' "$url"
  for name in "${names[@]}"; do
    if ! grep -q "<D:href>/dav/${name}</D:href>" "$work/body"; then
      echo "virtual root did not list /${name}" >&2
      cat "$work/body" >&2
      tail -80 "$log" >&2
      exit 1
    fi
  done
  echo "PASS $table virtual root lists all ${#names[@]} volumes"

  local file="auto-smoke-$table.txt"
  local payload="$table auto volume check"
  for name in "${names[@]}"; do
    expect_status 201 -X PUT --data-binary "$payload" "${url}${name}/${file}"
    expect_status 200 "${url}${name}/${file}"
    [[ "$(cat "$work/body")" == "$payload" ]]
    expect_status 204 -X DELETE "${url}${name}/${file}"
    echo "PASS volume /${name} on $table card"
  done

  expect_status 400 "http://localhost:${port}/${names[0]}/${file}"
  expect_status 403 -X PUT --data-binary "blocked" "${url}newfile.txt"
  echo "PASS base_path hides the unprefixed share"

  grep -q 'found FAT volume' "$log"
  grep -q 'found ext2 volume' "$log"
  grep -q "auto mode: exposing ${#names[@]} volumes" "$log"
  grep -q 'FAT LBA' "$log"
  grep -q 'ext2 mounted' "$log"
  if grep -qi 'format' "$log"; then
    echo "firmware attempted to format a volume" >&2
    exit 1
  fi

  stop_emulator
}

run_case "$work/card-auto-mbr.img" 18091 MBR fat fat-2 ext2 ext2-2
run_case "$work/card-auto-gpt.img" 18092 GPT fatdata fatdata2 davdata davdata2

echo 'auto volume smoke test passed'
