# SPDX-License-Identifier: MIT
"""Build tiny FAT16 images for the SD-card emulators."""

from __future__ import annotations

import sys
from pathlib import Path

SECTOR_SIZE = 512
RESERVED_SECTORS = 1
FAT_COUNT = 2
ROOT_ENTRIES = 512
ROOT_DIR_SECTORS = (ROOT_ENTRIES * 32) // SECTOR_SIZE


def _put_le(buf: bytearray, offset: int, value: int, size: int) -> None:
    buf[offset:offset + size] = value.to_bytes(size, "little")


def build_fat16(sector_count: int) -> bytes:
    """Return a minimal, mountable FAT16 image of ``sector_count`` sectors."""
    if sector_count < 64:
        raise ValueError("FAT16 image is too small")

    # Keep the volume below FatFs's FAT32 cluster threshold by growing the
    # cluster until the data area has fewer than 4085 clusters.
    sectors_per_cluster = 1
    fat_size = 16
    while True:
        data_sectors = sector_count - RESERVED_SECTORS - FAT_COUNT * fat_size - ROOT_DIR_SECTORS
        cluster_count = data_sectors // sectors_per_cluster
        if cluster_count < 4085:
            break
        sectors_per_cluster *= 2
        if sectors_per_cluster > 128:
            raise ValueError("FAT16 image is too large for this helper")

    image = bytearray(sector_count * SECTOR_SIZE)
    boot = image[:SECTOR_SIZE]
    boot[0:3] = b"\xeb\x3c\x90"
    boot[3:11] = b"MSWIN4.1"
    _put_le(boot, 0x0B, SECTOR_SIZE, 2)
    boot[0x0D] = sectors_per_cluster
    _put_le(boot, 0x0E, RESERVED_SECTORS, 2)
    _put_le(boot, 0x10, FAT_COUNT, 2)
    _put_le(boot, 0x11, ROOT_ENTRIES, 2)
    _put_le(boot, 0x13, sector_count, 2)
    boot[0x15] = 0xF8
    _put_le(boot, 0x16, fat_size, 2)
    _put_le(boot, 0x18, 63, 2)   # sectors per track
    _put_le(boot, 0x1A, 255, 2)  # heads
    _put_le(boot, 0x1C, 0, 4)    # hidden sectors
    _put_le(boot, 0x20, 0, 4)
    boot[0x36:0x3E] = b"FAT16   "
    boot[0x40] = 0x80
    boot[0x42] = 0x29
    _put_le(boot, 0x43, 0x12345678, 4)
    boot[0x47:0x52] = b"NO NAME    "
    boot[0x1FE] = 0x55
    boot[0x1FF] = 0xAA
    image[:SECTOR_SIZE] = boot

    fat_offset = RESERVED_SECTORS * SECTOR_SIZE
    for fat in range(FAT_COUNT):
        table = image[fat_offset + fat * fat_size * SECTOR_SIZE:
                      fat_offset + (fat + 1) * fat_size * SECTOR_SIZE]
        _put_le(table, 0, 0xFFF8, 2)
        _put_le(table, 2, 0xFFFF, 2)
    return bytes(image)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: fat16_image.py IMAGE SECTOR_COUNT", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    sector_count = int(sys.argv[2], 0)
    path.write_bytes(build_fat16(sector_count))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
