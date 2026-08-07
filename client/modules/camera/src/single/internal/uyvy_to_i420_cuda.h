#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace rtc_camera {
namespace single {

class UyvyToI420CudaConverter {
 public:
  UyvyToI420CudaConverter();
  ~UyvyToI420CudaConverter();

  UyvyToI420CudaConverter(const UyvyToI420CudaConverter&) = delete;
  UyvyToI420CudaConverter& operator=(const UyvyToI420CudaConverter&) = delete;

  bool Init(size_t width,
            size_t height,
            size_t input_stride_bytes,
            bool is_yuyv,
            bool is_nv12,
            std::string* error_message);
  bool Convert(const uint8_t* src_host,
               size_t src_size,
               const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);
  bool Enqueue(const uint8_t* src_host,
               size_t src_size,
               std::string* error_message);
  bool Dequeue(const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);
  void DiscardPending();
  size_t pending_count() const;

  size_t y_stride() const;
  size_t u_stride() const;
  size_t v_stride() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // 命名空间 single
}  // 命名空间 rtc_camera
