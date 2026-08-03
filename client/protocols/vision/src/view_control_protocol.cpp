#include "rtc_vision/view_control_protocol.h"

#include <stddef.h>

namespace vts_rtc {
namespace vision {
namespace {

constexpr size_t kPayloadSize = 20;

bool IsKnownViewMode(ViewMode mode) {
  return mode == ViewMode::Binocular || mode == ViewMode::Surround;
}

void WriteU16(std::vector<uint8_t>* payload, uint16_t value) {
  payload->push_back(static_cast<uint8_t>(value & 0xffu));
  payload->push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void WriteU32(std::vector<uint8_t>* payload, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    payload->push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
  }
}

void WriteU64(std::vector<uint8_t>* payload, uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    payload->push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
  }
}

uint16_t ReadU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadU32(const uint8_t* data) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(data[i]) << (i * 8);
  }
  return value;
}

uint64_t ReadU64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(data[i]) << (i * 8);
  }
  return value;
}

ViewControlDecodeResult DecodeError(ViewControlDecodeStatus status,
                                    const std::string& error) {
  ViewControlDecodeResult result;
  result.status = status;
  result.error_message = error;
  return result;
}

}  // namespace

ViewControlEncodeResult EncodeViewControl(uint64_t seq, ViewMode enable_view) {
  if (!IsKnownViewMode(enable_view)) {
    return {{}, "enable_view is unknown"};
  }

  ViewControlEncodeResult result;
  result.payload.reserve(kPayloadSize);
  WriteU32(&result.payload, kVideoViewControlMagic);
  WriteU16(&result.payload, kVideoViewControlProtocolMajor);
  WriteU16(&result.payload, kVideoViewControlProtocolMinor);
  WriteU64(&result.payload, seq);
  WriteU32(&result.payload, static_cast<uint32_t>(enable_view));
  return result;
}

ViewControlDecodeResult DecodeViewControl(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return DecodeError(ViewControlDecodeStatus::EmptyPayload, "empty payload");
  }
  if (size != kPayloadSize) {
    return DecodeError(ViewControlDecodeStatus::InvalidSize,
                       "invalid payload size");
  }

  if (ReadU32(data) != kVideoViewControlMagic) {
    return DecodeError(ViewControlDecodeStatus::InvalidEnvelope,
                       "missing or invalid magic");
  }
  const uint16_t protocol_major = ReadU16(data + 4);
  if (protocol_major != kVideoViewControlProtocolMajor) {
    return DecodeError(ViewControlDecodeStatus::UnsupportedProtocol,
                       "unsupported protocol_major");
  }

  const ViewMode enable_view = static_cast<ViewMode>(ReadU32(data + 16));
  if (!IsKnownViewMode(enable_view)) {
    return DecodeError(ViewControlDecodeStatus::InvalidField,
                       "enable_view is unknown");
  }

  ViewControlDecodeResult result;
  result.status = ViewControlDecodeStatus::Ok;
  result.envelope.seq = ReadU64(data + 8);
  result.envelope.command.enable_view = enable_view;
  return result;
}

const char* ViewModeText(ViewMode mode) {
  switch (mode) {
    case ViewMode::Binocular:
      return "binocular";
    case ViewMode::Surround:
      return "surround";
    case ViewMode::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace vision
}  // namespace vts_rtc
