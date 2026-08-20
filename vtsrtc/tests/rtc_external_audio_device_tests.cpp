#include "rtc_external_audio_device.h"

#include <api/scoped_refptr.h>
#include <rtc_base/ref_counted_object.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

class FakeAudioTransport : public webrtc::AudioTransport {
 public:
  int32_t RecordedDataIsAvailable(const void* audio_samples,
                                  size_t sample_count,
                                  size_t bytes_per_sample,
                                  size_t channel_count,
                                  uint32_t sample_rate,
                                  uint32_t total_delay_ms,
                                  int32_t clock_drift,
                                  uint32_t current_mic_level,
                                  bool key_pressed,
                                  uint32_t& new_mic_level) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++recorded_calls_;
    recorded_sample_count_ = sample_count;
    recorded_bytes_per_sample_ = bytes_per_sample;
    recorded_channel_count_ = channel_count;
    recorded_sample_rate_ = sample_rate;
    recorded_first_sample_ =
        audio_samples == nullptr
            ? 0
            : static_cast<const int16_t*>(audio_samples)[0];
    new_mic_level = current_mic_level;
    condition_.notify_all();
    return 0;
  }

  int32_t NeedMorePlayData(size_t sample_count,
                           size_t bytes_per_sample,
                           size_t channel_count,
                           uint32_t sample_rate,
                           void* audio_samples,
                           size_t& samples_out,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    if (audio_samples != nullptr) {
      std::memset(audio_samples, 0,
                  sample_count * bytes_per_sample * channel_count);
    }
    samples_out = sample_count;
    if (elapsed_time_ms != nullptr) {
      *elapsed_time_ms = 0;
    }
    if (ntp_time_ms != nullptr) {
      *ntp_time_ms = 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ++playout_calls_;
    playout_sample_count_ = sample_count;
    playout_bytes_per_sample_ = bytes_per_sample;
    playout_channel_count_ = channel_count;
    playout_sample_rate_ = sample_rate;
    condition_.notify_all();
    return 0;
  }

  void PullRenderData(int bits_per_sample,
                      int sample_rate,
                      size_t channel_count,
                      size_t frame_count,
                      void* audio_data,
                      int64_t* elapsed_time_ms,
                      int64_t* ntp_time_ms) override {
    if (audio_data != nullptr) {
      std::memset(audio_data, 0,
                  frame_count * channel_count * bits_per_sample / 8);
    }
  }

  bool WaitForPlayoutCalls(size_t expected_calls) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, std::chrono::milliseconds(500),
        [this, expected_calls]() { return playout_calls_ >= expected_calls; });
  }

  size_t recorded_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recorded_calls_;
  }

  size_t playout_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playout_calls_;
  }

  void CheckRecordedFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Check(recorded_sample_count_ == 480, "recorded sample count");
    Check(recorded_bytes_per_sample_ == sizeof(int16_t),
          "recorded bytes per sample");
    Check(recorded_channel_count_ == 1, "recorded channel count");
    Check(recorded_sample_rate_ == 48000, "recorded sample rate");
    Check(recorded_first_sample_ == 1234, "recorded PCM data");
  }

  void CheckPlayoutFormat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Check(playout_sample_count_ == 480, "playout sample count");
    Check(playout_bytes_per_sample_ == sizeof(int16_t),
          "playout bytes per sample");
    Check(playout_channel_count_ == 2, "playout channel count");
    Check(playout_sample_rate_ == 48000, "playout sample rate");
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  size_t recorded_calls_ = 0;
  size_t playout_calls_ = 0;
  size_t recorded_sample_count_ = 0;
  size_t recorded_bytes_per_sample_ = 0;
  size_t recorded_channel_count_ = 0;
  uint32_t recorded_sample_rate_ = 0;
  int16_t recorded_first_sample_ = 0;
  size_t playout_sample_count_ = 0;
  size_t playout_bytes_per_sample_ = 0;
  size_t playout_channel_count_ = 0;
  uint32_t playout_sample_rate_ = 0;
};

