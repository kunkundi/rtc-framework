#pragma once

#include "rtc_camera/dual/async_image_source.h"
#include "rtc_vision/vision_detection_sender.h"

#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

namespace rtc_camera_headless {

class YoloOnnxDetector {
 public:
  static std::unique_ptr<YoloOnnxDetector> CreateDefault(
      std::string* error_message);
  static std::unique_ptr<YoloOnnxDetector> Create(
      const std::string& model_path,
      std::string* error_message);

  ~YoloOnnxDetector();

  YoloOnnxDetector(const YoloOnnxDetector&) = delete;
  YoloOnnxDetector& operator=(const YoloOnnxDetector&) = delete;

  bool Detect(const rtc_camera::dual::ImageFrame& frame,
              std::vector<YoloDetectionBox>* boxes,
              std::string* error_message);
  bool DetectStereo(const rtc_camera::dual::ImageFrame& left_frame,
                    const rtc_camera::dual::ImageFrame& right_frame,
                    std::vector<YoloDetectionBox>* left_boxes,
                    std::vector<YoloDetectionBox>* right_boxes,
                    std::string* error_message);

  const std::string& model_path() const;

 private:
  struct Impl;

  explicit YoloOnnxDetector(std::unique_ptr<Impl> impl);
  bool DetectBatch(const rtc_camera::dual::ImageFrame* frames,
                   size_t frame_count,
                   std::vector<YoloDetectionBox>* batch_boxes,
                   std::string* error_message);

  std::unique_ptr<Impl> impl_;
};

}  // namespace rtc_camera_headless
