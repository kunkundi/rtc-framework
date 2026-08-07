#include "rtc_vision/stereo_detection_fuser.h"

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <vector>

namespace rtc_camera_headless {

namespace {

constexpr size_t kMaxStereoDetections = 64;
constexpr int kOrbFeatures = 900;
constexpr int kOrbFastThreshold = 12;
constexpr int kMinGoodMatches = 12;
constexpr int kMinGlobalMatches = 8;
constexpr int kMinLocalMatches = 3;
constexpr float kMatchRatio = 0.78f;
constexpr double kRansacReprojThreshold = 2.5;
constexpr float kDuplicateIoUThreshold = 0.55f;
constexpr float kLocalBoxPadding = 0.35f;

struct PixelBox {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct MatchOffset {
  bool valid = false;
  float dx = 0.0f;
  float dy = 0.0f;
};

struct StereoMatch {
  cv::Point2f left;
  cv::Point2f right;
};

struct DetectionCandidate {
  YoloDetectionBox detection;
  PixelBox box;
  bool source_is_left = true;
  bool suppressed = false;
};

template <typename T>
T Clamp(T value, T low, T high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

uint32_t ToNormU16(float value, float maximum) {
  if (maximum <= 0.0f) {
    return 0;
  }
  const float normalized = Clamp(value / maximum, 0.0f, 1.0f);
  return static_cast<uint32_t>(std::lround(normalized * 65535.0f));
}

PixelBox ToPixelBox(const YoloDetectionBox& box, size_t width, size_t height) {
  PixelBox result;
  result.x = static_cast<float>(box.x) * static_cast<float>(width) / 65535.0f;
  result.y = static_cast<float>(box.y) * static_cast<float>(height) / 65535.0f;
  result.w = static_cast<float>(box.w) * static_cast<float>(width) / 65535.0f;
  result.h = static_cast<float>(box.h) * static_cast<float>(height) / 65535.0f;
  result.x = Clamp(result.x, 0.0f, static_cast<float>(width));
  result.y = Clamp(result.y, 0.0f, static_cast<float>(height));
  result.w = Clamp(result.w, 0.0f, static_cast<float>(width) - result.x);
  result.h = Clamp(result.h, 0.0f, static_cast<float>(height) - result.y);
  return result;
}

YoloDetectionBox ToDetectionBox(const YoloDetectionBox& source,
                                const PixelBox& box,
                                size_t width,
                                size_t height,
                                uint32_t detection_id_suffix) {
  YoloDetectionBox result = source;
  result.x = ToNormU16(box.x, static_cast<float>(width));
  result.y = ToNormU16(box.y, static_cast<float>(height));
  result.w = ToNormU16(box.w, static_cast<float>(width));
  result.h = ToNormU16(box.h, static_cast<float>(height));
  result.has_detection_id = true;
  result.detection_id =
      (source.has_detection_id ? source.detection_id : 0) * 2u +
      detection_id_suffix;
  return result;
}

float Median(std::vector<float>* values) {
  if (!values || values->empty()) {
    return 0.0f;
  }
  const size_t middle = values->size() / 2;
  std::nth_element(values->begin(), values->begin() + middle, values->end());
  float median = (*values)[middle];
  if ((values->size() % 2) == 0) {
    std::nth_element(values->begin(), values->begin() + middle - 1,
                     values->end());
    median = (median + (*values)[middle - 1]) * 0.5f;
  }
  return median;
}

bool IsMostlyInLeftHalf(const PixelBox& box, size_t left_width) {
  return box.x + box.w * 0.5f < static_cast<float>(left_width);
}

bool CrossesStereoBoundary(const PixelBox& box, size_t left_width) {
  const float boundary = static_cast<float>(left_width);
  return box.x < boundary && box.x + box.w > boundary;
}

float IoU(const PixelBox& a, const PixelBox& b) {
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

PixelBox ClampBox(PixelBox box,
                  float x_min,
                  float x_max,
                  float y_min,
                  float y_max) {
  box.w = Clamp(box.w, 1.0f, std::max(1.0f, x_max - x_min));
  box.h = Clamp(box.h, 1.0f, std::max(1.0f, y_max - y_min));
  box.x = Clamp(box.x, x_min, x_max - box.w);
  box.y = Clamp(box.y, y_min, y_max - box.h);
  return box;
}

bool PointInBox(const cv::Point2f& point, const PixelBox& box, float padding) {
  const float pad_x = box.w * padding;
  const float pad_y = box.h * padding;
  return point.x >= box.x - pad_x && point.x <= box.x + box.w + pad_x &&
         point.y >= box.y - pad_y && point.y <= box.y + box.h + pad_y;
}

MatchOffset MedianOffset(const std::vector<StereoMatch>& matches,
                         const PixelBox& source_box,
                         bool source_is_left,
                         size_t left_width) {
  MatchOffset result;
  std::vector<float> dx_values;
  std::vector<float> dy_values;
  dx_values.reserve(matches.size());
  dy_values.reserve(matches.size());

  for (const StereoMatch& match : matches) {
    if (source_is_left) {
      if (!PointInBox(match.left, source_box, kLocalBoxPadding)) {
        continue;
      }
      dx_values.push_back(match.right.x + static_cast<float>(left_width) -
                          match.left.x);
      dy_values.push_back(match.right.y - match.left.y);
    } else {
      PixelBox local_box = source_box;
      local_box.x -= static_cast<float>(left_width);
      if (!PointInBox(match.right, local_box, kLocalBoxPadding)) {
        continue;
      }
      dx_values.push_back(match.left.x -
                          (match.right.x + static_cast<float>(left_width)));
      dy_values.push_back(match.left.y - match.right.y);
    }
  }

  if (dx_values.size() < kMinLocalMatches) {
    return result;
  }

  result.valid = true;
  result.dx = Median(&dx_values);
  result.dy = Median(&dy_values);
  return result;
}

MatchOffset GlobalMedianOffset(const std::vector<StereoMatch>& matches,
                               bool source_is_left,
                               size_t left_width) {
  MatchOffset result;
  if (matches.size() < kMinGlobalMatches) {
    return result;
  }

  std::vector<float> dx_values;
  std::vector<float> dy_values;
  dx_values.reserve(matches.size());
  dy_values.reserve(matches.size());

  for (const StereoMatch& match : matches) {
    if (source_is_left) {
      dx_values.push_back(match.right.x + static_cast<float>(left_width) -
                          match.left.x);
      dy_values.push_back(match.right.y - match.left.y);
    } else {
      dx_values.push_back(match.left.x -
                          (match.right.x + static_cast<float>(left_width)));
      dy_values.push_back(match.left.y - match.right.y);
    }
  }

  result.valid = true;
  result.dx = Median(&dx_values);
  result.dy = Median(&dy_values);
  return result;
}

std::vector<StereoMatch> ComputeOrbRansacMatches(
    const StereoLumaFrame& frame,
    size_t left_width) {
  std::vector<StereoMatch> result;
  if (!frame.data || frame.width == 0 || frame.height == 0 ||
      frame.stride == 0 || left_width == 0 || left_width >= frame.width) {
    return result;
  }

  const int left_w = static_cast<int>(left_width);
  const int right_w = static_cast<int>(frame.width - left_width);
  const int height = static_cast<int>(frame.height);
  cv::Mat y_plane(height, static_cast<int>(frame.width), CV_8UC1,
                  const_cast<uint8_t*>(frame.data), frame.stride);
  cv::Mat left = y_plane(cv::Rect(0, 0, left_w, height));
  cv::Mat right = y_plane(cv::Rect(left_w, 0, right_w, height));

  cv::Ptr<cv::ORB> orb = cv::ORB::create(
      kOrbFeatures, 1.2f, 8, 31, 0, 2, cv::ORB::HARRIS_SCORE, 31,
      kOrbFastThreshold);
  std::vector<cv::KeyPoint> left_keypoints;
  std::vector<cv::KeyPoint> right_keypoints;
  cv::Mat left_descriptors;
  cv::Mat right_descriptors;
  orb->detectAndCompute(left, cv::noArray(), left_keypoints, left_descriptors);
  orb->detectAndCompute(right, cv::noArray(), right_keypoints,
                        right_descriptors);
  if (left_descriptors.empty() || right_descriptors.empty()) {
    return result;
  }

  cv::BFMatcher matcher(cv::NORM_HAMMING, false);
  std::vector<std::vector<cv::DMatch> > knn_matches;
  matcher.knnMatch(left_descriptors, right_descriptors, knn_matches, 2);

  std::vector<cv::DMatch> good_matches;
  good_matches.reserve(knn_matches.size());
  for (const std::vector<cv::DMatch>& pair : knn_matches) {
    if (pair.size() < 2) {
      continue;
    }
    if (pair[0].distance < pair[1].distance * kMatchRatio) {
      good_matches.push_back(pair[0]);
    }
  }
  if (good_matches.size() < kMinGoodMatches) {
    return result;
  }

  std::vector<cv::Point2f> left_points;
  std::vector<cv::Point2f> right_points;
  left_points.reserve(good_matches.size());
  right_points.reserve(good_matches.size());
  for (const cv::DMatch& match : good_matches) {
    left_points.push_back(left_keypoints[match.queryIdx].pt);
    right_points.push_back(right_keypoints[match.trainIdx].pt);
  }

  cv::Mat inlier_mask;
  cv::findFundamentalMat(left_points, right_points, cv::FM_RANSAC,
                         kRansacReprojThreshold, 0.99, inlier_mask);
  if (inlier_mask.empty()) {
    return result;
  }

  result.reserve(good_matches.size());
  for (int i = 0; i < inlier_mask.rows; ++i) {
    if (inlier_mask.at<uint8_t>(i, 0) == 0) {
      continue;
    }
    StereoMatch match;
    match.left = left_points[static_cast<size_t>(i)];
    match.right = right_points[static_cast<size_t>(i)];
    result.push_back(match);
  }
  return result;
}

PixelBox ProjectToOtherHalf(const PixelBox& box,
                            const MatchOffset& offset) {
  PixelBox projected = box;
  projected.x = box.x + offset.dx;
  projected.y = box.y + offset.dy;
  return projected;
}

void AddStereoPair(const StereoLumaFrame& frame,
                   size_t left_width,
                   const YoloDetectionBox& source,
                   const PixelBox& source_box,
                   bool source_is_left,
                   const MatchOffset& offset,
                   std::vector<YoloDetectionBox>* output) {
  if (!output || output->size() + 2 > kMaxStereoDetections) {
    return;
  }
  if (!offset.valid) {
    return;
  }

  const float image_height = static_cast<float>(frame.height);
  const float frame_width = static_cast<float>(frame.width);
  const float left_min_x = 0.0f;
  const float left_max_x = static_cast<float>(left_width);
  const float right_min_x = static_cast<float>(left_width);
  const float right_max_x = frame_width;
  const float common_w = source_box.w;
  const float common_h = source_box.h;

  PixelBox left_box;
  PixelBox right_box;
  if (source_is_left) {
    left_box = source_box;
    right_box = ProjectToOtherHalf(source_box, offset);
  } else {
    right_box = source_box;
    left_box = ProjectToOtherHalf(source_box, offset);
  }

  left_box.w = common_w;
  left_box.h = common_h;
  right_box.w = common_w;
  right_box.h = common_h;
  const float fused_y = (left_box.y + right_box.y) * 0.5f;
  left_box.y = fused_y;
  right_box.y = fused_y;

  left_box = ClampBox(left_box, left_min_x, left_max_x, 0.0f, image_height);
  right_box = ClampBox(right_box, right_min_x, right_max_x, 0.0f,
                       image_height);

  output->push_back(ToDetectionBox(source, left_box, frame.width, frame.height,
                                   0u));
  if (output->size() < kMaxStereoDetections) {
    output->push_back(ToDetectionBox(source, right_box, frame.width,
                                     frame.height, 1u));
  }
}

}  // namespace

std::vector<YoloDetectionBox> FuseStereoDetectionsWithLocalMatching(
    const StereoLumaFrame& frame,
    size_t left_width,
    const std::vector<YoloDetectionBox>& detections) {
  std::vector<YoloDetectionBox> fused;
  if (!frame.data || frame.width == 0 || frame.height == 0 ||
      frame.stride == 0 || left_width == 0 || left_width >= frame.width) {
    return detections;
  }

  std::vector<DetectionCandidate> candidates;
  candidates.reserve(detections.size());
  for (const YoloDetectionBox& detection : detections) {
    const PixelBox box = ToPixelBox(detection, frame.width, frame.height);
    if (box.w < 2.0f || box.h < 2.0f || CrossesStereoBoundary(box, left_width)) {
      continue;
    }
    DetectionCandidate candidate;
    candidate.detection = detection;
    candidate.box = box;
    candidate.source_is_left = IsMostlyInLeftHalf(box, left_width);
    candidates.push_back(candidate);
  }
  if (candidates.empty()) {
    return detections;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const DetectionCandidate& a, const DetectionCandidate& b) {
              return a.detection.confidence > b.detection.confidence;
            });

  const std::vector<StereoMatch> matches =
      ComputeOrbRansacMatches(frame, left_width);
  const MatchOffset left_global_offset =
      GlobalMedianOffset(matches, true, left_width);
  const MatchOffset right_global_offset =
      GlobalMedianOffset(matches, false, left_width);

  fused.reserve(std::min(kMaxStereoDetections, candidates.size() * 2));
  for (size_t i = 0; i < candidates.size(); ++i) {
    DetectionCandidate& candidate = candidates[i];
    if (candidate.suppressed || fused.size() >= kMaxStereoDetections) {
      continue;
    }

    const PixelBox box = candidate.box;
    const bool source_is_left = candidate.source_is_left;
    MatchOffset offset = MedianOffset(matches, box, source_is_left, left_width);
    if (!offset.valid) {
      offset = source_is_left ? left_global_offset : right_global_offset;
    }
    if (!offset.valid) {
      continue;
    }
    const PixelBox projected = ProjectToOtherHalf(box, offset);

    for (size_t j = i + 1; j < candidates.size(); ++j) {
      DetectionCandidate& other = candidates[j];
      if (other.suppressed ||
          other.detection.class_id != candidate.detection.class_id ||
          other.source_is_left == source_is_left) {
        continue;
      }
      if (IoU(projected, other.box) > kDuplicateIoUThreshold) {
        other.suppressed = true;
      }
    }

    AddStereoPair(frame, left_width, candidate.detection, box, source_is_left,
                  offset, &fused);
  }

  return fused.empty() ? detections : fused;
}

}  // namespace rtc_camera_headless
