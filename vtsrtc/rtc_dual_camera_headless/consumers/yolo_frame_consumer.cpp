#include "yolo_frame_consumer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <stdint.h>
#include <sstream>
#include <string>
#include <vector>

#include "dual_uyvy_frame_converter.h"
#include "rtc_camera_common.h"
#include "stereo_detection_fuser.h"
#include "vision_detection_sender.h"
#ifdef VTSRTC_ENABLE_YOLO_TENSORRT
#include "yolo_onnx_detector.h"
#endif

namespace rtc_camera_headless {
namespace {

constexpr bool kSendYoloDetections = true;
constexpr size_t kMaxProcessingDownscale = 8;
constexpr int kVisionStatsIntervalSeconds = 2;
constexpr float kNormMax = 65535.0f;
constexpr float kTrackMatchIouThreshold = 0.16f;
constexpr float kTrackMatchCenterThreshold = 0.10f;
constexpr float kTrackPositionAlpha = 0.18f;
constexpr float kTrackFastPositionAlpha = 0.48f;
constexpr float kTrackSizeAlpha = 0.10f;
constexpr float kStereoPairPositionAlpha = 0.07f;
constexpr float kStereoPairFastPositionAlpha = 0.42f;
constexpr float kStereoPairSizeAlpha = 0.04f;
constexpr float kStereoPairDisparityAlpha = 0.06f;
constexpr float kStereoPairFastDisparityAlpha = 0.24f;
constexpr float kTrackCenterDeadbandNorm = 180.0f;
constexpr float kTrackSizeDeadbandNorm = 220.0f;
constexpr float kStereoPairCenterDeadbandNorm = 180.0f;
constexpr float kStereoPairYDeadbandNorm = 260.0f;
constexpr float kStereoPairSizeDeadbandNorm = 220.0f;
constexpr float kStereoPairDisparityDeadbandNorm = 180.0f;
constexpr int kTrackHoldFrames = 2;
constexpr int kTrackMaxMissedFrames = 6;
constexpr size_t kMaxStabilizedDetections = 64;

struct VisionWorkFrame {
  ConvertedI420Frame frame;
  std::vector<uint8_t> storage;
};

struct VisionPerfStats {
  std::chrono::steady_clock::time_point window_start =
      std::chrono::steady_clock::now();
  uint64_t received_frames = 0;
  uint64_t processed_frames = 0;
  uint64_t sent_detection_frames = 0;
  uint64_t total_boxes = 0;
  double convert_ms = 0.0;
  double downscale_ms = 0.0;
  double yolo_ms = 0.0;
  double fuse_ms = 0.0;
  double stabilize_ms = 0.0;
  double send_ms = 0.0;
  double total_ms = 0.0;
};

struct NormBox {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct DetectionTrack {
  uint32_t id = 0;
  uint32_t class_id = 0;
  uint32_t confidence = 0;
  NormBox box;
  bool left_side = true;
  bool matched = false;
  int missed_frames = 0;
};

struct StereoPairTrack {
  uint32_t id = 0;
  uint32_t class_id = 0;
  uint32_t confidence = 0;
  NormBox left_box;
  NormBox right_box;
  bool matched = false;
  int missed_frames = 0;
};

double MsSince(const std::chrono::steady_clock::time_point& start,
               const std::chrono::steady_clock::time_point& end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Rate(uint64_t count, double seconds) {
  return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
}

double AverageMs(double total_ms, uint64_t count) {
  return count > 0 ? total_ms / static_cast<double>(count) : 0.0;
}

void MaybeLogVisionPerfStats(VisionPerfStats* stats) {
  if (!stats) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const double seconds =
      std::chrono::duration<double>(now - stats->window_start).count();
  if (seconds < kVisionStatsIntervalSeconds) {
    return;
  }

  std::ostringstream oss;
  oss << "YOLO/OpenCV stats: recv_fps="
      << Rate(stats->received_frames, seconds)
      << " processed_fps=" << Rate(stats->processed_frames, seconds)
      << " box_fps=" << Rate(stats->sent_detection_frames, seconds)
      << " avg_boxes=" << AverageMs(static_cast<double>(stats->total_boxes),
                                    stats->sent_detection_frames)
      << " avg_ms total=" << AverageMs(stats->total_ms, stats->processed_frames)
      << " convert=" << AverageMs(stats->convert_ms, stats->processed_frames)
      << " downscale=" << AverageMs(stats->downscale_ms,
                                    stats->processed_frames)
      << " yolo=" << AverageMs(stats->yolo_ms, stats->processed_frames)
      << " fuse=" << AverageMs(stats->fuse_ms, stats->processed_frames)
      << " stabilize=" << AverageMs(stats->stabilize_ms,
                                    stats->processed_frames)
      << " send=" << AverageMs(stats->send_ms, stats->processed_frames);
  LogInfo(oss.str());

  *stats = VisionPerfStats();
  stats->window_start = now;
}

size_t ClampProcessingDownscale(size_t value) {
  return std::max(static_cast<size_t>(1),
                  std::min(value, kMaxProcessingDownscale));
}

float ClampFloat(float value, float low, float high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

uint32_t ClampNormToU16(float value) {
  return static_cast<uint32_t>(
      std::lround(ClampFloat(value, 0.0f, kNormMax)));
}

NormBox ToNormBox(const YoloDetectionBox& box) {
  NormBox result;
  result.x = static_cast<float>(box.x);
  result.y = static_cast<float>(box.y);
  result.w = static_cast<float>(box.w);
  result.h = static_cast<float>(box.h);
  result.x = ClampFloat(result.x, 0.0f, kNormMax);
  result.y = ClampFloat(result.y, 0.0f, kNormMax);
  result.w = ClampFloat(result.w, 1.0f, kNormMax - result.x);
  result.h = ClampFloat(result.h, 1.0f, kNormMax - result.y);
  return result;
}

void ApplyNormBox(YoloDetectionBox* detection, const NormBox& box) {
  if (!detection) {
    return;
  }
  detection->x = ClampNormToU16(box.x);
  detection->y = ClampNormToU16(box.y);
  detection->w = ClampNormToU16(box.w);
  detection->h = ClampNormToU16(box.h);
}

float BoxCenterX(const NormBox& box) {
  return box.x + box.w * 0.5f;
}

float BoxCenterY(const NormBox& box) {
  return box.y + box.h * 0.5f;
}

float NormBoxIoU(const NormBox& a, const NormBox& b) {
  const float x1 = std::max(a.x, b.x);
  const float y1 = std::max(a.y, b.y);
  const float x2 = std::min(a.x + a.w, b.x + b.w);
  const float y2 = std::min(a.y + a.h, b.y + b.h);
  const float intersection =
      std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
  const float area_a = std::max(0.0f, a.w) * std::max(0.0f, a.h);
  const float area_b = std::max(0.0f, b.w) * std::max(0.0f, b.h);
  const float union_area = area_a + area_b - intersection;
  return union_area > 0.0f ? intersection / union_area : 0.0f;
}

float CenterDistanceNorm(const NormBox& a, const NormBox& b) {
  const float dx = (BoxCenterX(a) - BoxCenterX(b)) / kNormMax;
  const float dy = (BoxCenterY(a) - BoxCenterY(b)) / kNormMax;
  return std::sqrt(dx * dx + dy * dy);
}

float SmoothScalarWithDeadband(float previous,
                               float measurement,
                               float alpha,
                               float deadband) {
  const float delta = measurement - previous;
  const float abs_delta = std::fabs(delta);
  if (abs_delta <= deadband) {
    return previous;
  }
  const float adjusted_delta =
      delta > 0.0f ? (delta - deadband) : (delta + deadband);
  return previous + adjusted_delta * alpha;
}

bool IsLeftSide(const NormBox& box, float stereo_boundary_norm) {
  return BoxCenterX(box) < stereo_boundary_norm;
}

NormBox ClampNormBoxToSide(NormBox box,
                           bool left_side,
                           float stereo_boundary_norm) {
  const float x_min = left_side ? 0.0f : stereo_boundary_norm;
  const float x_max = left_side ? stereo_boundary_norm : kNormMax;
  box.w = ClampFloat(box.w, 1.0f, std::max(1.0f, x_max - x_min));
  box.h = ClampFloat(box.h, 1.0f, kNormMax);
  box.x = ClampFloat(box.x, x_min, x_max - box.w);
  box.y = ClampFloat(box.y, 0.0f, kNormMax - box.h);
  return box;
}

NormBox SmoothNormBox(const NormBox& previous,
                      const NormBox& measurement,
                      bool left_side,
                      float stereo_boundary_norm) {
  const float distance = CenterDistanceNorm(previous, measurement);
  const float iou = NormBoxIoU(previous, measurement);
  const float position_alpha =
      (distance > 0.045f || iou < 0.20f) ? kTrackFastPositionAlpha
                                         : kTrackPositionAlpha;
  const float previous_cx = BoxCenterX(previous);
  const float previous_cy = BoxCenterY(previous);
  const float measured_cx = BoxCenterX(measurement);
  const float measured_cy = BoxCenterY(measurement);

  NormBox result;
  const float smoothed_cx = SmoothScalarWithDeadband(
      previous_cx, measured_cx, position_alpha, kTrackCenterDeadbandNorm);
  const float smoothed_cy = SmoothScalarWithDeadband(
      previous_cy, measured_cy, position_alpha, kTrackCenterDeadbandNorm);
  result.w = SmoothScalarWithDeadband(previous.w, measurement.w,
                                      kTrackSizeAlpha,
                                      kTrackSizeDeadbandNorm);
  result.h = SmoothScalarWithDeadband(previous.h, measurement.h,
                                      kTrackSizeAlpha,
                                      kTrackSizeDeadbandNorm);
  result.x = smoothed_cx - result.w * 0.5f;
  result.y = smoothed_cy - result.h * 0.5f;
  return ClampNormBoxToSide(result, left_side, stereo_boundary_norm);
}

bool CurrentDetectionsLookLikeStereoPair(
    const std::vector<YoloDetectionBox>& boxes,
    size_t index,
    float stereo_boundary_norm) {
  if (index + 1 >= boxes.size()) {
    return false;
  }
  const YoloDetectionBox& a = boxes[index];
  const YoloDetectionBox& b = boxes[index + 1];
  if (a.class_id != b.class_id) {
    return false;
  }
  if (a.has_detection_id && b.has_detection_id &&
      (a.detection_id / 2u) != (b.detection_id / 2u)) {
    return false;
  }
  const bool a_left = IsLeftSide(ToNormBox(a), stereo_boundary_norm);
  const bool b_left = IsLeftSide(ToNormBox(b), stereo_boundary_norm);
  return a_left != b_left;
}

float PairMatchScore(const StereoPairTrack& track,
                     const NormBox& left_box,
                     const NormBox& right_box) {
  const float left_iou = NormBoxIoU(track.left_box, left_box);
  const float right_iou = NormBoxIoU(track.right_box, right_box);
  const float left_distance = CenterDistanceNorm(track.left_box, left_box);
  const float right_distance = CenterDistanceNorm(track.right_box, right_box);
  return (left_iou + right_iou) * 0.5f -
         (left_distance + right_distance) * 0.20f;
}

float AveragePairCenterDistance(const StereoPairTrack& track,
                                const NormBox& left_box,
                                const NormBox& right_box) {
  return (CenterDistanceNorm(track.left_box, left_box) +
          CenterDistanceNorm(track.right_box, right_box)) *
         0.5f;
}

float PairDisparity(const NormBox& left_box, const NormBox& right_box) {
  return BoxCenterX(right_box) - BoxCenterX(left_box);
}

void MakePairGeometryConsistent(NormBox* left_box,
                                NormBox* right_box,
                                float stereo_boundary_norm) {
  if (!left_box || !right_box) {
    return;
  }
  const float common_w = (left_box->w + right_box->w) * 0.5f;
  const float common_h = (left_box->h + right_box->h) * 0.5f;
  const float common_y = (left_box->y + right_box->y) * 0.5f;
  left_box->w = common_w;
  right_box->w = common_w;
  left_box->h = common_h;
  right_box->h = common_h;
  left_box->y = common_y;
  right_box->y = common_y;
  *left_box = ClampNormBoxToSide(*left_box, true, stereo_boundary_norm);
  *right_box = ClampNormBoxToSide(*right_box, false, stereo_boundary_norm);
}

void SmoothStereoPairBoxes(const StereoPairTrack& previous,
                           const NormBox& measured_left,
                           const NormBox& measured_right,
                           float stereo_boundary_norm,
                           NormBox* smoothed_left,
                           NormBox* smoothed_right) {
  if (!smoothed_left || !smoothed_right) {
    return;
  }

  const float average_distance =
      AveragePairCenterDistance(previous, measured_left, measured_right);
  const float average_iou =
      (NormBoxIoU(previous.left_box, measured_left) +
       NormBoxIoU(previous.right_box, measured_right)) *
      0.5f;
  const float position_alpha =
      (average_distance > 0.055f || average_iou < 0.16f)
          ? kStereoPairFastPositionAlpha
          : kStereoPairPositionAlpha;

  const float previous_mid_x =
      (BoxCenterX(previous.left_box) + BoxCenterX(previous.right_box)) * 0.5f;
  const float measured_mid_x =
      (BoxCenterX(measured_left) + BoxCenterX(measured_right)) * 0.5f;
  const float previous_y =
      (previous.left_box.y + previous.right_box.y) * 0.5f;
  const float measured_y = (measured_left.y + measured_right.y) * 0.5f;
  const float previous_w =
      (previous.left_box.w + previous.right_box.w) * 0.5f;
  const float measured_w = (measured_left.w + measured_right.w) * 0.5f;
  const float previous_h =
      (previous.left_box.h + previous.right_box.h) * 0.5f;
  const float measured_h = (measured_left.h + measured_right.h) * 0.5f;
  const float previous_disparity =
      PairDisparity(previous.left_box, previous.right_box);
  const float measured_disparity = PairDisparity(measured_left, measured_right);
  const float disparity_delta =
      std::fabs(measured_disparity - previous_disparity) / kNormMax;
  const float disparity_alpha =
      disparity_delta > 0.035f ? kStereoPairFastDisparityAlpha
                               : kStereoPairDisparityAlpha;

  const float mid_x =
      SmoothScalarWithDeadband(previous_mid_x, measured_mid_x, position_alpha,
                               kStereoPairCenterDeadbandNorm);
  const float y = SmoothScalarWithDeadband(
      previous_y, measured_y, position_alpha, kStereoPairYDeadbandNorm);
  const float w = SmoothScalarWithDeadband(
      previous_w, measured_w, kStereoPairSizeAlpha,
      kStereoPairSizeDeadbandNorm);
  const float h = SmoothScalarWithDeadband(
      previous_h, measured_h, kStereoPairSizeAlpha,
      kStereoPairSizeDeadbandNorm);
  const float disparity = SmoothScalarWithDeadband(
      previous_disparity, measured_disparity, disparity_alpha,
      kStereoPairDisparityDeadbandNorm);

  const float left_cx = mid_x - disparity * 0.5f;
  const float right_cx = mid_x + disparity * 0.5f;
  smoothed_left->x = left_cx - w * 0.5f;
  smoothed_left->y = y;
  smoothed_left->w = w;
  smoothed_left->h = h;
  smoothed_right->x = right_cx - w * 0.5f;
  smoothed_right->y = y;
  smoothed_right->w = w;
  smoothed_right->h = h;
  MakePairGeometryConsistent(smoothed_left, smoothed_right,
                             stereo_boundary_norm);
}

class DetectionStabilizer {
 public:
  std::vector<YoloDetectionBox> Stabilize(
      const std::vector<YoloDetectionBox>& detections,
      float stereo_boundary_norm) {
    for (DetectionTrack& track : tracks_) {
      track.matched = false;
    }
    for (StereoPairTrack& track : pair_tracks_) {
      track.matched = false;
    }

    std::vector<YoloDetectionBox> output;
    output.reserve(detections.size() + tracks_.size());

    for (size_t i = 0; i < detections.size();) {
      if (output.size() >= kMaxStabilizedDetections) {
        break;
      }
      if (CurrentDetectionsLookLikeStereoPair(detections, i,
                                              stereo_boundary_norm)) {
        StabilizeStereoPair(detections[i], detections[i + 1],
                            stereo_boundary_norm, &output);
        i += 2;
        continue;
      }

      const YoloDetectionBox& detection = detections[i];
      const NormBox measured_box = ToNormBox(detection);
      const bool left_side = IsLeftSide(measured_box, stereo_boundary_norm);
      const int track_index =
          FindBestTrack(detection, measured_box, left_side);
      if (track_index >= 0) {
        DetectionTrack& track = tracks_[static_cast<size_t>(track_index)];
        track.box = SmoothNormBox(track.box, measured_box, left_side,
                                  stereo_boundary_norm);
        track.confidence = detection.confidence;
        track.left_side = left_side;
        track.matched = true;
        track.missed_frames = 0;
        output.push_back(ToOutputDetection(detection, track));
      } else {
        DetectionTrack track;
        track.id = next_track_id_++;
        track.class_id = detection.class_id;
        track.confidence = detection.confidence;
        track.box = ClampNormBoxToSide(measured_box, left_side,
                                       stereo_boundary_norm);
        track.left_side = left_side;
        track.matched = true;
        tracks_.push_back(track);
        output.push_back(ToOutputDetection(detection, tracks_.back()));
      }
      ++i;
    }

    AppendHeldPairTracks(&output);
    AppendHeldTracks(&output);
    PruneStaleTracks();
    return output;
  }

 private:
  void StabilizeStereoPair(const YoloDetectionBox& first,
                           const YoloDetectionBox& second,
                           float stereo_boundary_norm,
                           std::vector<YoloDetectionBox>* output) {
    if (!output || output->size() + 2 > kMaxStabilizedDetections) {
      return;
    }

    const NormBox first_box = ToNormBox(first);
    const NormBox second_box = ToNormBox(second);
    const bool first_is_left = IsLeftSide(first_box, stereo_boundary_norm);
    const YoloDetectionBox& left_source = first_is_left ? first : second;
    const YoloDetectionBox& right_source = first_is_left ? second : first;
    NormBox measured_left = first_is_left ? first_box : second_box;
    NormBox measured_right = first_is_left ? second_box : first_box;
    MakePairGeometryConsistent(&measured_left, &measured_right,
                               stereo_boundary_norm);

    const int pair_track_index =
        FindBestPairTrack(left_source.class_id, measured_left, measured_right);
    if (pair_track_index >= 0) {
      StereoPairTrack& track =
          pair_tracks_[static_cast<size_t>(pair_track_index)];
      NormBox smoothed_left;
      NormBox smoothed_right;
      SmoothStereoPairBoxes(track, measured_left, measured_right,
                            stereo_boundary_norm, &smoothed_left,
                            &smoothed_right);
      track.left_box = smoothed_left;
      track.right_box = smoothed_right;
      track.confidence =
          std::max(left_source.confidence, right_source.confidence);
      track.matched = true;
      track.missed_frames = 0;
      output->push_back(ToPairOutputDetection(left_source, track, true));
      output->push_back(ToPairOutputDetection(right_source, track, false));
      return;
    }

    StereoPairTrack track;
    track.id = next_track_id_++;
    track.class_id = left_source.class_id;
    track.confidence = std::max(left_source.confidence, right_source.confidence);
    track.left_box = measured_left;
    track.right_box = measured_right;
    track.matched = true;
    pair_tracks_.push_back(track);
    output->push_back(ToPairOutputDetection(left_source, pair_tracks_.back(),
                                            true));
    output->push_back(ToPairOutputDetection(right_source, pair_tracks_.back(),
                                            false));
  }

  int FindBestPairTrack(uint32_t class_id,
                        const NormBox& left_box,
                        const NormBox& right_box) {
    int best_index = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < pair_tracks_.size(); ++i) {
      const StereoPairTrack& track = pair_tracks_[i];
      if (track.matched || track.class_id != class_id) {
        continue;
      }
      const float average_iou =
          (NormBoxIoU(track.left_box, left_box) +
           NormBoxIoU(track.right_box, right_box)) *
          0.5f;
      const float average_distance =
          AveragePairCenterDistance(track, left_box, right_box);
      if (average_iou < kTrackMatchIouThreshold &&
          average_distance > kTrackMatchCenterThreshold) {
        continue;
      }
      const float score = PairMatchScore(track, left_box, right_box);
      if (score > best_score) {
        best_score = score;
        best_index = static_cast<int>(i);
      }
    }
    return best_index;
  }

  int FindBestTrack(const YoloDetectionBox& detection,
                    const NormBox& measured_box,
                    bool left_side) {
    int best_index = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i < tracks_.size(); ++i) {
      const DetectionTrack& track = tracks_[i];
      if (track.matched || track.class_id != detection.class_id ||
          track.left_side != left_side) {
        continue;
      }
      const float iou = NormBoxIoU(track.box, measured_box);
      const float center_distance = CenterDistanceNorm(track.box, measured_box);
      if (iou < kTrackMatchIouThreshold &&
          center_distance > kTrackMatchCenterThreshold) {
        continue;
      }
      const float score = iou - center_distance * 0.35f;
      if (score > best_score) {
        best_score = score;
        best_index = static_cast<int>(i);
      }
    }
    return best_index;
  }

  YoloDetectionBox ToOutputDetection(const YoloDetectionBox& source,
                                     const DetectionTrack& track) const {
    YoloDetectionBox output = source;
    ApplyNormBox(&output, track.box);
    output.has_detection_id = true;
    output.detection_id = track.id * 2u;
    output.has_track_id = true;
    output.track_id = track.id;
    output.confidence = track.confidence;
    return output;
  }

  YoloDetectionBox ToPairOutputDetection(const YoloDetectionBox& source,
                                         const StereoPairTrack& track,
                                         bool left_side) const {
    YoloDetectionBox output = source;
    ApplyNormBox(&output, left_side ? track.left_box : track.right_box);
    output.has_detection_id = true;
    output.detection_id = track.id * 2u + (left_side ? 0u : 1u);
    output.has_track_id = true;
    output.track_id = track.id;
    output.confidence = track.confidence;
    return output;
  }

  void AppendHeldTracks(std::vector<YoloDetectionBox>* output) {
    if (!output) {
      return;
    }
    for (DetectionTrack& track : tracks_) {
      if (track.matched) {
        continue;
      }
      if (output->size() >= kMaxStabilizedDetections) {
        break;
      }
      ++track.missed_frames;
      if (track.missed_frames > kTrackHoldFrames) {
        continue;
      }
      YoloDetectionBox held;
      held.class_id = track.class_id;
      held.confidence =
          static_cast<uint32_t>(std::lround(track.confidence * 0.80f));
      held.has_detection_id = true;
      held.detection_id = track.id * 2u;
      held.has_track_id = true;
      held.track_id = track.id;
      ApplyNormBox(&held, track.box);
      output->push_back(held);
    }
  }

  void AppendHeldPairTracks(std::vector<YoloDetectionBox>* output) {
    if (!output) {
      return;
    }
    for (StereoPairTrack& track : pair_tracks_) {
      if (track.matched) {
        continue;
      }
      if (output->size() + 2 > kMaxStabilizedDetections) {
        break;
      }
      ++track.missed_frames;
      if (track.missed_frames > kTrackHoldFrames) {
        continue;
      }
      YoloDetectionBox left;
      left.class_id = track.class_id;
      left.confidence =
          static_cast<uint32_t>(std::lround(track.confidence * 0.80f));
      left.has_detection_id = true;
      left.detection_id = track.id * 2u;
      left.has_track_id = true;
      left.track_id = track.id;
      ApplyNormBox(&left, track.left_box);
      YoloDetectionBox right = left;
      right.detection_id = track.id * 2u + 1u;
      ApplyNormBox(&right, track.right_box);
      output->push_back(left);
      output->push_back(right);
    }
  }

  void PruneStaleTracks() {
    pair_tracks_.erase(
        std::remove_if(pair_tracks_.begin(), pair_tracks_.end(),
                       [](const StereoPairTrack& track) {
                         return track.missed_frames > kTrackMaxMissedFrames;
                       }),
        pair_tracks_.end());
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
                       [](const DetectionTrack& track) {
                         return track.missed_frames > kTrackMaxMissedFrames;
                       }),
        tracks_.end());
  }

