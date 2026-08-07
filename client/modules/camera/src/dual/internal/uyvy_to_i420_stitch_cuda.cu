#include "uyvy_to_i420_stitch_cuda.h"

#include <cuda_runtime.h>

#include <sstream>
#include <string>

namespace {

constexpr int kHorizontalSampleStep = 2;

std::string MakeCudaError(const char* action, cudaError_t code) {
  std::ostringstream oss;
  oss << action << " failed: " << cudaGetErrorString(code) << " ("
      << static_cast<int>(code) << ")";
  return oss.str();
}

__global__ void DualUyvyToSampledI420SideBySideKernel(
    const uint8_t* left_src,
    int left_stride,
    int left_width,
    int left_is_yuyv,
    const uint8_t* right_src,
    int right_stride,
    int right_width,
    int right_is_yuyv,
    uint8_t* dst_y,
    int dst_stride_y,
    uint8_t* dst_u,
    int dst_stride_u,
    uint8_t* dst_v,
    int dst_stride_v,
    int height,
    int left_sampled_width,
    int left_sampled_chroma_width,
    int total_chroma_width) {
  const int chroma_x = blockIdx.x * blockDim.x + threadIdx.x;
  const int chroma_y = blockIdx.y * blockDim.y + threadIdx.y;
  const int chroma_height = (height + 1) / 2;
  if (chroma_x >= total_chroma_width || chroma_y >= chroma_height) {
    return;
  }

  const bool use_left = chroma_x < left_sampled_chroma_width;
  const uint8_t* src = use_left ? left_src : right_src;
  const int src_stride = use_left ? left_stride : right_stride;
  const int is_yuyv = use_left ? left_is_yuyv : right_is_yuyv;
  const int dst_y_offset = use_left ? 0 : left_sampled_width;
  const int dst_uv_offset = use_left ? 0 : left_sampled_chroma_width;
  const int local_chroma_x =
      use_left ? chroma_x : (chroma_x - left_sampled_chroma_width);
  const int src_x = local_chroma_x * 4;
  const int dst_x = dst_y_offset + local_chroma_x * 2;
  const int dst_uv_x = dst_uv_offset + local_chroma_x;
  const int src_y0 = chroma_y * 2;
  const int src_y1 = src_y0 + 1;

  const uint8_t* row0 = src + src_y0 * src_stride;
  const uint8_t* pair00 = row0 + src_x * 2;
  const uint8_t* pair01 = row0 + (src_x + 2) * 2;

  uint8_t y00, u00, v00;
  uint8_t y01, u01, v01;
  if (is_yuyv) {
    // YUYV 字节顺序：Y0 U0 Y1 V0。
    y00 = pair00[0]; u00 = pair00[1]; v00 = pair00[3];
    y01 = pair01[0]; u01 = pair01[1]; v01 = pair01[3];
  } else {
    // UYVY 字节顺序：U0 Y0 V0 Y1。
    u00 = pair00[0]; y00 = pair00[1]; v00 = pair00[2];
    u01 = pair01[0]; y01 = pair01[1]; v01 = pair01[2];
  }

  dst_y[src_y0 * dst_stride_y + dst_x] = y00;
  dst_y[src_y0 * dst_stride_y + dst_x + 1] = y01;

  int u_sum = static_cast<int>(u00) + static_cast<int>(u01);
  int v_sum = static_cast<int>(v00) + static_cast<int>(v01);
  int uv_samples = 2;

  if (src_y1 < height) {
    const uint8_t* row1 = src + src_y1 * src_stride;
    const uint8_t* pair10 = row1 + src_x * 2;
    const uint8_t* pair11 = row1 + (src_x + 2) * 2;

    uint8_t y10, u10, v10;
    uint8_t y11, u11, v11;
    if (is_yuyv) {
      y10 = pair10[0]; u10 = pair10[1]; v10 = pair10[3];
      y11 = pair11[0]; u11 = pair11[1]; v11 = pair11[3];
    } else {
      u10 = pair10[0]; y10 = pair10[1]; v10 = pair10[2];
      u11 = pair11[0]; y11 = pair11[1]; v11 = pair11[2];
    }

    dst_y[src_y1 * dst_stride_y + dst_x] = y10;
    dst_y[src_y1 * dst_stride_y + dst_x + 1] = y11;

    u_sum += static_cast<int>(u10) + static_cast<int>(u11);
    v_sum += static_cast<int>(v10) + static_cast<int>(v11);
    uv_samples += 2;
  }

  dst_u[chroma_y * dst_stride_u + dst_uv_x] =
      static_cast<uint8_t>((u_sum + uv_samples / 2) / uv_samples);
  dst_v[chroma_y * dst_stride_v + dst_uv_x] =
      static_cast<uint8_t>((v_sum + uv_samples / 2) / uv_samples);
}

}  // 匿名命名空间

