#include "rtc_audio/audio_device.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace rtc_audio {
namespace {

constexpr size_t kMaxCaptureQueuedMs = 20;
constexpr size_t kMaxPlaybackQueuedMs = 30;

bool InitializeAudioSubsystem(std::string* error_message) {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return true;
  }
  if (error_message != nullptr) {
    *error_message = std::string("SDL audio initialization failed: ") +
                     SDL_GetError();
  }
  return false;
}

bool EnumerateDevices(bool capture,
                      std::vector<AudioDeviceInfo>* devices,
                      std::string* error_message) {
  if (devices == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Audio device output list must not be null";
    }
    return false;
  }
  devices->clear();
  if (!InitializeAudioSubsystem(error_message)) {
    return false;
  }

  int count = 0;
  SDL_AudioDeviceID* device_ids =
      capture ? SDL_GetAudioRecordingDevices(&count)
              : SDL_GetAudioPlaybackDevices(&count);
  if (device_ids == nullptr) {
    if (error_message != nullptr) {
      *error_message = std::string("Audio device enumeration failed: ") +
                       SDL_GetError();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }

  devices->reserve(static_cast<size_t>(std::max(count, 0)));
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(device_ids[i]);
    AudioDeviceInfo device;
    device.id = static_cast<uint32_t>(device_ids[i]);
    device.name = name != nullptr ? name : "(unknown)";
    devices->push_back(device);
  }
  SDL_free(device_ids);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  return true;
}

SDL_AudioFormat BitsToSdlFormat(size_t bits_per_sample) {
  switch (bits_per_sample) {
    case 8:
      return SDL_AUDIO_S8;
    case 16:
      return SDL_AUDIO_S16;
    case 32:
      return SDL_AUDIO_S32;
    default:
      return SDL_AUDIO_UNKNOWN;
  }
}

bool ResolveConfiguredDevice(bool capture,
                             const std::string& configured_name,
                             SDL_AudioDeviceID* sdl_device_id,
                             std::string* error_message) {
  if (sdl_device_id == nullptr) {
    return false;
  }
  if (configured_name.empty() || configured_name == "default") {
    *sdl_device_id = capture ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
                             : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    return true;
  }

  std::vector<AudioDeviceInfo> devices;
  if (!EnumerateDevices(capture, &devices, error_message)) {
    return false;
  }
  uint32_t resolved_id = kSystemDefaultDeviceId;
  if (!ResolveAudioDeviceName(devices, configured_name, &resolved_id,
                              error_message)) {
    return false;
  }
  *sdl_device_id = static_cast<SDL_AudioDeviceID>(resolved_id);
  return true;
}

}  // 匿名命名空间

bool EnumerateCaptureDevices(std::vector<AudioDeviceInfo>* devices,
                             std::string* error_message) {
  return EnumerateDevices(true, devices, error_message);
}

bool EnumeratePlaybackDevices(std::vector<AudioDeviceInfo>* devices,
                              std::string* error_message) {
  return EnumerateDevices(false, devices, error_message);
}

bool ResolveAudioDeviceName(const std::vector<AudioDeviceInfo>& devices,
                            const std::string& configured_name,
                            uint32_t* device_id,
                            std::string* error_message) {
  if (device_id == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Audio device ID output must not be null";
    }
    return false;
  }
  if (configured_name.empty() || configured_name == "default") {
    *device_id = kSystemDefaultDeviceId;
    return true;
  }
  for (const AudioDeviceInfo& device : devices) {
    if (device.name == configured_name) {
      *device_id = device.id;
      return true;
    }
  }
  if (error_message != nullptr) {
    *error_message = std::string("Audio device not found: ") + configured_name;
    if (!devices.empty()) {
      *error_message += "; available devices:";
      for (const AudioDeviceInfo& device : devices) {
        *error_message += std::string(" [") + device.name + "]";
      }
    }
  }
  return false;
}

class AudioCapture::Impl {
 public:
  ~Impl() { Stop(); }

  bool Start(const AudioCaptureOptions& options,
             const AudioFrameCallback& callback,
             std::string* error_message) {
    Stop();
    if (!callback) {
      if (error_message != nullptr) {
        *error_message = "Audio capture callback must not be empty";
      }
      return false;
    }
    if (options.sample_rate <= 0 || options.sample_rate > 384000 ||
        options.channels <= 0 || options.channels > 32 ||
        options.frame_duration_ms <= 0 || options.frame_duration_ms > 1000 ||
        (static_cast<int64_t>(options.sample_rate) *
             options.frame_duration_ms) %
            1000 != 0) {
      if (error_message != nullptr) {
        *error_message = "Invalid audio capture format";
      }
      return false;
    }
    if (!InitializeAudioSubsystem(error_message)) {
      return false;
    }
    audio_initialized_ = true;

    SDL_AudioSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_S16;
    spec.channels = options.channels;
    spec.freq = options.sample_rate;
    SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!ResolveConfiguredDevice(true, options.device_name, &device_id,
                                 error_message)) {
      Stop();
      return false;
    }
    stream_ =
        SDL_OpenAudioDeviceStream(device_id, &spec, nullptr, nullptr);
    if (stream_ == nullptr) {
      if (error_message != nullptr) {
        *error_message = std::string("Audio capture open failed: ") +
                         SDL_GetError();
      }
      Stop();
      return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      if (error_message != nullptr) {
        *error_message = std::string("Audio capture start failed: ") +
                         SDL_GetError();
      }
      Stop();
      return false;
    }

