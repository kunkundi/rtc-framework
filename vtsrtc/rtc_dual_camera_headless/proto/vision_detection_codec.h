#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace vts_rtc {
namespace vision {

constexpr const char* kVisionDetectionChannelLabel = "vision.detect.v1";
constexpr uint32_t kVisionDetectionMagic = 0x31544456u;
constexpr uint32_t kVisionProtocolMajor = 1;
constexpr uint32_t kVisionProtocolMinor = 0;

constexpr size_t kMaxClassesPerMap = 256;
constexpr size_t kMaxDetectionsPerFrame = 64;
constexpr size_t kMaxBoundedStringLength = 31;

enum class MessageType : uint32_t {
  Unknown = 0,
  Capability = 1,
  ClassMap = 2,
  FrameDetections = 3,
};

enum class CoordType : uint32_t {
  Unknown = 0,
  NormU16Xywh = 1,
  NormU16Xyxy = 2,
  PixelI32Xywh = 3,
};

enum class DecodeStatus {
  Ok,
  EmptyPayload,
  DecodeFailed,
  InvalidEnvelope,
  UnsupportedProtocol,
  UnexpectedPayload,
};

struct Capability {
  uint32_t max_detections_per_frame = 0;
  uint32_t supported_coord_types = 0;
  uint32_t max_source_id_size = 0;
  uint32_t max_label_size = 0;
};

struct ClassInfo {
  uint32_t class_id = 0;
  std::string label;
  bool has_color_argb = false;
  uint32_t color_argb = 0;
};

struct ClassMap {
  uint32_t map_version = 0;
  std::string model_name;
  std::string model_version;
  std::vector<ClassInfo> classes;
};

struct Detection {
  uint32_t class_id = 0;
  uint32_t confidence = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t w = 0;
  uint32_t h = 0;
  bool has_detection_id = false;
  uint32_t detection_id = 0;
  bool has_track_id = false;
  uint32_t track_id = 0;
  uint32_t flags = 0;
};

struct FrameDetections {
  std::string source_id;
  uint64_t frame_id = 0;
  uint64_t capture_ts_ms = 0;
  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  uint32_t class_map_version = 0;
  CoordType coord_type = CoordType::NormU16Xywh;
  std::vector<Detection> detections;
};

struct Envelope {
  uint32_t seq = 0;
  MessageType type = MessageType::Unknown;
  Capability capability;
  ClassMap class_map;
  FrameDetections frame_detections;
};

struct EncodeResult {
  std::vector<uint8_t> payload;
  std::string error_message;

  explicit operator bool() const { return error_message.empty(); }
};

struct DecodeResult {
  DecodeStatus status = DecodeStatus::DecodeFailed;
  Envelope envelope;
  std::string error_message;

  explicit operator bool() const { return status == DecodeStatus::Ok; }
};

inline uint32_t CoordTypeMask(CoordType type) {
  return 1u << static_cast<uint32_t>(type);
}

Capability DefaultCapability();

EncodeResult EncodeCapabilityEnvelope(uint32_t seq,
                                      const Capability& capability);
EncodeResult EncodeClassMapEnvelope(uint32_t seq, const ClassMap& class_map);
EncodeResult EncodeFrameDetectionsEnvelope(
    uint32_t seq,
    const FrameDetections& frame_detections);

DecodeResult DecodeEnvelope(const uint8_t* data, size_t size);

inline DecodeResult DecodeEnvelope(const std::vector<uint8_t>& payload) {
  return DecodeEnvelope(payload.data(), payload.size());
}

}  // namespace vision
}  // namespace vts_rtc