  std::vector<DetectionTrack> tracks_;
  std::vector<StereoPairTrack> pair_tracks_;
  uint32_t next_track_id_ = 1;
};

bool ValidateI420Frame(const ConvertedI420Frame& frame,
                       std::string* error_message) {
  if (!frame.data || frame.width == 0 || frame.height == 0 ||
      frame.stride_y == 0 || frame.stride_u == 0 || frame.stride_v == 0) {
    if (error_message) {
      *error_message = "invalid I420 frame";
    }
    return false;
  }

  const size_t chroma_height = (frame.height + 1) / 2;
  const size_t required_size =
      frame.stride_y * frame.height + frame.stride_u * chroma_height +
      frame.stride_v * chroma_height;
  if (frame.data_size < required_size) {
    if (error_message) {
      *error_message = "I420 frame buffer is smaller than its strides";
    }
    return false;
  }

  return true;
}

void DownscalePlaneBox(const uint8_t* src,
                       size_t src_stride,
                       size_t src_width,
                       size_t src_height,
                       uint8_t* dst,
                       size_t dst_stride,
                       size_t dst_width,
                       size_t dst_height) {
  for (size_t y = 0; y < dst_height; ++y) {
    const size_t src_y0 = y * src_height / dst_height;
    size_t src_y1 = (y + 1) * src_height / dst_height;
    if (src_y1 <= src_y0) {
      src_y1 = std::min(src_height, src_y0 + 1);
    }
    for (size_t x = 0; x < dst_width; ++x) {
      const size_t src_x0 = x * src_width / dst_width;
      size_t src_x1 = (x + 1) * src_width / dst_width;
      if (src_x1 <= src_x0) {
        src_x1 = std::min(src_width, src_x0 + 1);
      }
      uint32_t sum = 0;
      uint32_t count = 0;
      for (size_t src_y = src_y0; src_y < src_y1; ++src_y) {
        const uint8_t* src_row = src + src_y * src_stride;
        for (size_t src_x = src_x0; src_x < src_x1; ++src_x) {
          sum += src_row[src_x];
          ++count;
        }
      }
      dst[y * dst_stride + x] =
          count > 0 ? static_cast<uint8_t>((sum + count / 2) / count) : 0;
    }
  }
}

bool BuildVisionWorkFrame(const ConvertedI420Frame& source,
                          size_t requested_downscale,
                          VisionWorkFrame* output,
                          std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null vision work frame output";
    }
    return false;
  }
  output->frame = ConvertedI420Frame();
  output->storage.clear();

  if (!ValidateI420Frame(source, error_message)) {
    return false;
  }

  const size_t factor = ClampProcessingDownscale(requested_downscale);
  if (factor <= 1) {
    output->frame = source;
    return true;
  }

  const size_t dst_width = (source.width / factor) & ~static_cast<size_t>(1);
  const size_t dst_height = (source.height / factor) & ~static_cast<size_t>(1);
  if (dst_width < 2 || dst_height < 2) {
    if (error_message) {
      *error_message = "vision processing downscale factor is too large";
    }
    return false;
  }

  const size_t src_chroma_width = (source.width + 1) / 2;
  const size_t src_chroma_height = (source.height + 1) / 2;
  const size_t dst_chroma_width = dst_width / 2;
  const size_t dst_chroma_height = dst_height / 2;
  const size_t dst_stride_y = dst_width;
  const size_t dst_stride_u = dst_chroma_width;
  const size_t dst_stride_v = dst_chroma_width;
  const size_t dst_y_bytes = dst_stride_y * dst_height;
  const size_t dst_u_bytes = dst_stride_u * dst_chroma_height;
  const size_t dst_v_bytes = dst_stride_v * dst_chroma_height;

  output->storage.resize(dst_y_bytes + dst_u_bytes + dst_v_bytes);
  uint8_t* dst_y = output->storage.data();
  uint8_t* dst_u = dst_y + dst_y_bytes;
  uint8_t* dst_v = dst_u + dst_u_bytes;

  const size_t src_y_bytes = source.stride_y * source.height;
  const size_t src_u_bytes = source.stride_u * src_chroma_height;
  const uint8_t* src_y = source.data;
  const uint8_t* src_u = src_y + src_y_bytes;
  const uint8_t* src_v = src_u + src_u_bytes;

  DownscalePlaneBox(src_y, source.stride_y, source.width, source.height,
                    dst_y, dst_stride_y, dst_width, dst_height);
  DownscalePlaneBox(src_u, source.stride_u, src_chroma_width,
                    src_chroma_height, dst_u, dst_stride_u, dst_chroma_width,
                    dst_chroma_height);
  DownscalePlaneBox(src_v, source.stride_v, src_chroma_width,
                    src_chroma_height, dst_v, dst_stride_v, dst_chroma_width,
                    dst_chroma_height);

  output->frame.data = output->storage.data();
  output->frame.data_size = output->storage.size();
  output->frame.width = dst_width;
  output->frame.height = dst_height;
  output->frame.stride_y = dst_stride_y;
  output->frame.stride_u = dst_stride_u;
  output->frame.stride_v = dst_stride_v;
  return true;
}