namespace rtc_camera {
namespace dual {

struct DualUyvyToI420StitchCudaConverter::Impl {
  struct Slot {
    uint8_t* device_left_input = nullptr;
    uint8_t* device_right_input = nullptr;
    uint8_t* device_output = nullptr;
    uint8_t* host_output = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t completed = nullptr;
    bool pending = false;
  };

  size_t left_width = 0;
  size_t left_height = 0;
  size_t left_input_stride_bytes = 0;
  size_t left_input_size = 0;
  bool left_is_yuyv = false;
  size_t right_width = 0;
  size_t right_height = 0;
  size_t right_input_stride_bytes = 0;
  size_t right_input_size = 0;
  bool right_is_yuyv = false;
  size_t output_width = 0;
  size_t output_height = 0;
  size_t y_stride = 0;
  size_t u_stride = 0;
  size_t v_stride = 0;
  size_t output_size = 0;
  Slot slots[2];
  size_t pending_order[2] = {0, 0};
  size_t pending_head = 0;
  size_t pending_count = 0;
  size_t next_slot = 0;
  bool initialized = false;
};

DualUyvyToI420StitchCudaConverter::DualUyvyToI420StitchCudaConverter()
    : impl_(new Impl()) {}

DualUyvyToI420StitchCudaConverter::~DualUyvyToI420StitchCudaConverter() {
  if (!impl_) {
    return;
  }
  for (size_t i = 0; i < 2; ++i) {
    Impl::Slot& slot = impl_->slots[i];
    if (slot.completed) {
      cudaEventDestroy(slot.completed);
      slot.completed = nullptr;
    }
    if (slot.stream) {
      cudaStreamDestroy(slot.stream);
      slot.stream = nullptr;
    }
    if (slot.device_left_input) {
      cudaFree(slot.device_left_input);
      slot.device_left_input = nullptr;
    }
    if (slot.device_right_input) {
      cudaFree(slot.device_right_input);
      slot.device_right_input = nullptr;
    }
    if (slot.device_output) {
      cudaFree(slot.device_output);
      slot.device_output = nullptr;
    }
    if (slot.host_output) {
      cudaFreeHost(slot.host_output);
      slot.host_output = nullptr;
    }
  }
  delete impl_;
  impl_ = nullptr;
}

bool DualUyvyToI420StitchCudaConverter::Init(
    size_t left_width,
    size_t left_height,
    size_t left_input_stride_bytes,
    bool left_is_yuyv,
    size_t right_width,
    size_t right_height,
    size_t right_input_stride_bytes,
    bool right_is_yuyv,
    std::string* error_message) {
  if (!impl_) {
    if (error_message) {
      *error_message = "internal converter state is null";
    }
    return false;
  }

  if (left_width == 0 || left_height == 0 || right_width == 0 ||
      right_height == 0) {
    if (error_message) {
      *error_message = "invalid frame size";
    }
    return false;
  }
  if (left_height != right_height) {
    if (error_message) {
      *error_message = "camera heights must match";
    }
    return false;
  }
  if ((left_width % (kHorizontalSampleStep * 2)) != 0 ||
      (right_width % (kHorizontalSampleStep * 2)) != 0) {
    if (error_message) {
      *error_message =
          "camera widths must be multiples of 4 for sampled I420 stitching";
    }
    return false;
  }
  if (left_input_stride_bytes < left_width * 2 ||
      right_input_stride_bytes < right_width * 2) {
    if (error_message) {
      *error_message = "input stride is smaller than width * 2";
    }
    return false;
  }

  int device_count = 0;
  cudaError_t cuda_code = cudaGetDeviceCount(&device_count);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaGetDeviceCount", cuda_code);
    }
    return false;
  }
  if (device_count <= 0) {
    if (error_message) {
      *error_message = "no CUDA device found";
    }
    return false;
  }

