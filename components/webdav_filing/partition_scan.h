// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace webdav_filing {

enum class PartitionTable : uint8_t { RAW, MBR, GPT };

enum class PartitionSelectorKind : uint8_t { AUTO, RAW, NUMBER, NAME };

struct PartitionSelector {
  PartitionSelectorKind kind{PartitionSelectorKind::AUTO};
  uint32_t number{0};
  char name[48]{};
};

struct PartitionSelection {
  PartitionTable table{PartitionTable::RAW};
  uint32_t number{0};  // 0 means the whole card / raw ext2 region.
  uint32_t start_sector{0};
  uint32_t sector_count{0};
  uint8_t mbr_type{0};
  bool gpt_linux{false};
  bool ext2_magic{false};
  bool fat_magic{false};
  char name[48]{};
};

// Reads `count` 512-byte sectors starting at `lba` into `buffer`.
using SectorReader = bool (*)(void *context, uint32_t lba, void *buffer, uint32_t count);

// Finds the ext2 volume on a raw card, in MBR primary/logical partitions, or
// in GPT partitions. AUTO only selects a partition with an ext2 superblock, so
// it never mistakes a FAT partition for the ext2 volume. An explicit NUMBER or
// NAME selects that partition; the caller still requires an existing ext2
// superblock when mounting.
bool scan_for_ext2(void *context, SectorReader reader, uint32_t total_sectors,
                   const PartitionSelector &selector, PartitionSelection *out);

// Enumerates every recognizable partition/volume in table order. Unlike
// scan_for_ext2() this includes FAT volumes, so auto mode can mount both
// filesystems. Unformatted partitions are not returned.
bool scan_partitions(void *context, SectorReader reader, uint32_t total_sectors,
                     std::vector<PartitionSelection> *out);

// Checks the ext2 superblock magic at byte 1024 of `start_sector`.
bool has_ext2_superblock(void *context, SectorReader reader, uint32_t start_sector,
                         uint32_t total_sectors);

const char *partition_table_name(PartitionTable table);

}  // namespace webdav_filing
}  // namespace esphome
