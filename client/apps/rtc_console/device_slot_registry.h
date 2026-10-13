#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace rtc_console {

struct DeviceSlot {
  std::string device_id;
  uint32_t session_id = 0;
  bool conflict = false;
};

class DeviceSlotRegistry {
 public:
  void Configure(const std::array<std::string, 4>& bindings) {
    bindings_ = bindings;
  }

  void Identify(uint32_t session_id, const std::string& device_id) {
    identities_[session_id] = device_id;
  }

  void Disconnect(uint32_t session_id) { identities_.erase(session_id); }
  void Clear() { identities_.clear(); }

  std::string Label(uint32_t session_id) const {
    const auto found = identities_.find(session_id);
    return found == identities_.end()
        ? "session " + std::to_string(session_id) : found->second;
  }

  std::array<DeviceSlot, 4> Snapshot() const {
    std::array<DeviceSlot, 4> slots;
    for (size_t index = 0; index < slots.size(); ++index) {
      slots[index].device_id = bindings_[index];
      if (bindings_[index].empty()) continue;
      for (const auto& identity : identities_) {
        if (identity.second != bindings_[index]) continue;
        if (slots[index].session_id != 0) slots[index].conflict = true;
        slots[index].session_id = identity.first;
      }
      if (slots[index].conflict) slots[index].session_id = 0;
    }
    return slots;
  }

 private:
  std::array<std::string, 4> bindings_;
  std::map<uint32_t, std::string> identities_;
};

}