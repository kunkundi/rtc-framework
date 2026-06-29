#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

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
            size_t right_width,
            size_t right_height,
            size_t right_input_stride_bytes,
            std::string* error_message);
  bool Convert(const uint8_t* left_src_host,
               size_t left_src_size,
               const uint8_t* right_src_host,
               size_t right_src_size,
               const uint8_t** dst_host,
               size_t* dst_size,
               std::string* error_message);

  size_t output_width() const;
  size_t output_height() const;
  size_t y_stride() const;
  size_t u_stride() const;
  size_t v_stride() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};
