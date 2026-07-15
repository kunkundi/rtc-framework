#include "rtc_vision/yolo_onnx_detector.h"

#include "rtc_headless/rtc_camera_common.h"
#include "rtc_logging/rtc_logging.h"

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace rtc_camera_headless {
namespace {

constexpr int kDefaultInputSize = 640;
constexpr float kConfidenceThreshold = 0.25f;
constexpr float kNmsThreshold = 0.45f;
constexpr size_t kMaxDetections = 64;
constexpr size_t kTensorRtWorkspaceBytes = 1ull << 30;
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr const char* kEngineCacheMetadataVersion =
    "vtsrtc_yolo_trt_cache_v1";

struct PreprocessInfo {
  size_t original_width = 0;
  size_t original_height = 0;
  int input_width = 0;
  int input_height = 0;
  int resized_width = 0;
  int resized_height = 0;
  int pad_x = 0;
  int pad_y = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
};

struct Candidate {
  int class_id = 0;
  float score = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
};

struct ModelFingerprint {
  uint64_t size = 0;
  uint64_t hash = 0;
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

__device__ uint8_t ClampToByteDevice(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return static_cast<uint8_t>(value);
}

float Sigmoid(float value) {
  return 1.0f / (1.0f + std::exp(-value));
}

bool LooksLikeLogit(float value) {
  return value < 0.0f || value > 1.0f;
}

float ScoreValue(float value) {
  return LooksLikeLogit(value) ? Sigmoid(value) : value;
}

uint32_t ToNormU16(float value, float maximum) {
  if (maximum <= 0.0f) {
    return 0;
  }
  const float normalized = Clamp(value / maximum, 0.0f, 1.0f);
  return static_cast<uint32_t>(std::lround(normalized * 65535.0f));
}

std::string ShapeToString(const nvinfer1::Dims& shape) {
  std::ostringstream oss;
  oss << "[";
  for (int i = 0; i < shape.nbDims; ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << shape.d[i];
  }
  oss << "]";
  return oss.str();
}

std::string ResolveDefaultModelPath() {
  const char* env_model = std::getenv("VTSRTC_YOLO_MODEL");
  if (env_model && env_model[0] != '\0') {
    return env_model;
  }

  const std::string exe_dir = ExecutableDir();
  const std::vector<std::string> candidates = {
      JoinPath("models", "yolo26n.onnx"),
      "yolo26n.onnx",
      JoinPath(JoinPath(exe_dir, "models"), "yolo26n.onnx"),
      JoinPath(exe_dir, "yolo26n.onnx"),
      JoinPath(JoinPath(JoinPath(exe_dir, ".."), "models"), "yolo26n.onnx"),
  };

  for (const std::string& candidate : candidates) {
    if (FileExists(candidate)) {
      return candidate;
    }
  }
  return candidates.front();
}

std::string TensorRtEnginePath(const std::string& model_path) {
  const char* env_engine = std::getenv("VTSRTC_YOLO_TRT_ENGINE");
  if (env_engine && env_engine[0] != '\0') {
    return env_engine;
  }
  return model_path + ".trt";
}

std::string TensorRtEngineMetadataPath(const std::string& engine_path) {
  return engine_path + ".meta";
}

bool SameFingerprint(const ModelFingerprint& a,
                     const ModelFingerprint& b) {
  return a.size == b.size && a.hash == b.hash;
}

bool ComputeModelFingerprint(const std::string& path,
                             ModelFingerprint* fingerprint,
                             std::string* error_message) {
  if (!fingerprint) {
    if (error_message) {
      *error_message = "null model fingerprint output";
    }
    return false;
  }

  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    if (error_message) {
      *error_message = "failed to stat " + path;
    }
    return false;
  }

  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    if (error_message) {
      *error_message = "failed to open " + path;
    }
    return false;
  }

  uint64_t hash = kFnvOffsetBasis;
  char buffer[64 * 1024];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const std::streamsize count = input.gcount();
    for (std::streamsize i = 0; i < count; ++i) {
      hash ^= static_cast<uint8_t>(buffer[i]);
      hash *= kFnvPrime;
    }
  }
  if (!input.eof()) {
    if (error_message) {
      *error_message = "failed to hash " + path;
    }
    return false;
  }

  fingerprint->size = static_cast<uint64_t>(st.st_size);
  fingerprint->hash = hash;
  return true;
}

bool ReadEngineCacheMetadata(const std::string& metadata_path,
                             ModelFingerprint* fingerprint) {
  if (!fingerprint) {
    return false;
  }

  std::ifstream input(metadata_path.c_str());
  if (!input) {
    return false;
  }

  std::string version;
  uint64_t size = 0;
  uint64_t hash = 0;
  input >> version >> size >> hash;
  if (!input || version != kEngineCacheMetadataVersion) {
    return false;
  }

  fingerprint->size = size;
  fingerprint->hash = hash;
  return true;
}

bool WriteEngineCacheMetadata(const std::string& metadata_path,
                              const ModelFingerprint& fingerprint,
                              std::string* error_message) {
  std::ofstream output(metadata_path.c_str());
  if (!output) {
    if (error_message) {
      *error_message = "failed to open " + metadata_path + " for writing";
    }
    return false;
  }
  output << kEngineCacheMetadataVersion << " " << fingerprint.size << " "
         << fingerprint.hash << "\n";
  if (!output) {
    if (error_message) {
      *error_message = "failed to write " + metadata_path;
    }
    return false;
  }
  return true;
}