    options_ = options;
    callback_ = callback;
    stop_requested_.store(false);
    running_.store(true);
    try {
      worker_ = std::thread([this]() { CaptureLoop(); });
    } catch (const std::exception& ex) {
      if (error_message != nullptr) {
        *error_message = std::string("Audio capture worker failed: ") +
                         ex.what();
      }
      Stop();
      return false;
    }
    return true;
  }

  void Stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) {
      worker_.join();
    }
    running_.store(false);
    callback_ = AudioFrameCallback();
    if (stream_ != nullptr) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    if (audio_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      audio_initialized_ = false;
    }
  }

  bool running() const { return running_.load(); }

 private:
  void CaptureLoop() {
    const size_t frames_per_packet =
        static_cast<size_t>(options_.sample_rate) *
        static_cast<size_t>(options_.frame_duration_ms) / 1000;
    const size_t packet_bytes = frames_per_packet *
                                static_cast<size_t>(options_.channels) *
                                sizeof(int16_t);
    std::vector<char> pending;
    pending.reserve(packet_bytes * 4);
    std::vector<char> read_buffer(packet_bytes * 4);
    size_t consumed = 0;
    const size_t max_buffered_packets = std::max<size_t>(
        1, kMaxCaptureQueuedMs /
               static_cast<size_t>(options_.frame_duration_ms));
    const size_t max_buffered_bytes =
        packet_bytes * max_buffered_packets;

    while (!stop_requested_.load()) {
      const int available = SDL_GetAudioStreamAvailable(stream_);
      if (available <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (static_cast<size_t>(available) > max_buffered_bytes) {
        // 实时音频积压时旧数据已经失去价值，直接等待下一批最新采样。
        SDL_ClearAudioStream(stream_);
        pending.clear();
        consumed = 0;
        continue;
      }
      const int read_capacity = static_cast<int>(std::min<size_t>(
          read_buffer.size(), static_cast<size_t>(available)));
      const int read_size =
          SDL_GetAudioStreamData(stream_, read_buffer.data(), read_capacity);
      if (read_size <= 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      pending.insert(pending.end(), read_buffer.data(),
                     read_buffer.data() + read_size);

      const size_t pending_packets =
          (pending.size() - consumed) / packet_bytes;
      if (pending_packets > max_buffered_packets) {
        consumed +=
            (pending_packets - max_buffered_packets) * packet_bytes;
      }

      while (pending.size() - consumed >= packet_bytes) {
        if (stop_requested_.load()) {
          break;
        }
        AudioFrameView frame;
        frame.bits_per_sample = 16;
        frame.sample_rate = static_cast<size_t>(options_.sample_rate);
        frame.number_of_channels = static_cast<size_t>(options_.channels);
        frame.number_of_frames = frames_per_packet;
        frame.data = pending.data() + consumed;
        frame.data_size = packet_bytes;
        callback_(frame);
        consumed += packet_bytes;
      }
      if (consumed > 0 &&
          (consumed == pending.size() || consumed >= packet_bytes * 4)) {
        pending.erase(pending.begin(), pending.begin() + consumed);
        consumed = 0;
      }
    }
  }

  AudioCaptureOptions options_;
  AudioFrameCallback callback_;
  SDL_AudioStream* stream_ = nullptr;
  std::thread worker_;
  std::atomic<bool> stop_requested_{true};
  std::atomic<bool> running_{false};
  bool audio_initialized_ = false;
};

class AudioPlayback::Impl {
 public:
  ~Impl() { Stop(); }

  bool Start(const AudioPlaybackOptions& options,
             std::string* error_message) {
    Stop();
    if (!InitializeAudioSubsystem(error_message)) {
      return false;
    }
    audio_initialized_ = true;
    options_ = options;
    SDL_AudioSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = 48000;
    SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (!ResolveConfiguredDevice(false, options_.device_name, &device_id,
                                 error_message)) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      audio_initialized_ = false;
      return false;
    }
    stream_ =
        SDL_OpenAudioDeviceStream(device_id, &spec, nullptr, nullptr);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
      if (error_message != nullptr) {
        *error_message = std::string("Audio playback open failed: ") +
                         SDL_GetError();
      }
      CloseStream();
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      audio_initialized_ = false;
      return false;
    }
    format_ = spec.format;
    sample_rate_ = static_cast<size_t>(spec.freq);
    channels_ = static_cast<size_t>(spec.channels);
    running_.store(true);
    return true;
  }

  bool PushFrame(const AudioFrameView& frame) {
    if (!running_.load() || frame.data == nullptr || frame.data_size == 0 ||
        frame.sample_rate == 0 || frame.sample_rate > 384000 ||
        frame.number_of_channels == 0 || frame.number_of_channels > 32 ||
        frame.number_of_frames == 0) {
      return false;
    }
    const size_t bytes_per_sample = frame.bits_per_sample / 8;
    if (bytes_per_sample == 0 ||
        frame.number_of_channels >
            std::numeric_limits<size_t>::max() / bytes_per_sample ||
        frame.number_of_frames >
            std::numeric_limits<size_t>::max() /
                (bytes_per_sample * frame.number_of_channels)) {
      return false;
    }
    const size_t expected_size = bytes_per_sample *
                                 frame.number_of_channels *
                                 frame.number_of_frames;
    if (expected_size != frame.data_size ||
        frame.sample_rate > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        frame.number_of_channels >
            static_cast<size_t>(std::numeric_limits<int>::max()) ||
        frame.data_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) {
      return false;
    }
    if (!EnsureStream(frame)) {
      return false;
    }
    const size_t bytes_per_second = bytes_per_sample *
                                    frame.number_of_channels *
                                    frame.sample_rate;
    const size_t max_queued_bytes =
        bytes_per_second * kMaxPlaybackQueuedMs / 1000;
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued > 0 &&
        static_cast<size_t>(queued) + frame.data_size > max_queued_bytes) {
      // 播放侧优先保留最新语音，避免声卡时钟漂移累积成长延迟。
      SDL_ClearAudioStream(stream_);
    }
    return SDL_PutAudioStreamData(stream_, frame.data,
                                  static_cast<int>(frame.data_size));
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr) {
      SDL_ClearAudioStream(stream_);
    }
  }

  void Stop() {
    running_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    CloseStream();
    if (audio_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      audio_initialized_ = false;
    }
  }

  bool running() const { return running_.load(); }

 private:
  bool EnsureStream(const AudioFrameView& frame) {
    const SDL_AudioFormat format = BitsToSdlFormat(frame.bits_per_sample);
    if (format == SDL_AUDIO_UNKNOWN) {
      return false;
    }
    if (stream_ != nullptr && format_ == format &&
        sample_rate_ == frame.sample_rate &&
        channels_ == frame.number_of_channels) {
      return true;
    }

    CloseStream();
    SDL_AudioSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    spec.format = format;
    spec.channels = static_cast<int>(frame.number_of_channels);
    spec.freq = static_cast<int>(frame.sample_rate);
    SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    std::string ignored_error;
    if (!ResolveConfiguredDevice(false, options_.device_name, &device_id,
                                 &ignored_error)) {
      return false;
    }
    stream_ =
        SDL_OpenAudioDeviceStream(device_id, &spec, nullptr, nullptr);
    if (stream_ == nullptr) {
      return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      CloseStream();
      return false;
    }
    format_ = format;
    sample_rate_ = frame.sample_rate;
    channels_ = frame.number_of_channels;
    return true;
  }

  void CloseStream() {
    if (stream_ != nullptr) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    format_ = SDL_AUDIO_UNKNOWN;
    sample_rate_ = 0;
    channels_ = 0;
  }

  AudioPlaybackOptions options_;
  SDL_AudioStream* stream_ = nullptr;
  SDL_AudioFormat format_ = SDL_AUDIO_UNKNOWN;
  size_t sample_rate_ = 0;
  size_t channels_ = 0;
  std::mutex mutex_;
  std::atomic<bool> running_{false};
  bool audio_initialized_ = false;
};

AudioCapture::AudioCapture() : impl_(new Impl()) {}

AudioCapture::~AudioCapture() = default;

bool AudioCapture::Start(const AudioCaptureOptions& options,
                         const AudioFrameCallback& callback,
                         std::string* error_message) {
  return impl_->Start(options, callback, error_message);
}

void AudioCapture::Stop() { impl_->Stop(); }

bool AudioCapture::running() const { return impl_->running(); }

AudioPlayback::AudioPlayback() : impl_(new Impl()) {}

AudioPlayback::~AudioPlayback() = default;

bool AudioPlayback::Start(const AudioPlaybackOptions& options,
                          std::string* error_message) {
  return impl_->Start(options, error_message);
}

bool AudioPlayback::PushFrame(const AudioFrameView& frame) {
  return impl_->PushFrame(frame);
}

void AudioPlayback::Clear() { impl_->Clear(); }

void AudioPlayback::Stop() { impl_->Stop(); }

bool AudioPlayback::running() const { return impl_->running(); }

}  // 命名空间 rtc_audio
