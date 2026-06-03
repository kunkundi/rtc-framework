#include "vision_detection_sender.h"

#include "c_rtc.h"
#include "rtc_camera_common.h"
#include "vision_detection_codec.h"

#include <atomic>
#include <chrono>
#include <sstream>

namespace rtc_camera_headless {
namespace {

constexpr uint32_t kClassMapVersion = 1;
constexpr const char* kSourceId = "merged_image";

std::atomic<uint32_t> g_vision_seq{1};
std::atomic<uint64_t> g_detection_frame_id{1};

uint32_t NextVisionSeq() {
  return g_vision_seq.fetch_add(1, std::memory_order_relaxed);
}

uint64_t NowMs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool BroadcastVisionPayload(const char* action,
                            const vts_rtc::vision::EncodeResult& encoded) {
  if (!encoded) {
    LogError(std::string(action) + " encode failed: " +
             encoded.error_message);
    return false;
  }

  const RtcErrorCode code = RtcBroadcastData(
      vts_rtc::vision::kVisionDetectionChannelLabel,
      reinterpret_cast<const char*>(encoded.payload.data()),
      encoded.payload.size());
  if (code != RtcErrorCode::OK) {
    static bool logged_send_failure = false;
    if (!logged_send_failure) {
      logged_send_failure = true;
      std::ostringstream oss;
      oss << action << " broadcast failed, code=" << static_cast<int>(code)
          << ". Make sure the RTC session has opened data channel "
          << vts_rtc::vision::kVisionDetectionChannelLabel;
      LogError(oss.str());
    }
    return false;
  }

  return true;
}

bool SendVisionMetadata() {
  const vts_rtc::vision::Capability capability =
      vts_rtc::vision::DefaultCapability();
  if (!BroadcastVisionPayload(
          "vision capability",
          vts_rtc::vision::EncodeCapabilityEnvelope(NextVisionSeq(),
                                                    capability))) {
    return false;
  }

  vts_rtc::vision::ClassMap class_map;
  class_map.map_version = kClassMapVersion;
  class_map.model_name = "yolo";

  return BroadcastVisionPayload(
      "vision class map",
      vts_rtc::vision::EncodeClassMapEnvelope(NextVisionSeq(), class_map));
}

vts_rtc::vision::Detection ConvertYoloBox(const YoloDetectionBox& box) {
  vts_rtc::vision::Detection detection;
  detection.class_id = box.class_id;
  detection.confidence = box.confidence;
  detection.x = box.x;
  detection.y = box.y;
  detection.w = box.w;
  detection.h = box.h;
  detection.has_detection_id = box.has_detection_id;
  detection.detection_id = box.detection_id;
  detection.has_track_id = box.has_track_id;
  detection.track_id = box.track_id;
  detection.flags = box.flags;
  return detection;
}

vts_rtc::vision::FrameDetections BuildYoloFrameDetections(
    const std::vector<YoloDetectionBox>& yolo_boxes) {
  vts_rtc::vision::FrameDetections frame_detections;
  frame_detections.source_id = kSourceId;
  frame_detections.frame_id =
      g_detection_frame_id.fetch_add(1, std::memory_order_relaxed);
  frame_detections.capture_ts_ms = NowMs();
  frame_detections.class_map_version = kClassMapVersion;
  frame_detections.coord_type = vts_rtc::vision::CoordType::NormU16Xywh;
  frame_detections.detections.reserve(yolo_boxes.size());
  for (const YoloDetectionBox& box : yolo_boxes) {
    frame_detections.detections.push_back(ConvertYoloBox(box));
  }
  return frame_detections;
}

}  // namespace

void SendYoloDetections(const std::vector<YoloDetectionBox>& yolo_boxes) {
  static bool metadata_sent = false;
  if (!metadata_sent) {
    metadata_sent = SendVisionMetadata();
    if (!metadata_sent) {
      return;
    }
  }

  const vts_rtc::vision::FrameDetections frame_detections =
      BuildYoloFrameDetections(yolo_boxes);
  BroadcastVisionPayload(
      "vision frame detections",
      vts_rtc::vision::EncodeFrameDetectionsEnvelope(NextVisionSeq(),
                                                     frame_detections));
}

}  // namespace rtc_camera_headless