bool EngineCacheIsUsable(const std::string& engine_path,
                         const ModelFingerprint& model_fingerprint) {
  if (!FileExists(engine_path)) {
    return false;
  }

  const std::string metadata_path = TensorRtEngineMetadataPath(engine_path);
  ModelFingerprint cached_fingerprint;
  if (ReadEngineCacheMetadata(metadata_path, &cached_fingerprint)) {
    if (SameFingerprint(cached_fingerprint, model_fingerprint)) {
      return true;
    }
    rtc_logging::LogInfo("TensorRT YOLO engine cache is stale; rebuilding: " +
            engine_path);
    return false;
  }

  rtc_logging::LogInfo("TensorRT YOLO legacy engine cache has no metadata; reusing it: " +
          engine_path);
  std::string metadata_error;
  if (!WriteEngineCacheMetadata(metadata_path, model_fingerprint,
                                &metadata_error)) {
    rtc_logging::LogError("TensorRT YOLO metadata write failed: " + metadata_error);
  }
  return true;
}

bool ReadBinaryFile(const std::string& path,
                    std::vector<uint8_t>* data,
                    std::string* error_message) {
  if (!data) {
    if (error_message) {
      *error_message = "null binary file output";
    }
    return false;
  }

  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    if (error_message) {
      *error_message = "failed to open " + path;
    }
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size <= 0) {
    if (error_message) {
      *error_message = "empty file " + path;
    }
    return false;
  }
  input.seekg(0, std::ios::beg);
  data->resize(static_cast<size_t>(size));
  input.read(reinterpret_cast<char*>(data->data()), size);
  if (!input) {
    if (error_message) {
      *error_message = "failed to read " + path;
    }
    return false;
  }
  return true;
}

bool WriteBinaryFile(const std::string& path,
                     const void* data,
                     size_t size,
                     std::string* error_message) {
  std::ofstream output(path.c_str(), std::ios::binary);
  if (!output) {
    if (error_message) {
      *error_message = "failed to open " + path + " for writing";
    }
    return false;
  }
  output.write(reinterpret_cast<const char*>(data),
               static_cast<std::streamsize>(size));
  if (!output) {
    if (error_message) {
      *error_message = "failed to write " + path;
    }
    return false;
  }
  return true;
}

std::string MakeCudaError(const char* action, cudaError_t code) {
  std::ostringstream oss;
  oss << action << " failed: " << cudaGetErrorString(code) << " ("
      << static_cast<int>(code) << ")";
  return oss.str();
}

bool SetCudaError(const char* action,
                  cudaError_t code,
                  std::string* error_message) {
  if (code == cudaSuccess) {
    return true;
  }
  if (error_message) {
    *error_message = MakeCudaError(action, code);
  }
  return false;
}

bool HasDynamicDim(const nvinfer1::Dims& dims) {
  for (int i = 0; i < dims.nbDims; ++i) {
    if (dims.d[i] < 0) {
      return true;
    }
  }
  return false;
}

size_t Volume(const nvinfer1::Dims& dims) {
  if (dims.nbDims < 0) {
    return 0;
  }
  size_t volume = 1;
  for (int i = 0; i < dims.nbDims; ++i) {
    if (dims.d[i] <= 0) {
      return 0;
    }
    volume *= static_cast<size_t>(dims.d[i]);
  }
  return volume;
}

bool FillNchwInputShape(const nvinfer1::Dims& model_shape,
                        int* input_width,
                        int* input_height,
                        nvinfer1::Dims* concrete_shape,
                        std::string* error_message) {
  if (!input_width || !input_height || !concrete_shape) {
    if (error_message) {
      *error_message = "null TensorRT input shape output";
    }
    return false;
  }
  if (model_shape.nbDims != 4) {
    if (error_message) {
      *error_message = "expected NCHW YOLO input, got shape " +
                       ShapeToString(model_shape);
    }
    return false;
  }

  *concrete_shape = model_shape;
  if (concrete_shape->d[0] < 0) {
    concrete_shape->d[0] = 1;
  }
  if (concrete_shape->d[1] < 0) {
    concrete_shape->d[1] = 3;
  }
  if (concrete_shape->d[2] > 0) {
    *input_height = static_cast<int>(concrete_shape->d[2]);
  } else {
    concrete_shape->d[2] = *input_height;
  }
  if (concrete_shape->d[3] > 0) {
    *input_width = static_cast<int>(concrete_shape->d[3]);
  } else {
    concrete_shape->d[3] = *input_width;
  }

  if (concrete_shape->d[0] != 1 || concrete_shape->d[1] != 3 ||
      *input_width <= 0 || *input_height <= 0) {
    if (error_message) {
      *error_message = "unsupported YOLO input shape " +
                       ShapeToString(model_shape);
    }
    return false;
  }
  return true;
}

bool ResolveEngineInputShape(nvinfer1::ICudaEngine* engine,
                             const std::string& input_name,
                             int* input_width,
                             int* input_height,
                             nvinfer1::Dims* concrete_shape,
                             std::string* error_message) {
  if (!engine) {
    if (error_message) {
      *error_message = "TensorRT engine is null";
    }
    return false;
  }

  const nvinfer1::Dims engine_shape =
      engine->getTensorShape(input_name.c_str());
  nvinfer1::Dims shape = engine_shape;
  if (HasDynamicDim(engine_shape)) {
    shape = engine->getProfileShape(input_name.c_str(), 0,
                                    nvinfer1::OptProfileSelector::kOPT);
    if (shape.nbDims < 0 || HasDynamicDim(shape)) {
      if (error_message) {
        *error_message = "failed to resolve TensorRT input profile shape " +
                         ShapeToString(shape);
      }
      return false;
    }
  }

  return FillNchwInputShape(shape, input_width, input_height, concrete_shape,
                            error_message);
}