  cuda_code = cudaSetDevice(0);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaSetDevice", cuda_code);
    }
    return false;
  }

  impl_->left_width = left_width;
  impl_->left_height = left_height;
  impl_->left_input_stride_bytes = left_input_stride_bytes;
  impl_->left_is_yuyv = left_is_yuyv;
  impl_->left_input_size = left_input_stride_bytes * left_height;
  impl_->right_width = right_width;
  impl_->right_height = right_height;
  impl_->right_input_stride_bytes = right_input_stride_bytes;
  impl_->right_is_yuyv = right_is_yuyv;
  impl_->right_input_size = right_input_stride_bytes * right_height;
  impl_->output_width = left_width / kHorizontalSampleStep +
                        right_width / kHorizontalSampleStep;
  impl_->output_height = left_height;
  impl_->y_stride = impl_->output_width;
  impl_->u_stride = impl_->output_width / 2;
  impl_->v_stride = impl_->output_width / 2;
  impl_->output_size =
      impl_->output_width * impl_->output_height +
      impl_->u_stride * ((impl_->output_height + 1) / 2) * 2;

  for (size_t i = 0; i < 2; ++i) {
    Impl::Slot& slot = impl_->slots[i];
    cuda_code =
        cudaStreamCreateWithFlags(&slot.stream, cudaStreamNonBlocking);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message = MakeCudaError("cudaStreamCreateWithFlags", cuda_code);
      }
      return false;
    }
    cuda_code = cudaEventCreateWithFlags(&slot.completed,
                                         cudaEventDisableTiming);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message = MakeCudaError("cudaEventCreateWithFlags", cuda_code);
      }
      return false;
    }
    cuda_code =
        cudaMalloc(&slot.device_left_input, impl_->left_input_size);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message =
            MakeCudaError("cudaMalloc(device_left_input)", cuda_code);
      }
      return false;
    }
    cuda_code =
        cudaMalloc(&slot.device_right_input, impl_->right_input_size);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message =
            MakeCudaError("cudaMalloc(device_right_input)", cuda_code);
      }
      return false;
    }
    cuda_code = cudaMalloc(&slot.device_output, impl_->output_size);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message = MakeCudaError("cudaMalloc(device_output)", cuda_code);
      }
      return false;
    }
    cuda_code = cudaMallocHost(&slot.host_output, impl_->output_size);
    if (cuda_code != cudaSuccess) {
      if (error_message) {
        *error_message = MakeCudaError("cudaMallocHost(host_output)", cuda_code);
      }
      return false;
    }
  }

  impl_->initialized = true;
  return true;
}

bool DualUyvyToI420StitchCudaConverter::Convert(
    const uint8_t* left_src_host,
    size_t left_src_size,
    const uint8_t* right_src_host,
    size_t right_src_size,
    const uint8_t** dst_host,
    size_t* dst_size,
    std::string* error_message) {
  if (pending_count() != 0) {
    if (error_message) {
      *error_message = "converter has pending asynchronous frames";
    }
    return false;
  }
  return Enqueue(left_src_host, left_src_size, right_src_host, right_src_size,
                 error_message) &&
         Dequeue(dst_host, dst_size, error_message);
}

