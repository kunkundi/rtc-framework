#include "uyvy_to_i420_cuda.h"

#include <cuda_runtime.h>

#include <sstream>
#include <string>

namespace {

std::string MakeCudaError(const char* action, cudaError_t code) {
  std::ostringstream oss;
  oss << action << " failed: " << cudaGetErrorString(code) << " ("
      << static_cast<int>(code) << ")";
  return oss.str();
}

__global__ void UYVYToI420Kernel(const uint8_t* src,
                                 int src_stride,
                                 uint8_t* dst_y,
                                 int dst_stride_y,
                                 uint8_t* dst_u,
                                 int dst_stride_u,
                                 uint8_t* dst_v,
                                 int dst_stride_v,
                                 int width,
                                 int height) {
  const int chroma_x = blockIdx.x * blockDim.x + threadIdx.x;
  const int chroma_y = blockIdx.y * blockDim.y + threadIdx.y;
  const int uv_width = width / 2;
  const int uv_height = (height + 1) / 2;
  if (chroma_x >= uv_width || chroma_y >= uv_height) {
    return;
  }

  const int x = chroma_x * 2;
  const int y = chroma_y * 2;

  const uint8_t* row0 = src + y * src_stride + x * 2;
  const uint8_t u0 = row0[0];
  const uint8_t y00 = row0[1];
  const uint8_t v0 = row0[2];
  const uint8_t y01 = row0[3];

  dst_y[y * dst_stride_y + x] = y00;
  dst_y[y * dst_stride_y + x + 1] = y01;

  uint8_t u_out = u0;
  uint8_t v_out = v0;
  if (y + 1 < height) {
    const uint8_t* row1 = src + (y + 1) * src_stride + x * 2;
    const uint8_t u1 = row1[0];
    const uint8_t y10 = row1[1];
    const uint8_t v1 = row1[2];
    const uint8_t y11 = row1[3];

    dst_y[(y + 1) * dst_stride_y + x] = y10;
    dst_y[(y + 1) * dst_stride_y + x + 1] = y11;

    u_out = static_cast<uint8_t>((static_cast<int>(u0) + static_cast<int>(u1) + 1) / 2);
    v_out = static_cast<uint8_t>((static_cast<int>(v0) + static_cast<int>(v1) + 1) / 2);
  }

  dst_u[chroma_y * dst_stride_u + chroma_x] = u_out;
  dst_v[chroma_y * dst_stride_v + chroma_x] = v_out;
}

}  // namespace

struct UyvyToI420CudaConverter::Impl {
  size_t width = 0;
  size_t height = 0;
  size_t input_stride_bytes = 0;
  size_t y_stride = 0;
  size_t u_stride = 0;
  size_t v_stride = 0;
  size_t input_size = 0;
  size_t output_size = 0;
  uint8_t* device_input = nullptr;
  uint8_t* device_output = nullptr;
  uint8_t* host_output = nullptr;
  cudaStream_t stream = nullptr;
  bool initialized = false;
};

UyvyToI420CudaConverter::UyvyToI420CudaConverter() : impl_(new Impl()) {}

UyvyToI420CudaConverter::~UyvyToI420CudaConverter() {
  if (!impl_) {
    return;
  }
  if (impl_->stream) {
    cudaStreamDestroy(impl_->stream);
    impl_->stream = nullptr;
  }
  if (impl_->device_input) {
    cudaFree(impl_->device_input);
    impl_->device_input = nullptr;
  }
  if (impl_->device_output) {
    cudaFree(impl_->device_output);
    impl_->device_output = nullptr;
  }
  if (impl_->host_output) {
    cudaFreeHost(impl_->host_output);
    impl_->host_output = nullptr;
  }
  delete impl_;
  impl_ = nullptr;
}