bool BuildPreprocessInfo(const rtc_dual_camera::ImageFrame& frame,
                         int input_width,
                         int input_height,
                         PreprocessInfo* info,
                         std::string* error_message) {
  if (!info) {
    if (error_message) {
      *error_message = "null preprocess info output";
    }
    return false;
  }
  if (frame.empty() || frame.width == 0 || frame.height == 0 ||
      frame.stride_y == 0 || frame.stride_u == 0 || frame.stride_v == 0) {
    if (error_message) {
      *error_message = "invalid I420 frame";
    }
    return false;
  }

  const size_t chroma_height = (frame.height + 1) / 2;
  const size_t y_bytes = frame.stride_y * frame.height;
  const size_t u_bytes = frame.stride_u * chroma_height;
  const size_t v_bytes = frame.stride_v * chroma_height;
  if (frame.data_size < y_bytes + u_bytes + v_bytes) {
    if (error_message) {
      *error_message = "I420 frame buffer is smaller than its strides";
    }
    return false;
  }

  const float scale =
      std::min(static_cast<float>(input_width) / frame.width,
               static_cast<float>(input_height) / frame.height);
  const int resized_width =
      std::max(1, static_cast<int>(std::lround(frame.width * scale)));
  const int resized_height =
      std::max(1, static_cast<int>(std::lround(frame.height * scale)));
  const int pad_x = (input_width - resized_width) / 2;
  const int pad_y = (input_height - resized_height) / 2;

  info->original_width = frame.width;
  info->original_height = frame.height;
  info->input_width = input_width;
  info->input_height = input_height;
  info->resized_width = resized_width;
  info->resized_height = resized_height;
  info->pad_x = pad_x;
  info->pad_y = pad_y;
  info->scale_x = static_cast<float>(resized_width) / frame.width;
  info->scale_y = static_cast<float>(resized_height) / frame.height;
  return true;
}

float IoU(const Candidate& a, const Candidate& b) {
  const float x1 = std::max(a.x1, b.x1);
  const float y1 = std::max(a.y1, b.y1);
  const float x2 = std::min(a.x2, b.x2);
  const float y2 = std::min(a.y2, b.y2);
  const float intersection =
      std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
  const float a_area = std::max(0.0f, a.x2 - a.x1) *
                       std::max(0.0f, a.y2 - a.y1);
  const float b_area = std::max(0.0f, b.x2 - b.x1) *
                       std::max(0.0f, b.y2 - b.y1);
  const float union_area = a_area + b_area - intersection;
  return union_area > 0.0f ? intersection / union_area : 0.0f;
}

void AddCandidate(float x1,
                  float y1,
                  float x2,
                  float y2,
                  float score,
                  int class_id,
                  const PreprocessInfo& info,
                  std::vector<Candidate>* candidates) {
  if (!std::isfinite(score) || score < kConfidenceThreshold ||
      class_id < 0 || !candidates) {
    return;
  }

  const float max_coord =
      std::max(std::max(std::fabs(x1), std::fabs(y1)),
               std::max(std::fabs(x2), std::fabs(y2)));
  if (max_coord <= 2.0f) {
    x1 *= info.input_width;
    x2 *= info.input_width;
    y1 *= info.input_height;
    y2 *= info.input_height;
  }

  x1 = (x1 - info.pad_x) / info.scale_x;
  x2 = (x2 - info.pad_x) / info.scale_x;
  y1 = (y1 - info.pad_y) / info.scale_y;
  y2 = (y2 - info.pad_y) / info.scale_y;

  if (x2 < x1) {
    std::swap(x1, x2);
  }
  if (y2 < y1) {
    std::swap(y1, y2);
  }

  x1 = Clamp(x1, 0.0f, static_cast<float>(info.original_width));
  x2 = Clamp(x2, 0.0f, static_cast<float>(info.original_width));
  y1 = Clamp(y1, 0.0f, static_cast<float>(info.original_height));
  y2 = Clamp(y2, 0.0f, static_cast<float>(info.original_height));

  if ((x2 - x1) < 1.0f || (y2 - y1) < 1.0f) {
    return;
  }

  Candidate candidate;
  candidate.class_id = class_id;
  candidate.score = Clamp(score, 0.0f, 1.0f);
  candidate.x1 = x1;
  candidate.y1 = y1;
  candidate.x2 = x2;
  candidate.y2 = y2;
  candidates->push_back(candidate);
}

void AddCxcywhCandidate(float cx,
                        float cy,
                        float width,
                        float height,
                        float score,
                        int class_id,
                        const PreprocessInfo& info,
                        std::vector<Candidate>* candidates) {
  AddCandidate(cx - width * 0.5f, cy - height * 0.5f, cx + width * 0.5f,
               cy + height * 0.5f, score, class_id, info, candidates);
}

