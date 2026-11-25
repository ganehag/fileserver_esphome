#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

config=${DAVBOX_CONFIG:-tests/emulator.yaml}
build=${DAVBOX_BUILD:-davbox-s3}
case "$config" in
  tests/*) build_root=tests/.esphome/build ;;
  *) build_root=.esphome/build ;;
esac
firmware=${build_root}/${build}/build/firmware.factory.bin
elf=${build_root}/${build}/build/firmware.elf
card=${DAVBOX_CARD:-/tmp/opencode/esphome-ext2-tests/card.img}
port=${DAVBOX_PORT:-18080}

if [[ ! -x .tools/esp-emu ]]; then
  cat >&2 <<'EOF'
esp-emu not found in .tools/. Install v1.2.0 locally with:

  curl -fsSL https://github.com/espressif/esp-emulator/releases/download/v1.2.0/esp-emu-1.2.0-x86_64-unknown-linux-gnu.tar.gz \
    -o /tmp/esp-emu.tar.gz
  tar -xzf /tmp/esp-emu.tar.gz -C .tools
  ln -sf esp-emu-1.2.0-x86_64-unknown-linux-gnu/esp-emu .tools/esp-emu

Or use the official installer:
  curl -fsSL https://raw.githubusercontent.com/espressif/esp-emulator/main/install.sh | sh
EOF
  exit 1
fi

if [[ ! -f "$firmware" || "$config" -nt "$firmware" ||
      -n "$(find components/webdav_filing -type f -newer "$firmware" -print -quit)" ]]; then
  .venv/bin/esphome compile "$config"
fi

mkdir -p "$(dirname "$card")"
if [[ ! -s "$card" ]]; then
  # The firmware only mounts existing filesystems; prepare the FAT fixture here.
  python3 tests/fat16_image.py "$card" 32768  # 16 MiB
fi

# The test configs use fixed credentials; mirror them into the emulator's
# virtual AP unless the caller supplied overrides.
WIFI_SSID=${WIFI_SSID:-emulator-smoke}
WIFI_PASSWORD=${WIFI_PASSWORD:-emulator-smoke-password}

echo "WebDAV URL: http://localhost:${port}/"
echo "SD image:   ${card}"
echo "Ctrl-C to stop the emulator."

exec .tools/esp-emu \
  --chip esp32s3 \
  --psram-size "${DAVBOX_PSRAM:-8M}" \
  --firmware "$firmware" \
  --elf "$elf" \
  --sd "${card},cs=10" \
  --net "user,net=10.0.2.0/24,hostfwd=tcp::${port}-:8080" \
  --wifi-ssid "${WIFI_SSID:-myssid}" \
  --wifi-password "${WIFI_PASSWORD:-mypassword}" \
  "$@"
