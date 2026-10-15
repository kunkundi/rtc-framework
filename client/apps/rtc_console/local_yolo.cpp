#include "local_yolo.h"
#include "rtc_vision/yolo_onnx_detector.h"
#include <libyuv.h>
#include <algorithm>
#include <limits>

namespace rtc_console {

LocalYolo::~LocalYolo() { Stop(); }

bool LocalYolo::Start(const std::string& model, int max_fps, Result result,
                      std::string* error, std::function<void(const std::string&)> failure) {
  Stop();
  if (max_fps < 1 || max_fps > 240 || !result) {
    if (error) *error = "invalid local YOLO worker options";
    return false;
  }
  auto detector = model.empty()
      ? rtc_camera_headless::YoloOnnxDetector::CreateDefault(error)
      : rtc_camera_headless::YoloOnnxDetector::Create(model, error);
  if (!detector) return false;
  stopped_ = false;
  worker_ = std::thread([this, detector = std::move(detector), max_fps, result, failure]() {
    std::map<std::string, std::chrono::steady_clock::time_point> next;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
      ready_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
      if (stopped_) break;
      auto selected = pending_.end();
      auto deadline = std::chrono::steady_clock::time_point::max();
      const auto now = std::chrono::steady_clock::now();
      for (auto entry = pending_.begin(); entry != pending_.end(); ++entry) {
        if (next[entry->first] <= now &&
            (selected == pending_.end() || entry->second.received < selected->second.received)) {
          selected = entry;
        }
        deadline = std::min(deadline, next[entry->first]);
      }
      if (selected == pending_.end()) {
        ready_.wait_until(lock, deadline);
        continue;
      }
      auto frame = std::move(selected->second);
      pending_.erase(selected);
      const auto generation = generation_;
      next[frame.key] = now + std::chrono::microseconds(1000000 / max_fps);
      lock.unlock();
      bool succeeded = true;
      std::string inference_error;
      vts_rtc::vision::FrameDetections detections;
      detections.source_id = frame.source;
      detections.frame_id = frame.sequence;
      detections.frame_width = frame.width;
      detections.frame_height = frame.height;
      const int columns = frame.source == "stereo" || frame.source == "merged_image" ||
                          frame.source == "surround_grid" ? 2 : 1;
      const int rows = frame.source == "surround_grid" ? 2 : 1;
      for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
          const int left = frame.width * column / columns;
          const int top = frame.height * row / rows;
          const int width = frame.width * (column + 1) / columns - left;
          const int height = frame.height * (row + 1) / rows - top;
          const int chroma_width = (width + 1) / 2;
          const int chroma_height = (height + 1) / 2;
          std::vector<uint8_t> storage(width * height + 2 * chroma_width * chroma_height);
          rtc_camera::dual::ImageFrame image;
          image.width = width;
          image.height = height;
          image.stride_y = width;
          image.stride_u = image.stride_v = chroma_width;
          image.data = storage.data();
          image.data_size = storage.size();
          image.sequence = frame.sequence;
          if (libyuv::BGRAToI420(frame.pixels.data() + (top * frame.width + left) * 4,
              frame.width * 4, storage.data(), width, storage.data() + width * height,
              chroma_width, storage.data() + width * height + chroma_width * chroma_height,
              chroma_width, width, height) != 0) continue;
          std::vector<rtc_camera_headless::YoloDetectionBox> boxes;
          std::string error;
          if (!detector->Detect(image, &boxes, &error)) {
            succeeded = false;
            inference_error = error;
            continue;
          }
          for (const auto& box : boxes) {
            if (detections.detections.size() >= vts_rtc::vision::kMaxDetectionsPerFrame) break;
            vts_rtc::vision::Detection detection;
            detection.class_id = box.class_id;
            detection.confidence = box.confidence;
            detection.x = (static_cast<uint64_t>(left) * 65535 + static_cast<uint64_t>(box.x) * width) / frame.width;
            detection.y = (static_cast<uint64_t>(top) * 65535 + static_cast<uint64_t>(box.y) * height) / frame.height;
            detection.w = static_cast<uint64_t>(box.w) * width / frame.width;
            detection.h = static_cast<uint64_t>(box.h) * height / frame.height;
            detections.detections.push_back(detection);
          }
        }
      }
      lock.lock();
      if (!stopped_ && generation == generation_) {
        if (succeeded) result(frame, detections);
        else if (failure) failure(inference_error);
      }
    }
  });
  return true;
}

void LocalYolo::Submit(LocalYoloFrame frame) {
  if (frame.width <= 0 || frame.height <= 0 || frame.width > 16384 || frame.height > 16384 ||
      frame.pixels.size() != static_cast<size_t>(frame.width) * frame.height * 4) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) return;
  if (pending_.count(frame.key) == 0 && pending_.size() >= 32) return;
  pending_[frame.key] = std::move(frame);
  ready_.notify_one();
}

void LocalYolo::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++generation_;
  pending_.clear();
}

void LocalYolo::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    pending_.clear();
  }
  ready_.notify_one();
  if (worker_.joinable()) worker_.join();
}

}