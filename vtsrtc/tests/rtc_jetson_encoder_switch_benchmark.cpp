#include <linux/videodev2.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include "api/video/i420_buffer.h"
#include "log/log_manager.h"
#include "video/encode/nvidia-jetson/jetson_encoder.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Resolution {
  int width;
  int height;
};

struct CallbackState {
  std::mutex mutex;
  std::condition_variable condition;
  uint64_t count = 0;
  Clock::time_point last_callback_time;
  bool last_keyframe = false;
  bool last_idr = false;
  bool last_sps = false;
};

struct SwitchSample {
  Resolution from;
  Resolution to;
  double reconfigure_ms = 0.0;
  double callback_gap_ms = 0.0;
  double first_frame_ms = 0.0;
  bool first_frame_keyframe = false;
  bool first_frame_idr = false;
  bool first_frame_sps = false;
};

double Milliseconds(Clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
             duration)
      .count();
}

void FillMovingPattern(const rtc::scoped_refptr<webrtc::I420Buffer>& buffer,
                       uint32_t frame_index) {
  const int width = buffer->width();
  const int height = buffer->height();
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buffer->MutableDataY() + y * buffer->StrideY();
    for (int x = 0; x < width; ++x) {
      row[x] = static_cast<uint8_t>((x + y + frame_index * 7) & 0xff);
    }
  }

  const int chroma_width = (width + 1) / 2;
  const int chroma_height = (height + 1) / 2;
  for (int y = 0; y < chroma_height; ++y) {
    uint8_t* u_row = buffer->MutableDataU() + y * buffer->StrideU();
    uint8_t* v_row = buffer->MutableDataV() + y * buffer->StrideV();
    for (int x = 0; x < chroma_width; ++x) {
      u_row[x] = static_cast<uint8_t>(96 + ((x + frame_index * 3) & 0x3f));
      v_row[x] = static_cast<uint8_t>(96 + ((y + frame_index * 5) & 0x3f));
    }
  }
}

bool ContainsNalType(const uint8_t* data, size_t size, uint8_t nal_type) {
  for (size_t i = 0; i + 4 < size; ++i) {
    size_t header = size;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      header = i + 3;
    } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
               data[i + 2] == 0 && data[i + 3] == 1) {
      header = i + 4;
    }
    if (header < size && (data[header] & 0x1f) == nal_type) {
      return true;
    }
  }
  return false;
}

bool SubmitAndWait(webrtc::JetsonEncoder* encoder,
                   const rtc::scoped_refptr<webrtc::I420Buffer>& buffer,
                   CallbackState* state, uint32_t frame_index,
                   double* callback_latency_ms, bool* keyframe, bool* idr,
                   bool* sps) {
  uint64_t expected_count = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    expected_count = state->count + 1;
  }

  const Clock::time_point submit_time = Clock::now();
  std::unique_lock<std::mutex> lock(state->mutex);
  for (int attempt = 0; attempt < 4; ++attempt) {
    lock.unlock();
    FillMovingPattern(buffer, frame_index + static_cast<uint32_t>(attempt));
    encoder->EmplaceBuffer(
        buffer,
        [state](const uint8_t* data, size_t size, bool is_keyframe, uint64_t) {
          {
            std::lock_guard<std::mutex> callback_lock(state->mutex);
            ++state->count;
            state->last_callback_time = Clock::now();
            state->last_keyframe = is_keyframe;
            state->last_idr = ContainsNalType(data, size, 5);
            state->last_sps = ContainsNalType(data, size, 7);
          }
          state->condition.notify_all();
        });
    lock.lock();
    if (state->condition.wait_for(lock, std::chrono::milliseconds(34), [&]() {
          return state->count >= expected_count;
        })) {
      break;
    }
  }

  if (state->count < expected_count) {
    return false;
  }

  if (callback_latency_ms) {
    *callback_latency_ms = Milliseconds(state->last_callback_time - submit_time);
  }
  if (keyframe) {
    *keyframe = state->last_keyframe;
  }
  if (idr) {
    *idr = state->last_idr;
  }
  if (sps) {
    *sps = state->last_sps;
  }
  return true;
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = percentile * static_cast<double>(values.size() - 1);
  const size_t lower = static_cast<size_t>(position);
  const size_t upper = std::min(lower + 1, values.size() - 1);
  const double fraction = position - static_cast<double>(lower);
  return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

void PrintSummary(const char* name, const std::vector<double>& values) {
  if (values.empty()) {
    return;
  }
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  std::cout << "SUMMARY metric=" << name << " samples=" << values.size()
            << " avg_ms=" << sum / values.size()
            << " p50_ms=" << Percentile(values, 0.50)
            << " p95_ms=" << Percentile(values, 0.95)
            << " p99_ms=" << Percentile(values, 0.99)
            << " max_ms=" << *std::max_element(values.begin(), values.end())
            << std::endl;
}

