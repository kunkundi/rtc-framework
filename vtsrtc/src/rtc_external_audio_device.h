#pragma once

#include "rtc_types.h"

#include <modules/audio_device/include/audio_device_default.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class RtcExternalAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<
          webrtc::AudioDeviceModule> {
 public:
  ~RtcExternalAudioDeviceModule() override {
    StopPlayout();
  }

  int32_t ActiveAudioLayer(AudioLayer* audio_layer) const override {
    if (audio_layer == nullptr) {
      return -1;
    }
    *audio_layer = AudioLayer::kDummyAudio;
    return 0;
  }

  int32_t RegisterAudioCallback(
      webrtc::AudioTransport* audio_callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_callback_ = audio_callback;
    return 0;
  }

  int32_t Init() override {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = true;
    return 0;
  }

  int32_t Terminate() override {
    StopPlayout();
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    recording_initialized_ = false;
    playout_initialized_ = false;
    initialized_ = false;
    audio_callback_ = nullptr;
    return 0;
  }

  bool Initialized() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
  }

  int32_t PlayoutIsAvailable(bool* available) override {
    if (available == nullptr) {
      return -1;
    }
    *available = true;
    return 0;
  }

  int32_t InitPlayout() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return -1;
    }
    playout_initialized_ = true;
    return 0;
  }

  bool PlayoutIsInitialized() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return playout_initialized_;
  }

  int32_t StartPlayout() override {
    std::lock_guard<std::mutex> control_lock(playout_control_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !playout_initialized_ || audio_callback_ == nullptr) {
      return -1;
    }
    if (playing_) {
      return 0;
    }

    playing_ = true;
    try {
      playout_thread_ = std::thread(&RtcExternalAudioDeviceModule::PlayoutLoop,
                                    this);
    } catch (...) {
      playing_ = false;
      return -1;
    }
    return 0;
  }

  int32_t StopPlayout() override {
    std::lock_guard<std::mutex> control_lock(playout_control_mutex_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      playing_ = false;
    }
    playout_condition_.notify_all();
    if (playout_thread_.joinable()) {
      playout_thread_.join();
    }
    return 0;
  }

  bool Playing() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
  }

  int32_t RecordingIsAvailable(bool* available) override {
    if (available == nullptr) {
      return -1;
    }
    *available = true;
    return 0;
  }

  int32_t InitRecording() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      return -1;
    }
    recording_initialized_ = true;
    return 0;
  }

  bool RecordingIsInitialized() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_initialized_;
  }

  int32_t StartRecording() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !recording_initialized_) {
      return -1;
    }
    recording_ = true;
    return 0;
  }

  int32_t StopRecording() override {
    std::lock_guard<std::mutex> lock(mutex_);
    recording_ = false;
    return 0;
  }

  bool Recording() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return recording_;
  }

  bool PushRecordedData(const vts_rtc::PCMData& pcm_data) {
    if (pcm_data.buffer == nullptr || pcm_data.bits_per_sample != 16 ||
        !IsSupportedSampleRate(pcm_data.sample_rate) ||
        pcm_data.number_of_channels == 0 ||
        pcm_data.number_of_channels > 2 ||
        pcm_data.number_of_frames != pcm_data.sample_rate / 100 ||
        pcm_data.sz_buffer != pcm_data.number_of_frames *
                                  pcm_data.number_of_channels *
                                  sizeof(int16_t)) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !recording_ || audio_callback_ == nullptr) {
      return false;
    }

    uint32_t new_mic_level = 0;
    const size_t bytes_per_frame =
        pcm_data.bits_per_sample / 8 * pcm_data.number_of_channels;
    return audio_callback_->RecordedDataIsAvailable(
               pcm_data.buffer, pcm_data.number_of_frames,
               bytes_per_frame, pcm_data.number_of_channels,
               static_cast<uint32_t>(pcm_data.sample_rate), 0, 0, 0, false,
               new_mic_level) == 0;
  }

 private:
  void PlayoutLoop() {
    // 持续拉取混音数据以驱动远端音频解码器
    const size_t samples_per_channel = kPlayoutSampleRate / 100;
    std::vector<int16_t> samples(samples_per_channel * kPlayoutChannels);
    auto next_frame_time = std::chrono::steady_clock::now();

    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!playing_) {
          break;
        }

        size_t samples_out = 0;
        int64_t elapsed_time_ms = -1;
        int64_t ntp_time_ms = -1;
        if (audio_callback_ != nullptr) {
          audio_callback_->NeedMorePlayData(
              samples_per_channel, sizeof(int16_t) * kPlayoutChannels,
              kPlayoutChannels,
              kPlayoutSampleRate, samples.data(), samples_out,
              &elapsed_time_ms, &ntp_time_ms);
        }
      }

      next_frame_time += std::chrono::milliseconds(10);
      std::unique_lock<std::mutex> lock(mutex_);
      playout_condition_.wait_until(
          lock, next_frame_time, [this]() { return !playing_; });
    }
  }

  static bool IsSupportedSampleRate(size_t sample_rate) {
    return sample_rate == 8000 || sample_rate == 16000 ||
           sample_rate == 32000 || sample_rate == 44100 ||
           sample_rate == 48000;
  }

  mutable std::mutex mutex_;
  std::mutex playout_control_mutex_;
  std::condition_variable playout_condition_;
  std::thread playout_thread_;
  webrtc::AudioTransport* audio_callback_ = nullptr;
  bool initialized_ = false;
  bool playout_initialized_ = false;
  bool playing_ = false;
  bool recording_initialized_ = false;
  bool recording_ = false;
  static const uint32_t kPlayoutSampleRate = 48000;
  static const size_t kPlayoutChannels = 2;
};
