// SPDX-License-Identifier: MIT
#include "partition_scan.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace esphome {
namespace webdav_filing {
namespace {

constexpr size_t kSectorSize = 512;
constexpr size_t kMbrPartitionOffset = 446;
constexpr size_t kMbrEntrySize = 16;
constexpr uint16_t kMbrSignature = 0xAA55;
constexpr uint8_t kMbrTypeExtended = 0x05;
constexpr uint8_t kMbrTypeExtendedLba = 0x0F;
constexpr uint8_t kMbrTypeExtendedLinux = 0x85;
constexpr uint8_t kMbrTypeGptProtective = 0xEE;

// GPT on-disk GUIDs use mixed-endian fields.
constexpr uint8_t kGptLinuxFilesystem[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                              0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};

constexpr size_t kMaxLogicalPartitions = 16;
constexpr size_t kMaxGptEntries = 1024;
constexpr size_t kEntryChunkSectors = 8;

// GPT validation and enumeration run one after the other during setup. Keep
// their shared scratch buffer out of the already tight ESPHome loopTask stack.
uint8_t s_gpt_entries[kEntryChunkSectors * kSectorSize];

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_le64(const uint8_t *p) {
  return static_cast<uint64_t>(read_le32(p)) | (static_cast<uint64_t>(read_le32(p + 4)) << 32);
}

bool read_sectors(void *context, SectorReader reader, uint32_t lba, uint8_t *buffer,
                  uint32_t count) {
  return reader != nullptr && reader(context, lba, buffer, count);
}

bool is_extended_type(uint8_t type) {
  return type == kMbrTypeExtended || type == kMbrTypeExtendedLba ||
         type == kMbrTypeExtendedLinux;
}

bool all_zero(const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (bytes[i] != 0)
      return false;
  }
  return true;
}

// CRC-32/ISO-HDLC, as used by the GPT header and partition entry array.
uint32_t crc32_update(uint32_t crc, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
  return ~crc32_update(0xFFFFFFFFu, bytes, length);
}

bool names_equal(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    const unsigned ca = static_cast<unsigned char>(*a);
    const unsigned cb = static_cast<unsigned char>(*b);
    if (std::tolower(ca) != std::tolower(cb))
      return false;
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool matches(const PartitionSelection &candidate, const PartitionSelector &selector) {
  switch (selector.kind) {
    case PartitionSelectorKind::AUTO:
      // Require the ext2 magic so a FAT or unformatted partition on the same
      // card is never mistaken for an ext2 volume.
      return candidate.ext2_magic;
    case PartitionSelectorKind::RAW:
      return candidate.number == 0;
    case PartitionSelectorKind::NUMBER:
      return candidate.number == selector.number;
    case PartitionSelectorKind::NAME:
      return candidate.name[0] != '\0' && names_equal(candidate.name, selector.name);
  }
  return false;
}

void copy_name(const uint8_t *utf16, size_t bytes, char *out, size_t capacity) {
  size_t used = 0;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    const uint16_t code = read_le16(utf16 + i);
    if (code == 0)
      break;
    if (used + 1 >= capacity)
      break;
    out[used++] = code < 128 ? static_cast<char>(code) : '?';
  }
  out[used] = '\0';
  while (used > 0 && out[used - 1] == ' ')
    out[--used] = '\0';
}

bool fat_type(const uint8_t *sector) {
  static const char kFat12[] = "FAT12   ";
  static const char kFat16[] = "FAT16   ";
  static const char kFat32[] = "FAT32   ";
  if (sector[510] != 0x55 || sector[511] != 0xAA)
    return false;
  return std::memcmp(sector + 0x36, kFat12, sizeof(kFat12) - 1) == 0 ||
         std::memcmp(sector + 0x36, kFat16, sizeof(kFat16) - 1) == 0 ||
         std::memcmp(sector + 0x52, kFat32, sizeof(kFat32) - 1) == 0;
}

bool has_fat_boot_sector(void *context, SectorReader reader, uint32_t start_sector,
                         uint32_t total_sectors) {
  if (start_sector >= total_sectors)
    return false;
  uint8_t sector[kSectorSize];
  if (!read_sectors(context, reader, start_sector, sector, 1))
    return false;
  if (!fat_type(sector))
    return false;
  return read_le16(sector + 0x0B) == kSectorSize && sector[0x0D] != 0 && sector[0x10] != 0;
}

void fill_candidate(PartitionTable table, uint32_t number, uint32_t start, uint32_t count,
                    uint8_t mbr_type, bool gpt_linux, const char *name, void *context,
                    SectorReader reader, uint32_t total_sectors, PartitionSelection *out) {
  *out = PartitionSelection{};
  out->table = table;
  out->number = number;
  out->start_sector = start;
  out->sector_count = count;
  out->mbr_type = mbr_type;
  out->gpt_linux = gpt_linux;
  if (name != nullptr)
    std::strncpy(out->name, name, sizeof(out->name) - 1);
  out->ext2_magic = has_ext2_superblock(context, reader, start, total_sectors);
  out->fat_magic = has_fat_boot_sector(context, reader, start, total_sectors);
}

bool mbr_entry(const uint8_t *sector, size_t index, uint8_t *type, uint32_t *start,
               uint32_t *count) {
  const uint8_t *entry = sector + kMbrPartitionOffset + index * kMbrEntrySize;
  *type = entry[4];
  *start = read_le32(entry + 8);
  *count = read_le32(entry + 12);
  return *count != 0;
}

bool in_card(uint32_t start, uint32_t count, uint32_t total_sectors) {
  return start < total_sectors && count != 0 && count <= total_sectors - start;
}

void collect_mbr(void *context, SectorReader reader, uint32_t total_sectors, const uint8_t *mbr,
                 std::vector<PartitionSelection> *out) {
  uint32_t extended_base = 0;
  bool have_extended = false;

  for (size_t i = 0; i < 4; ++i) {
    uint8_t type = 0;
    uint32_t start = 0;
    uint32_t count = 0;
    if (!mbr_entry(mbr, i, &type, &start, &count) || type == kMbrTypeGptProtective)
      continue;
    if (is_extended_type(type)) {
      if (!have_extended && in_card(start, count, total_sectors)) {
        extended_base = start;
        have_extended = true;
      }
      continue;
    }
    if (!in_card(start, count, total_sectors))
      continue;
    PartitionSelection candidate;
    fill_candidate(PartitionTable::MBR, static_cast<uint32_t>(i + 1), start, count, type, false,
                   nullptr, context, reader, total_sectors, &candidate);
    out->push_back(candidate);
  }

  if (!have_extended)
    return;

  uint32_t ebr = extended_base;
  for (size_t logical = 0; logical < kMaxLogicalPartitions; ++logical) {
    uint8_t sector[kSectorSize];
    if (!read_sectors(context, reader, ebr, sector, 1))
      return;
    if (read_le16(sector + 510) != kMbrSignature)
      return;

    uint8_t type = 0;
    uint32_t relative = 0;
    uint32_t count = 0;
    if (mbr_entry(sector, 0, &type, &relative, &count) && type != 0 &&
        !is_extended_type(type) && relative < total_sectors - ebr &&
        in_card(ebr + relative, count, total_sectors)) {
      PartitionSelection candidate;
      fill_candidate(PartitionTable::MBR, static_cast<uint32_t>(5 + logical), ebr + relative,
                     count, type, false, nullptr, context, reader, total_sectors, &candidate);
      out->push_back(candidate);
    }

    uint8_t next_type = 0;
    uint32_t next_relative = 0;
    uint32_t next_count = 0;
    if (!mbr_entry(sector, 1, &next_type, &next_relative, &next_count) ||
        !is_extended_type(next_type) || next_relative == 0 ||
        next_relative >= total_sectors - extended_base)
      return;
    const uint32_t next_ebr = extended_base + next_relative;
    if (next_ebr <= ebr || !in_card(next_ebr, 1, total_sectors))
      return;
    ebr = next_ebr;
  }
}

bool validate_gpt_header(const uint8_t *header, uint32_t header_lba, uint32_t total_sectors,
                         uint64_t *entries_lba, uint64_t *first_usable, uint64_t *last_usable,
                         uint32_t *entry_count, uint32_t *entry_size) {
  if (std::memcmp(header, "EFI PART", 8) != 0)
    return false;
  const uint32_t header_size = read_le32(header + 12);
  if (header_size < 92 || header_size > kSectorSize)
    return false;

  // The header CRC is calculated with its own CRC field cleared.
  const uint32_t stored_crc = read_le32(header + 16);
  uint8_t crc_sector[kSectorSize];
  std::memcpy(crc_sector, header, header_size);
  crc_sector[16] = 0;
  crc_sector[17] = 0;
  crc_sector[18] = 0;
  crc_sector[19] = 0;
  if (crc32_bytes(crc_sector, header_size) != stored_crc)
    return false;

  const uint64_t current_lba = read_le64(header + 24);
  const uint64_t backup_lba = read_le64(header + 32);
  const uint64_t alternate_lba = header_lba == 1 ? total_sectors - 1 : 1;
  if (current_lba != header_lba || backup_lba != alternate_lba)
    return false;

  *first_usable = read_le64(header + 40);
  *last_usable = read_le64(header + 48);
  if (*first_usable >= total_sectors || *last_usable < *first_usable ||
      *last_usable >= total_sectors)
    return false;

  *entries_lba = read_le64(header + 0x48);
  *entry_count = read_le32(header + 0x50);
  *entry_size = read_le32(header + 0x54);
  if (*entries_lba == 0 || *entries_lba >= total_sectors || *entry_count == 0 ||
      *entry_count > kMaxGptEntries || *entry_size < 128 ||
      kSectorSize % *entry_size != 0 || *entry_size > kEntryChunkSectors * kSectorSize)
    return false;

  const uint64_t entry_sectors =
      (static_cast<uint64_t>(*entry_count) * *entry_size + kSectorSize - 1) / kSectorSize;
  if (entry_sectors > total_sectors - *entries_lba)
    return false;
  const uint64_t entries_end = *entries_lba + entry_sectors;
  if (*entries_lba <= header_lba && header_lba < entries_end)
    return false;
  return true;
}

bool verify_gpt_entries(void *context, SectorReader reader, uint64_t entries_lba,
                        uint32_t entry_count, uint32_t entry_size, uint32_t expected_crc) {
  uint8_t *const entries = s_gpt_entries;
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t processed = 0;
  uint32_t lba_offset = 0;
  while (processed < entry_count) {
    const size_t batch = std::min<size_t>(entry_count - processed,
                                          kEntryChunkSectors * kSectorSize / entry_size);
    const size_t bytes = batch * entry_size;
    const uint32_t sectors = static_cast<uint32_t>((bytes + kSectorSize - 1) / kSectorSize);
    if (!read_sectors(context, reader, static_cast<uint32_t>(entries_lba + lba_offset), entries,
                      sectors))
      return false;
    crc = crc32_update(crc, entries, bytes);
    processed += static_cast<uint32_t>(batch);
    lba_offset += sectors;
  }
  return ~crc == expected_crc;
}

bool collect_gpt(void *context, SectorReader reader, uint32_t total_sectors,
                 const uint8_t *header, uint32_t header_lba,
                 std::vector<PartitionSelection> *out) {
  uint64_t entries_lba = 0;
  uint64_t first_usable = 0;
  uint64_t last_usable = 0;
  uint32_t entry_count = 0;
  uint32_t entry_size = 0;
  if (!validate_gpt_header(header, header_lba, total_sectors, &entries_lba, &first_usable,
                           &last_usable, &entry_count, &entry_size))
    return false;
  if (!verify_gpt_entries(context, reader, entries_lba, entry_count, entry_size,
                          read_le32(header + 88)))
    return false;

  uint8_t *const entries = s_gpt_entries;
  uint32_t processed = 0;
  uint32_t lba_offset = 0;
  while (processed < entry_count) {
    const size_t batch = std::min<size_t>(entry_count - processed,
                                          kEntryChunkSectors * kSectorSize / entry_size);
    const uint32_t sectors = static_cast<uint32_t>(
        (static_cast<size_t>(batch) * entry_size + kSectorSize - 1) / kSectorSize);
    if (!read_sectors(context, reader, static_cast<uint32_t>(entries_lba + lba_offset), entries,
                      sectors))
      return false;

    for (size_t i = 0; i < batch; ++i) {
      const uint8_t *entry = entries + i * entry_size;
      if (all_zero(entry, 16))
        continue;
      uint64_t first = read_le64(entry + 32);
      uint64_t last = read_le64(entry + 40);
      if (first < first_usable || last > last_usable || last < first ||
          first >= total_sectors || last >= total_sectors)
        continue;
      const uint64_t count64 = last - first + 1;
      if (count64 > total_sectors - first)
        continue;

      char name[sizeof(PartitionSelection::name)];
      copy_name(entry + 56, 72, name, sizeof(name));
      PartitionSelection candidate;
      fill_candidate(PartitionTable::GPT, processed + static_cast<uint32_t>(i) + 1,
                     static_cast<uint32_t>(first), static_cast<uint32_t>(count64), 0,
                     std::memcmp(entry, kGptLinuxFilesystem, 16) == 0, name, context, reader,
                     total_sectors, &candidate);
      out->push_back(candidate);
    }

    processed += static_cast<uint32_t>(batch);
    lba_offset += sectors;
  }
  return true;
}

bool collect_candidates(void *context, SectorReader reader, uint32_t total_sectors,
                        std::vector<PartitionSelection> *out) {
  out->clear();
  if (reader == nullptr || out == nullptr || total_sectors < 4)
    return false;

  uint8_t sector[kSectorSize];
  if (!read_sectors(context, reader, 0, sector, 1))
    return false;
  const bool mbr = read_le16(sector + 510) == kMbrSignature;
  bool primary_gpt = false;
  bool backup_gpt = false;
  if (mbr && sector[kMbrPartitionOffset + 4] == kMbrTypeGptProtective) {
    uint8_t header[kSectorSize];
    primary_gpt = read_sectors(context, reader, 1, header, 1) &&
                  std::memcmp(header, "EFI PART", 8) == 0;
    backup_gpt = read_sectors(context, reader, total_sectors - 1, header, 1) &&
                 std::memcmp(header, "EFI PART", 8) == 0;
  }

  if (primary_gpt || backup_gpt) {
    uint8_t header[kSectorSize];
    if (primary_gpt && read_sectors(context, reader, 1, header, 1) &&
        collect_gpt(context, reader, total_sectors, header, 1, out))
      return true;
    out->clear();
    if (backup_gpt && read_sectors(context, reader, total_sectors - 1, header, 1) &&
        collect_gpt(context, reader, total_sectors, header, total_sectors - 1, out))
      return true;
    out->clear();
    return false;
  }

  if (mbr) {
    collect_mbr(context, reader, total_sectors, sector, out);
    return true;
  }

  PartitionSelection raw;
  fill_candidate(PartitionTable::RAW, 0, 0, total_sectors, 0, false, nullptr, context, reader,
                 total_sectors, &raw);
  out->push_back(raw);
  return true;
}

}  // namespace