int ParseIterations(int argc, char** argv) {
  if (argc < 2) {
    return 10;
  }
  const int value = std::atoi(argv[1]);
  return std::max(1, value);
}

std::unique_ptr<webrtc::JetsonEncoder> CreateWarmEncoder(
    const Resolution& resolution,
    const webrtc::JetsonEncoder::StrategyConfig& strategy) {
  auto encoder = webrtc::JetsonEncoder::Create(
      resolution.width, resolution.height, V4L2_PIX_FMT_H264, false, strategy,
      30, 6000000);
  if (!encoder) {
    return nullptr;
  }

  auto state = std::make_shared<CallbackState>();
  auto buffer =
      webrtc::I420Buffer::Create(resolution.width, resolution.height);
  FillMovingPattern(buffer, 0);
  encoder->ForceKeyFrame();
  encoder->EmplaceBuffer(
      buffer,
      [state](const uint8_t*, size_t, bool, uint64_t) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          ++state->count;
        }
        state->condition.notify_all();
      });
  std::unique_lock<std::mutex> lock(state->mutex);
  if (!state->condition.wait_for(lock, std::chrono::seconds(1),
                                 [state]() { return state->count > 0; })) {
    return nullptr;
  }
  return encoder;
}

}  // namespace

int main(int argc, char** argv) {
  LogInst->init("/tmp/rtc_jetson_encoder_switch_benchmark");

  const int iterations = ParseIterations(argc, argv);
  const std::vector<Resolution> resolutions = {
      {1920, 1200}, {1280, 800}, {960, 600}, {640, 400},
      {960, 600},   {1280, 800}, {1920, 1200},
  };

  webrtc::JetsonEncoder::StrategyConfig strategy;
  strategy.bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
  strategy.gop_size = 3000;

  std::unique_ptr<webrtc::JetsonEncoder> active_encoder =
      CreateWarmEncoder(resolutions.front(), strategy);
  std::unique_ptr<webrtc::JetsonEncoder> standby_encoder =
      CreateWarmEncoder(resolutions.front(), strategy);
  if (!active_encoder || !standby_encoder) {
    std::cerr << "无法创建并预热双 Jetson 编码器" << std::endl;
    return 1;
  }

  CallbackState callback_state;
  uint32_t frame_index = 0;
  auto initial_buffer = webrtc::I420Buffer::Create(
      resolutions.front().width, resolutions.front().height);
  for (int i = 0; i < 12; ++i) {
    if (!SubmitAndWait(active_encoder.get(), initial_buffer, &callback_state,
                       frame_index++, nullptr, nullptr, nullptr, nullptr)) {
      std::cerr << "预热帧编码超时" << std::endl;
      return 2;
    }
  }

  std::vector<SwitchSample> samples;
  std::vector<double> standby_refresh_values;
  Resolution current = resolutions.front();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    for (size_t i = 1; i < resolutions.size(); ++i) {
      const Resolution target = resolutions[i];
      Clock::time_point previous_callback;
      {
        std::lock_guard<std::mutex> lock(callback_state.mutex);
        previous_callback = callback_state.last_callback_time;
      }

      const Clock::time_point reconfigure_start = Clock::now();
      const bool upscale =
          target.width > current.width || target.height > current.height;
      std::unique_ptr<webrtc::JetsonEncoder> retired_encoder;
      bool switch_ok = false;
      if (!upscale) {
        switch_ok = active_encoder->Reconfigure(target.width, target.height);
      } else if (standby_encoder) {
        if (target.width == resolutions.front().width &&
            target.height == resolutions.front().height) {
          standby_encoder->ForceKeyFrame();
          switch_ok = true;
        } else {
          switch_ok =
              standby_encoder->Reconfigure(target.width, target.height);
        }
        if (switch_ok) {
          retired_encoder = std::move(active_encoder);
          active_encoder = std::move(standby_encoder);
        }
      }
      if (!switch_ok) {
        std::cerr << "分辨率重配失败: " << current.width << "x"
                  << current.height << " -> " << target.width << "x"
                  << target.height << std::endl;
        return 3;
      }
      const Clock::time_point reconfigure_end = Clock::now();
      const double reconfigure_ms =
          Milliseconds(reconfigure_end - reconfigure_start);
      std::cout << std::fixed << std::setprecision(3)
                << "RECONFIGURE iteration=" << iteration << " from="
                << current.width << "x" << current.height << " to="
                << target.width << "x" << target.height
                << " reconfigure_ms=" << reconfigure_ms << std::endl;

      auto target_buffer =
          webrtc::I420Buffer::Create(target.width, target.height);
      double first_frame_ms = 0.0;
      bool first_frame_keyframe = false;
      bool first_frame_idr = false;
      bool first_frame_sps = false;
      if (!SubmitAndWait(active_encoder.get(), target_buffer, &callback_state,
                         frame_index++, &first_frame_ms,
                         &first_frame_keyframe, &first_frame_idr,
                         &first_frame_sps)) {
        std::cerr << "切换后首帧编码超时" << std::endl;
        return 4;
      }

      Clock::time_point first_callback;
      {
        std::lock_guard<std::mutex> lock(callback_state.mutex);
        first_callback = callback_state.last_callback_time;
      }

      SwitchSample sample;
      sample.from = current;
      sample.to = target;
      sample.reconfigure_ms = reconfigure_ms;
      sample.callback_gap_ms =
          Milliseconds(first_callback - previous_callback);
      sample.first_frame_ms = first_frame_ms;
      sample.first_frame_keyframe = first_frame_keyframe;
      sample.first_frame_idr = first_frame_idr;
      sample.first_frame_sps = first_frame_sps;
      samples.push_back(sample);

      std::cout << std::fixed << std::setprecision(3)
                << "SAMPLE iteration=" << iteration << " from="
                << current.width << "x" << current.height << " to="
                << target.width << "x" << target.height
                << " reconfigure_ms=" << sample.reconfigure_ms
                << " callback_gap_ms=" << sample.callback_gap_ms
                << " first_frame_ms=" << sample.first_frame_ms
                << " keyframe=" << (sample.first_frame_keyframe ? 1 : 0)
                << " idr=" << (sample.first_frame_idr ? 1 : 0)
                << " sps=" << (sample.first_frame_sps ? 1 : 0)
                << std::endl;

      for (int frame = 0; frame < 3; ++frame) {
        if (!SubmitAndWait(active_encoder.get(), target_buffer,
                           &callback_state,
                           frame_index++, nullptr, nullptr, nullptr,
                           nullptr)) {
          std::cerr << "稳定帧编码超时" << std::endl;
          return 5;
        }
      }

      if (upscale) {
        const Clock::time_point refresh_start = Clock::now();
        // 实际实现在线程中销毁旧会话并补充最高分辨率热备，这里同步执行以
        // 测量后台任务成本，但不计入上面的实时切换间隔。
        retired_encoder.reset();
        standby_encoder = CreateWarmEncoder(resolutions.front(), strategy);
        const double refresh_ms =
            Milliseconds(Clock::now() - refresh_start);
        if (!standby_encoder) {
          std::cerr << "补充热备编码器失败" << std::endl;
          return 8;
        }
        standby_refresh_values.push_back(refresh_ms);
        std::cout << "STANDBY_REFRESH iteration=" << iteration
                  << " target=" << target.width << "x" << target.height
                  << " refresh_ms=" << refresh_ms << std::endl;

        // 基准程序同步补充热备；实际实现会在后台完成。补一帧用于模拟热备
        // 创建期间主编码器持续输出，避免把后台耗时计入下一次切换间隔。
        if (!SubmitAndWait(active_encoder.get(), target_buffer,
                           &callback_state, frame_index++, nullptr, nullptr,
                           nullptr, nullptr)) {
          std::cerr << "热备补充后的连续编码超时" << std::endl;
          return 9;
        }
      }

      current = target;
    }

    if (current.width != resolutions.front().width ||
        current.height != resolutions.front().height) {
      std::cerr << "分辨率序列没有回到初始尺寸" << std::endl;
      return 6;
    }
  }

  std::vector<double> reconfigure_values;
  std::vector<double> callback_gap_values;
  std::vector<double> first_frame_values;
  size_t keyframe_count = 0;
  size_t idr_count = 0;
  size_t sps_count = 0;
  for (const SwitchSample& sample : samples) {
    reconfigure_values.push_back(sample.reconfigure_ms);
    callback_gap_values.push_back(sample.callback_gap_ms);
    first_frame_values.push_back(sample.first_frame_ms);
    if (sample.first_frame_keyframe) {
      ++keyframe_count;
    }
    if (sample.first_frame_idr) {
      ++idr_count;
    }
    if (sample.first_frame_sps) {
      ++sps_count;
    }
  }

  PrintSummary("reconfigure", reconfigure_values);
  PrintSummary("callback_gap", callback_gap_values);
  PrintSummary("first_frame", first_frame_values);
  PrintSummary("standby_refresh", standby_refresh_values);
  std::cout << "SUMMARY metric=keyframe samples=" << samples.size()
            << " keyframes=" << keyframe_count << " idr=" << idr_count
            << " sps=" << sps_count << std::endl;
  return idr_count == samples.size() && sps_count == samples.size() ? 0 : 7;
}