bool BuildUyvyI420Frame(const uint8_t* src,
                        size_t src_size,
                        size_t width,
                        size_t height,
                        size_t stride_bytes,
                        VisionWorkFrame* output,
                        std::string* error_message) {
  if (!output) {
    if (error_message) {
      *error_message = "null UYVY I420 output";
    }
    return false;
  }
  output->frame = ConvertedI420Frame();
  output->storage.clear();

  if (!src || width == 0 || height == 0 || (width % 2) != 0 ||
      stride_bytes < width * 2 || src_size < stride_bytes * height) {
    if (error_message) {
      *error_message = "invalid UYVY frame for monocular YOLO";
    }
    return false;
  }

  const size_t chroma_width = width / 2;
  const size_t chroma_height = (height + 1) / 2;
  const size_t dst_stride_y = width;
  const size_t dst_stride_u = chroma_width;
  const size_t dst_stride_v = chroma_width;
  const size_t dst_y_bytes = dst_stride_y * height;
  const size_t dst_u_bytes = dst_stride_u * chroma_height;
  const size_t dst_v_bytes = dst_stride_v * chroma_height;
  output->storage.resize(dst_y_bytes + dst_u_bytes + dst_v_bytes);

  uint8_t* dst_y = output->storage.data();
  uint8_t* dst_u = dst_y + dst_y_bytes;
  uint8_t* dst_v = dst_u + dst_u_bytes;

  for (size_t y = 0; y < height; ++y) {
    const uint8_t* row = src + y * stride_bytes;
    uint8_t* y_row = dst_y + y * dst_stride_y;
    for (size_t x = 0; x < width; x += 2) {
      const uint8_t* pair = row + x * 2;
      y_row[x] = pair[1];
      y_row[x + 1] = pair[3];
    }
  }

  for (size_t cy = 0; cy < chroma_height; ++cy) {
    const size_t src_y0 = cy * 2;
    const size_t src_y1 = src_y0 + 1;
    const uint8_t* row0 = src + src_y0 * stride_bytes;
    const uint8_t* row1 =
        src_y1 < height ? (src + src_y1 * stride_bytes) : nullptr;
    for (size_t cx = 0; cx < chroma_width; ++cx) {
      const size_t src_x = cx * 2;
      const uint8_t* pair0 = row0 + src_x * 2;
      int u_sum = pair0[0];
      int v_sum = pair0[2];
      int samples = 1;
      if (row1) {
        const uint8_t* pair1 = row1 + src_x * 2;
        u_sum += pair1[0];
        v_sum += pair1[2];
        ++samples;
      }
      dst_u[cy * dst_stride_u + cx] =
          static_cast<uint8_t>((u_sum + samples / 2) / samples);
      dst_v[cy * dst_stride_v + cx] =
          static_cast<uint8_t>((v_sum + samples / 2) / samples);
    }
  }

  output->frame.data = output->storage.data();
  output->frame.data_size = output->storage.size();
  output->frame.width = width;
  output->frame.height = height;
  output->frame.stride_y = dst_stride_y;
  output->frame.stride_u = dst_stride_u;
  output->frame.stride_v = dst_stride_v;
  return true;
}

