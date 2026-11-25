// SPDX-License-Identifier: MIT
#include "dav_props.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

namespace esphome {
namespace webdav_filing {

namespace {

constexpr size_t kMaxPropsSize = 256 * 1024;
constexpr size_t kWriteChunk = 1024;

bool is_resource_or_child(const std::string &path, const std::string &base) {
  return path == base ||
         (path.size() > base.size() && path.compare(0, base.size(), base) == 0 &&
          path[base.size()] == '/');
}

}  // namespace

std::string property_key(const std::string &uri, const std::string &local) {
  return "{" + uri + "}" + local;
}

void DavPropStore::clear() {
  this->props_.clear();
}

void DavPropStore::set(const std::string &resource, const std::string &key,
                       const std::string &xml) {
  if (key.empty())
    return;
  this->props_[resource][key] = xml;
}

bool DavPropStore::remove(const std::string &resource, const std::string &key) {
  auto resource_it = this->props_.find(resource);
  if (resource_it == this->props_.end())
    return false;
  auto it = resource_it->second.find(key);
  if (it == resource_it->second.end())
    return false;
  resource_it->second.erase(it);
  if (resource_it->second.empty())
    this->props_.erase(resource_it);
  return true;
}

std::vector<DeadProperty> DavPropStore::list(const std::string &resource) const {
  std::vector<DeadProperty> out;
  auto it = this->props_.find(resource);
  if (it == this->props_.end())
    return out;
  for (const auto &item : it->second)
    out.push_back(DeadProperty{item.first, item.second});
  return out;
}

size_t DavPropStore::remove_resource(const std::string &resource) {
  size_t removed = 0;
  auto it = this->props_.lower_bound(resource);
  while (it != this->props_.end() && is_resource_or_child(it->first, resource)) {
    it = this->props_.erase(it);
    ++removed;
  }
  return removed;
}

size_t DavPropStore::rename_resource(const std::string &from, const std::string &to) {
  this->remove_resource(to);
  std::vector<std::pair<std::string, std::map<std::string, std::string>>> moved;
  auto it = this->props_.lower_bound(from);
  while (it != this->props_.end() && is_resource_or_child(it->first, from)) {
    moved.emplace_back(it->first, std::move(it->second));
    it = this->props_.erase(it);
  }
  for (auto &item : moved) {
    const std::string suffix = item.first.substr(from.size());
    this->props_[to + suffix] = std::move(item.second);
  }
  return moved.size();
}

bool DavPropStore::load(Vfs *vfs, const std::string &path) {
  if (vfs == nullptr)
    return false;
  this->clear();

  VfsStat st;
  if (!vfs->stat(path, &st))
    return false;
  if (!st.exists)
    return true;
  if (st.size > kMaxPropsSize)
    return false;

  std::string data;
  data.resize(st.size);
  size_t offset = 0;
  uint8_t buf[kWriteChunk];
  while (offset < st.size) {
    size_t want = std::min(kWriteChunk, st.size - offset);
    size_t got = 0;
    if (!vfs->read_file(path, offset, buf, want, &got) || got == 0)
      return false;
    std::memcpy(&data[offset], buf, got);
    offset += got;
  }

  XmlNode root;
  if (!parse_xml(data, &root) || root.local != "davprops")
    return false;

  for (const auto *resource : children(root, "DAV:", "resource")) {
    auto href = resource->attrs.find("href");
    if (href == resource->attrs.end())
      continue;
    for (const auto *prop : children(*resource, "DAV:", "prop")) {
      auto key = prop->attrs.find("key");
      if (key == prop->attrs.end())
        continue;
      this->set(href->second, key->second, prop->text);
    }
  }
  return true;
}

bool DavPropStore::save(Vfs *vfs, const std::string &path) const {
  if (vfs == nullptr)
    return false;

  std::string data = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<D:davprops xmlns:D=\"DAV:\">\n";
  for (const auto &resource : this->props_) {
    data += "<D:resource href=\"" + xml_escape(resource.first) + "\">";
    for (const auto &prop : resource.second)
      data += "<D:prop key=\"" + xml_escape(prop.first) + "\">" + xml_escape(prop.second) +
              "</D:prop>";
    data += "</D:resource>\n";
  }
  data += "</D:davprops>\n";
  if (data.size() > kMaxPropsSize)
    return false;

  const std::string tmp = path + ".tmp";
  vfs->remove(tmp);
  VfsStat tmp_st;
  if (!vfs->stat(tmp, &tmp_st) || tmp_st.exists)
    return false;
  for (size_t offset = 0; offset < data.size(); offset += kWriteChunk) {
    size_t len = std::min(kWriteChunk, data.size() - offset);
    if (!vfs->write_file(tmp, reinterpret_cast<const uint8_t *>(data.data() + offset), len)) {
      vfs->remove(tmp);
      return false;
    }
  }
  // rename() replaces the old file on every VFS backend, so there is no
  // window where a power cut can leave the property store missing.
  if (!vfs->rename(tmp, path)) {
    vfs->remove(tmp);
    return false;
  }
  return true;
}

}  // namespace webdav_filing
}  // namespace esphome