template <typename ValueAt>
void ParseBoxScoreRows(size_t rows,
                       size_t attrs,
                       const ValueAt& value_at,
                       const PreprocessInfo& info,
                       std::vector<Candidate>* candidates) {
  if (attrs < 6) {
    return;
  }

  if (attrs <= 8) {
    for (size_t row = 0; row < rows; ++row) {
      const float score = ScoreValue(value_at(row, 4));
      const int class_id =
          static_cast<int>(std::lround(std::max(0.0f, value_at(row, 5))));
      AddCandidate(value_at(row, 0), value_at(row, 1), value_at(row, 2),
                   value_at(row, 3), score, class_id, info, candidates);
    }
    return;
  }

  size_t class_offset = 4;
  float objectness = 1.0f;
  const bool has_objectness = attrs >= 85 && (attrs - 5) <= 256;
  if (has_objectness) {
    class_offset = 5;
  }

  for (size_t row = 0; row < rows; ++row) {
    if (has_objectness) {
      objectness = ScoreValue(value_at(row, 4));
    }

    float best_score = 0.0f;
    int best_class = -1;
    for (size_t attr = class_offset; attr < attrs; ++attr) {
      const float score = objectness * ScoreValue(value_at(row, attr));
      if (score > best_score) {
        best_score = score;
        best_class = static_cast<int>(attr - class_offset);
      }
    }

    AddCxcywhCandidate(value_at(row, 0), value_at(row, 1), value_at(row, 2),
                       value_at(row, 3), best_score, best_class, info,
                       candidates);
  }
}

bool ParseTensorOutput(const float* data,
                       const nvinfer1::Dims& shape,
                       const PreprocessInfo& info,
                       std::vector<Candidate>* candidates,
                       std::string* error_message) {
  if (!data) {
    return false;
  }
  if (shape.nbDims != 2 && shape.nbDims != 3) {
    return false;
  }

  size_t dim_a = 0;
  size_t dim_b = 0;
  size_t base_offset = 0;
  if (shape.nbDims == 3) {
    if (shape.d[0] < 1 || shape.d[1] <= 0 || shape.d[2] <= 0) {
      if (error_message) {
        *error_message = "invalid YOLO output shape " + ShapeToString(shape);
      }
      return false;
    }
    dim_a = static_cast<size_t>(shape.d[1]);
    dim_b = static_cast<size_t>(shape.d[2]);
  } else {
    if (shape.d[0] <= 0 || shape.d[1] <= 0) {
      if (error_message) {
        *error_message = "invalid YOLO output shape " + ShapeToString(shape);
      }
      return false;
    }
    dim_a = static_cast<size_t>(shape.d[0]);
    dim_b = static_cast<size_t>(shape.d[1]);
  }

  if (dim_b >= 6 && dim_b <= 512 && dim_a > dim_b) {
    const size_t rows = dim_a;
    const size_t attrs = dim_b;
    ParseBoxScoreRows(
        rows, attrs,
        [&](size_t row, size_t attr) {
          return data[base_offset + row * attrs + attr];
        },
        info, candidates);
    return true;
  }

  if (dim_a >= 6 && dim_a <= 512 && dim_b > dim_a) {
    const size_t attrs = dim_a;
    const size_t rows = dim_b;
    ParseBoxScoreRows(
        rows, attrs,
        [&](size_t row, size_t attr) {
          return data[base_offset + attr * rows + row];
        },
        info, candidates);
    return true;
  }

  return false;
}

std::vector<Candidate> ApplyNms(std::vector<Candidate>* candidates) {
  std::vector<Candidate> selected;
  if (!candidates) {
    return selected;
  }

  std::sort(candidates->begin(), candidates->end(),
            [](const Candidate& a, const Candidate& b) {
              return a.score > b.score;
            });

  for (const Candidate& candidate : *candidates) {
    bool suppress = false;
    for (const Candidate& kept : selected) {
      if (candidate.class_id == kept.class_id &&
          IoU(candidate, kept) > kNmsThreshold) {
        suppress = true;
        break;
      }
    }
    if (suppress) {
      continue;
    }

    selected.push_back(candidate);
    if (selected.size() >= kMaxDetections) {
      break;
    }
  }

  return selected;
}

std::vector<YoloDetectionBox> ToDetectionBoxes(
    const std::vector<Candidate>& candidates,
    const PreprocessInfo& info,
    uint64_t frame_sequence) {
  std::vector<YoloDetectionBox> boxes;
  boxes.reserve(candidates.size());

  for (size_t i = 0; i < candidates.size(); ++i) {
    const Candidate& candidate = candidates[i];
    YoloDetectionBox box;
    box.class_id = static_cast<uint32_t>(candidate.class_id);
    box.confidence =
        static_cast<uint32_t>(std::lround(Clamp(candidate.score, 0.0f, 1.0f) *
                                          1000.0f));
    box.x = ToNormU16(candidate.x1, static_cast<float>(info.original_width));
    box.y = ToNormU16(candidate.y1, static_cast<float>(info.original_height));
    box.w = ToNormU16(candidate.x2 - candidate.x1,
                      static_cast<float>(info.original_width));
    box.h = ToNormU16(candidate.y2 - candidate.y1,
                      static_cast<float>(info.original_height));
    box.has_detection_id = true;
    box.detection_id =
        static_cast<uint32_t>((frame_sequence * 131u + i) & 0xffffffffu);
    boxes.push_back(box);
  }

  return boxes;
}

class TensorRtLogger : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* msg) noexcept override {
    if (!msg || severity > Severity::kWARNING) {
      return;
    }
    if (severity <= Severity::kERROR) {
      rtc_logging::LogError(std::string("TensorRT: ") + msg);
    } else {
      rtc_logging::LogInfo(std::string("TensorRT: ") + msg);
    }
  }
};

template <typename T>
struct TensorRtDeleter {
  void operator()(T* value) const {
    delete value;
  }
};

template <typename T>
using TrtUniquePtr = std::unique_ptr<T, TensorRtDeleter<T> >;