YoloDetectionBox MapMonoDetectionToStereo(const YoloDetectionBox& box,
                                          size_t mono_x,
                                          size_t mono_width,
                                          size_t stereo_width,
                                          bool right_side) {
  YoloDetectionBox mapped = box;
  const float x =
      static_cast<float>(mono_x) +
      static_cast<float>(box.x) * static_cast<float>(mono_width) / kNormMax;
  const float w =
      static_cast<float>(box.w) * static_cast<float>(mono_width) / kNormMax;
  mapped.x = ClampNormToU16(x * kNormMax / static_cast<float>(stereo_width));
  mapped.w = ClampNormToU16(w * kNormMax / static_cast<float>(stereo_width));
  if (mapped.has_detection_id) {
    mapped.detection_id = mapped.detection_id * 2u + (right_side ? 1u : 0u);
  }
  return mapped;
}

bool RunYoloInference(const rtc_dual_camera::ImageFrame& source_frame,
                      const ConvertedI420Frame& converted_frame,
                      std::vector<YoloDetectionBox>* yolo_boxes);

bool RunYoloInferenceOnOriginalMonoFrames(
    const rtc_dual_camera::ImageFrame& source_frame,
    const ConvertedI420Frame& stereo_frame,
    size_t left_width,
    std::vector<YoloDetectionBox>* yolo_boxes,
    std::string* error_message) {
  if (!yolo_boxes) {
    if (error_message) {
      *error_message = "null original mono YOLO output";
    }
    return false;
  }
  yolo_boxes->clear();
  if (source_frame.format != rtc_dual_camera::ImagePixelFormat::kDualUyvy ||
      !source_frame.left_data || !source_frame.right_data ||
      source_frame.left_width == 0 || source_frame.right_width == 0 ||
      source_frame.left_height == 0 || source_frame.right_height == 0 ||
      left_width == 0 || left_width >= stereo_frame.width) {
    if (error_message) {
      *error_message = "raw dual UYVY frame is unavailable";
    }
    return false;
  }

  VisionWorkFrame left_frame;
  VisionWorkFrame right_frame;
  if (!BuildUyvyI420Frame(source_frame.left_data, source_frame.left_data_size,
                          source_frame.left_width, source_frame.left_height,
                          source_frame.left_stride_bytes, &left_frame,
                          error_message) ||
      !BuildUyvyI420Frame(source_frame.right_data,
                          source_frame.right_data_size,
                          source_frame.right_width, source_frame.right_height,
                          source_frame.right_stride_bytes, &right_frame,
                          error_message)) {
    return false;
  }

  std::vector<YoloDetectionBox> left_boxes;
  std::vector<YoloDetectionBox> right_boxes;
  if (!RunYoloInference(source_frame, left_frame.frame, &left_boxes) ||
      !RunYoloInference(source_frame, right_frame.frame, &right_boxes)) {
    if (error_message) {
      *error_message = "original monocular YOLO inference failed";
    }
    return false;
  }

  const size_t right_width = stereo_frame.width - left_width;
  yolo_boxes->reserve(left_boxes.size() + right_boxes.size());
  for (const YoloDetectionBox& box : left_boxes) {
    yolo_boxes->push_back(MapMonoDetectionToStereo(
        box, 0, left_width, stereo_frame.width, false));
  }
  for (const YoloDetectionBox& box : right_boxes) {
    yolo_boxes->push_back(MapMonoDetectionToStereo(
        box, left_width, right_width, stereo_frame.width, true));
  }
  return true;
}

