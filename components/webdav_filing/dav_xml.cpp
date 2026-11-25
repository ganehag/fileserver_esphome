// SPDX-License-Identifier: MIT
#include "dav_xml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace esphome {
namespace webdav_filing {

namespace {

constexpr int kMaxDepth = 24;

std::string trim(const std::string &s) {
  size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

void skip_space(const std::string &in, size_t *pos) {
  while (*pos < in.size() && std::isspace(static_cast<unsigned char>(in[*pos])))
    (*pos)++;
}

size_t find_tag_end(const std::string &in, size_t start) {
  char quote = 0;
  for (size_t i = start; i < in.size(); i++) {
    char c = in[i];
    if (quote != 0) {
      if (c == quote)
        quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i;
    }
  }
  return std::string::npos;
}

std::string parse_name(const std::string &in, size_t *pos) {
  size_t start = *pos;
  while (*pos < in.size()) {
    unsigned char c = static_cast<unsigned char>(in[*pos]);
    if (std::isspace(c) || c == '=' || c == '/' || c == '>')
      break;
    (*pos)++;
  }
  return in.substr(start, *pos - start);
}

class Parser {
 public:
  Parser(const std::string &input) : in_(input) {}

  bool parse(XmlNode *root) {
    skip_misc();
    if (pos_ >= in_.size() || in_[pos_] != '<')
      return false;
    return parse_element(nullptr, {}, 0, root);
  }

 private:
  void skip_misc() {
    for (;;) {
      skip_space(in_, &pos_);
      if (in_.compare(pos_, 2, "<?") == 0) {
        size_t end = in_.find("?>", pos_ + 2);
        pos_ = end == std::string::npos ? in_.size() : end + 2;
      } else if (in_.compare(pos_, 4, "<!--") == 0) {
        size_t end = in_.find("-->", pos_ + 4);
        pos_ = end == std::string::npos ? in_.size() : end + 3;
      } else if (in_.compare(pos_, 9, "<!DOCTYPE") == 0 || in_.compare(pos_, 9, "<!doctype") == 0) {
        size_t end = in_.find('>', pos_ + 9);
        pos_ = end == std::string::npos ? in_.size() : end + 1;
      } else {
        return;
      }
    }
  }

  bool parse_element(const XmlNode *parent, const std::map<std::string, std::string> &inherited,
                     int depth, XmlNode *node) {
    if (depth > kMaxDepth || pos_ >= in_.size() || in_[pos_] != '<')
      return false;

    const size_t start = pos_;
    size_t p = ++pos_;
    const std::string name = parse_name(in_, &p);
    if (name.empty())
      return false;
    node->name = name;
    size_t colon = name.find(':');
    node->prefix = colon == std::string::npos ? "" : name.substr(0, colon);
    node->local = colon == std::string::npos ? name : name.substr(colon + 1);

    (void) parent;
    std::map<std::string, std::string> ns = inherited;

    for (;;) {
      skip_space(in_, &p);
      if (p >= in_.size())
        return false;
      if (in_[p] == '/' && p + 1 < in_.size() && in_[p + 1] == '>') {
        node->self_closing = true;
        pos_ = p + 2;
        node->raw = in_.substr(start, pos_ - start);
        break;
      }
      if (in_[p] == '>') {
        pos_ = p + 1;
        break;
      }
      std::string attr = parse_name(in_, &p);
      if (attr.empty())
        return false;
      skip_space(in_, &p);
      if (p >= in_.size() || in_[p] != '=')
        return false;
      p++;
      skip_space(in_, &p);
      if (p >= in_.size() || (in_[p] != '"' && in_[p] != '\''))
        return false;
      char quote = in_[p++];
      size_t value_start = p;
      while (p < in_.size() && in_[p] != quote)
        p++;
      if (p >= in_.size())
        return false;
      node->attrs[attr] = xml_unescape(in_.substr(value_start, p - value_start));
      p++;
    }

    for (const auto &item : node->attrs) {
      if (item.first == "xmlns")
        ns[""] = item.second;
      else if (item.first.rfind("xmlns:", 0) == 0)
        ns[item.first.substr(6)] = item.second;
    }
    auto found = ns.find(node->prefix);
    if (found != ns.end())
      node->uri = found->second;

    if (node->self_closing)
      return true;

    for (;;) {
      if (pos_ >= in_.size())
        return false;
      if (in_.compare(pos_, 2, "</") == 0) {
        size_t end = find_tag_end(in_, pos_ + 2);
        if (end == std::string::npos)
          return false;
        std::string closing = trim(in_.substr(pos_ + 2, end - (pos_ + 2)));
        if (closing != name)
          return false;
        pos_ = end + 1;
        node->raw = in_.substr(start, pos_ - start);
        return true;
      }
      if (in_.compare(pos_, 4, "<!--") == 0) {
        size_t end = in_.find("-->", pos_ + 4);
        pos_ = end == std::string::npos ? in_.size() : end + 3;
      } else if (in_.compare(pos_, 2, "<?") == 0) {
        size_t end = in_.find("?>", pos_ + 2);
        pos_ = end == std::string::npos ? in_.size() : end + 2;
      } else if (in_.compare(pos_, 9, "<![CDATA[") == 0) {
        size_t end = in_.find("]]>", pos_ + 9);
        if (end == std::string::npos)
          return false;
        node->text.append(in_, pos_ + 9, end - (pos_ + 9));
        pos_ = end + 3;
      } else if (in_[pos_] == '<') {
        XmlNode child;
        if (!parse_element(node, ns, depth + 1, &child))  // NOLINT(readability-simplify-boolean-expr)
          return false;
        node->children.push_back(std::move(child));
      } else {
        size_t end = in_.find('<', pos_);
        if (end == std::string::npos)
          return false;
        node->text += xml_unescape(in_.substr(pos_, end - pos_));
        pos_ = end;
      }
    }
  }

  const std::string &in_;
  size_t pos_{0};
};

}  // namespace

std::string xml_unescape(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] != '&') {
      out.push_back(in[i]);
      continue;
    }
    size_t semi = in.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
      out.push_back(in[i]);
      continue;
    }
    std::string entity = in.substr(i + 1, semi - i - 1);
    if (entity == "amp")
      out.push_back('&');
    else if (entity == "lt")
      out.push_back('<');
    else if (entity == "gt")
      out.push_back('>');
    else if (entity == "quot")
      out.push_back('"');
    else if (entity == "apos")
      out.push_back('\'');
    else if (!entity.empty() && entity[0] == '#') {
      unsigned long code = 0;
      if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X'))
        code = std::strtoul(entity.c_str() + 2, nullptr, 16);
      else
        code = std::strtoul(entity.c_str() + 1, nullptr, 10);
      if (code > 0 && code <= 0x10FFFF) {
        if (code < 0x80) {
          out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
          out.push_back(static_cast<char>(0xC0 | (code >> 6)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
          out.push_back(static_cast<char>(0xE0 | (code >> 12)));
          out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
          out.push_back(static_cast<char>(0xF0 | (code >> 18)));
          out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
      }
    } else {
      out.push_back('&');
      out.append(entity);
      out.push_back(';');
    }
    i = semi;
  }
  return out;
}

std::string xml_escape(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(static_cast<char>(c)); break;
    }
  }
  return out;
}

bool parse_xml(const std::string &input, XmlNode *root) {
  Parser parser(input);
  return parser.parse(root);
}

const XmlNode *first_child(const XmlNode &node, const std::string &uri, const std::string &local) {
  for (const auto &child : node.children) {
    if (child.local == local && (uri.empty() || child.uri == uri))
      return &child;
  }
  return nullptr;
}

std::vector<const XmlNode *> children(const XmlNode &node, const std::string &uri,
                                      const std::string &local) {
  std::vector<const XmlNode *> out;
  for (const auto &child : node.children) {
    if (child.local == local && (uri.empty() || child.uri == uri))
      out.push_back(&child);
  }
  return out;
}

}  // namespace webdav_filing
}  // namespace esphome