class DeviceBuffer {
 public:
  DeviceBuffer() = default;
  ~DeviceBuffer() { Reset(); }

  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  DeviceBuffer(DeviceBuffer&& other) noexcept {
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }

  bool Resize(size_t bytes, std::string* error_message) {
    if (bytes <= capacity_) {
      size_ = bytes;
      return true;
    }
    Reset();
    if (!SetCudaError("cudaMalloc", cudaMalloc(&data_, bytes),
                      error_message)) {
      return false;
    }
    capacity_ = bytes;
    size_ = bytes;
    return true;
  }

  void Reset() {
    if (data_) {
      cudaFree(data_);
      data_ = nullptr;
    }
    size_ = 0;
    capacity_ = 0;
  }

  void* data() const { return data_; }
 private:
  void* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

class HostPinnedBuffer {
 public:
  HostPinnedBuffer() = default;
  ~HostPinnedBuffer() { Reset(); }

  HostPinnedBuffer(const HostPinnedBuffer&) = delete;
  HostPinnedBuffer& operator=(const HostPinnedBuffer&) = delete;
  HostPinnedBuffer(HostPinnedBuffer&& other) noexcept {
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }

  bool Resize(size_t bytes, std::string* error_message) {
    if (bytes <= capacity_) {
      size_ = bytes;
      return true;
    }
    Reset();
    if (!SetCudaError("cudaMallocHost", cudaMallocHost(&data_, bytes),
                      error_message)) {
      return false;
    }
    capacity_ = bytes;
    size_ = bytes;
    return true;
  }

  void Reset() {
    if (data_) {
      cudaFreeHost(data_);
      data_ = nullptr;
    }
    size_ = 0;
    capacity_ = 0;
  }

  void* data() const { return data_; }
 private:
  void* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

__global__ void I420ToRgbNchwLetterboxKernel(const uint8_t* src_y,
                                             size_t src_stride_y,
                                             const uint8_t* src_u,
                                             size_t src_stride_u,
                                             const uint8_t* src_v,
                                             size_t src_stride_v,
                                             int src_width,
                                             int src_height,
                                             float* dst,
                                             int input_width,
                                             int input_height,
                                             int resized_width,
                                             int resized_height,
                                             int pad_x,
                                             int pad_y) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  const int image_size = input_width * input_height;
  if (index >= image_size) {
    return;
  }

  const int dst_y = index / input_width;
  const int dst_x = index - dst_y * input_width;
  float red = 114.0f / 255.0f;
  float green = 114.0f / 255.0f;
  float blue = 114.0f / 255.0f;

  const int inner_x = dst_x - pad_x;
  const int inner_y = dst_y - pad_y;
  if (inner_x >= 0 && inner_x < resized_width && inner_y >= 0 &&
      inner_y < resized_height) {
    const int src_x =
        min(src_width - 1,
            static_cast<int>(static_cast<long long>(inner_x) * src_width /
                             resized_width));
    const int src_y_index =
        min(src_height - 1,
            static_cast<int>(static_cast<long long>(inner_y) * src_height /
                             resized_height));
    const int src_uv_x = src_x / 2;
    const int src_uv_y = src_y_index / 2;

    const int y_value = src_y[src_y_index * src_stride_y + src_x];
    const int u_value = src_u[src_uv_y * src_stride_u + src_uv_x];
    const int v_value = src_v[src_uv_y * src_stride_v + src_uv_x];
    const int c = y_value - 16;
    const int d = u_value - 128;
    const int e = v_value - 128;

    red = ClampToByteDevice((298 * c + 409 * e + 128) >> 8) / 255.0f;
    green =
        ClampToByteDevice((298 * c - 100 * d - 208 * e + 128) >> 8) / 255.0f;
    blue = ClampToByteDevice((298 * c + 516 * d + 128) >> 8) / 255.0f;
  }