bool RunYoloInference(const rtc_dual_camera::ImageFrame& source_frame,
                      const ConvertedI420Frame& converted_frame,
                      std::vector<YoloDetectionBox>* yolo_boxes) {
#ifdef VTSRTC_ENABLE_YOLO_TENSORRT
  static bool detector_initialized = false;
  static bool detector_missing_logged = false;
  static bool inference_error_logged = false;
  static std::unique_ptr<YoloOnnxDetector> detector;

  if (!detector_initialized) {
    detector_initialized = true;
    std::string error_message;
    detector = YoloOnnxDetector::CreateDefault(&error_message);
    if (detector) {
      LogInfo(std::string("YOLO TensorRT model loaded: ") +
              detector->model_path());
    } else if (!detector_missing_logged) {
      detector_missing_logged = true;
      LogError(
          "YOLO TensorRT model is unavailable: " + error_message +
          ". Run `xmake yolo_model` or set VTSRTC_YOLO_MODEL to "
          "models/yolo26n.onnx.");
    }
  }

  if (!detector) {
    return false;
  }

  std::string error_message;
  rtc_dual_camera::ImageFrame i420_frame;
  i420_frame.format = rtc_dual_camera::ImagePixelFormat::kI420;
  i420_frame.sequence = source_frame.sequence;
  i420_frame.timestamp_us = source_frame.timestamp_us;
  i420_frame.width = converted_frame.width;
  i420_frame.height = converted_frame.height;
  i420_frame.stride_y = converted_frame.stride_y;
  i420_frame.stride_u = converted_frame.stride_u;
  i420_frame.stride_v = converted_frame.stride_v;
  i420_frame.data = converted_frame.data;
  i420_frame.data_size = converted_frame.data_size;

  if (!detector->Detect(i420_frame, yolo_boxes, &error_message)) {
    if (!inference_error_logged) {
      inference_error_logged = true;
      LogError(std::string("YOLO inference disabled after error: ") +
               error_message);
    }
    detector.reset();
    return false;
  }

  return true;
#else
  static bool yolo_disabled_logged = false;
  (void)source_frame;
  (void)converted_frame;
  if (yolo_boxes) {
    yolo_boxes->clear();
  }
  if (!yolo_disabled_logged) {
    yolo_disabled_logged = true;
    LogInfo("YOLO inference is not enabled; configure with --enable_yolo=true.");
  }
  return false;
#endif
}

