#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace vts_rtc {
namespace vision {

constexpr const char* kVideoViewControlChannelLabel = "video.view_control.v1";

constexpr uint32_t kVideoViewControlMagic = 0x31435656u;
constexpr uint16_t kVideoViewControlProtocolMajor = 1;
constexpr uint16_t kVideoViewControlProtocolMinor = 0;

enum class ViewMode : uint32_t {
  Unknown = 0,
  Binocular = 1,
  Surround = 2,
};

enum class ViewControlDecodeStatus {
  Ok,
  EmptyPayload,
  InvalidSize,
  InvalidEnvelope,
  UnsupportedProtocol,
  InvalidField,
};

struct ViewControlCommand {
  ViewMode enable_view = ViewMode::Unknown;
};

struct ViewControlEnvelope {
  uint64_t seq = 0;
  ViewControlCommand command;
};

struct ViewControlEncodeResult {
  std::vector<uint8_t> payload;
  std::string error_message;

  explicit operator bool() const { return error_message.empty(); }
};

struct ViewControlDecodeResult {
  ViewControlDecodeStatus status = ViewControlDecodeStatus::InvalidEnvelope;
  ViewControlEnvelope envelope;
  std::string error_message;

  explicit operator bool() const {
    return status == ViewControlDecodeStatus::Ok;
  }
};

ViewControlEncodeResult EncodeViewControl(uint64_t seq, ViewMode enable_view);
ViewControlDecodeResult DecodeViewControl(const uint8_t* data, size_t size);
const char* ViewModeText(ViewMode mode);

inline ViewControlDecodeResult DecodeViewControl(
    const std::vector<uint8_t>& payload) {
  return DecodeViewControl(payload.data(), payload.size());
}

}  // namespace vision
}  // namespace vts_rtc
