#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

config=tests/ext2-partition.yaml
build=davbox-ext2-partition
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

printf 'Preparing a formatted base ext2 image...\n'
tests/run_ext2_smoke.sh > "$work/base.log" 2>&1 || {
  cat "$work/base.log" >&2
  exit 1
}
[[ -s "$base" ]] || { echo "base ext2 image missing: $base" >&2; exit 1; }

.venv/bin/esphome compile "$config" > "$work/build.log" 2>&1 || {
  cat "$work/build.log" >&2
  exit 1
}
# A config under tests/ builds into tests/.esphome; run_emulator.sh looks in the
# project root, so expose the same build directory there.
mkdir -p .esphome/build
ln -sfn "$PWD/tests/.esphome/build/$build" ".esphome/build/$build"

python3 - "$base" "$work/card-ext2-mbr.img" "$work/card-ext2-gpt.img" <<'PY'
import sys
import zlib
from pathlib import Path

base, mbr_path, gpt_path = sys.argv[1:]
ext2 = Path(base).read_bytes()
start = 3072
total = 65536  # 32 MiB card; the 16 MiB ext2 image fits inside the partition.


def new_card():
    card = bytearray(total * 512)
    card[start * 512:start * 512 + len(ext2)] = ext2
    return card


def mbr_entry(card, index, partition_type, lba, count):
    p = 446 + index * 16
    card[p + 4] = partition_type
    card[p + 8:p + 12] = lba.to_bytes(4, "little")
    card[p + 12:p + 16] = count.to_bytes(4, "little")


mbr = new_card()
mbr_entry(mbr, 0, 0x0C, 2048, 1024)
mbr_entry(mbr, 1, 0x83, start, total - start)
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
header[8:12] = (0x00010000).to_bytes(4, "little")
header[12:16] = (92).to_bytes(4, "little")
header[16:20] = (0).to_bytes(4, "little")
header[24:32] = (1).to_bytes(8, "little")
header[32:40] = (total - 1).to_bytes(8, "little")
header[40:48] = (34).to_bytes(8, "little")
header[48:56] = (total - 34).to_bytes(8, "little")
header[72:80] = entries_lba.to_bytes(8, "little")
header[80:84] = entry_count.to_bytes(4, "little")
header[84:88] = entry_size.to_bytes(4, "little")
gpt[512:1024] = header


def gpt_entry(card, index, guid, first, last, name):
    p = 2 * 512 + index * 128
    card[p:p + 16] = guid
    card[p + 32:p + 40] = first.to_bytes(8, "little")
    card[p + 40:p + 48] = last.to_bytes(8, "little")
    raw = name.encode("utf-16-le")[:72]
    card[p + 56:p + 56 + len(raw)] = raw


gpt_entry(gpt, 0, bytes([1]) + bytes(15), 2048, start - 1, "fat")
gpt_entry(
    gpt,
    1,
    bytes([0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
           0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4]),
    start,
    total - 34,
    "davdata",
)

entry_bytes = entry_count * entry_size
gpt[512 + 88:512 + 92] = (
    zlib.crc32(gpt[entries_lba * 512:entries_lba * 512 + entry_bytes]) & 0xFFFFFFFF
).to_bytes(4, "little")
gpt[512 + 16:512 + 20] = (0).to_bytes(4, "little")
gpt[512 + 16:512 + 20] = (
    zlib.crc32(gpt[512:512 + 92]) & 0xFFFFFFFF
).to_bytes(4, "little")
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

run_case() {
  local image=$1 port=$2 table=$3 pattern=$4
  log="$work/emulator-$table.log"
  : > "$log"

  timeout 80s env \
    WIFI_SSID=partition-smoke \
    WIFI_PASSWORD=partition-smoke-password \
    DAVBOX_CONFIG="$config" \
    DAVBOX_BUILD="$build" \
    DAVBOX_CARD="$image" \
    DAVBOX_PORT="$port" \
    tests/run_emulator.sh --timeout 70s > "$log" 2>&1 &
  emu=$!
  wait_ready "http://localhost:${port}/"

  local url="http://localhost:${port}/"
  local name="partition-smoke-$table.txt"
  [[ "$(curl -sS -o /dev/null -w '%{http_code}' -X OPTIONS "$url")" == 204 ]]
  [[ "$(curl -sS -o /dev/null -w '%{http_code}' -X PUT --data-binary "$table partition check" "${url}${name}")" == 201 ]]
  [[ "$(curl -fsS "${url}${name}")" == "$table partition check" ]]
  [[ "$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "${url}${name}")" == 204 ]]
  grep -q "$pattern" "$log"
  grep -q 'ext2 mounted' "$log"
  echo "PASS ext2 volume inside $table partition"

  stop_emulator
}

run_case "$work/card-ext2-mbr.img" 18089 MBR 'found ext2 volume in MBR partition 2 at LBA 3072'
run_case "$work/card-ext2-gpt.img" 18090 GPT 'found ext2 volume in GPT partition 2 at LBA 3072.*name=davdata'

echo 'ext2 partition smoke test passed'