bool DualUyvyToI420StitchCudaConverter::Enqueue(
    const uint8_t* left_src_host,
    size_t left_src_size,
    const uint8_t* right_src_host,
    size_t right_src_size,
    std::string* error_message) {
  if (!impl_ || !impl_->initialized) {
    if (error_message) {
      *error_message = "converter is not initialized";
    }
    return false;
  }
  if (!left_src_host || !right_src_host) {
    if (error_message) {
      *error_message = "input buffer is null";
    }
    return false;
  }
  if (left_src_size < impl_->left_input_size ||
      right_src_size < impl_->right_input_size) {
    if (error_message) {
      *error_message = "input buffer is smaller than required UYVY frame size";
    }
    return false;
  }
  if (impl_->pending_count >= 2) {
    if (error_message) {
      *error_message = "converter pipeline is full";
    }
    return false;
  }

  size_t slot_index = impl_->next_slot;
  if (impl_->slots[slot_index].pending) {
    slot_index = (slot_index + 1) % 2;
  }
  Impl::Slot& slot = impl_->slots[slot_index];

  cudaError_t cuda_code = cudaMemcpyAsync(
      slot.device_left_input, left_src_host, impl_->left_input_size,
      cudaMemcpyHostToDevice, slot.stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMemcpyAsync(left H2D)", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMemcpyAsync(slot.device_right_input, right_src_host,
                              impl_->right_input_size, cudaMemcpyHostToDevice,
                              slot.stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMemcpyAsync(right H2D)", cuda_code);
    }
    return false;
  }

  uint8_t* dst_y = slot.device_output;
  uint8_t* dst_u = dst_y + impl_->y_stride * impl_->output_height;
  uint8_t* dst_v = dst_u + impl_->u_stride * ((impl_->output_height + 1) / 2);
  const int total_chroma_width = static_cast<int>(impl_->u_stride);
  const int left_sampled_width = static_cast<int>(impl_->left_width / 2);
  const int left_sampled_chroma_width =
      static_cast<int>(impl_->left_width / 4);

  const dim3 block(16, 16);
  const dim3 grid(
      (static_cast<unsigned int>(total_chroma_width) + block.x - 1) / block.x,
      (static_cast<unsigned int>((impl_->output_height + 1) / 2) + block.y - 1) /
          block.y);
  DualUyvyToSampledI420SideBySideKernel<<<grid, block, 0, slot.stream>>>(
      slot.device_left_input,
      static_cast<int>(impl_->left_input_stride_bytes),
      static_cast<int>(impl_->left_width),
      impl_->left_is_yuyv ? 1 : 0,
      slot.device_right_input,
      static_cast<int>(impl_->right_input_stride_bytes),
      static_cast<int>(impl_->right_width),
      impl_->right_is_yuyv ? 1 : 0,
      dst_y,
      static_cast<int>(impl_->y_stride), dst_u, static_cast<int>(impl_->u_stride),
      dst_v, static_cast<int>(impl_->v_stride),
      static_cast<int>(impl_->output_height), left_sampled_width,
      left_sampled_chroma_width, total_chroma_width);

  cuda_code = cudaGetLastError();
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError(
          "DualUyvyToSampledI420SideBySideKernel launch", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMemcpyAsync(slot.host_output, slot.device_output,
                              impl_->output_size, cudaMemcpyDeviceToHost,
                              slot.stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMemcpyAsync(D2H)", cuda_code);
    }
    return false;
  }

  cuda_code = cudaEventRecord(slot.completed, slot.stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaEventRecord", cuda_code);
    }
    cudaStreamSynchronize(slot.stream);
    return false;
  }

  slot.pending = true;
  impl_->pending_order[(impl_->pending_head + impl_->pending_count) % 2] =
      slot_index;
  ++impl_->pending_count;
  impl_->next_slot = (slot_index + 1) % 2;
  return true;
}

bool DualUyvyToI420StitchCudaConverter::Dequeue(
    const uint8_t** dst_host,
    size_t* dst_size,
    std::string* error_message) {
  if (!impl_ || !impl_->initialized || impl_->pending_count == 0) {
    if (error_message) {
      *error_message = "converter pipeline is empty";
    }
    return false;
  }

  const size_t slot_index = impl_->pending_order[impl_->pending_head];
  Impl::Slot& slot = impl_->slots[slot_index];
  const cudaError_t cuda_code = cudaEventSynchronize(slot.completed);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaEventSynchronize", cuda_code);
    }
    return false;
  }

  if (dst_host) {
    *dst_host = slot.host_output;
  }
  if (dst_size) {
    *dst_size = impl_->output_size;
  }
  slot.pending = false;
  impl_->pending_head = (impl_->pending_head + 1) % 2;
  --impl_->pending_count;
  return true;
}

void DualUyvyToI420StitchCudaConverter::DiscardPending() {
  if (!impl_) {
    return;
  }
  while (impl_->pending_count > 0) {
    const size_t slot_index = impl_->pending_order[impl_->pending_head];
    Impl::Slot& slot = impl_->slots[slot_index];
    cudaEventSynchronize(slot.completed);
    slot.pending = false;
    impl_->pending_head = (impl_->pending_head + 1) % 2;
    --impl_->pending_count;
  }
}

size_t DualUyvyToI420StitchCudaConverter::pending_count() const {
  return impl_ ? impl_->pending_count : 0;
}

size_t DualUyvyToI420StitchCudaConverter::output_width() const {
  return impl_ ? impl_->output_width : 0;
}

size_t DualUyvyToI420StitchCudaConverter::output_height() const {
  return impl_ ? impl_->output_height : 0;
}

size_t DualUyvyToI420StitchCudaConverter::y_stride() const {
  return impl_ ? impl_->y_stride : 0;
}

size_t DualUyvyToI420StitchCudaConverter::u_stride() const {
  return impl_ ? impl_->u_stride : 0;
}

size_t DualUyvyToI420StitchCudaConverter::v_stride() const {
  return impl_ ? impl_->v_stride : 0;
}

}  // 命名空间 dual
}  // 命名空间 rtc_camera
