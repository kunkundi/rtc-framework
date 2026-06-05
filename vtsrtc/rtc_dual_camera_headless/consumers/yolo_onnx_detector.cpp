#include "yolo_onnx_detector.h"

#include "rtc_camera_common.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rtc_camera_headless {
namespace {

constexpr int kDefaultInputSize = 640;
constexpr float kConfidenceThreshold = 0.25f;
constexpr float kNmsThreshold = 0.45f;
constexpr size_t kMaxDetections = 64;

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

uint8_t ClampToByte(int value) {
  return static_cast<uint8_t>(Clamp(value, 0, 255));
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

std::string ShapeToString(const std::vector<int64_t>& shape) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) {
      oss << ",";
    }
    oss << shape[i];
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

bool BuildInputTensor(const rtc_dual_camera::ImageFrame& frame,
                      int input_width,
                      int input_height,
                      std::vector<float>* input,
                      PreprocessInfo* info,
                      std::string* error_message) {
  if (!input || !info) {
    if (error_message) {
      *error_message = "null output buffer";
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

  const size_t image_size =
      static_cast<size_t>(input_width) * static_cast<size_t>(input_height);
  input->assign(image_size * 3, 114.0f / 255.0f);

  const uint8_t* y_plane = frame.data;
  const uint8_t* u_plane = y_plane + y_bytes;
  const uint8_t* v_plane = u_plane + u_bytes;
  float* r_plane = input->data();
  float* g_plane = r_plane + image_size;
  float* b_plane = g_plane + image_size;

  for (int y = 0; y < resized_height; ++y) {
    const size_t src_y =
        std::min(frame.height - 1,
                 static_cast<size_t>(static_cast<double>(y) * frame.height /
                                     resized_height));
    const size_t src_uv_y = src_y / 2;
    const int dst_y = y + pad_y;
    for (int x = 0; x < resized_width; ++x) {
      const size_t src_x =
          std::min(frame.width - 1,
                   static_cast<size_t>(static_cast<double>(x) * frame.width /
                                       resized_width));
      const size_t src_uv_x = src_x / 2;
      const int dst_x = x + pad_x;
      const size_t dst_index =
          static_cast<size_t>(dst_y) * input_width + static_cast<size_t>(dst_x);

      const int y_value = y_plane[src_y * frame.stride_y + src_x];
      const int u_value = u_plane[src_uv_y * frame.stride_u + src_uv_x];
      const int v_value = v_plane[src_uv_y * frame.stride_v + src_uv_x];
      const int c = y_value - 16;
      const int d = u_value - 128;
      const int e = v_value - 128;

      const uint8_t red = ClampToByte((298 * c + 409 * e + 128) >> 8);
      const uint8_t green =
          ClampToByte((298 * c - 100 * d - 208 * e + 128) >> 8);
      const uint8_t blue = ClampToByte((298 * c + 516 * d + 128) >> 8);

      r_plane[dst_index] = red / 255.0f;
      g_plane[dst_index] = green / 255.0f;
      b_plane[dst_index] = blue / 255.0f;
    }
  }

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

bool ParseTensorOutput(const Ort::Value& output,
                       const PreprocessInfo& info,
                       std::vector<Candidate>* candidates,
                       std::string* error_message) {
  if (!output.IsTensor()) {
    return false;
  }

  const Ort::TensorTypeAndShapeInfo tensor_info =
      output.GetTensorTypeAndShapeInfo();
  if (tensor_info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    return false;
  }

  const std::vector<int64_t> shape = tensor_info.GetShape();
  if (shape.size() != 2 && shape.size() != 3) {
    return false;
  }

  size_t dim_a = 0;
  size_t dim_b = 0;
  size_t base_offset = 0;
  if (shape.size() == 3) {
    if (shape[0] < 1 || shape[1] <= 0 || shape[2] <= 0) {
      if (error_message) {
        *error_message = "invalid YOLO output shape " + ShapeToString(shape);
      }
      return false;
    }
    dim_a = static_cast<size_t>(shape[1]);
    dim_b = static_cast<size_t>(shape[2]);
  } else {
    if (shape[0] <= 0 || shape[1] <= 0) {
      if (error_message) {
        *error_message = "invalid YOLO output shape " + ShapeToString(shape);
      }
      return false;
    }
    dim_a = static_cast<size_t>(shape[0]);
    dim_b = static_cast<size_t>(shape[1]);
  }

  const float* data = output.GetTensorData<float>();
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

}  // namespace

struct YoloOnnxDetector::Impl {
  explicit Impl(const std::string& path)
      : env(ORT_LOGGING_LEVEL_WARNING, "vtsrtc_yolo"),
        model_path(path) {}

  Ort::Env env;
  Ort::SessionOptions session_options;
  std::unique_ptr<Ort::Session> session;
  std::string model_path;
  std::vector<std::string> input_names;
  std::vector<std::string> output_names;
  int input_width = kDefaultInputSize;
  int input_height = kDefaultInputSize;
  std::vector<float> input_buffer;
};

YoloOnnxDetector::YoloOnnxDetector(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

YoloOnnxDetector::~YoloOnnxDetector() = default;

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

  try {
    std::unique_ptr<Impl> impl(new Impl(model_path));
    impl->session_options.SetIntraOpNumThreads(1);
    impl->session_options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    impl->session.reset(new Ort::Session(
        impl->env, model_path.c_str(), impl->session_options));

    Ort::AllocatorWithDefaultOptions allocator;
    const size_t input_count = impl->session->GetInputCount();
    if (input_count != 1) {
      if (error_message) {
        std::ostringstream oss;
        oss << "expected 1 YOLO input, got " << input_count;
        *error_message = oss.str();
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    for (size_t i = 0; i < input_count; ++i) {
      Ort::AllocatedStringPtr name =
          impl->session->GetInputNameAllocated(i, allocator);
      impl->input_names.push_back(name.get());
    }

    const Ort::TypeInfo input_type =
        impl->session->GetInputTypeInfo(0);
    const std::vector<int64_t> input_shape =
        input_type.GetTensorTypeAndShapeInfo().GetShape();
    if (input_shape.size() != 4) {
      if (error_message) {
        *error_message = "expected NCHW YOLO input, got shape " +
                         ShapeToString(input_shape);
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }

    if (input_shape[2] > 0) {
      impl->input_height = static_cast<int>(input_shape[2]);
    }
    if (input_shape[3] > 0) {
      impl->input_width = static_cast<int>(input_shape[3]);
    }
    if (impl->input_width <= 0 || impl->input_height <= 0) {
      impl->input_width = kDefaultInputSize;
      impl->input_height = kDefaultInputSize;
    }

    const size_t output_count = impl->session->GetOutputCount();
    if (output_count == 0) {
      if (error_message) {
        *error_message = "YOLO model has no outputs";
      }
      return std::unique_ptr<YoloOnnxDetector>();
    }
    for (size_t i = 0; i < output_count; ++i) {
      Ort::AllocatedStringPtr name =
          impl->session->GetOutputNameAllocated(i, allocator);
      impl->output_names.push_back(name.get());
    }

    return std::unique_ptr<YoloOnnxDetector>(
        new YoloOnnxDetector(std::move(impl)));
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = std::string("failed to load YOLO ONNX model: ") +
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
  if (!BuildInputTensor(frame, impl_->input_width, impl_->input_height,
                        &impl_->input_buffer, &preprocess, error_message)) {
    return false;
  }

  try {
    std::array<int64_t, 4> input_shape = {
        1, 3, impl_->input_height, impl_->input_width};
    Ort::MemoryInfo memory_info =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, impl_->input_buffer.data(), impl_->input_buffer.size(),
        input_shape.data(), input_shape.size());

    std::vector<const char*> input_names;
    input_names.reserve(impl_->input_names.size());
    for (const std::string& name : impl_->input_names) {
      input_names.push_back(name.c_str());
    }

    std::vector<const char*> output_names;
    output_names.reserve(impl_->output_names.size());
    for (const std::string& name : impl_->output_names) {
      output_names.push_back(name.c_str());
    }

    Ort::RunOptions run_options;
    std::vector<Ort::Value> outputs = impl_->session->Run(
        run_options, input_names.data(), &input_tensor, 1, output_names.data(),
        output_names.size());

    std::vector<Candidate> candidates;
    for (const Ort::Value& output : outputs) {
      std::string parse_error;
      ParseTensorOutput(output, preprocess, &candidates, &parse_error);
    }

    const std::vector<Candidate> selected = ApplyNms(&candidates);
    *boxes = ToDetectionBoxes(selected, preprocess, frame.sequence);
    return true;
  } catch (const std::exception& ex) {
    if (error_message) {
      *error_message = std::string("YOLO inference failed: ") + ex.what();
    }
    return false;
  }
}

const std::string& YoloOnnxDetector::model_path() const {
  return impl_->model_path;
}

}  // namespace rtc_camera_headless
