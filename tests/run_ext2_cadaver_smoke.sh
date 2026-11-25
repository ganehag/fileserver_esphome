#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

config=${DAVBOX_CONFIG:-tests/emulator-ext2.yaml}
build=${DAVBOX_BUILD:-davbox-ext2}
port=${DAVBOX_PORT:-18089}
card=${DAVBOX_CARD:-/tmp/opencode/esphome-ext2-tests/cadaver-ext2.img}
url=${DAVBOX_URL:-http://localhost:${port}/}
log=/tmp/opencode/esphome-ext2-tests/emulator-ext2-cadaver.log

if [[ -z "${CADAVER:-}" ]]; then
  if command -v cadaver > /dev/null 2>&1; then
    CADAVER=cadaver
  elif [[ -x /tmp/opencode/cadaver-pkg/root/usr/bin/cadaver ]]; then
    CADAVER=/tmp/opencode/cadaver-pkg/root/usr/bin/cadaver
    local_lib=/tmp/opencode/cadaver-pkg/root/usr/lib/x86_64-linux-gnu
    export LD_LIBRARY_PATH=${local_lib}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}
  else
    echo "cadaver not found; set CADAVER=/path/to/cadaver" >&2
    exit 1
  fi
fi

mkdir -p "$(dirname "$card")" "$(dirname "$log")"
rm -f "$card"
tests/make_ext2_image.sh "$card" 16M

timeout 180s env \
  DAVBOX_CONFIG="$config" \
  DAVBOX_BUILD="$build" \
  DAVBOX_CARD="$card" \
  DAVBOX_PORT="$port" \
  tests/run_emulator.sh --timeout 170s > "$log" 2>&1 &
emu=$!

cleanup() {
  kill "$emu" 2>/dev/null || true
  wait "$emu" 2>/dev/null || true
}
trap cleanup EXIT

ready=0
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

CADAVER="$CADAVER" DAVBOX_URL="$url" tests/run_cadaver_smoke.sh
grep -q 'ext2 mounted' "$log"
echo "ext2 cadaver smoke test passed"
