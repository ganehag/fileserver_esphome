// SPDX-License-Identifier: MIT
#pragma once

#include "vfs.h"

#include <string>
#include <vector>

namespace esphome {
namespace webdav_filing {

// Presents several backend roots as one Vfs. The virtual root is a collection
// containing one directory per mounted volume. With exactly one volume,
// finalize() aliases that volume at the root instead of adding a directory
// level, so a single-FAT or single-ext2 card keeps the original URL layout.
class VfsRouter : public Vfs {
 public:
  struct Mount {
    std::string name;
    Vfs *vfs{nullptr};
  };

  bool add(const std::string &name, Vfs *vfs);
  void finalize();

  const std::vector<Mount> &mounts() const { return mounts_; }
  bool single_volume() const { return mounts_.size() == 1; }
  static bool valid_name(const std::string &name);

  bool stat(const std::string &path, VfsStat *out) override;
  bool is_virtual_collection(const std::string &path) const override;
  bool is_hidden(const std::string &path) const override;
  bool read_dir(const std::string &path, std::vector<VfsDirEntry> *out) override;
  bool read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                 size_t *read) override;
  bool write_file(const std::string &path, const uint8_t *buf, size_t len) override;
  bool mkdir(const std::string &path) override;
  bool remove(const std::string &path) override;
  bool rename(const std::string &from, const std::string &to) override;
  bool sync() override;
  void shutdown() override;

 protected:
  bool resolve_(const std::string &path, Vfs **vfs, std::string *relative) const;

  std::vector<Mount> mounts_;
  bool alias_root_{false};
};

}  // namespace webdav_filing
}  // namespace esphome
