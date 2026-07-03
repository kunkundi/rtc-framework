#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

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
            std::string* error_message);
  bool Convert(const uint8_t* src_host,
               size_t src_size,
               const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);

  size_t y_stride() const;
  size_t u_stride() const;
  size_t v_stride() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};
