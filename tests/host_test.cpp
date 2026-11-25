// SPDX-License-Identifier: MIT
#include "ext2_ram_blockdev.h"
#include "partition_scan.h"
#include "vfs_ext2.h"
#include "vfs_router.h"
#include "webdav_filing.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using esphome::webdav_filing::PartitionSelection;
using esphome::webdav_filing::PartitionSelector;
using esphome::webdav_filing::PartitionTable;
using esphome::webdav_filing::VfsRouter;
using esphome::webdav_filing::scan_for_ext2;
using esphome::webdav_filing::scan_partitions;

struct MockResponse {
  std::string status;
  std::string type;
  std::map<std::string, std::string> headers;
  std::string body;
  std::vector<std::string> interim;
  std::vector<std::string> header_conflicts;
  int chunks{0};
  bool finished{false};
};

MockResponse g_resp;
// IDF stores header pointers until the response is sent. Keep the same
// lifetime semantics here so a pointer to a destroyed temporary is caught.
struct PendingHeader {
  std::string field;
  const char *value;
};
std::vector<PendingHeader> g_pending_headers;
httpd_config_t g_httpd_config{};
bool g_server_started{false};
bool g_handler_registered{false};
int g_failures{0};

// 4096 x 4096-byte blocks = 16 MiB, enough for a journal-less ext2 volume.
uint8_t g_ext2_storage[4096 * EXT2_BLOCK_BYTES];

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

void check(bool ok, const char *name) {
  if (ok) {
    std::cout << "  PASS  " << name << '\n';
  } else {
    std::cout << "  FAIL  " << name << '\n';
    g_failures++;
  }
}

void check(bool ok, const std::string &name) { check(ok, name.c_str()); }