size_t StereoLeftWidth(const rtc_dual_camera::ImageFrame& frame,
                       const ConvertedI420Frame& converted_frame) {
  if (frame.format == rtc_dual_camera::ImagePixelFormat::kDualUyvy &&
      frame.left_width > 0 && frame.right_width > 0) {
    const size_t total_sampled = frame.left_width / 2 + frame.right_width / 2;
    if (total_sampled > 0) {
      return converted_frame.width * (frame.left_width / 2) / total_sampled;
    }
  }
  return converted_frame.width / 2;
}

void ProcessYoloFrame(const rtc_dual_camera::ImageFrame& frame,
                      DualUyvyFrameConverter* frame_converter,
                      const YoloFrameConsumerOptions& options,
                      DetectionStabilizer* stabilizer,
                      VisionPerfStats* stats) {
  if (stats) {
    ++stats->received_frames;
  }

  if (!kSendYoloDetections) {
    (void)frame;
    (void)frame_converter;
    (void)options;
    (void)stabilizer;
    (void)stats;
    return;
  }

#ifndef VTSRTC_ENABLE_YOLO_TENSORRT
  std::vector<YoloDetectionBox> unused_boxes;
  RunYoloInference(frame, ConvertedI420Frame(), &unused_boxes);
  (void)frame_converter;
  (void)options;
  (void)stabilizer;
  (void)stats;
  return;
#endif

  const auto total_start = std::chrono::steady_clock::now();
  ConvertedI420Frame converted_frame;
  std::string error_message;
  const auto convert_start = std::chrono::steady_clock::now();
  if (!frame_converter ||
      !frame_converter->ConvertToI420(frame, &converted_frame,
                                      &error_message)) {
    static bool convert_error_logged = false;
    if (!convert_error_logged) {
      convert_error_logged = true;
      LogError(std::string("YOLO frame conversion disabled after error: ") +
               error_message);
    }
    return;
  }
  const auto convert_end = std::chrono::steady_clock::now();

  VisionWorkFrame work_frame;
  const auto downscale_start = convert_end;
  if (!BuildVisionWorkFrame(converted_frame, options.processing_downscale,
                            &work_frame, &error_message)) {
    static bool resize_error_logged = false;
    if (!resize_error_logged) {
      resize_error_logged = true;
      LogError(std::string("YOLO/OpenCV downscale disabled after error: ") +
               error_message);
    }
    return;
  }
  const auto downscale_end = std::chrono::steady_clock::now();

  static bool work_frame_logged = false;
  const size_t left_width = StereoLeftWidth(frame, work_frame.frame);
  if (!work_frame_logged) {
    work_frame_logged = true;
    std::ostringstream oss;
    oss << "YOLO monocular processing raw cameras "
        << frame.left_width << "x" << frame.left_height << " and "
        << frame.right_width << "x" << frame.right_height
        << ", mapped to stereo frame " << work_frame.frame.width << "x"
        << work_frame.frame.height << " from " << converted_frame.width
        << "x" << converted_frame.height << ", display downscale="
        << ClampProcessingDownscale(options.processing_downscale);
    LogInfo(oss.str());
  }

  std::vector<YoloDetectionBox> yolo_boxes;
  const auto yolo_start = downscale_end;
  const bool yolo_ok = RunYoloInferenceOnOriginalMonoFrames(
      frame, work_frame.frame, left_width, &yolo_boxes, &error_message);
  if (!yolo_ok) {
    static bool raw_yolo_error_logged = false;
    if (!raw_yolo_error_logged) {
      raw_yolo_error_logged = true;
      LogError(std::string("YOLO raw monocular inference disabled after "
                           "error: ") +
               error_message);
    }
    return;
  }
  const auto yolo_end = std::chrono::steady_clock::now();

  const auto fuse_start = yolo_end;
  const std::vector<YoloDetectionBox> fused_boxes =
      FuseStereoDetectionsWithLocalMatching(work_frame.frame, left_width,
                                            yolo_boxes);
  const auto fuse_end = std::chrono::steady_clock::now();
  const auto stabilize_start = fuse_end;
  std::vector<YoloDetectionBox> stabilized_boxes = fused_boxes;
  if (stabilizer && work_frame.frame.width > 0) {
    const float stereo_boundary_norm =
        static_cast<float>(left_width) * kNormMax /
        static_cast<float>(work_frame.frame.width);
    stabilized_boxes =
        stabilizer->Stabilize(fused_boxes, stereo_boundary_norm);
  }
  const auto stabilize_end = std::chrono::steady_clock::now();
  const auto send_start = stabilize_end;
  SendYoloDetections(stabilized_boxes,
                     static_cast<uint32_t>(converted_frame.width),
                     static_cast<uint32_t>(converted_frame.height));
  const auto send_end = std::chrono::steady_clock::now();

  if (stats) {
    ++stats->processed_frames;
    ++stats->sent_detection_frames;
    stats->total_boxes += stabilized_boxes.size();
    stats->convert_ms += MsSince(convert_start, convert_end);
    stats->downscale_ms += MsSince(downscale_start, downscale_end);
    stats->yolo_ms += MsSince(yolo_start, yolo_end);
    stats->fuse_ms += MsSince(fuse_start, fuse_end);
    stats->stabilize_ms += MsSince(stabilize_start, stabilize_end);
    stats->send_ms += MsSince(send_start, send_end);
    stats->total_ms += MsSince(total_start, send_end);
  }
}

}  // namespace