  dst[index] = red;
  dst[image_size + index] = green;
  dst[image_size * 2 + index] = blue;
}

bool LaunchPreprocessKernel(const rtc_dual_camera::ImageFrame& frame,
                            const PreprocessInfo& info,
                            DeviceBuffer* input_device,
                            DeviceBuffer* frame_device,
                            cudaStream_t stream,
                            std::string* error_message) {
  if (!input_device || !frame_device) {
    if (error_message) {
      *error_message = "null CUDA preprocessing buffer";
    }
    return false;
  }

  const size_t chroma_height = (frame.height + 1) / 2;
  const size_t y_bytes = frame.stride_y * frame.height;
  const size_t u_bytes = frame.stride_u * chroma_height;
  const size_t v_bytes = frame.stride_v * chroma_height;
  const size_t frame_bytes = y_bytes + u_bytes + v_bytes;
  const size_t input_bytes =
      static_cast<size_t>(info.input_width) * info.input_height * 3 *
      sizeof(float);

  if (!frame_device->Resize(frame_bytes, error_message) ||
      !input_device->Resize(input_bytes, error_message)) {
    return false;
  }

  if (!SetCudaError("cudaMemcpyAsync(frame to device)",
                    cudaMemcpyAsync(frame_device->data(), frame.data,
                                    frame_bytes, cudaMemcpyHostToDevice,
                                    stream),
                    error_message)) {
    return false;
  }

  const uint8_t* device_frame =
      static_cast<const uint8_t*>(frame_device->data());
  const uint8_t* device_y = device_frame;
  const uint8_t* device_u = device_y + y_bytes;
  const uint8_t* device_v = device_u + u_bytes;
  const int image_size = info.input_width * info.input_height;
  const int threads = 256;
  const int blocks = (image_size + threads - 1) / threads;
  I420ToRgbNchwLetterboxKernel<<<blocks, threads, 0, stream>>>(
      device_y, frame.stride_y, device_u, frame.stride_u, device_v,
      frame.stride_v, static_cast<int>(frame.width),
      static_cast<int>(frame.height), static_cast<float*>(input_device->data()),
      info.input_width, info.input_height, info.resized_width,
      info.resized_height, info.pad_x, info.pad_y);

  return SetCudaError("I420ToRgbNchwLetterboxKernel",
                      cudaGetLastError(), error_message);
}

bool BuildSerializedEngine(const std::string& model_path,
                           const std::string& engine_path,
                           const ModelFingerprint& model_fingerprint,
                           TensorRtLogger* logger,
                           std::vector<uint8_t>* serialized_engine,
                           int* input_width,
                           int* input_height,
                           std::string* error_message) {
  if (!logger || !serialized_engine || !input_width || !input_height) {
    if (error_message) {
      *error_message = "null TensorRT engine build output";
    }
    return false;
  }

  TrtUniquePtr<nvinfer1::IBuilder> builder(
      nvinfer1::createInferBuilder(*logger));
  if (!builder) {
    if (error_message) {
      *error_message = "failed to create TensorRT builder";
    }
    return false;
  }

  TrtUniquePtr<nvinfer1::INetworkDefinition> network(
      builder->createNetworkV2(0));
  TrtUniquePtr<nvonnxparser::IParser> parser(
      nvonnxparser::createParser(*network, *logger));
  if (!network || !parser) {
    if (error_message) {
      *error_message = "failed to create TensorRT ONNX parser";
    }
    return false;
  }

  if (!parser->parseFromFile(
          model_path.c_str(),
          static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
    std::ostringstream oss;
    oss << "failed to parse ONNX model with TensorRT";
    for (int i = 0; i < parser->getNbErrors(); ++i) {
      const nvonnxparser::IParserError* error = parser->getError(i);
      if (error) {
        oss << "; " << error->desc();
      }
    }
    if (error_message) {
      *error_message = oss.str();
    }
    return false;
  }

  if (network->getNbInputs() != 1) {
    if (error_message) {
      std::ostringstream oss;
      oss << "expected 1 YOLO input, got " << network->getNbInputs();
      *error_message = oss.str();
    }
    return false;
  }

  nvinfer1::ITensor* input = network->getInput(0);
  if (!input) {
    if (error_message) {
      *error_message = "TensorRT network input is null";
    }
    return false;
  }

  nvinfer1::Dims concrete_input_shape;
  if (!FillNchwInputShape(input->getDimensions(), input_width, input_height,
                          &concrete_input_shape, error_message)) {
    return false;
  }

  TrtUniquePtr<nvinfer1::IBuilderConfig> config(
      builder->createBuilderConfig());
  if (!config) {
    if (error_message) {
      *error_message = "failed to create TensorRT builder config";
    }
    return false;
  }
  config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                             kTensorRtWorkspaceBytes);
  config->setBuilderOptimizationLevel(3);
  if (builder->platformHasFastFp16()) {
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
  }

  if (HasDynamicDim(input->getDimensions())) {
    nvinfer1::IOptimizationProfile* profile =
        builder->createOptimizationProfile();
    if (!profile) {
      if (error_message) {
        *error_message = "failed to create TensorRT optimization profile";
      }
      return false;
    }
    if (!profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kMIN,
                                concrete_input_shape) ||
        !profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kOPT,
                                concrete_input_shape) ||
        !profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kMAX,
                                concrete_input_shape) ||
        profile->isValid() == false ||
        config->addOptimizationProfile(profile) < 0) {
      if (error_message) {
        *error_message = "failed to configure TensorRT optimization profile";
      }
      return false;
    }
  }

  rtc_logging::LogInfo("building TensorRT YOLO engine; first launch can take a while");
  TrtUniquePtr<nvinfer1::IHostMemory> engine_plan(
      builder->buildSerializedNetwork(*network, *config));
  if (!engine_plan) {
    if (error_message) {
      *error_message = "failed to build TensorRT engine";
    }
    return false;
  }

  serialized_engine->assign(
      static_cast<const uint8_t*>(engine_plan->data()),
      static_cast<const uint8_t*>(engine_plan->data()) + engine_plan->size());
  if (!WriteBinaryFile(engine_path, serialized_engine->data(),
                       serialized_engine->size(), error_message)) {
    rtc_logging::LogError("TensorRT engine built but cache write failed: " +
             (error_message ? *error_message : std::string()));
  } else {
    rtc_logging::LogInfo("TensorRT YOLO engine cached: " + engine_path);
    std::string metadata_error;
    if (!WriteEngineCacheMetadata(TensorRtEngineMetadataPath(engine_path),
                                  model_fingerprint, &metadata_error)) {
      rtc_logging::LogError("TensorRT YOLO metadata write failed: " + metadata_error);
    }
  }
  return true;
}

bool LoadOrBuildEngine(const std::string& model_path,
                       const std::string& engine_path,
                       TensorRtLogger* logger,
                       std::vector<uint8_t>* serialized_engine,
                       int* input_width,
                       int* input_height,
                       std::string* error_message) {
  ModelFingerprint model_fingerprint;
  if (!ComputeModelFingerprint(model_path, &model_fingerprint,
                               error_message)) {
    return false;
  }

  if (EngineCacheIsUsable(engine_path, model_fingerprint)) {
    std::string read_error;
    if (ReadBinaryFile(engine_path, serialized_engine, &read_error)) {
      return true;
    }
    rtc_logging::LogError("TensorRT YOLO engine cache read failed; rebuilding: " +
             read_error);
  }

  return BuildSerializedEngine(model_path, engine_path, model_fingerprint,
                               logger,
                               serialized_engine, input_width, input_height,
                               error_message);
}

}  // namespace

