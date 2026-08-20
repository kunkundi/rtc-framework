#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rtc_audio {

constexpr uint32_t kSystemDefaultDeviceId = 0;

struct AudioDeviceInfo {
  // 设备 ID 仅用于区分本次枚举结果，重新打开设备时应使用名称。
  uint32_t id = kSystemDefaultDeviceId;
  std::string name;
};

struct AudioFrameView {
  size_t bits_per_sample = 0;
  size_t sample_rate = 0;
  size_t number_of_channels = 0;
  size_t number_of_frames = 0;
  const void* data = nullptr;
  size_t data_size = 0;
};

struct AudioCaptureOptions {
  std::string device_name;
  int sample_rate = 48000;
  int channels = 1;
  int frame_duration_ms = 10;
};

struct AudioPlaybackOptions {
  std::string device_name;
};

// 帧数据只在回调调用期间有效，异步处理前必须复制。
using AudioFrameCallback = std::function<void(const AudioFrameView&)>;

bool EnumerateCaptureDevices(std::vector<AudioDeviceInfo>* devices,
                             std::string* error_message);
bool EnumeratePlaybackDevices(std::vector<AudioDeviceInfo>* devices,
                              std::string* error_message);

bool ResolveAudioDeviceName(const std::vector<AudioDeviceInfo>& devices,
                            const std::string& configured_name,
                            uint32_t* device_id,
                            std::string* error_message);

class AudioCapture {
 public:
  AudioCapture();
  ~AudioCapture();

  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  bool Start(const AudioCaptureOptions& options,
             const AudioFrameCallback& callback,
             std::string* error_message);
  void Stop();
  bool running() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class AudioPlayback {
 public:
  AudioPlayback();
  ~AudioPlayback();

  AudioPlayback(const AudioPlayback&) = delete;
  AudioPlayback& operator=(const AudioPlayback&) = delete;

  bool Start(const AudioPlaybackOptions& options,
             std::string* error_message);
  bool PushFrame(const AudioFrameView& frame);
  void Clear();
  void Stop();
  bool running() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // 命名空间 rtc_audio