YoloFrameConsumer::YoloFrameConsumer(
    const std::shared_ptr<rtc_dual_camera::AsyncImageFrameSubscription>& frames,
    const YoloFrameConsumerOptions& options)
    : frames_(frames), options_(options) {}

YoloFrameConsumer::~YoloFrameConsumer() { Stop(); }

void YoloFrameConsumer::Start() {
  if (thread_.joinable()) {
    return;
  }
  stop_requested_.store(false);
  thread_ = std::thread(&YoloFrameConsumer::Run, this);
}

void YoloFrameConsumer::Stop() {
  stop_requested_.store(true);
  if (frames_) {
    frames_->Close();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

void YoloFrameConsumer::Run() {
  bool first_frame_logged = false;
  DualUyvyFrameConverter frame_converter;
  DetectionStabilizer stabilizer;
  VisionPerfStats stats;
  while (!stop_requested_.load() && !StopRequested()) {
    rtc_dual_camera::ImageFrame frame;
    if (!frames_ || !frames_->WaitNext(&frame, std::chrono::milliseconds(50))) {
      MaybeLogVisionPerfStats(&stats);
      continue;
    }

    if (frame.empty()) {
      MaybeLogVisionPerfStats(&stats);
      continue;
    }

    if (!first_frame_logged) {
      first_frame_logged = true;
      LogInfo("first raw frame received by YOLO consumer");
    }

    ProcessYoloFrame(frame, &frame_converter, options_, &stabilizer, &stats);
    MaybeLogVisionPerfStats(&stats);
  }
}

}  // namespace rtc_camera_headless