void check_eq(const std::string &actual, const std::string &expected, const char *name) {
  if (actual == expected) {
    std::cout << "  PASS  " << name << '\n';
  } else {
    std::cout << "  FAIL  " << name << " (got \"" << actual << "\", want \"" << expected
              << "\")\n";
    g_failures++;
  }
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

void reset_response() {
  g_resp = MockResponse{};
  g_pending_headers.clear();
}

void flush_headers_() {
  for (const auto &header : g_pending_headers)
    g_resp.headers[lower(header.field)] = header.value != nullptr ? header.value : "";
  g_pending_headers.clear();
}

constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kFakeDiskSectors = 32768;

struct FakeDisk {
  std::vector<uint8_t> bytes{std::vector<uint8_t>(kFakeDiskSectors * kSectorSize)};
};

bool fake_read_sectors(void *context, uint32_t lba, void *buffer, uint32_t count) {
  auto *const disk = static_cast<FakeDisk *>(context);
  if (disk == nullptr || static_cast<uint64_t>(lba) + count > kFakeDiskSectors)
    return false;
  std::memcpy(buffer, disk->bytes.data() + static_cast<size_t>(lba) * kSectorSize,
              static_cast<size_t>(count) * kSectorSize);
  return true;
}

uint8_t *sector_at(FakeDisk *disk, uint32_t lba) {
  return disk->bytes.data() + static_cast<size_t>(lba) * kSectorSize;
}

struct SparseBlockDevice {
  ext2_blockdev_t device{};
  std::map<uint32_t, std::vector<uint8_t>> blocks;
};

bool sparse_read(void *context, uint32_t lba, void *buffer, uint32_t count) {
  auto *const dev = static_cast<SparseBlockDevice *>(context);
  auto *const out = static_cast<uint8_t *>(buffer);
  for (uint32_t i = 0; i < count; ++i) {
    auto it = dev->blocks.find(lba + i);
    uint8_t *const dst = out + static_cast<size_t>(i) * EXT2_BLOCK_BYTES;
    if (it == dev->blocks.end())
      std::memset(dst, 0, EXT2_BLOCK_BYTES);
    else
      std::memcpy(dst, it->second.data(), EXT2_BLOCK_BYTES);
  }
  return true;
}

bool sparse_write(void *context, uint32_t lba, const void *buffer, uint32_t count) {
  auto *const dev = static_cast<SparseBlockDevice *>(context);
  const auto *const in = static_cast<const uint8_t *>(buffer);
  for (uint32_t i = 0; i < count; ++i) {
    dev->blocks[lba + i].assign(in + static_cast<size_t>(i) * EXT2_BLOCK_BYTES,
                                in + static_cast<size_t>(i + 1) * EXT2_BLOCK_BYTES);
  }
  return true;
}

// A host-formatted ext2 geometry with mke2fs defaults: 4 KiB blocks, 32768
// blocks per group, 8192 inodes per group and 256-byte inodes. Only the blocks
// the driver actually reads are present; everything else reads back as zeros.
struct SparseExt2Image {
  SparseBlockDevice sparse;
  std::vector<ext2_group_desc_t> descriptors;
};

void init_sparse_ext2(SparseExt2Image *image, uint32_t groups,
                      uint32_t inodes_per_group = 8192) {
  constexpr uint32_t kBlocksPerGroup = 32768;
  const uint32_t kInodesPerGroup = inodes_per_group;
  const uint32_t kBlocks = groups * kBlocksPerGroup;

  image->sparse = SparseBlockDevice{};
  image->sparse.device.block_size = EXT2_BLOCK_BYTES;
  image->sparse.device.block_count = kBlocks;
  image->sparse.device.context = &image->sparse;
  image->sparse.device.read = sparse_read;
  image->sparse.device.write = sparse_write;

  ext2_super_t super{};
  super.inodes_count = groups * kInodesPerGroup;
  super.blocks_count = kBlocks;
  super.free_blocks_count = kBlocks - 1024;
  super.free_inodes_count = super.inodes_count - 11;
  super.first_data_block = 0;
  super.log_block_size = 2;
  super.log_frag_size = 2;
  super.blocks_per_group = kBlocksPerGroup;
  super.frags_per_group = kBlocksPerGroup;
  super.inodes_per_group = kInodesPerGroup;
  super.magic = 0xEF53;
  super.state = 1;
  super.rev_level = 1;
  super.first_ino = 11;
  super.inode_size = 256;
  super.feature_compat = 0x0008 | 0x0010 | 0x0020;
  super.feature_ro_compat = 0x0001 | 0x0002;
  super.feature_incompat = 0x0002;

  std::vector<uint8_t> superblock(EXT2_BLOCK_BYTES, 0);
  std::memcpy(superblock.data() + 1024, &super, sizeof(super));
  image->sparse.blocks[0] = superblock;

  const size_t gdt_bytes = static_cast<size_t>(groups) * sizeof(ext2_group_desc_t);
  const size_t gdt_block_count =
      (gdt_bytes + EXT2_BLOCK_BYTES - 1) / EXT2_BLOCK_BYTES;
  std::vector<uint8_t> gdt(gdt_block_count * EXT2_BLOCK_BYTES, 0);
  image->descriptors.assign(groups, ext2_group_desc_t{});
  auto *const descriptors = image->descriptors.data();
  for (uint32_t g = 0; g < groups; ++g) {
    descriptors[g].block_bitmap = 1024 + g * 3;
    descriptors[g].inode_bitmap = 1025 + g * 3;
    descriptors[g].inode_table = 1026 + g * 3;
    descriptors[g].free_blocks_count = static_cast<uint16_t>(kBlocksPerGroup);
    descriptors[g].free_inodes_count = static_cast<uint16_t>(kInodesPerGroup);
  }
  std::memcpy(gdt.data(), descriptors, gdt_bytes);
  for (size_t b = 0; b < gdt_block_count; ++b) {
    image->sparse.blocks[1 + static_cast<uint32_t>(b)].assign(
        gdt.begin() + static_cast<std::ptrdiff_t>(b * EXT2_BLOCK_BYTES),
        gdt.begin() + static_cast<std::ptrdiff_t>((b + 1) * EXT2_BLOCK_BYTES));
  }

  std::vector<uint8_t> inode_bitmap(EXT2_BLOCK_BYTES, 0);
  inode_bitmap[1 / 8] = static_cast<uint8_t>(1u << (1 % 8));  // inode 2: root
  image->sparse.blocks[descriptors[0].inode_bitmap] = inode_bitmap;

  std::vector<uint8_t> inode_table(EXT2_BLOCK_BYTES, 0);
  ext2_inode_t root{};
  root.mode = 0x41ED;  // 040755
  root.size = 1024;
  root.links_count = 2;
  root.blocks = 2;
  root.block[0] = 2000;
  std::memcpy(inode_table.data() + super.inode_size, &root, sizeof(root));
  image->sparse.blocks[descriptors[0].inode_table] = inode_table;

  // The root directory's data block, so the image also supports mutations.
  std::vector<uint8_t> root_dir(EXT2_BLOCK_BYTES, 0);
  auto put16 = [&root_dir](size_t offset, uint16_t value) {
    root_dir[offset] = static_cast<uint8_t>(value);
    root_dir[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  auto put32 = [&root_dir](size_t offset, uint32_t value) {
    root_dir[offset] = static_cast<uint8_t>(value);
    root_dir[offset + 1] = static_cast<uint8_t>(value >> 8);
    root_dir[offset + 2] = static_cast<uint8_t>(value >> 16);
    root_dir[offset + 3] = static_cast<uint8_t>(value >> 24);
  };
  put32(0, 2);
  put16(4, 12);
  put16(6, 1);
  root_dir[8] = 2;
  root_dir[9] = '.';
  put32(12, 2);
  put16(16, static_cast<uint16_t>(EXT2_BLOCK_BYTES - 12));
  put16(18, 2);
  root_dir[20] = 2;
  root_dir[21] = '.';
  root_dir[22] = '.';
  image->sparse.blocks[root.block[0]] = root_dir;
}

void put_le16(uint8_t *p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void put_le32(uint8_t *p, uint32_t value) {
  for (size_t i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void put_le64(uint8_t *p, uint64_t value) {
  for (size_t i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void put_ext2_magic(FakeDisk *disk, uint32_t start) {
  uint8_t *const super = sector_at(disk, start + 2);
  super[56] = 0x53;
  super[57] = 0xEF;
}

void put_fat_boot_sector(FakeDisk *disk, uint32_t start) {
  uint8_t *const boot = sector_at(disk, start);
  boot[0] = 0xEB;
  boot[1] = 0x3C;
  boot[2] = 0x90;
  put_le16(boot + 0x0B, kSectorSize);
  boot[0x0D] = 8;
  boot[0x10] = 2;
  std::memcpy(boot + 0x52, "FAT32   ", 8);
  boot[510] = 0x55;
  boot[511] = 0xAA;
}

void put_mbr_partition(FakeDisk *disk, uint32_t entry, uint8_t type, uint32_t start,
                       uint32_t count) {
  uint8_t *const p = sector_at(disk, 0) + 446 + entry * 16;
  p[4] = type;
  put_le32(p + 8, start);
  put_le32(p + 12, count);
  put_le16(sector_at(disk, 0) + 510, 0xAA55);
}

void put_gpt_name(uint8_t *entry, const std::string &name) {
  for (size_t i = 0; i < name.size() && i < 47; ++i)
    put_le16(entry + 56 + i * 2, static_cast<uint8_t>(name[i]));
}

uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

constexpr uint32_t kGptEntryCount = 128;
constexpr uint32_t kGptEntrySize = 128;
constexpr uint32_t kGptPrimaryEntries = 2;
constexpr uint32_t kGptEntrySectors = kGptEntryCount * kGptEntrySize / kSectorSize;
constexpr uint32_t kGptBackupEntries = kFakeDiskSectors - 1 - kGptEntrySectors;

void put_protective_mbr(FakeDisk *disk) {
  uint8_t *const mbr = sector_at(disk, 0);
  mbr[446 + 4] = 0xEE;
  put_le32(mbr + 446 + 8, 1);
  put_le32(mbr + 446 + 12, kFakeDiskSectors - 1);
  put_le16(mbr + 510, 0xAA55);
}

void put_gpt_header(FakeDisk *disk, uint32_t header_lba, uint32_t entries_lba,
                    uint32_t entry_count, uint32_t entry_size) {
  uint8_t *const header = sector_at(disk, header_lba);
  std::memset(header, 0, kSectorSize);
  std::memcpy(header, "EFI PART", 8);
  put_le32(header + 8, 0x00010000);
  put_le32(header + 12, 92);
  put_le64(header + 24, header_lba);
  put_le64(header + 32, header_lba == 1 ? kFakeDiskSectors - 1 : 1);
  put_le64(header + 40, 34);
  put_le64(header + 48, kFakeDiskSectors - 34);
  put_le64(header + 0x48, entries_lba);
  put_le32(header + 0x50, entry_count);
  put_le32(header + 0x54, entry_size);
}

void finish_gpt_header(FakeDisk *disk, uint32_t header_lba, uint32_t entries_lba,
                       uint32_t entry_count, uint32_t entry_size) {
  uint8_t *const header = sector_at(disk, header_lba);
  put_le32(header + 16, 0);
  put_le32(header + 88,
           crc32_bytes(sector_at(disk, entries_lba), entry_count * entry_size));
  put_le32(header + 16, crc32_bytes(header, 92));
}

void copy_gpt_entries_to_backup(FakeDisk *disk) {
  std::memcpy(sector_at(disk, kGptBackupEntries), sector_at(disk, kGptPrimaryEntries),
              kGptEntryCount * kGptEntrySize);
}

}  // namespace

namespace esphome {

uint32_t millis() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
          .count());
}

}  // namespace esphome

// ---- esp_http_server mock -------------------------------------------------

const char *esp_err_to_name(esp_err_t err) {
  return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

bool httpd_uri_match_wildcard(const char *, const char *, size_t) {
  return true;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config) {
  g_httpd_config = *config;
  *handle = reinterpret_cast<httpd_handle_t>(1);
  g_server_started = true;
  return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t) {
  return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *) {
  g_handler_registered = true;
  return ESP_OK;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field) {
  auto it = req->headers.find(lower(field));
  return it == req->headers.end() ? 0 : it->second.size();
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *value,
                                      size_t value_size) {
  auto it = req->headers.find(lower(field));
  if (it == req->headers.end())
    return ESP_FAIL;
  if (it->second.size() + 1 > value_size)
    return ESP_FAIL;
  std::memcpy(value, it->second.data(), it->second.size());
  value[it->second.size()] = '\0';
  return ESP_OK;
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len) {
  if (req->recv_offset >= req->body.size())
    return 0;
  // Deliberately return short reads like a real TCP socket so PUT buffering
  // and XML body reads are exercised across buffer boundaries.
  constexpr size_t kShortRead = 1373;
  size_t n = std::min({buf_len, kShortRead, req->body.size() - req->recv_offset});
  std::memcpy(buf, req->body.data() + req->recv_offset, n);
  req->recv_offset += n;
  return static_cast<int>(n);
}

esp_err_t httpd_resp_set_status(httpd_req_t *, const char *status) {
  g_resp.status = status;
  return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *, const char *type) {
  g_resp.type = type;
  return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *field, const char *value) {
  g_pending_headers.push_back(PendingHeader{lower(field), value});
  return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t len) {
  flush_headers_();
  // IDF invalidates request headers as soon as a response is sent.
  if (req != nullptr)
    req->headers.clear();

  if (!g_resp.status.empty() && g_resp.status[0] == '1') {
    g_resp.interim.push_back(g_resp.status);
    return ESP_OK;
  }
  // IDF always emits its own Content-Length before the stored headers. Model
  // that so a manual Content-Length cannot silently coexist on hardware.
  const ssize_t effective_len = (len < 0 && buf != nullptr) ? std::strlen(buf) : len;
  const std::string content_length = std::to_string(effective_len);
  auto existing = g_resp.headers.find("content-length");
  if (existing == g_resp.headers.end())
    g_resp.headers["content-length"] = content_length;
  else if (existing->second != content_length)
    g_resp.header_conflicts.push_back("content-length " + existing->second + " vs " +
                                      content_length);
  if (buf != nullptr && len > 0)
    g_resp.body.append(buf, static_cast<size_t>(len));
  g_resp.finished = true;
  return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t *, const char *buf, ssize_t len) {
  flush_headers_();
  if (g_resp.headers.count("content-length") != 0)
    g_resp.header_conflicts.push_back("Content-Length with chunked response");
  g_resp.headers["transfer-encoding"] = "chunked";
  if (buf == nullptr) {
    g_resp.finished = true;
    return ESP_OK;
  }
  if (len > 0)
    g_resp.body.append(buf, static_cast<size_t>(len));
  g_resp.chunks++;
  return ESP_OK;
}

// ---- In-memory VFS --------------------------------------------------------

class MemoryVfs : public esphome::webdav_filing::Vfs {
 public:
  static constexpr time_t kMtime = 1700000000;  // Tue, 14 Nov 2023 22:13:20 GMT

  struct Node {
    bool is_dir{false};
    std::string data;
    time_t mtime{kMtime};
  };

  MemoryVfs() { nodes_[""] = Node{true, "", kMtime}; }

  void hide(const std::string &path) { hidden_.push_back(path); }
  void fail_reads_after(size_t offset) { fail_read_offset_ = offset; }
  void clear_read_failure() { fail_read_offset_ = std::numeric_limits<size_t>::max(); }

  bool is_hidden(const std::string &path) const override {
    return std::find(hidden_.begin(), hidden_.end(), path) != hidden_.end();
  }

  bool stat(const std::string &path, esphome::webdav_filing::VfsStat *out) override {
    out->exists = false;
    out->is_dir = false;
    out->size = 0;
    auto it = nodes_.find(path);
    if (it == nodes_.end())
      return true;
    out->exists = true;
    out->is_dir = it->second.is_dir;
    out->size = it->second.data.size();
    out->mtime = it->second.mtime;
    return true;
  }

  bool read_dir(const std::string &path,
                std::vector<esphome::webdav_filing::VfsDirEntry> *out) override {
    out->clear();
    for (const auto &item : nodes_) {
      if (item.first == path || parent(item.first) != path)
        continue;
      esphome::webdav_filing::VfsDirEntry entry;
      entry.name = item.first.substr(item.first.find_last_of('/') + 1);
      entry.is_dir = item.second.is_dir;
      entry.size = item.second.data.size();
      entry.mtime = item.second.mtime;
      out->push_back(entry);
    }
    return true;
  }

  bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                 size_t *read) override {
    auto it = nodes_.find(path);
    if (it == nodes_.end() || it->second.is_dir || offset > it->second.data.size() ||
        offset >= fail_read_offset_)
      return false;
    size_t n = std::min(len, it->second.data.size() - offset);
    n = std::min(n, fail_read_offset_ - offset);
    std::memcpy(buf, it->second.data.data() + offset, n);
    *read = n;
    return true;
  }

  bool write_file(const std::string &path, const uint8_t *buf, size_t len) override {
    Node &node = nodes_[path];
    node.is_dir = false;
    node.data.append(reinterpret_cast<const char *>(buf), len);
    return true;
  }

  bool mkdir(const std::string &path) override {
    if (nodes_.count(path))
      return false;
    nodes_[path] = Node{true, "", kMtime};
    return true;
  }

  bool remove(const std::string &path) override {
    if (!nodes_.count(path))
      return false;
    for (auto it = nodes_.begin(); it != nodes_.end();) {
      if (it->first == path || it->first.rfind(path + "/", 0) == 0)
        it = nodes_.erase(it);
      else
        ++it;
    }
    return true;
  }

  bool rename(const std::string &from, const std::string &to) override {
    if (!nodes_.count(from))
      return false;
    remove(to);
    std::map<std::string, Node> next;
    for (auto &item : nodes_) {
      if (item.first == from) {
        next[to] = std::move(item.second);
      } else if (item.first.rfind(from + "/", 0) == 0) {
        next[to + item.first.substr(from.size())] = std::move(item.second);
      } else {
        next[item.first] = std::move(item.second);
      }
    }
    nodes_.swap(next);
    return true;
  }

  bool has(const std::string &path) const { return nodes_.count(path) != 0; }
  std::string data(const std::string &path) const { return nodes_.at(path).data; }

 private:
  static std::string parent(const std::string &path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
  }

  std::map<std::string, Node> nodes_;
  std::vector<std::string> hidden_;
  size_t fail_read_offset_{std::numeric_limits<size_t>::max()};
};

class TestWebDAVFiling : public esphome::webdav_filing::WebDAVFiling {
 public:
  using WebDAVFiling::WebDAVFiling;
  using WebDAVFiling::handle_;
  using WebDAVFiling::md5_hex_;
};

std::string digest_header(const std::string &method, const std::string &uri,
                          const std::string &nonce) {
  const std::string realm = "webdav_filing";
  const std::string cnonce = "clientnonce";
  const std::string nc = "00000001";
  const std::string ha1 = TestWebDAVFiling::md5_hex_("davuser:" + realm + ":davpass");
  const std::string ha2 = TestWebDAVFiling::md5_hex_(method + ":" + uri);
  const std::string response =
      TestWebDAVFiling::md5_hex_(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2);
  return "Digest username=\"davuser\", realm=\"" + realm + "\", nonce=\"" + nonce +
         "\", uri=\"" + uri + "\", algorithm=MD5, qop=auth, nc=" + nc + ", cnonce=\"" +
         cnonce + "\", response=\"" + response + "\"";
}

struct Request {
  Request(int method, std::string uri, std::string body = "")
      : uri(std::move(uri)), body(std::move(body)) {
    raw.uri = this->uri.c_str();
    raw.method = method;
    raw.content_len = this->body.size();
    raw.body = this->body;
  }

  void header(const char *name, const std::string &value) { raw.headers[lower(name)] = value; }

  std::string uri;
  std::string body;
  httpd_req_t raw{};
};

esp_err_t call(TestWebDAVFiling &dav, Request &req) {
  reset_response();
  req.raw.recv_offset = 0;
  req.raw.user_ctx = &dav;
  return dav.handle_(&req.raw);
}

int main() {
  MemoryVfs vfs;
  TestWebDAVFiling dav(8080, 18, 19, 23, 5, 4000000);
  dav.set_vfs(&vfs);
  dav.setup();

  std::cout << "host tests: WebDAVFiling\n";
  check(g_server_started && g_handler_registered, "setup starts server and registers handler");
  check(g_httpd_config.task_priority == 1, "HTTPD shares the loop task priority");
  check(g_httpd_config.stack_size >= 16 * 1024, "HTTPD stack has long-path headroom");
  check(g_httpd_config.max_open_sockets == 12, "HTTPD allows 12 client sessions");
  check(g_httpd_config.backlog_conn == 16, "HTTPD queues 16 pending connections");
  check(g_httpd_config.max_resp_headers == 12, "HTTPD reserves room for WebDAV response headers");
  check(g_httpd_config.send_wait_timeout == 15, "HTTPD tolerates temporary player backpressure");
  check(g_httpd_config.lru_purge_enable, "HTTPD purges stale connections when full");
  check(g_httpd_config.max_uri_len >= 4096, "HTTPD accepts percent-encoded long paths");
  check(g_httpd_config.max_req_hdr_len >= 8192, "HTTPD keeps enough header space for long URIs");

  {
    Request req(HTTP_OPTIONS, "/");
    check(call(dav, req) == ESP_OK, "OPTIONS returns OK");
    check_eq(g_resp.status, "204 No Content", "OPTIONS status");
    check_eq(g_resp.headers["dav"], "1,2", "OPTIONS advertises DAV: 1,2");
    check_eq(g_resp.headers["ms-author-via"], "DAV", "OPTIONS advertises MS-Author-Via");
    check(contains(g_resp.headers["allow"], "PROPFIND"), "OPTIONS advertises PROPFIND");
  }

  {
    Request req(HTTP_PUT, "/hello.txt", "hello world");
    check(call(dav, req) == ESP_OK, "PUT returns OK");
    check_eq(g_resp.status, "201 Created", "PUT status for new file");
    check(vfs.has("hello.txt"), "PUT creates file in VFS");
    check_eq(vfs.data("hello.txt"), "hello world", "PUT stores exact bytes");

    Request create_only(HTTP_PUT, "/hello.txt", "replaced");
    create_only.header("If-None-Match", "*");
    check(call(dav, create_only) == ESP_OK && g_resp.status == "412 Precondition Failed",
          "PUT If-None-Match: * rejects an existing file");
    check_eq(vfs.data("hello.txt"), "hello world", "rejected create-only PUT keeps old bytes");

    Request must_exist(HTTP_PUT, "/missing-new.txt", "created");
    must_exist.header("If-Match", "*");
    check(call(dav, must_exist) == ESP_OK && g_resp.status == "412 Precondition Failed",
          "PUT If-Match: * rejects a missing file");
    check(!vfs.has("missing-new.txt"), "rejected If-Match PUT creates nothing");
  }

  {
    std::string payload;
    for (int i = 0; i < 20000; ++i)
      payload.push_back(static_cast<char>('a' + (i % 26)));
    Request req(HTTP_PUT, "/large.txt", payload);
    check(call(dav, req) == ESP_OK, "large PUT returns OK");
    check_eq(g_resp.status, "201 Created", "large PUT status");
    check_eq(vfs.data("large.txt"), payload, "PUT accumulates short recv chunks");
  }

  {
    Request req(HTTP_GET, "/hello.txt");
    check(call(dav, req) == ESP_OK, "GET returns OK");
    check_eq(g_resp.status, "200 OK", "GET status");
    check_eq(g_resp.body, "hello world", "GET streams file bytes");
    check(g_resp.chunks > 0, "GET uses chunked streaming");
    check_eq(g_resp.headers["last-modified"], "Tue, 14 Nov 2023 22:13:20 GMT",
             "GET reports Last-Modified");
  }

  {
    const std::string etag = "\"11-" + std::to_string(MemoryVfs::kMtime) + "\"";
    Request req(HTTP_GET, "/hello.txt");
    req.header("If-None-Match", etag);
    check(call(dav, req) == ESP_OK && g_resp.status == "304 Not Modified",
          "GET honors If-None-Match");
    check(contains(g_resp.headers["etag"], "11-"), "304 includes ETag");
  }

  {
    Request req(HTTP_GET, "/fs/hello.txt");
    check(call(dav, req) == ESP_OK && g_resp.body == "hello world",
          "legacy /fs/<name> route still works");
  }

  {
    vfs.mkdir("hidden");
    vfs.hide("hidden");
    Request get(HTTP_GET, "/hidden");
    check(call(dav, get) == ESP_OK && g_resp.status == "404 Not Found",
          "hidden backend paths are not published");
    Request put(HTTP_PUT, "/hidden", "nope");
    check(call(dav, put) == ESP_OK && g_resp.status == "404 Not Found",
          "PUT to a hidden path is rejected");
    Request propfind(HTTP_PROPFIND, "/hidden");
    propfind.header("Depth", "0");
    check(call(dav, propfind) == ESP_OK && g_resp.status == "404 Not Found",
          "PROPFIND on a hidden path is rejected");
    Request copy(HTTP_COPY, "/hello.txt");
    copy.header("Destination", "http://localhost/hidden");
    check(call(dav, copy) == ESP_OK && g_resp.status == "404 Not Found",
          "COPY to a hidden destination is rejected");
    check(vfs.data("hello.txt") == "hello world", "hidden-path rejection leaves data intact");
    vfs.remove("hidden");
  }

  {
    Request req(HTTP_HEAD, "/hello.txt");
    check(call(dav, req) == ESP_OK, "HEAD returns OK");
    check_eq(g_resp.status, "200 OK", "HEAD status");
    check_eq(g_resp.body, "", "HEAD sends no body");
    check_eq(g_resp.headers["content-length"], "11", "HEAD reports content length");
    check(g_resp.header_conflicts.empty(), "HEAD emits one Content-Length");

    Request missing(HTTP_HEAD, "/missing.txt");
    check(call(dav, missing) == ESP_OK && g_resp.status == "404 Not Found",
          "HEAD missing file returns 404");
    check(g_resp.body.empty(), "HEAD error sends no body");
    check(g_resp.header_conflicts.empty(), "HEAD error emits one Content-Length");

    vfs.mkdir("head-dir");
    Request dir(HTTP_HEAD, "/head-dir");
    check(call(dav, dir) == ESP_OK && g_resp.status == "403 Forbidden",
          "HEAD directory returns 403");
    check(g_resp.body.empty(), "HEAD directory error sends no body");
    check(g_resp.header_conflicts.empty(), "HEAD directory error emits one Content-Length");
    vfs.remove("head-dir");
  }

  {
    vfs.fail_reads_after(5);
    Request req(HTTP_GET, "/hello.txt");
    check(call(dav, req) == ESP_FAIL, "GET aborts on a short read");
    check_eq(g_resp.body, "hello", "GET sent only the bytes read before failure");
    check(!g_resp.finished, "failed GET does not send a terminating chunk");
    vfs.clear_read_failure();
  }

  {
    Request req(HTTP_PUT, "/continue.txt", "streamed");
    req.header("Expect", "100-continue");
    check(call(dav, req) == ESP_OK, "PUT with Expect returns OK");
    check_eq(g_resp.status, "201 Created", "PUT with Expect final status");
    check(g_resp.interim.size() == 1 && g_resp.interim[0] == "100 Continue",
          "PUT sends interim 100 Continue");
    check_eq(vfs.data("continue.txt"), "streamed", "PUT with Expect stores body");
  }

  {
    Request req(HTTP_PUT, "/bad-expect.txt", "nope");
    req.header("Expect", "banana");
    check(call(dav, req) == ESP_FAIL && g_resp.status == "417 Expectation Failed",
          "PUT rejects unsupported Expect and closes the connection");
    check(!vfs.has("bad-expect.txt"), "unsupported Expect writes nothing");
  }

  {
    Request req(HTTP_PROPFIND, "/");
    req.header("Depth", "1");
    check(call(dav, req) == ESP_OK, "PROPFIND returns OK");
    check_eq(g_resp.status, "207 Multi-Status", "PROPFIND status");
    check(contains(g_resp.body, "xmlns:D=\"DAV:\""), "PROPFIND body has DAV namespace");
    check(contains(g_resp.body, "<D:href>/hello.txt</D:href>"), "PROPFIND lists child href");
    check(contains(g_resp.body, "<D:collection/>"), "PROPFIND marks root as collection");
    check(contains(g_resp.body, "<D:getcontentlength>11</D:getcontentlength>"),
          "PROPFIND reports file size");
    check(contains(g_resp.body, "<D:getlastmodified>Tue, 14 Nov 2023 22:13:20 GMT</D:getlastmodified>"),
          "PROPFIND reports getlastmodified");
    check(contains(g_resp.body, "<D:supportedlock>"), "PROPFIND advertises supportedlock");
    check(contains(g_resp.body, "<D:lockdiscovery/>"), "PROPFIND shows no lock initially");
  }

  {
    vfs.mkdir("many");
    for (int i = 0; i < 520; ++i)
      vfs.write_file("many/f" + std::to_string(i), reinterpret_cast<const uint8_t *>("x"), 1);
    Request req(HTTP_PROPFIND, "/many");
    req.header("Depth", "1");
    check(call(dav, req) == ESP_OK && g_resp.status == "207 Multi-Status",
          "large PROPFIND returns 207");
    size_t responses = 0;
    constexpr char kResponseTag[] = "<D:response>";
    for (size_t pos = 0; (pos = g_resp.body.find(kResponseTag, pos)) != std::string::npos;
         pos += sizeof(kResponseTag) - 1)
      ++responses;
    check(responses == 521, "large PROPFIND returns every entry");
    vfs.remove("many");
  }

  {
    std::string deep = "deep";
    check(vfs.mkdir(deep), "deep tree root created");
    for (int i = 0; i < 60; ++i) {
      deep += "/d";
      check(vfs.mkdir(deep), "deep tree level created");
    }
    Request del(HTTP_DELETE, "/deep");
    check(call(dav, del) == ESP_OK && g_resp.status == "204 No Content",
          "DELETE handles a deep tree without recursion");
    check(!vfs.has("deep"), "deep tree removed");
  }

  {
    Request mkcol(HTTP_MKCOL, "/dir");
    check(call(dav, mkcol) == ESP_OK && g_resp.status == "201 Created", "MKCOL creates directory");

    Request put(HTTP_PUT, "/dir/a.txt", "nested");
    check(call(dav, put) == ESP_OK && vfs.data("dir/a.txt") == "nested",
          "PUT works in nested directory");

    Request copy_mkcol(HTTP_MKCOL, "/copy");
    check(call(dav, copy_mkcol) == ESP_OK, "MKCOL creates copy destination");

    Request copy(HTTP_COPY, "/dir/a.txt");
    copy.header("Destination", "/copy/b.txt");
    check(call(dav, copy) == ESP_OK && vfs.has("copy/b.txt"), "COPY writes destination file");
    check_eq(vfs.data("copy/b.txt"), "nested", "COPY preserves bytes");

    Request blocked(HTTP_COPY, "/dir/a.txt");
    blocked.header("Destination", "/copy/b.txt");
    blocked.header("Overwrite", "F");
    check(call(dav, blocked) == ESP_OK && g_resp.status == "412 Precondition Failed",
          "COPY respects Overwrite: F");

    Request self(HTTP_COPY, "/hello.txt");
    self.header("Destination", "/hello.txt");
    check(call(dav, self) == ESP_OK && g_resp.status == "403 Forbidden",
          "COPY onto itself is rejected");
    check_eq(vfs.data("hello.txt"), "hello world", "COPY onto itself preserves source");

    Request inside(HTTP_COPY, "/copy");
    inside.header("Destination", "/copy/inside");
    check(call(dav, inside) == ESP_OK && g_resp.status == "403 Forbidden",
          "COPY inside itself is rejected");
    check(vfs.has("copy/b.txt") && !vfs.has("copy/inside"),
          "COPY inside itself creates nothing");

    Request move(HTTP_MOVE, "/copy/b.txt");
    move.header("Destination", "http://davbox.local:18080/moved.txt");
    check(call(dav, move) == ESP_OK && vfs.has("moved.txt") && !vfs.has("copy/b.txt"),
          "MOVE accepts absolute Destination URL");

    Request del(HTTP_DELETE, "/dir");
    check(call(dav, del) == ESP_OK && g_resp.status == "204 No Content", "DELETE status");
    check(g_resp.body.empty() && g_resp.headers["content-length"] == "0",
          "DELETE 204 has no body");
    check(!vfs.has("dir/a.txt") && !vfs.has("dir"), "DELETE removes directory recursively");
  }

  {
    Request lock(HTTP_LOCK, "/moved.txt");
    lock.header("Timeout", "Second-600");
    check(call(dav, lock) == ESP_OK && g_resp.status == "200 OK", "LOCK returns 200");
    check(contains(g_resp.headers["lock-token"], "opaquelocktoken"),
          "LOCK returns a lock token header");
    check(contains(g_resp.body, "<D:activelock>"), "LOCK returns lock discovery XML");
    size_t first_discovery = g_resp.body.find("<D:lockdiscovery>");
    check(first_discovery != std::string::npos &&
              g_resp.body.find("<D:lockdiscovery>", first_discovery + 1) == std::string::npos,
          "LOCK emits exactly one lockdiscovery element");

    std::string token = g_resp.headers["lock-token"];
    if (token.size() >= 2 && token.front() == '<' && token.back() == '>')
      token = token.substr(1, token.size() - 2);

    Request propfind(HTTP_PROPFIND, "/moved.txt");
    call(dav, propfind);
    check(contains(g_resp.body, token), "PROPFIND exposes the active lock token");

    Request conflict(HTTP_LOCK, "/moved.txt");
    check(call(dav, conflict) == ESP_OK && g_resp.status == "423 Locked",
          "second LOCK without token returns 423");

    Request refresh(HTTP_LOCK, "/moved.txt");
    refresh.header("If", "(<" + token + ">)");
    check(call(dav, refresh) == ESP_OK && g_resp.status == "200 OK",
          "LOCK with matching If refreshes the lock");

    Request blocked_put(HTTP_PUT, "/moved.txt", "blocked");
    check(call(dav, blocked_put) == ESP_OK && g_resp.status == "423 Locked",
          "PUT to locked file returns 423");
    check_eq(vfs.data("moved.txt"), "nested", "blocked PUT leaves file unchanged");

    Request owned_put(HTTP_PUT, "/moved.txt", "owned");
    owned_put.header("If", "(<" + token + ">)");
    check(call(dav, owned_put) == ESP_OK && g_resp.status == "204 No Content",
          "PUT with matching lock token succeeds");
    check_eq(vfs.data("moved.txt"), "owned", "lock-authorized PUT writes new bytes");

    Request negated(HTTP_PUT, "/moved.txt", "negated");
    negated.header("If", "(Not <" + token + ">)");
    check(call(dav, negated) == ESP_OK && g_resp.status == "423 Locked",
          "If: (Not <token>) does not authorize a locked write");
    check_eq(vfs.data("moved.txt"), "owned", "negated lock token leaves file unchanged");

    Request prefix(HTTP_PUT, "/moved.txt", "prefix");
    prefix.header("If", "(<" + token + "0>)");
    check(call(dav, prefix) == ESP_OK && g_resp.status == "423 Locked",
          "lock-token prefix does not match a longer token");
    check_eq(vfs.data("moved.txt"), "owned", "prefix token leaves file unchanged");

    Request no_token(HTTP_UNLOCK, "/moved.txt");
    check(call(dav, no_token) == ESP_OK && g_resp.status == "400 Bad Request",
          "UNLOCK without Lock-Token returns 400");

    Request wrong_token(HTTP_UNLOCK, "/moved.txt");
    wrong_token.header("Lock-Token", "<opaquelocktoken:wrong>");
    check(call(dav, wrong_token) == ESP_OK && g_resp.status == "409 Conflict",
          "UNLOCK with wrong token returns 409");

    Request unlock(HTTP_UNLOCK, "/moved.txt");
    unlock.header("Lock-Token", "<" + token + ">");
    check(call(dav, unlock) == ESP_OK && g_resp.status == "204 No Content",
          "UNLOCK with correct token returns 204");

    Request after_unlock(HTTP_PUT, "/moved.txt", "after");
    check(call(dav, after_unlock) == ESP_OK && g_resp.status == "204 No Content",
          "PUT works after UNLOCK");
  }

  {
    Request patch(HTTP_PROPPATCH, "/moved.txt",
                  "<?xml version=\"1.0\"?><D:propertyupdate xmlns:D=\"DAV:\"/>");
    patch.header("Content-Type", "application/xml");
    check(call(dav, patch) == ESP_OK && g_resp.status == "207 Multi-Status",
          "PROPPATCH returns 207 Multi-Status");
  }

  {
    const std::string patch_body =
        "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
        "<P:color xmlns:P=\"urn:davbox\">blue</P:color>"
        "</D:prop></D:set></D:propertyupdate>";
    Request patch(HTTP_PROPPATCH, "/hello.txt", patch_body);
    check(call(dav, patch) == ESP_OK && g_resp.status == "207 Multi-Status",
          "PROPPATCH stores a dead property");
    check(vfs.data(".davprops").find("urn:davbox") != std::string::npos,
          "dead properties are persisted to the VFS");

    const std::string propfind_body =
        "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<P:color xmlns:P=\"urn:davbox\"/>"
        "</D:prop></D:propfind>";
    Request propfind(HTTP_PROPFIND, "/hello.txt", propfind_body);
    propfind.header("Depth", "0");
    call(dav, propfind);
    check(contains(g_resp.body, "blue"), "PROPFIND returns the dead property value");
    check(contains(g_resp.body, "<P:color xmlns:P=\"urn:davbox\">"),
          "dead property XML separates the element name and namespace declaration");

    const std::string remove_body =
        "<D:propertyupdate xmlns:D=\"DAV:\"><D:remove><D:prop>"
        "<P:color xmlns:P=\"urn:davbox\"/>"
        "</D:prop></D:remove></D:propertyupdate>";
    Request remove(HTTP_PROPPATCH, "/hello.txt", remove_body);
    call(dav, remove);
    check(g_resp.status == "207 Multi-Status", "PROPPATCH removes a dead property");
    Request check_removed(HTTP_PROPFIND, "/hello.txt", propfind_body);
    check_removed.header("Depth", "0");
    call(dav, check_removed);
    check(contains(g_resp.body, "404 Not Found"), "removed property reports 404");
  }

  {
    Request create(HTTP_PUT, "/prop-source.txt", "property source");
    check(call(dav, create) == ESP_OK && g_resp.status == "201 Created",
          "property lifecycle fixture created");
    const std::string set_body =
        "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
        "<P:color xmlns:P=\"urn:davbox\">green</P:color>"
        "</D:prop></D:set></D:propertyupdate>";
    Request patch(HTTP_PROPPATCH, "/prop-source.txt", set_body);
    check(call(dav, patch) == ESP_OK && g_resp.status == "207 Multi-Status",
          "property lifecycle fixture patched");
    Request move(HTTP_MOVE, "/prop-source.txt");
    move.header("Destination", "/prop-target.txt");
    check(call(dav, move) == ESP_OK, "property lifecycle fixture moved");
    const std::string find_body =
        "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
        "<P:color xmlns:P=\"urn:davbox\"/>"
        "</D:prop></D:propfind>";
    Request find(HTTP_PROPFIND, "/prop-target.txt", find_body);
    find.header("Depth", "0");
    call(dav, find);
    check(contains(g_resp.body, "green"), "MOVE re-keys dead properties");
    Request del(HTTP_DELETE, "/prop-target.txt");
    check(call(dav, del) == ESP_OK && g_resp.status == "204 No Content",
          "property lifecycle fixture deleted");
    Request recreate(HTTP_PUT, "/prop-target.txt", "fresh");
    check(call(dav, recreate) == ESP_OK, "property lifecycle fixture recreated");
    Request after(HTTP_PROPFIND, "/prop-target.txt", find_body);
    after.header("Depth", "0");
    call(dav, after);
    check(!contains(g_resp.body, "green"), "DELETE removes dead properties");
    check(vfs.remove("prop-target.txt"), "property lifecycle fixture cleaned up");
  }

  {
    Request range(HTTP_GET, "/hello.txt");
    range.header("Range", "bytes=0-4");
    check(call(dav, range) == ESP_OK && g_resp.status == "206 Partial Content",
          "GET range returns 206");
    check_eq(g_resp.body, "hello", "GET range returns the selected bytes");
    check_eq(g_resp.headers["content-range"], "bytes 0-4/11", "GET range sets Content-Range");

    Request suffix(HTTP_GET, "/hello.txt");
    suffix.header("Range", "bytes=-5");
    call(dav, suffix);
    check_eq(g_resp.body, "world", "GET supports suffix ranges");

    Request bad_range(HTTP_GET, "/hello.txt");
    bad_range.header("Range", "bytes=99-");
    call(dav, bad_range);
    check(g_resp.status == "416 Range Not Satisfiable", "GET rejects unsatisfiable ranges");
    check_eq(g_resp.headers["content-range"], "bytes */11", "416 sets Content-Range");
  }

  {
    Request mismatch(HTTP_PUT, "/hello.txt", "wrong");
    mismatch.header("If-Match", "\"nope\"");
    call(dav, mismatch);
    check(g_resp.status == "412 Precondition Failed", "PUT enforces If-Match");
    check(contains(g_resp.body, "<D:error"), "precondition failure uses a DAV error body");

    Request stale(HTTP_PUT, "/hello.txt", "wrong");
    stale.header("If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT");
    call(dav, stale);
    check(g_resp.status == "412 Precondition Failed", "PUT enforces If-Unmodified-Since");

    Request chunked(HTTP_PUT, "/chunked.txt", "x");
    chunked.header("Transfer-Encoding", "chunked");
    call(dav, chunked);
    check(g_resp.status == "501 Not Implemented", "chunked request bodies are rejected");

    Request star(HTTP_OPTIONS, "*");
    call(dav, star);
    check(g_resp.status == "204 No Content" && g_resp.headers["dav"] == "1,2",
          "OPTIONS * is supported");
  }

  {
    const std::string lock_body =
        "<D:lockinfo xmlns:D=\"DAV:\"><D:lockscope><D:shared/></D:lockscope>"
        "<D:locktype><D:write/></D:locktype><D:owner>client-a</D:owner></D:lockinfo>";
    Request first(HTTP_LOCK, "/hello.txt", lock_body);
    call(dav, first);
    check(g_resp.status == "200 OK", "shared LOCK is accepted");
    std::string token_a = g_resp.headers["lock-token"];
    if (token_a.size() >= 2 && token_a.front() == '<')
      token_a = token_a.substr(1, token_a.size() - 2);
    check(contains(g_resp.body, "client-a"), "LOCK reports the owner");

    Request second(HTTP_LOCK, "/hello.txt", lock_body);
    call(dav, second);
    check(g_resp.status == "200 OK", "a second shared LOCK is accepted");
    std::string token_b = g_resp.headers["lock-token"];
    if (token_b.size() >= 2 && token_b.front() == '<')
      token_b = token_b.substr(1, token_b.size() - 2);
    check(token_b != token_a, "shared locks get distinct tokens");

    Request blocked(HTTP_PUT, "/hello.txt", "blocked");
    call(dav, blocked);
    check(g_resp.status == "423 Locked", "shared locks block non-holders");

    Request holder(HTTP_PUT, "/hello.txt", "shared-write");
    holder.header("If", "(<" + token_a + ">)");
    call(dav, holder);
    check(g_resp.status == "204 No Content", "a shared lock holder may write");

    for (const std::string &token : {token_a, token_b}) {
      Request unlock(HTTP_UNLOCK, "/hello.txt");
      unlock.header("Lock-Token", "<" + token + ">");
      call(dav, unlock);
      check(g_resp.status == "204 No Content", "shared lock can be unlocked");
    }
  }

  {
    Request bad_parent(HTTP_COPY, "/hello.txt");
    bad_parent.header("Destination", "/missing/deep/file.txt");
    call(dav, bad_parent);
    check(g_resp.status == "409 Conflict", "COPY checks the destination parent");

    Request other_host(HTTP_COPY, "/hello.txt");
    other_host.header("Destination", "http://evil.example/file.txt");
    other_host.header("Host", "davbox.local");
    call(dav, other_host);
    check(g_resp.status == "400 Bad Request", "COPY rejects a foreign Destination host");

    Request body_mkcol(HTTP_MKCOL, "/withbody", "x");
    call(dav, body_mkcol);
    check(g_resp.status == "415 Unsupported Media Type", "MKCOL rejects a body");

    Request missing_parent(HTTP_MKCOL, "/missing/child");
    call(dav, missing_parent);
    check(g_resp.status == "409 Conflict", "MKCOL requires an existing parent");

    Request bad_depth(HTTP_PROPFIND, "/hello.txt");
    bad_depth.header("Depth", "2");
    call(dav, bad_depth);
    check(g_resp.status == "400 Bad Request", "PROPFIND rejects invalid Depth");

    Request protected_body(HTTP_PROPPATCH, "/hello.txt",
                           "<D:propertyupdate xmlns:D=\"DAV:\"><D:set><D:prop>"
                           "<D:getetag/></D:prop></D:set></D:propertyupdate>");
    call(dav, protected_body);
    check(g_resp.status == "207 Multi-Status" && contains(g_resp.body, "403 Forbidden"),
          "PROPPATCH rejects protected live properties");
  }

  {
    Request traversal(HTTP_GET, "/../secret");
    check(call(dav, traversal) == ESP_OK && g_resp.status == "400 Bad Request",
          "GET rejects ../ traversal");

    Request traversal_put(HTTP_PUT, "/a/../../b", "bad");
    check(call(dav, traversal_put) == ESP_OK && g_resp.status == "400 Bad Request",
          "PUT rejects ../ traversal");
    check(!vfs.has("b"), "traversal PUT creates no file");
  }

  {
    // Music Assistant performs a PROPFIND per media file. FAT names may be
    // 255 UTF-16 characters (765 UTF-8 bytes); rejecting them here made a
    // whole sync fail with HTTP 400.
    Request long_name(HTTP_PROPFIND, "/" + std::string(765, 'a'));
    long_name.header("Depth", "0");
    check(call(dav, long_name) == ESP_OK && g_resp.status == "404 Not Found",
          "PROPFIND accepts a 765-byte filename");

    Request overlong_name(HTTP_PROPFIND, "/" + std::string(766, 'a'));
    overlong_name.header("Depth", "0");
    check(call(dav, overlong_name) == ESP_OK && g_resp.status == "400 Bad Request",
          "PROPFIND rejects a filename over 765 bytes");
  }

  {
    // Clients percent-encode UTF-8 names, tripling the URI length. The HTTPD
    // URI buffer must be larger than the decoded path limit, or long media
    // files are rejected before the component ever sees them.
    std::string utf8_name;
    for (int i = 0; i < 255; ++i)
      utf8_name += "\xE2\x82\xAC";
    std::string encoded_name;
    const char digits[] = "0123456789ABCDEF";
    for (unsigned char c : utf8_name) {
      encoded_name.push_back('%');
      encoded_name.push_back(digits[c >> 4]);
      encoded_name.push_back(digits[c & 0x0f]);
    }
    Request encoded(HTTP_PROPFIND, "/" + encoded_name);
    encoded.header("Depth", "0");
    check(encoded.uri.size() == 2296, "encoded long-name URI exceeds the old 512-byte limit");
    check(call(dav, encoded) == ESP_OK && g_resp.status == "404 Not Found",
          "PROPFIND accepts a percent-encoded 765-byte filename");
  }

  {
    Request del(HTTP_DELETE, "/moved.txt");
    check(call(dav, del) == ESP_OK && !vfs.has("moved.txt"), "DELETE removes file");
  }

  {
    std::cout << "host tests: VfsExt2\n";
    ext2_ram_blockdev_t ram{};
    check(ext2_ram_blockdev_init(&ram, g_ext2_storage, sizeof(g_ext2_storage), EXT2_BLOCK_BYTES),
          "ext2 RAM block device init");
    // The fixture is prepared here; the VFS under test only mounts it.
    ext2_t fixture{};
    check(ext2_format(&fixture, &ram.device) == EXT2_OK, "host ext2 fixture format");
    check(ext2_mkdir(&fixture, "/lost+found") == EXT2_OK, "ext2 fixture creates lost+found");
    check(ext2_commit(&fixture) == EXT2_OK, "ext2 fixture commits lost+found");
    check(ext2_unmount(&fixture) == EXT2_OK, "ext2 fixture detaches before VFS mount");

    esphome::webdav_filing::VfsExt2 ext2(&ram.device);
    check(ext2.init(), "ext2 mounts the fixture volume");

    check(ext2.is_hidden("/lost+found") && ext2.is_hidden("/lost+found/child") &&
              !ext2.is_hidden("/davdir/lost+found"),
          "only the root lost+found is hidden");
    esphome::webdav_filing::VfsStat hidden{};
    check(ext2.stat("/lost+found", &hidden) && !hidden.exists, "lost+found is hidden from stat");
    check(ext2.stat("/lost+found/", &hidden) && !hidden.exists,
          "lost+found with a trailing slash is hidden");
    check(ext2.stat("/lost+found/child", &hidden) && !hidden.exists,
          "lost+found children are hidden");
    std::vector<esphome::webdav_filing::VfsDirEntry> root_entries;
    check(ext2.read_dir("/", &root_entries) &&
              std::none_of(root_entries.begin(), root_entries.end(),
                           [](const auto &entry) { return entry.name == "lost+found"; }),
          "lost+found is hidden from root listing");
    uint8_t hidden_buf[1]{};
    size_t hidden_read = 0;
    check(!ext2.read_file("/lost+found/child", 0, hidden_buf, 1, &hidden_read),
          "lost+found reads are refused");
    check(!ext2.write_file("/lost+found/child", hidden_buf, 1), "lost+found writes are refused");
    check(!ext2.mkdir("/lost+found/child"), "lost+found mkdir is refused");
    check(!ext2.remove("/lost+found"), "lost+found removal is refused");
    check(!ext2.rename("/lost+found", "/davdir"), "lost+found rename is refused");

    check(ext2.mkdir("/davdir"), "ext2 mkdir");
    check(ext2.mkdir("/davdir/lost+found"), "nested lost+found is still allowed");
    esphome::webdav_filing::VfsStat nested{};
    check(ext2.stat("/davdir/lost+found", &nested) && nested.exists && nested.is_dir,
          "nested lost+found stays visible");
    check(ext2.remove("/davdir/lost+found"), "nested lost+found can be removed");

    const std::string payload = "ext2 webdav payload";
    check(ext2.write_file("/davdir/a.txt", reinterpret_cast<const uint8_t *>(payload.data()),
                          payload.size()),
          "ext2 write_file");

    esphome::webdav_filing::VfsStat st{};
    check(ext2.stat("/davdir/a.txt", &st) && st.exists && !st.is_dir &&
              st.size == payload.size(),
          "ext2 stat file");
    check(st.mtime >= 1767225600, "ext2 mtime is sane");

    uint8_t buf[64]{};
    size_t read = 0;
    check(ext2.read_file("/davdir/a.txt", 5, buf, 6, &read) &&
              std::string(reinterpret_cast<char *>(buf), read) == "webdav",
          "ext2 ranged read");

    std::vector<esphome::webdav_filing::VfsDirEntry> entries;
    check(ext2.read_dir("/davdir", &entries) && entries.size() == 1 &&
              entries[0].name == "a.txt",
          "ext2 read_dir");

    // Exercise the contiguous multi-block reader with a stream larger than the
    // 16-block (64 KiB) run limit, including the indirect-pointer boundary.
    constexpr size_t kLargeSize = 256 * 1024;
    std::vector<uint8_t> large(kLargeSize);
    for (size_t i = 0; i < large.size(); ++i)
      large[i] = static_cast<uint8_t>((i * 31 + (i >> 8)) & 0xff);
    bool large_written = true;
    for (size_t off = 0; off < large.size(); off += 8192)
      large_written = large_written &&
                      ext2.write_file("/davdir/large.bin", large.data() + off, 8192);
    check(large_written && ext2.sync(), "ext2 writes large file");

    std::vector<uint8_t> large_read_back(kLargeSize);
    size_t large_read = 0;
    check(ext2.read_file("/davdir/large.bin", 0, large_read_back.data(),
                         large_read_back.size(), &large_read) &&
              large_read == kLargeSize && large_read_back == large,
          "ext2 contiguous multi-block read");
    check(ext2.remove("/davdir/large.bin"), "ext2 remove large file");

    // Cross the single-indirect boundary (12 + 1024 blocks = ~4.24 MiB).
    constexpr size_t kHugeSize = 5 * 1024 * 1024;
    std::vector<uint8_t> huge(kHugeSize);
    for (size_t i = 0; i < huge.size(); ++i)
      huge[i] = static_cast<uint8_t>((i * 17 + (i >> 9)) & 0xff);
    bool huge_written = true;
    for (size_t off = 0; off < huge.size(); off += 8192)
      huge_written = huge_written &&
                     ext2.write_file("/davdir/huge.bin", huge.data() + off, 8192);
    check(huge_written && ext2.sync(), "ext2 writes a double-indirect file");
    std::vector<uint8_t> huge_read_back(kHugeSize);
    size_t huge_read = 0;
    check(ext2.read_file("/davdir/huge.bin", 0, huge_read_back.data(),
                         huge_read_back.size(), &huge_read) &&
              huge_read == kHugeSize && huge_read_back == huge,
          "ext2 reads a double-indirect file");
    TestWebDAVFiling ext2_http(8080, 18, 19, 23, 5, 4000000);
    ext2_http.set_vfs(&ext2);
    ext2_http.setup();
    Request huge_get(HTTP_GET, "/davdir/huge.bin");
    check(call(ext2_http, huge_get) == ESP_OK && g_resp.status == "200 OK" &&
              g_resp.body.size() == kHugeSize,
          "HTTP GET streams a double-indirect file");
    check(ext2.remove("/davdir/huge.bin"), "ext2 remove double-indirect file");

    check(ext2.rename("/davdir/a.txt", "/davdir/b.txt"), "ext2 rename");
    check(ext2.remove("/davdir/b.txt"), "ext2 remove file");

    std::string long_dir = "/long";
    std::vector<std::string> long_dirs;
    check(ext2.mkdir(long_dir), "ext2 mkdir long-path root");
    long_dirs.push_back(long_dir);
    for (int i = 0; i < 14; ++i) {
      long_dir += "/abcdefghijklmnopqrst";
      long_dirs.push_back(long_dir);
      check(ext2.mkdir(long_dir), "ext2 mkdir long nested path");
    }
    const std::string long_file = long_dir + "/f";
    check(long_file.size() > 255, "nested ext2 path exceeds one name");
    check(ext2.write_file(long_file, reinterpret_cast<const uint8_t *>(payload.data()),
                          payload.size()),
          "ext2 writes a path longer than 255 bytes");
    esphome::webdav_filing::VfsStat long_st{};
    check(ext2.stat(long_file, &long_st) && long_st.exists && long_st.size == payload.size(),
          "ext2 stats a path longer than 255 bytes");
    check(ext2.remove(long_file), "ext2 removes long-path file");
    for (auto it = long_dirs.rbegin(); it != long_dirs.rend(); ++it)
      check(ext2.remove(*it), "ext2 removes long-path directory");

    check(ext2.remove("/davdir"), "ext2 remove empty directory");
    check(ext2.sync(), "ext2 commit");
    ext2.shutdown();
  }

  {
    std::cout << "host tests: ext2 real-world inode count\n";
    // A 20 GiB partition formatted with mke2fs defaults has about 1.3M
    // inodes. It must mount without asking the card to be reformatted.
    constexpr uint32_t kBlocks = 5242880;
    constexpr uint32_t kBlocksPerGroup = 32768;
    constexpr uint32_t kInodesPerGroup = 8192;
    constexpr uint32_t kGroups = kBlocks / kBlocksPerGroup;

    SparseBlockDevice sparse{};
    sparse.device.block_size = EXT2_BLOCK_BYTES;
    sparse.device.block_count = kBlocks;
    sparse.device.context = &sparse;
    sparse.device.read = sparse_read;
    sparse.device.write = sparse_write;

    ext2_super_t super{};
    super.inodes_count = kGroups * kInodesPerGroup;
    super.blocks_count = kBlocks;
    super.free_blocks_count = kBlocks - 1024;
    super.free_inodes_count = super.inodes_count - 11;
    super.first_data_block = 0;
    super.log_block_size = 2;
    super.log_frag_size = 2;
    super.blocks_per_group = kBlocksPerGroup;
    super.frags_per_group = kBlocksPerGroup;
    super.inodes_per_group = kInodesPerGroup;
    super.magic = 0xEF53;
    super.state = 1;
    super.rev_level = 1;
    super.first_ino = 11;
    super.inode_size = 256;  // mke2fs default on current Linux distributions
    // Compatible ext_attr/resize_inode/dir_index, allowed sparse_super and
    // large_file, and the FILETYPE incompat bit Linux sets by default.
    super.feature_compat = 0x0008 | 0x0010 | 0x0020;
    super.feature_ro_compat = 0x0001 | 0x0002;
    super.feature_incompat = 0x0002;

    std::vector<uint8_t> superblock(EXT2_BLOCK_BYTES, 0);
    std::memcpy(superblock.data() + 1024, &super, sizeof(super));
    sparse.blocks[0] = superblock;

    const size_t gdt_bytes = static_cast<size_t>(kGroups) * sizeof(ext2_group_desc_t);
    std::vector<uint8_t> gdt((gdt_bytes + EXT2_BLOCK_BYTES - 1) & ~(EXT2_BLOCK_BYTES - 1), 0);
    auto *const descriptors = reinterpret_cast<ext2_group_desc_t *>(gdt.data());
    for (uint32_t g = 0; g < kGroups; ++g) {
      descriptors[g].block_bitmap = 1024 + g * 3;
      descriptors[g].inode_bitmap = 1025 + g * 3;
      descriptors[g].inode_table = 1026 + g * 3;
      descriptors[g].free_blocks_count = static_cast<uint16_t>(kBlocksPerGroup);
      descriptors[g].free_inodes_count = static_cast<uint16_t>(kInodesPerGroup);
    }
    sparse.blocks[1].assign(gdt.begin(), gdt.begin() + EXT2_BLOCK_BYTES);
    sparse.blocks[2].assign(gdt.begin() + EXT2_BLOCK_BYTES, gdt.end());

    // Mount validates the root inode, so mark inode 2 in use and give it a
    // directory body block inside the huge sparse device.
    std::vector<uint8_t> inode_bitmap(EXT2_BLOCK_BYTES, 0);
    inode_bitmap[1 / 8] = static_cast<uint8_t>(1u << (1 % 8));
    sparse.blocks[descriptors[0].inode_bitmap] = inode_bitmap;

    std::vector<uint8_t> inode_table(EXT2_BLOCK_BYTES, 0);
    ext2_inode_t root{};
    root.mode = 0x41ED;  // 040755
    root.size = 1024;
    root.links_count = 2;
    root.blocks = 2;
    root.block[0] = 2000;
    std::memcpy(inode_table.data() + super.inode_size, &root, sizeof(root));
    sparse.blocks[descriptors[0].inode_table] = inode_table;

    ext2_t large{};
    check(ext2_mount(&large, &sparse.device) == EXT2_OK,
          "ext2 mounts a volume with 1.3M inodes");
    check(large.inode_count == super.inodes_count, "ext2 reports the full inode count");
    check(ext2_unmount(&large) == EXT2_OK, "ext2 unmounts the large-inode volume");
  }

  {
    std::cout << "host tests: ext2 host-formatted large geometries\n";
    // mke2fs defaults: 953 groups for a 119 GiB card, 2048 groups for 256 GiB,
    // and 16384 groups for 2 TiB. Mounting must not depend on group count.
    for (uint32_t groups : {953u, 2048u, 16384u}) {
      const std::string label = std::to_string(groups) + " groups";
      SparseExt2Image image;
      init_sparse_ext2(&image, groups);
      ext2_t volume{};
      check(ext2_mount(&volume, &image.sparse.device) == EXT2_OK,
            "ext2 mounts " + label);
      check(volume.group_count == groups, "ext2 reports " + label);
      check(volume.inode_count == groups * 8192u, "ext2 reports inodes for " + label);
      check(ext2_unmount(&volume) == EXT2_OK, "ext2 unmounts " + label);
    }
  }

  {
    std::cout << "host tests: ext2 inode numbers above INT_MAX\n";
    // A 2 TiB-class volume with 32768 inodes per group has inode numbers
    // beyond INT_MAX. They must not be mistaken for an allocation failure.
    SparseExt2Image image;
    init_sparse_ext2(&image, 65537, 32768);
    ext2_t fs{};
    check(ext2_mount(&fs, &image.sparse.device) == EXT2_OK,
          "ext2 mounts the high-inode-number volume");
    constexpr uint32_t kInode = 0x8000000Bu;
    ext2_test_set_inode_hint(&fs, kInode);
    check(ext2_touch_at(&fs, EXT2_ROOT_INO, "high", 0100644) == EXT2_OK,
          "ext2 allocates an inode above INT_MAX");
    uint32_t found = 0;
    check(ext2_lookup(&fs, "/high", &found) == EXT2_OK && found == kInode,
          "ext2 resolves the high inode number");
    check(ext2_unmount(&fs) == EXT2_OK, "ext2 unmounts the high-inode-number volume");
  }

  {
    std::cout << "host tests: ext2 bitmap cache eviction\n";
    // More groups than bitmap-cache slots: every group must still be written
    // back when its entry is evicted, not only the last 32.
    constexpr uint32_t kGroups = 64;
    SparseExt2Image image;
    init_sparse_ext2(&image, kGroups);
    ext2_t fs{};
    check(ext2_mount(&fs, &image.sparse.device) == EXT2_OK,
          "ext2 mounts the bitmap-eviction volume");

    bool marked = true;
    for (uint32_t g = 0; g < kGroups; ++g) {
      const uint8_t value = static_cast<uint8_t>(0xA0u + (g & 0x0Fu));
      marked = marked &&
               ext2_test_set_group_bitmap_byte(&fs, g, 0, value) == EXT2_OK;
    }
    check(marked, "all groups pass through the fixed bitmap cache");
    check(ext2_commit(&fs) == EXT2_OK, "ext2 commits evicted bitmap writes");

    bool persisted = true;
    for (uint32_t g = 0; g < kGroups; ++g) {
      const uint8_t expected = static_cast<uint8_t>(0xA0u + (g & 0x0Fu));
      const auto block = image.sparse.blocks.find(image.descriptors[g].block_bitmap);
      persisted =
          persisted && block != image.sparse.blocks.end() &&
          block->second[0] == expected;
    }
    check(persisted, "evicted bitmaps reach the medium");
    check(ext2_unmount(&fs) == EXT2_OK, "ext2 unmounts the bitmap-eviction volume");
  }

  {
    std::cout << "host tests: ext2 inode-table cache eviction\n";
    // One file per group spreads the inode tables past the 128-entry cache.
    // The early inodes must survive eviction with their mode intact.
    constexpr uint32_t kGroups = 160;
    SparseExt2Image image;
    init_sparse_ext2(&image, kGroups);
    ext2_t fs{};
    check(ext2_mount(&fs, &image.sparse.device) == EXT2_OK,
          "ext2 mounts the inode-table-eviction volume");

    bool created = true;
    for (uint32_t g = 0; g < kGroups; ++g) {
      ext2_test_set_inode_hint(&fs, 11u + g * 8192u);
      const std::string name = "f" + std::to_string(g);
      created =
          created && ext2_touch_at(&fs, EXT2_ROOT_INO, name.c_str(), 0100644) == EXT2_OK;
    }
    check(created, "files survive inode-table cache eviction");

    // Reuse a free slot in group 0's first table block after that block was
    // evicted. Loading it must read the live inodes back, not zero the block.
    ext2_test_set_inode_hint(&fs, 12u);
    check(ext2_touch_at(&fs, EXT2_ROOT_INO, "again", 0100644) == EXT2_OK,
          "a free slot in an evicted table block can be reused");

    check(ext2_commit(&fs) == EXT2_OK, "ext2 commits evicted inode-table writes");

    bool persisted = true;
    for (uint32_t g = 0; g < kGroups; ++g) {
      const uint32_t lba = image.descriptors[g].inode_table;
      const auto block = image.sparse.blocks.find(lba);
      const uint8_t *inode = block == image.sparse.blocks.end()
                                 ? nullptr
                                 : block->second.data() + 10u * 256u;
      persisted = persisted && inode != nullptr && inode[0] == 0xA4 && inode[1] == 0x81;
    }
    check(persisted, "evicted inode tables reach the medium");
    check(ext2_unmount(&fs) == EXT2_OK,
          "ext2 unmounts the inode-table-eviction volume");
  }

  {
    std::cout << "host tests: ext2 metadata reload after recovery\n";
    // Journal replay writes metadata directly to the medium. A reload must
    // drop cached inode-table blocks instead of serving pre-replay bytes.
    SparseExt2Image image;
    init_sparse_ext2(&image, 8);
    ext2_t fs{};
    check(ext2_mount(&fs, &image.sparse.device) == EXT2_OK,
          "ext2 mounts the recovery-reload volume");
    check(ext2_touch_at(&fs, EXT2_ROOT_INO, "f0", 0100644) == EXT2_OK,
          "ext2 creates a file before the simulated replay");

    uint32_t inode = 0;
    check(ext2_lookup(&fs, "/f0", &inode) == EXT2_OK && inode == 11,
          "ext2 locates the file's inode");
    ext2_stat_t before{};
    check(ext2_stat_inode(&fs, inode, &before) == EXT2_OK && !before.is_dir,
          "the cached inode is a regular file");

    const uint32_t index = inode - 1;
    const uint32_t within = (index * 256u) / EXT2_BLOCK_BYTES;
    const size_t offset = (index * 256u) % EXT2_BLOCK_BYTES;
    auto &block = image.sparse.blocks[image.descriptors[0].inode_table + within];
    block[offset] = 0xED;  // 040755: directory, changed on the medium
    block[offset + 1] = 0x41;

    ext2_stat_t stale{};
    check(ext2_stat_inode(&fs, inode, &stale) == EXT2_OK && !stale.is_dir,
          "the cache still shows the stale inode before reload");
    check(ext2_test_reload_metadata(&fs) == EXT2_OK,
          "ext2 reloads metadata after recovery");
    ext2_stat_t after{};
    check(ext2_stat_inode(&fs, inode, &after) == EXT2_OK && after.is_dir,
          "reload drops the stale inode-table block");
    check(ext2_unmount(&fs) == EXT2_OK, "ext2 unmounts the recovery-reload volume");
  }

  {
    std::cout << "host tests: partition scan\n";
    FakeDisk disk;
    put_ext2_magic(&disk, 0);
    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection),
          "AUTO finds an ext2 volume on a tableless card");
    check(selection.table == esphome::webdav_filing::PartitionTable::RAW &&
              selection.number == 0 && selection.start_sector == 0 &&
              selection.sector_count == kFakeDiskSectors && selection.ext2_magic,
          "raw ext2 selection spans the card");
  }

  {
    FakeDisk disk;
    put_mbr_partition(&disk, 0, 0x0C, 2048, 8192);
    put_mbr_partition(&disk, 1, 0x83, 10240, 8192);
    put_ext2_magic(&disk, 10240);
    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection),
          "AUTO finds an ext2 volume in an MBR primary partition");
    check(selection.table == esphome::webdav_filing::PartitionTable::MBR && selection.number == 2 &&
              selection.start_sector == 10240 && selection.sector_count == 8192 &&
              selection.ext2_magic,
          "MBR primary selection reports the partition range");

    esphome::webdav_filing::PartitionSelector by_number{};
    by_number.kind = esphome::webdav_filing::PartitionSelectorKind::NUMBER;
    by_number.number = 1;
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, by_number, &selection) &&
              selection.number == 1 && selection.start_sector == 2048 && !selection.ext2_magic,
          "explicit MBR partition number can select an unformatted partition");
  }

  {
    FakeDisk disk;
    put_mbr_partition(&disk, 0, 0x0C, 2048, 8192);
    put_mbr_partition(&disk, 1, 0x05, 10240, 16384);
    uint8_t *const ebr = sector_at(&disk, 10240);
    ebr[446 + 4] = 0x83;
    put_le32(ebr + 446 + 8, 2048);
    put_le32(ebr + 446 + 12, 8192);
    put_le16(ebr + 510, 0xAA55);
    put_ext2_magic(&disk, 12288);
    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection) &&
              selection.table == esphome::webdav_filing::PartitionTable::MBR &&
              selection.number == 5 && selection.start_sector == 12288 && selection.ext2_magic,
          "AUTO finds an ext2 volume in an MBR logical partition");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);

    uint8_t *const entries = sector_at(&disk, kGptPrimaryEntries);
    entries[0] = 0x01;
    put_le64(entries + 32, 2048);
    put_le64(entries + 40, 10239);
    put_gpt_name(entries, "fat");

    uint8_t *const linux_entry = entries + kGptEntrySize;
    const uint8_t linux_guid[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    std::memcpy(linux_entry, linux_guid, sizeof(linux_guid));
    put_le64(linux_entry + 32, 10240);
    put_le64(linux_entry + 40, 20479);
    put_gpt_name(linux_entry, "davdata");
    put_ext2_magic(&disk, 10240);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);

    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection),
          "AUTO finds an ext2 volume in a GPT partition with valid CRCs");
    check(selection.table == esphome::webdav_filing::PartitionTable::GPT && selection.number == 2 &&
              selection.start_sector == 10240 && selection.sector_count == 10240 &&
              std::string(selection.name) == "davdata" && selection.gpt_linux,
          "GPT selection reports the partition range and name");

    esphome::webdav_filing::PartitionSelector by_name{};
    by_name.kind = esphome::webdav_filing::PartitionSelectorKind::NAME;
    std::strncpy(by_name.name, "DAVDATA", sizeof(by_name.name) - 1);
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, by_name, &selection) &&
              selection.number == 2 && selection.start_sector == 10240,
          "GPT partition can be selected by case-insensitive name");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    put_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount, kGptEntrySize);

    uint8_t *const linux_entry = sector_at(&disk, kGptPrimaryEntries) + kGptEntrySize;
    const uint8_t linux_guid[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    std::memcpy(linux_entry, linux_guid, sizeof(linux_guid));
    put_le64(linux_entry + 32, 10240);
    put_le64(linux_entry + 40, 20479);
    put_gpt_name(linux_entry, "davdata");
    put_ext2_magic(&disk, 10240);
    copy_gpt_entries_to_backup(&disk);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    finish_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount,
                      kGptEntrySize);
    sector_at(&disk, 1)[20] ^= 0x01;  // Corrupt only the primary header CRC.

    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection) &&
              selection.table == PartitionTable::GPT && selection.number == 2 &&
              selection.start_sector == 10240,
          "GPT falls back to the backup header when the primary CRC is bad");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    put_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount, kGptEntrySize);

    uint8_t *const linux_entry = sector_at(&disk, kGptPrimaryEntries) + kGptEntrySize;
    const uint8_t linux_guid[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    std::memcpy(linux_entry, linux_guid, sizeof(linux_guid));
    put_le64(linux_entry + 32, 10240);
    put_le64(linux_entry + 40, 20479);
    put_gpt_name(linux_entry, "davdata");
    put_ext2_magic(&disk, 10240);
    copy_gpt_entries_to_backup(&disk);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    finish_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount,
                      kGptEntrySize);
    sector_at(&disk, kGptPrimaryEntries)[127] ^= 0x01;  // Corrupt the primary entry array CRC.

    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection) &&
              selection.table == PartitionTable::GPT && selection.number == 2 &&
              selection.start_sector == 10240,
          "GPT falls back to the backup entry array when the primary CRC is bad");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    put_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount, kGptEntrySize);

    uint8_t *const linux_entry = sector_at(&disk, kGptBackupEntries) + kGptEntrySize;
    const uint8_t linux_guid[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    std::memcpy(linux_entry, linux_guid, sizeof(linux_guid));
    put_le64(linux_entry + 32, 10240);
    put_le64(linux_entry + 40, 20479);
    put_gpt_name(linux_entry, "davdata");
    put_ext2_magic(&disk, 10240);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    finish_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount,
                      kGptEntrySize);
    std::memset(sector_at(&disk, 1), 0, 8);  // Primary signature is gone.

    PartitionSelection selection{};
    check(scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                        &selection) &&
              selection.table == PartitionTable::GPT && selection.number == 2 &&
              selection.start_sector == 10240,
          "GPT backup is used when the primary signature is missing");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, 100);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, 100);
    PartitionSelection selection{};
    check(!scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                         &selection),
          "malformed GPT entry size is rejected");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    uint8_t *const entry = sector_at(&disk, kGptPrimaryEntries);
    entry[0] = 0x01;
    put_le64(entry + 32, 2048);
    put_le64(entry + 40, kFakeDiskSectors + 10);
    put_gpt_name(entry, "davdata");
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    PartitionSelection selection{};
    check(!scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                         &selection),
          "GPT partition extending past the card is rejected");
  }

  {
    FakeDisk disk;
    put_protective_mbr(&disk);
    put_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    put_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount, kGptEntrySize);
    uint8_t *const linux_entry = sector_at(&disk, kGptPrimaryEntries) + kGptEntrySize;
    const uint8_t linux_guid[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                    0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    std::memcpy(linux_entry, linux_guid, sizeof(linux_guid));
    put_le64(linux_entry + 32, 10240);
    put_le64(linux_entry + 40, 20479);
    put_gpt_name(linux_entry, "davdata");
    put_ext2_magic(&disk, 10240);
    copy_gpt_entries_to_backup(&disk);
    finish_gpt_header(&disk, 1, kGptPrimaryEntries, kGptEntryCount, kGptEntrySize);
    finish_gpt_header(&disk, kFakeDiskSectors - 1, kGptBackupEntries, kGptEntryCount,
                      kGptEntrySize);
    sector_at(&disk, 1)[20] ^= 0x01;
    sector_at(&disk, kFakeDiskSectors - 1)[20] ^= 0x01;

    PartitionSelection selection{};
    check(!scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                         &selection),
          "GPT with both headers corrupt is rejected");
  }

  {
    FakeDisk disk;
    put_mbr_partition(&disk, 0, 0x83, 2048, 8192);
    PartitionSelection selection{};
    check(!scan_for_ext2(&disk, fake_read_sectors, kFakeDiskSectors, PartitionSelector{},
                         &selection),
          "AUTO refuses to select an unformatted MBR partition");
  }

  {
    std::cout << "host tests: volume discovery\n";
    FakeDisk disk;
    put_mbr_partition(&disk, 0, 0x0B, 2048, 4096);
    put_fat_boot_sector(&disk, 2048);
    put_mbr_partition(&disk, 1, 0x83, 8192, 8192);
    put_ext2_magic(&disk, 8192);

    std::vector<PartitionSelection> volumes;
    check(scan_partitions(&disk, fake_read_sectors, kFakeDiskSectors, &volumes),
          "scan_partitions reads an MBR card");
    check(volumes.size() == 2, "scan_partitions finds FAT and ext2 volumes");
    check(volumes.size() == 2 && volumes[0].fat_magic && !volumes[0].ext2_magic &&
              volumes[0].start_sector == 2048 && volumes[0].sector_count == 4096,
          "FAT partition keeps its table range");
    check(volumes.size() == 2 && volumes[1].ext2_magic && !volumes[1].fat_magic &&
              volumes[1].start_sector == 8192 && volumes[1].sector_count == 8192,
          "ext2 partition keeps its table range");
  }

  {
    std::cout << "host tests: VfsRouter\n";
    MemoryVfs fat;
    MemoryVfs ext2;
    const std::string fat_data = "FAT volume";
    const std::string ext2_data = "EXT2 volume";
    check(fat.write_file("fat.txt", reinterpret_cast<const uint8_t *>(fat_data.data()),
                         fat_data.size()),
          "router fixture writes FAT file");
    check(ext2.write_file("ext2.txt", reinterpret_cast<const uint8_t *>(ext2_data.data()),
                          ext2_data.size()),
          "router fixture writes ext2 file");

    VfsRouter single;
    check(!single.add("", &fat), "router rejects an empty volume name");
    check(!single.add("fs", &fat), "router rejects a reserved volume name");
    check(single.add("fat", &fat), "router accepts a volume name");
    single.finalize();
    check(single.single_volume(), "one volume aliases the share root");

    esphome::webdav_filing::VfsStat st{};
    check(single.stat("fat.txt", &st) && st.exists && st.size == fat_data.size(),
          "single-volume router stats a file at the root");
    std::vector<esphome::webdav_filing::VfsDirEntry> entries;
    check(single.read_dir("", &entries) && entries.size() == 1 && entries[0].name == "fat.txt",
          "single-volume router lists the backend root");

    VfsRouter router;
    check(router.add("fat", &fat) && router.add("ext2", &ext2), "router adds two volumes");
    check(!router.add("fat", &fat), "router rejects duplicate volume names");
    router.finalize();
    check(!router.single_volume(), "two volumes expose a virtual root");

    check(router.stat("", &st) && st.exists && st.is_dir, "virtual root is a collection");
    check(router.stat("fat/fat.txt", &st) && st.exists && st.size == fat_data.size(),
          "router stats a named volume file");
    check(router.stat("missing", &st) && !st.exists, "unknown virtual child does not exist");

    entries.clear();
    check(router.read_dir("", &entries) && entries.size() == 2 && entries[0].name == "fat" &&
              entries[1].name == "ext2" && entries[0].is_dir && entries[1].is_dir,
          "virtual root lists both volumes as directories");
    entries.clear();
    check(router.read_dir("ext2", &entries) && entries.size() == 1 &&
              entries[0].name == "ext2.txt",
          "router lists a named volume's root");

    std::string routed(fat_data.size(), '\0');
    size_t read = 0;
    check(router.read_file("fat/fat.txt", 0, reinterpret_cast<uint8_t *>(routed.data()),
                           routed.size(), &read) &&
              routed == fat_data,
          "router reads from the selected volume");

    const std::string added = "root-write";
    check(!router.write_file("new.txt", reinterpret_cast<const uint8_t *>(added.data()),
                             added.size()),
          "router rejects writes at the virtual root");
    check(!router.mkdir("newdir"), "router rejects mkdir at the virtual root");
    check(!router.remove("fat"), "router rejects removal of a mounted volume");
    check(!router.rename("fat/fat.txt", "ext2/fat.txt"),
          "router rejects cross-volume renames");
    check(router.write_file("fat/new.txt", reinterpret_cast<const uint8_t *>(added.data()),
                            added.size()) && fat.has("new.txt"),
          "router writes inside a named volume");
  }

  {
    std::cout << "host tests: base_path and virtual root HTTP\n";
    MemoryVfs prefixed;
    TestWebDAVFiling dav(8080, 18, 19, 23, 5, 4000000);
    dav.set_base_path("/dav/");
    dav.set_vfs(&prefixed);
    dav.setup();
    check_eq(esphome::webdav_filing::WebDAVFiling::normalize_base_path("/dav/"), "/dav",
             "base path removes a trailing slash");
    check_eq(esphome::webdav_filing::WebDAVFiling::normalize_base_path("../dav"), "/",
             "base path rejects traversal");

    Request put(HTTP_PUT, "/dav/from-base.txt", "prefixed");
    check(call(dav, put) == ESP_OK && g_resp.status == "201 Created" &&
              prefixed.has("from-base.txt"),
          "PUT works under base_path");

    Request get(HTTP_GET, "/dav/from-base.txt");
    check(call(dav, get) == ESP_OK && g_resp.body == "prefixed",
          "GET works under base_path");

    Request outside(HTTP_GET, "/from-base.txt");
    check(call(dav, outside) == ESP_OK && g_resp.status == "400 Bad Request",
          "paths outside base_path are not published");

    Request index(HTTP_GET, "/dav/");
    check(call(dav, index) == ESP_OK && contains(g_resp.body, "fetch('/dav/list')"),
          "root HTML uses the configured prefix");

    Request propfind(HTTP_PROPFIND, "/dav/");
    propfind.header("Depth", "1");
    call(dav, propfind);
    check(contains(g_resp.body, "<D:href>/dav/</D:href>") &&
              contains(g_resp.body, "<D:href>/dav/from-base.txt</D:href>"),
          "PROPFIND hrefs include base_path");

    Request copy(HTTP_COPY, "/dav/from-base.txt");
    copy.header("Destination", "/dav/copied.txt");
    call(dav, copy);
    check(g_resp.status == "201 Created" && prefixed.has("copied.txt"),
          "COPY Destination is interpreted under base_path");

    MemoryVfs left;
    MemoryVfs right;
    const std::string left_data = "left";
    left.write_file("left.txt", reinterpret_cast<const uint8_t *>(left_data.data()),
                    left_data.size());
    VfsRouter router;
    check(router.add("disk-a", &left) && router.add("disk-b", &right),
          "HTTP router fixture adds two volumes");
    router.finalize();

    TestWebDAVFiling dav_root(8080, 18, 19, 23, 5, 4000000);
    dav_root.set_vfs(&router);
    dav_root.setup();

    Request root_propfind(HTTP_PROPFIND, "/");
    root_propfind.header("Depth", "1");
    call(dav_root, root_propfind);
    check(contains(g_resp.body, "<D:href>/disk-a</D:href>") &&
              contains(g_resp.body, "<D:href>/disk-b</D:href>"),
          "PROPFIND of virtual root lists volume directories");

    Request volume_get(HTTP_GET, "/disk-a/left.txt");
    check(call(dav_root, volume_get) == ESP_OK && g_resp.body == "left",
          "GET routes into a named volume");

    Request root_put(HTTP_PUT, "/blocked.txt", "no");
    check(call(dav_root, root_put) == ESP_OK && g_resp.status == "403 Forbidden",
          "PUT at virtual root is refused safely");

    Request volume_put(HTTP_PUT, "/disk-a/inside.txt", "inside");
    check(call(dav_root, volume_put) == ESP_OK && g_resp.status == "201 Created" &&
              left.has("inside.txt"),
          "PUT stages a temporary file inside the named volume");

    Request root_delete(HTTP_DELETE, "/disk-a");
    check(call(dav_root, root_delete) == ESP_OK && g_resp.status == "403 Forbidden",
          "DELETE cannot remove a mounted volume");
  }

  {
    check_eq(TestWebDAVFiling::md5_hex_(""), "d41d8cd98f00b204e9800998ecf8427e",
             "MD5 matches the empty-string test vector");
    check_eq(TestWebDAVFiling::md5_hex_("abc"), "900150983cd24fb0d6963f7d28e17f72",
             "MD5 matches the abc test vector");

    TestWebDAVFiling basic_dav(8080, 18, 19, 23, 5, 4000000);
    basic_dav.set_vfs(&vfs);
    basic_dav.set_auth("davuser", "davpass", false);
    basic_dav.setup();

    Request basic_unauth(HTTP_GET, "/hello.txt");
    check(call(basic_dav, basic_unauth) == ESP_OK && g_resp.status == "401 Unauthorized",
          "Basic auth rejects a request without credentials");
    check(contains(g_resp.headers["www-authenticate"], "Basic realm=\"webdav_filing\""),
          "Basic auth sends a WWW-Authenticate challenge");

    Request basic_wrong(HTTP_GET, "/hello.txt");
    basic_wrong.header("Authorization", "Basic ZGF2dXNlcjpidWZmZXI=");
    check(call(basic_dav, basic_wrong) == ESP_OK && g_resp.status == "401 Unauthorized",
          "Basic auth rejects a wrong password");

    Request basic_ok(HTTP_GET, "/hello.txt");
    basic_ok.header("Authorization", "Basic ZGF2dXNlcjpkYXZwYXNz");
    check(call(basic_dav, basic_ok) == ESP_OK && g_resp.status == "200 OK" &&
              g_resp.body == vfs.data("hello.txt"),
          "Basic auth serves the file with correct credentials");

    // Credentials whose length is 1 mod 3 end in "==" after Base64 encoding;
    // rejecting the second padding character made this request fail with 401.
    TestWebDAVFiling double_padded_dav(8080, 18, 19, 23, 5, 4000000);
    double_padded_dav.set_vfs(&vfs);
    double_padded_dav.set_auth("mikael", "secret", false);
    double_padded_dav.setup();
    Request double_padded(HTTP_GET, "/hello.txt");
    double_padded.header("Authorization", "Basic bWlrYWVsOnNlY3JldA==");
    check(call(double_padded_dav, double_padded) == ESP_OK && g_resp.status == "200 OK" &&
              g_resp.body == vfs.data("hello.txt"),
          "Basic auth accepts credentials with double Base64 padding");

    TestWebDAVFiling digest_dav(8080, 18, 19, 23, 5, 4000000);
    digest_dav.set_vfs(&vfs);
    digest_dav.set_auth("davuser", "davpass", true);
    digest_dav.setup();

    Request digest_unauth(HTTP_GET, "/hello.txt");
    check(call(digest_dav, digest_unauth) == ESP_OK && g_resp.status == "401 Unauthorized",
          "Digest auth rejects a request without credentials");
    check(contains(g_resp.headers["www-authenticate"], "Digest realm=\"webdav_filing\"") &&
              contains(g_resp.headers["www-authenticate"], "nonce=\"testnonce\"") &&
              contains(g_resp.headers["www-authenticate"], "qop=\"auth\""),
          "Digest auth sends a complete WWW-Authenticate challenge");

    Request digest_basic(HTTP_GET, "/hello.txt");
    digest_basic.header("Authorization", "Basic ZGF2dXNlcjpkYXZwYXNz");
    check(call(digest_dav, digest_basic) == ESP_OK && g_resp.status == "401 Unauthorized",
          "Digest auth does not accept a Basic downgrade");

    Request digest_stale(HTTP_GET, "/hello.txt");
    digest_stale.header("Authorization", digest_header("GET", "/hello.txt", "wrongnonce"));
    check(call(digest_dav, digest_stale) == ESP_OK && g_resp.status == "401 Unauthorized" &&
              contains(g_resp.headers["www-authenticate"], "stale=true"),
          "Digest auth marks an unknown nonce stale");

    Request digest_ok(HTTP_GET, "/hello.txt");
    digest_ok.header("Authorization", digest_header("GET", "/hello.txt", "testnonce"));
    check(call(digest_dav, digest_ok) == ESP_OK && g_resp.status == "200 OK" &&
              g_resp.body == vfs.data("hello.txt"),
          "Digest auth serves the file with a valid response");

    Request digest_wrong_uri(HTTP_GET, "/hello.txt");
    digest_wrong_uri.header("Authorization", digest_header("GET", "/other.txt", "testnonce"));
    check(call(digest_dav, digest_wrong_uri) == ESP_OK && g_resp.status == "401 Unauthorized",
          "Digest auth binds the response to the request URI");
  }

  std::cout << (g_failures == 0 ? "ALL TESTS PASSED\n" : "TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
