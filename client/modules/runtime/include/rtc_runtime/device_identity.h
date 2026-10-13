#pragma once

#include <string>

namespace rtc_runtime {

constexpr const char* kDeviceIdentityChannelLabel = "device.identity.v1";

inline bool IsValidDeviceId(const std::string& device_id) {
  if (device_id.empty() || device_id.size() > 64) {
    return false;
  }
  for (const char character : device_id) {
    if (!((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') ||
          character == '_' || character == '-' || character == '.')) {
      return false;
    }
  }
  return true;
}

}