vts_rtc::PCMData MakePcmData(std::vector<int16_t>* samples) {
  vts_rtc::PCMData pcm_data;
  pcm_data.bits_per_sample = 16;
  pcm_data.sample_rate = 48000;
  pcm_data.number_of_channels = 1;
  pcm_data.number_of_frames = samples->size();
  pcm_data.buffer = samples->data();
  pcm_data.sz_buffer = samples->size() * sizeof((*samples)[0]);
  return pcm_data;
}

void TestRecordingAndPlayout() {
  rtc::scoped_refptr<RtcExternalAudioDeviceModule> module =
      new rtc::RefCountedObject<RtcExternalAudioDeviceModule>();
  FakeAudioTransport transport;
  std::vector<int16_t> samples(480, 1234);
  vts_rtc::PCMData pcm_data = MakePcmData(&samples);

  Check(!module->PushRecordedData(pcm_data),
        "reject recording before initialization");
  Check(module->RegisterAudioCallback(&transport) == 0,
        "register audio callback");
  Check(module->Init() == 0, "initialize audio device");

  bool available = false;
  Check(module->PlayoutIsAvailable(&available) == 0 && available,
        "playout is available");
  Check(module->InitPlayout() == 0, "initialize playout");
  Check(module->StartPlayout() == 0, "start playout");
  Check(transport.WaitForPlayoutCalls(3), "drive WebRTC playout");
  transport.CheckPlayoutFormat();
  Check(module->StopPlayout() == 0, "stop playout");
  const size_t stopped_playout_calls = transport.playout_calls();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Check(transport.playout_calls() == stopped_playout_calls,
        "playout stops producing frames");

  Check(module->InitRecording() == 0, "initialize recording");
  Check(module->StartRecording() == 0, "start recording");
  Check(module->PushRecordedData(pcm_data), "push valid PCM frame");
  Check(transport.recorded_calls() == 1, "recording callback count");
  transport.CheckRecordedFormat();
  Check(module->StopRecording() == 0, "stop recording");
  Check(!module->PushRecordedData(pcm_data),
        "reject recording after stop");
  Check(module->Terminate() == 0, "terminate audio device");
}

void TestRejectInvalidPcm() {
  rtc::scoped_refptr<RtcExternalAudioDeviceModule> module =
      new rtc::RefCountedObject<RtcExternalAudioDeviceModule>();
  FakeAudioTransport transport;
  std::vector<int16_t> samples(480, 0);
  vts_rtc::PCMData pcm_data = MakePcmData(&samples);

  Check(module->RegisterAudioCallback(&transport) == 0,
        "register validation callback");
  Check(module->Init() == 0, "initialize validation device");
  Check(module->InitRecording() == 0, "initialize validation recording");
  Check(module->StartRecording() == 0, "start validation recording");

  pcm_data.bits_per_sample = 24;
  Check(!module->PushRecordedData(pcm_data), "reject non-16-bit PCM");
  pcm_data.bits_per_sample = 16;
  pcm_data.sample_rate = 12000;
  Check(!module->PushRecordedData(pcm_data), "reject unsupported sample rate");
  pcm_data.sample_rate = 48000;
  pcm_data.number_of_frames = 479;
  Check(!module->PushRecordedData(pcm_data), "reject non-10-ms PCM frame");
  pcm_data.number_of_frames = 480;
  pcm_data.sz_buffer -= sizeof(int16_t);
  Check(!module->PushRecordedData(pcm_data), "reject invalid PCM byte size");
  Check(transport.recorded_calls() == 0,
        "invalid PCM never reaches WebRTC");
}

}  // 匿名命名空间

int main() {
  TestRecordingAndPlayout();
  TestRejectInvalidPcm();
  std::cout << "rtc_external_audio_device_tests passed" << std::endl;
  return 0;
}
