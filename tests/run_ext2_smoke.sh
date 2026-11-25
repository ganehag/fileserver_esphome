#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

config=${DAVBOX_CONFIG:-tests/emulator-ext2.yaml}
build=${DAVBOX_BUILD:-davbox-ext2}
port=${DAVBOX_PORT:-18088}
card=${DAVBOX_CARD:-/tmp/opencode/esphome-ext2-tests/card-ext2.img}
url=${DAVBOX_URL:-http://localhost:${port}/}
log=/tmp/opencode/esphome-ext2-tests/emulator-ext2.log
case "$config" in
  tests/*) build_root=tests/.esphome/build ;;
  *) build_root=.esphome/build ;;
esac
firmware=${build_root}/${build}/build/firmware.factory.bin
elf=${build_root}/${build}/build/firmware.elf

if [[ ! -f "$firmware" || ! -f "$elf" || "$config" -nt "$firmware" ||
      -n "$(find components/webdav_filing -type f -newer "$firmware" -print -quit)" ]]; then
  .venv/bin/esphome compile "$config"
fi

mkdir -p "$(dirname "$card")" "$(dirname "$log")"
rm -f "$card"
tests/make_ext2_image.sh "$card" 16M
: > "$log"

work=$(mktemp -d)
emu=

stop_emulator() {
  if [[ -n "$emu" ]]; then
    kill "$emu" 2>/dev/null || true
    wait "$emu" 2>/dev/null || true
    emu=
  fi
}

trap 'stop_emulator; rm -rf "$work"' EXIT

wait_ready() {
  local ready=0
  for _ in $(seq 1 80); do
    if curl -fsS -m 1 -X OPTIONS "$url" > /dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.5
  done
  if [[ "$ready" != 1 ]]; then
    echo "emulator did not become ready" >&2
    tail -80 "$log" >&2
    exit 1
  fi
}

start_emulator() {
  timeout 120s env \
    DAVBOX_CONFIG="$config" \
    DAVBOX_BUILD="$build" \
    DAVBOX_CARD="$card" \
    DAVBOX_PORT="$port" \
    tests/run_emulator.sh --timeout 110s >> "$log" 2>&1 &
  emu=$!
  wait_ready
}

status() {
  curl -sS -m 10 -o "$work/body" -w '%{http_code}' "$@"
}

expect_status() {
  local expected=$1
  shift
  local actual
  actual=$(status "$@")
  if [[ "$actual" != "$expected" ]]; then
    echo "expected HTTP $expected, got $actual" >&2
    cat "$work/body" >&2
    exit 1
  fi
  echo "PASS $* -> $actual"
}

start_emulator

expect_status 201 -X PUT --data-binary 'ext2-over-webdav' "${url}ext2-hello.txt"
actual=$(curl -fsS -m 10 "${url}ext2-hello.txt")
if [[ "$actual" != 'ext2-over-webdav' ]]; then
  echo "GET returned '$actual'" >&2
  exit 1
fi
echo "PASS GET ext2-hello.txt"

curl -fsS -m 10 -X PROPFIND -H 'Depth: 1' "$url" > "$work/propfind.xml"
grep -q 'ext2-hello.txt' "$work/propfind.xml"
echo "PASS PROPFIND sees ext2 file"

expect_status 201 -X MKCOL "${url}ext2dir"
expect_status 201 -X COPY -H "Destination: ${url}ext2dir/copied.txt" "${url}ext2-hello.txt"
expect_status 201 -X MOVE -H "Destination: ${url}ext2dir/moved.txt" "${url}ext2dir/copied.txt"

curl -fsS -m 10 -X PROPPATCH -H 'Content-Type: application/xml' \
  --data '<D:propertyupdate xmlns:D="DAV:"><D:set><D:prop><P:author xmlns:P="urn:davbox">mikael</P:author></D:prop></D:set></D:propertyupdate>' \
  "${url}ext2-hello.txt" > "$work/proppatch.xml"
curl -fsS -m 10 -X PROPFIND -H 'Depth: 0' -H 'Content-Type: application/xml' \
  --data '<D:propfind xmlns:D="DAV:"><D:prop><P:author xmlns:P="urn:davbox"/></D:prop></D:propfind>' \
  "${url}ext2-hello.txt" > "$work/propget.xml"
grep -q 'mikael' "$work/propget.xml"
echo "PASS dead property round-trip"

curl -fsS -m 10 -D "$work/lock.headers" -o /dev/null -X LOCK \
  -H 'Timeout: Second-60' \
  --data '<?xml version="1.0" encoding="utf-8"?><D:lockinfo xmlns:D="DAV:"><D:lockscope><D:exclusive/></D:lockscope><D:locktype><D:write/></D:locktype><D:owner>smoke</D:owner></D:lockinfo>' \
  "${url}ext2-hello.txt"
token=$(tr -d '\r' < "$work/lock.headers" | sed -n 's/^[Ll]ock-[Tt]oken:[[:space:]]*//p' | tail -1)
if [[ -z "$token" ]]; then
  echo "LOCK response had no Lock-Token" >&2
  cat "$work/lock.headers" >&2
  exit 1
fi
echo "PASS LOCK ($token)"
expect_status 423 -X PUT --data-binary 'forbidden' "${url}ext2-hello.txt"
expect_status 204 -X UNLOCK -H "Lock-Token: $token" "${url}ext2-hello.txt"
expect_status 204 -X DELETE "${url}ext2dir"

expect_status 201 -X PUT --data-binary 'survives reboot' "${url}persisted.txt"
stop_emulator

# Second boot against the same ext2 image: ext2 metadata and data survive.
start_emulator
actual=$(curl -fsS -m 10 "${url}persisted.txt")
if [[ "$actual" != 'survives reboot' ]]; then
  echo "persistent GET returned '$actual'" >&2
  exit 1
fi
echo "PASS ext2 data survives reboot"
curl -fsS -m 10 -X PROPFIND -H 'Depth: 0' -H 'Content-Type: application/xml' \
  --data '<D:propfind xmlns:D="DAV:"><D:prop><P:author xmlns:P="urn:davbox"/></D:prop></D:propfind>' \
  "${url}ext2-hello.txt" > "$work/propget2.xml"
grep -q 'mikael' "$work/propget2.xml"
echo "PASS dead property survives reboot"

expect_status 204 -X DELETE "${url}persisted.txt"
expect_status 204 -X DELETE "${url}ext2-hello.txt"

if grep -qi 'format' "$log"; then
  echo "firmware attempted to format the card" >&2
  exit 1
fi
grep -q 'ext2 mounted' "$log"
echo "ext2 emulator smoke test passed"
