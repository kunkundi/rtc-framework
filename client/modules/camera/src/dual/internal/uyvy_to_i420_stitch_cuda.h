#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace rtc_camera {
namespace dual {

class DualUyvyToI420StitchCudaConverter {
 public:
  DualUyvyToI420StitchCudaConverter();
  ~DualUyvyToI420StitchCudaConverter();

  DualUyvyToI420StitchCudaConverter(
      const DualUyvyToI420StitchCudaConverter&) = delete;
  DualUyvyToI420StitchCudaConverter& operator=(
      const DualUyvyToI420StitchCudaConverter&) = delete;

  bool Init(size_t left_width,
            size_t left_height,
            size_t left_input_stride_bytes,
            bool left_is_yuyv,
            bool left_is_nv12,
            size_t right_width,
            size_t right_height,
            size_t right_input_stride_bytes,
            bool right_is_yuyv,
            bool right_is_nv12,
            std::string* error_message);
  bool Convert(const uint8_t* left_src_host,
               size_t left_src_size,
               bool left_src_mmap_backed,
               const uint8_t* right_src_host,
               size_t right_src_size,
               bool right_src_mmap_backed,
               const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);
  bool Enqueue(const uint8_t* left_src_host,
               size_t left_src_size,
               bool left_src_mmap_backed,
               const uint8_t* right_src_host,
               size_t right_src_size,
               bool right_src_mmap_backed,
               std::string* error_message);
  bool Dequeue(const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);
  void DiscardPending();
  size_t pending_count() const;

  size_t output_width() const;
  size_t output_height() const;
  size_t y_stride() const;
  size_t u_stride() const;
  size_t v_stride() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // 命名空间 dual
}  // 命名空间 rtc_camera