bool has_ext2_superblock(void *context, SectorReader reader, uint32_t start_sector,
                         uint32_t total_sectors) {
  if (start_sector > total_sectors || total_sectors - start_sector < 3)
    return false;
  uint8_t sector[kSectorSize];
  if (!read_sectors(context, reader, start_sector + 2, sector, 1))
    return false;
  // ext2's superblock starts 1024 bytes into the volume; s_magic is at +56.
  return sector[56] == 0x53 && sector[57] == 0xEF;
}

const char *partition_table_name(PartitionTable table) {
  switch (table) {
    case PartitionTable::MBR:
      return "MBR";
    case PartitionTable::GPT:
      return "GPT";
    case PartitionTable::RAW:
    default:
      return "raw";
  }
}

bool scan_for_ext2(void *context, SectorReader reader, uint32_t total_sectors,
                   const PartitionSelector &selector, PartitionSelection *out) {
  if (out == nullptr)
    return false;
  *out = PartitionSelection{};

  std::vector<PartitionSelection> candidates;
  if (!collect_candidates(context, reader, total_sectors, &candidates))
    return false;
  for (const auto &candidate : candidates) {
    if (matches(candidate, selector)) {
      *out = candidate;
      return true;
    }
  }
  return false;
}

bool scan_partitions(void *context, SectorReader reader, uint32_t total_sectors,
                     std::vector<PartitionSelection> *out) {
  if (out == nullptr)
    return false;
  std::vector<PartitionSelection> candidates;
  if (!collect_candidates(context, reader, total_sectors, &candidates))
    return false;
  out->clear();
  for (const auto &candidate : candidates) {
    if (candidate.ext2_magic || candidate.fat_magic)
      out->push_back(candidate);
  }
  return true;
}

}  // namespace webdav_filing
}  // namespace esphome
