#pragma once

#include "rtc_dual_camera/dual_camera_async_image_source.h"
#include "vision_detection_sender.h"

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

  bool Detect(const rtc_dual_camera::ImageFrame& frame,
              std::vector<YoloDetectionBox>* boxes,
              std::string* error_message);

  const std::string& model_path() const;

 private:
  struct Impl;

  explicit YoloOnnxDetector(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace rtc_camera_headless
