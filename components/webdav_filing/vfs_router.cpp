// SPDX-License-Identifier: MIT
#include "vfs_router.h"

#include <algorithm>
#include <cctype>

namespace esphome {
namespace webdav_filing {

bool VfsRouter::valid_name(const std::string &name) {
  if (name.empty() || name.size() > 48 || name[0] == '.')
    return false;
  for (unsigned char c : name) {
    if (!(std::isalnum(c) || c == '-' || c == '_'))
      return false;
  }
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  static const char *const kReserved[] = {"fs", "list", "bench", "davprops"};
  for (const char *reserved : kReserved) {
    if (lower == reserved)
      return false;
  }
  return true;
}

bool VfsRouter::add(const std::string &name, Vfs *vfs) {
  if (vfs == nullptr || !valid_name(name))
    return false;
  for (const auto &mount : mounts_) {
    if (mount.name == name)
      return false;
  }
  mounts_.push_back({name, vfs});
  return true;
}

void VfsRouter::finalize() {
  alias_root_ = mounts_.size() == 1;
}

bool VfsRouter::resolve_(const std::string &path, Vfs **vfs, std::string *relative) const {
  if (alias_root_) {
    *vfs = mounts_.front().vfs;
    *relative = path;
    return true;
  }
  if (path.empty())
    return false;

  const size_t slash = path.find('/');
  const std::string head = slash == std::string::npos ? path : path.substr(0, slash);
  for (const auto &mount : mounts_) {
    if (mount.name != head)
      continue;
    *vfs = mount.vfs;
    *relative = slash == std::string::npos ? "" : path.substr(slash + 1);
    return true;
  }
  return false;
}

bool VfsRouter::is_virtual_collection(const std::string &path) const {
  if (alias_root_)
    return false;
  if (path.empty())
    return true;
  // With a virtual root every top-level name is router-owned: known names are
  // volume collections, unknown names are not writable either. Only paths
  // inside a named volume (<volume>/<file>) belong to a real backend.
  return path.find('/') == std::string::npos;
}

bool VfsRouter::is_hidden(const std::string &path) const {
  Vfs *backend = nullptr;
  std::string relative;
  return this->resolve_(path, &backend, &relative) && backend->is_hidden(relative);
}

bool VfsRouter::stat(const std::string &path, VfsStat *out) {
  if (path.empty() && !alias_root_) {
    *out = VfsStat{};
    out->exists = true;
    out->is_dir = true;
    return true;
  }
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative)) {
    *out = VfsStat{};
    return true;
  }
  return backend->stat(relative, out);
}

bool VfsRouter::read_dir(const std::string &path, std::vector<VfsDirEntry> *out) {
  if (path.empty() && !alias_root_) {
    out->clear();
    for (const auto &mount : mounts_) {
      VfsDirEntry entry;
      entry.name = mount.name;
      entry.is_dir = true;
      out->push_back(entry);
    }
    return true;
  }
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative))
    return false;
  return backend->read_dir(relative, out);
}

bool VfsRouter::read_file(const std::string &path, size_t offset, uint8_t *buf, size_t len,
                          size_t *read) {
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative) || relative.empty())
    return false;
  return backend->read_file(relative, offset, buf, len, read);
}

bool VfsRouter::write_file(const std::string &path, const uint8_t *buf, size_t len) {
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative) || relative.empty())
    return false;
  return backend->write_file(relative, buf, len);
}

bool VfsRouter::mkdir(const std::string &path) {
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative) || relative.empty())
    return false;
  return backend->mkdir(relative);
}

bool VfsRouter::remove(const std::string &path) {
  Vfs *backend = nullptr;
  std::string relative;
  if (!resolve_(path, &backend, &relative) || relative.empty())
    return false;
  return backend->remove(relative);
}

bool VfsRouter::rename(const std::string &from, const std::string &to) {
  Vfs *src = nullptr;
  Vfs *dst = nullptr;
  std::string src_path;
  std::string dst_path;
  if (!resolve_(from, &src, &src_path) || !resolve_(to, &dst, &dst_path))
    return false;
  if (src != dst || src_path.empty() || dst_path.empty())
    return false;
  return src->rename(src_path, dst_path);
}

bool VfsRouter::sync() {
  bool ok = true;
  for (const auto &mount : mounts_)
    ok = mount.vfs->sync() && ok;
  return ok;
}

void VfsRouter::shutdown() {
  for (const auto &mount : mounts_)
    mount.vfs->shutdown();
}

}  // namespace webdav_filing
}  // namespace esphome