struct YoloOnnxDetector::Impl {
  explicit Impl(const std::string& path)
      : model_path(path),
        engine_path(TensorRtEnginePath(path)) {}

  TensorRtLogger logger;
  TrtUniquePtr<nvinfer1::IRuntime> runtime;
  TrtUniquePtr<nvinfer1::ICudaEngine> engine;
  TrtUniquePtr<nvinfer1::IExecutionContext> context;
  std::string model_path;
  std::string engine_path;
  std::string input_name;
  std::vector<std::string> output_names;
  std::vector<nvinfer1::Dims> output_shapes;
  std::vector<size_t> output_sizes;
  std::vector<DeviceBuffer> output_device_buffers;
  std::vector<HostPinnedBuffer> output_host_buffers;
  DeviceBuffer input_device_buffer;
  DeviceBuffer frame_device_buffer;
  cudaStream_t stream = nullptr;
  int input_width = kDefaultInputSize;
  int input_height = kDefaultInputSize;
};

YoloOnnxDetector::YoloOnnxDetector(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

YoloOnnxDetector::~YoloOnnxDetector() {
  if (impl_ && impl_->stream) {
    cudaStreamSynchronize(impl_->stream);
    cudaStreamDestroy(impl_->stream);
    impl_->stream = nullptr;
  }
}

std::unique_ptr<YoloOnnxDetector> YoloOnnxDetector::CreateDefault(
    std::string* error_message) {
  return Create(ResolveDefaultModelPath(), error_message);
}

std::unique_ptr<YoloOnnxDetector> YoloOnnxDetector::Create(
    const std::string& model_path,
    std::string* error_message) {
  if (!FileExists(model_path)) {
    if (error_message) {
      *error_message = "YOLO model not found: " + model_path;
    }
    return std::unique_ptr<YoloOnnxDetector>();
  }

  int device_count = 0;
  cudaError_t cuda_code = cudaGetDeviceCount(&device_count);
  if (cuda_code != cudaSuccess || device_count <= 0) {
    if (error_message) {
      *error_message = cuda_code == cudaSuccess
                           ? "no CUDA device found"
                           : MakeCudaError("cudaGetDeviceCount", cuda_code);
    }
    return std::unique_ptr<YoloOnnxDetector>();
  }
  if (!SetCudaError("cudaSetDevice", cudaSetDevice(0), error_message)) {
    return std::unique_ptr<YoloOnnxDetector>();
  }

  try {
    std::unique_ptr<Impl> impl(new Impl(model_path));
    if (!initLibNvInferPlugins(&impl->logger, "")) {
      if (error_message) {
        *error_message = "failed to initialize TensorRT plugins";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }
    if (!SetCudaError("cudaStreamCreateWithFlags",
                      cudaStreamCreateWithFlags(&impl->stream,
                                                cudaStreamNonBlocking),
                      error_message)) {
      return std::unique_ptr<YoloOnnxDetector>();
    }

    std::vector<uint8_t> serialized_engine;
    if (!LoadOrBuildEngine(model_path, impl->engine_path, &impl->logger,
                           &serialized_engine, &impl->input_width,
                           &impl->input_height, error_message)) {
      return std::unique_ptr<YoloOnnxDetector>();
    }

    impl->runtime.reset(nvinfer1::createInferRuntime(impl->logger));
    if (!impl->runtime) {
      if (error_message) {
        *error_message = "failed to create TensorRT runtime";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    impl->engine.reset(impl->runtime->deserializeCudaEngine(
        serialized_engine.data(), serialized_engine.size()));
    if (!impl->engine) {
      if (error_message) {
        *error_message = "failed to deserialize TensorRT engine";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    int input_count = 0;
    for (int i = 0; i < impl->engine->getNbIOTensors(); ++i) {
      const char* name = impl->engine->getIOTensorName(i);
      if (!name) {
        continue;
      }
      const nvinfer1::TensorIOMode mode =
          impl->engine->getTensorIOMode(name);
      if (mode == nvinfer1::TensorIOMode::kINPUT) {
        impl->input_name = name;
        ++input_count;
      } else if (mode == nvinfer1::TensorIOMode::kOUTPUT) {
        if (impl->engine->getTensorDataType(name) !=
            nvinfer1::DataType::kFLOAT) {
          if (error_message) {
            *error_message = "YOLO TensorRT output is not float: " +
                             std::string(name);
          }
          return std::unique_ptr<YoloOnnxDetector>();
        }
        impl->output_names.push_back(name);
      }
    }
    if (input_count != 1 || impl->output_names.empty()) {
      if (error_message) {
        std::ostringstream oss;
        oss << "invalid TensorRT YOLO IO tensors: inputs=" << input_count
            << " outputs=" << impl->output_names.size();
        *error_message = oss.str();
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    nvinfer1::Dims concrete_input_shape;
    if (!ResolveEngineInputShape(impl->engine.get(), impl->input_name,
                                 &impl->input_width, &impl->input_height,
                                 &concrete_input_shape, error_message)) {
      return std::unique_ptr<YoloOnnxDetector>();
    }
    const nvinfer1::Dims input_shape =
        impl->engine->getTensorShape(impl->input_name.c_str());

    impl->context.reset(impl->engine->createExecutionContext());
    if (!impl->context) {
      if (error_message) {
        *error_message = "failed to create TensorRT execution context";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    if (HasDynamicDim(input_shape) &&
        !impl->context->setInputShape(impl->input_name.c_str(),
                                      concrete_input_shape)) {
      if (error_message) {
        *error_message = "failed to set TensorRT input shape";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    const size_t input_bytes =
        static_cast<size_t>(impl->input_width) * impl->input_height * 3 *
        sizeof(float);
    if (!impl->input_device_buffer.Resize(input_bytes, error_message)) {
      return std::unique_ptr<YoloOnnxDetector>();
    }
    if (!impl->context->setTensorAddress(
            impl->input_name.c_str(), impl->input_device_buffer.data())) {
      if (error_message) {
        *error_message = "failed to set TensorRT input address";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    const int32_t infer_shape_result = impl->context->inferShapes(0, nullptr);
    if (infer_shape_result != 0) {
      if (error_message) {
        std::ostringstream oss;
        oss << "TensorRT shape inference failed: " << infer_shape_result;
        *error_message = oss.str();
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    impl->output_shapes.reserve(impl->output_names.size());
    impl->output_sizes.reserve(impl->output_names.size());
    impl->output_device_buffers.resize(impl->output_names.size());
    impl->output_host_buffers.resize(impl->output_names.size());
    for (size_t i = 0; i < impl->output_names.size(); ++i) {
      const std::string& name = impl->output_names[i];
      nvinfer1::Dims shape = impl->context->getTensorShape(name.c_str());
      if (HasDynamicDim(shape)) {
        if (error_message) {
          *error_message = "unresolved TensorRT output shape " +
                           ShapeToString(shape) + " for " + name;
        }
        return std::unique_ptr<YoloOnnxDetector>();
      }
      const size_t element_count = Volume(shape);
      if (element_count == 0) {
        if (error_message) {
          *error_message = "invalid TensorRT output shape " +
                           ShapeToString(shape) + " for " + name;
        }
        return std::unique_ptr<YoloOnnxDetector>();
      }
      const size_t output_bytes = element_count * sizeof(float);
      if (!impl->output_device_buffers[i].Resize(output_bytes,
                                                 error_message) ||
          !impl->output_host_buffers[i].Resize(output_bytes,
                                               error_message)) {
        return std::unique_ptr<YoloOnnxDetector>();
      }
      if (!impl->context->setTensorAddress(
              name.c_str(), impl->output_device_buffers[i].data())) {
        if (error_message) {
          *error_message = "failed to set TensorRT output address for " + name;
        }
        return std::unique_ptr<YoloOnnxDetector>();
      }
      impl->output_shapes.push_back(shape);
      impl->output_sizes.push_back(element_count);
    }

    std::ostringstream oss;
    oss << "YOLO TensorRT engine loaded: " << impl->engine_path
        << " input=" << impl->input_width << "x" << impl->input_height
        << " outputs=" << impl->output_names.size();
    rtc_logging::LogInfo(oss.str());

    return std::unique_ptr<YoloOnnxDetector>(
        new YoloOnnxDetector(std::move(impl)));
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = std::string("failed to load YOLO TensorRT model: ") +
                       ex.what();
    }
    return std::unique_ptr<YoloOnnxDetector>();
  }
}

bool YoloOnnxDetector::Detect(const rtc_dual_camera::ImageFrame& frame,
                              std::vector<YoloDetectionBox>* boxes,
                              std::string* error_message) {
  if (!boxes) {
    if (error_message) {
      *error_message = "null detection output";
    }
    return false;
  }
  boxes->clear();

  PreprocessInfo preprocess;
  if (!BuildPreprocessInfo(frame, impl_->input_width, impl_->input_height,
                           &preprocess, error_message)) {
    return false;
  }

  if (!LaunchPreprocessKernel(frame, preprocess, &impl_->input_device_buffer,
                              &impl_->frame_device_buffer, impl_->stream,
                              error_message)) {
    return false;
  }

  if (!impl_->context->enqueueV3(impl_->stream)) {
    if (error_message) {
      *error_message = "TensorRT enqueueV3 failed";
    }
    return false;
  }

  for (size_t i = 0; i < impl_->output_names.size(); ++i) {
    const size_t output_bytes = impl_->output_sizes[i] * sizeof(float);
    if (!SetCudaError("cudaMemcpyAsync(output to host)",
                      cudaMemcpyAsync(impl_->output_host_buffers[i].data(),
                                      impl_->output_device_buffers[i].data(),
                                      output_bytes, cudaMemcpyDeviceToHost,
                                      impl_->stream),
                      error_message)) {
      return false;
    }
  }

  if (!SetCudaError("cudaStreamSynchronize",
                    cudaStreamSynchronize(impl_->stream), error_message)) {
    return false;
  }

  std::vector<Candidate> candidates;
  for (size_t i = 0; i < impl_->output_names.size(); ++i) {
    std::string parse_error;
    ParseTensorOutput(
        static_cast<const float*>(impl_->output_host_buffers[i].data()),
        impl_->output_shapes[i], preprocess, &candidates, &parse_error);
  }

  const std::vector<Candidate> selected = ApplyNms(&candidates);
  *boxes = ToDetectionBoxes(selected, preprocess, frame.sequence);
  return true;
}

const std::string& YoloOnnxDetector::model_path() const {
  return impl_->model_path;
}

}  // namespace rtc_camera_headless
