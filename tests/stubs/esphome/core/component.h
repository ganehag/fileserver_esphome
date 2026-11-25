#pragma once

namespace esphome {
namespace setup_priority {
static constexpr float AFTER_WIFI = 250.0f;
}  // namespace setup_priority

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual void on_shutdown() {}
  virtual float get_setup_priority() const { return 0.0f; }

 protected:
  void mark_failed() { failed_ = true; }

  bool failed_{false};
};

}  // namespace esphome