bool UyvyToI420CudaConverter::Init(size_t width,
                                   size_t height,
                                   size_t input_stride_bytes,
                                   std::string* error_message) {
  if (!impl_) {
    if (error_message) {
      *error_message = "internal converter state is null";
    }
    return false;
  }

  if (width == 0 || height == 0) {
    if (error_message) {
      *error_message = "invalid frame size";
    }
    return false;
  }
  if ((width % 2) != 0) {
    if (error_message) {
      *error_message = "UYVY width must be even";
    }
    return false;
  }
  if (input_stride_bytes < width * 2) {
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

  impl_->width = width;
  impl_->height = height;
  impl_->input_stride_bytes = input_stride_bytes;
  impl_->y_stride = width;
  impl_->u_stride = width / 2;
  impl_->v_stride = width / 2;
  impl_->input_size = input_stride_bytes * height;
  impl_->output_size = width * height * 3 / 2;

  cuda_code = cudaStreamCreateWithFlags(&impl_->stream, cudaStreamNonBlocking);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaStreamCreateWithFlags", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMalloc(&impl_->device_input, impl_->input_size);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMalloc(device_input)", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMalloc(&impl_->device_output, impl_->output_size);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMalloc(device_output)", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMallocHost(&impl_->host_output, impl_->output_size);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMallocHost(host_output)", cuda_code);
    }
    return false;
  }

  impl_->initialized = true;
  return true;
}

bool UyvyToI420CudaConverter::Convert(const uint8_t* src_host,
                                      size_t src_size,
                                      const uint8_t** dst_host,
                                      size_t* dst_size,
                                      std::string* error_message) {
  if (!impl_ || !impl_->initialized) {
    if (error_message) {
      *error_message = "converter is not initialized";
    }
    return false;
  }
  if (!src_host) {
    if (error_message) {
      *error_message = "input buffer is null";
    }
    return false;
  }
  if (src_size < impl_->input_size) {
    if (error_message) {
      *error_message = "input buffer is smaller than required UYVY frame size";
    }
    return false;
  }

  cudaError_t cuda_code =
      cudaMemcpyAsync(impl_->device_input, src_host, impl_->input_size,
                      cudaMemcpyHostToDevice, impl_->stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMemcpyAsync(H2D)", cuda_code);
    }
    return false;
  }

  uint8_t* dst_y = impl_->device_output;
  uint8_t* dst_u = dst_y + impl_->width * impl_->height;
  uint8_t* dst_v = dst_u + (impl_->width / 2) * ((impl_->height + 1) / 2);

  const dim3 block(16, 16);
  const dim3 grid((static_cast<unsigned int>(impl_->u_stride) + block.x - 1) / block.x,
                  (static_cast<unsigned int>((impl_->height + 1) / 2) + block.y - 1) / block.y);
  UYVYToI420Kernel<<<grid, block, 0, impl_->stream>>>(
      impl_->device_input, static_cast<int>(impl_->input_stride_bytes), dst_y,
      static_cast<int>(impl_->y_stride), dst_u, static_cast<int>(impl_->u_stride),
      dst_v, static_cast<int>(impl_->v_stride), static_cast<int>(impl_->width),
      static_cast<int>(impl_->height));

  cuda_code = cudaGetLastError();
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("UYVYToI420Kernel launch", cuda_code);
    }
    return false;
  }

  cuda_code = cudaMemcpyAsync(impl_->host_output, impl_->device_output,
                              impl_->output_size, cudaMemcpyDeviceToHost,
                              impl_->stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaMemcpyAsync(D2H)", cuda_code);
    }
    return false;
  }

  cuda_code = cudaStreamSynchronize(impl_->stream);
  if (cuda_code != cudaSuccess) {
    if (error_message) {
      *error_message = MakeCudaError("cudaStreamSynchronize", cuda_code);
    }
    return false;
  }

  if (dst_host) {
    *dst_host = impl_->host_output;
  }
  if (dst_size) {
    *dst_size = impl_->output_size;
  }
  return true;
}

size_t UyvyToI420CudaConverter::y_stride() const {
  return impl_ ? impl_->y_stride : 0;
}

size_t UyvyToI420CudaConverter::u_stride() const {
  return impl_ ? impl_->u_stride : 0;
}

size_t UyvyToI420CudaConverter::v_stride() const {
  return impl_ ? impl_->v_stride : 0;
}
