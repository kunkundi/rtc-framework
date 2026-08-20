#include "rtc_audio/audio_device.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

void TestResolveDefaultDevice() {
  std::vector<rtc_audio::AudioDeviceInfo> devices;
  uint32_t device_id = 99;
  std::string error;
  Check(rtc_audio::ResolveAudioDeviceName(devices, "default", &device_id,
                                          &error),
        "resolve default device");
  Check(device_id == rtc_audio::kSystemDefaultDeviceId,
        "default device ID");
}

void TestResolveNamedDevice() {
  std::vector<rtc_audio::AudioDeviceInfo> devices;
  rtc_audio::AudioDeviceInfo microphone;
  microphone.id = 42;
  microphone.name = "USB microphone";
  devices.push_back(microphone);

  uint32_t device_id = 0;
  std::string error;
  Check(rtc_audio::ResolveAudioDeviceName(
            devices, "USB microphone", &device_id, &error),
        "resolve named device");
  Check(device_id == 42, "named device ID");
}

void TestRejectUnknownDevice() {
  std::vector<rtc_audio::AudioDeviceInfo> devices;
  uint32_t device_id = 0;
  std::string error;
  Check(!rtc_audio::ResolveAudioDeviceName(
            devices, "missing device", &device_id, &error),
        "reject unknown device");
  Check(error.find("missing device") != std::string::npos,
        "unknown device error");
}

void TestRejectFractionalCaptureFrame() {
  rtc_audio::AudioCapture capture;
  rtc_audio::AudioCaptureOptions options;
  options.sample_rate = 44100;
  options.frame_duration_ms = 7;
  std::string error;
  Check(!capture.Start(options, [](const rtc_audio::AudioFrameView&) {},
                       &error),
        "reject fractional capture frame");
  Check(error.find("Invalid audio capture format") != std::string::npos,
        "fractional capture frame error");
}

void TestDummyCaptureHandlesOversizedDeviceBlocks() {
  Check(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_AUDIODRIVER",
                                   "dummy", true),
        "select SDL dummy audio driver");
  Check(SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1920"),
        "set large dummy capture block");

  std::mutex mutex;
  std::condition_variable condition;
  size_t received_frames = 0;
  rtc_audio::AudioCapture capture;
  rtc_audio::AudioCaptureOptions options;
  options.sample_rate = 48000;
  options.channels = 1;
  options.frame_duration_ms = 10;
  std::string error;
  Check(capture.Start(
            options,
            [&](const rtc_audio::AudioFrameView&) {
              std::lock_guard<std::mutex> lock(mutex);
              ++received_frames;
              condition.notify_all();
            },
            &error),
        std::string("start dummy capture: ") + error);

  {
    std::unique_lock<std::mutex> lock(mutex);
    Check(condition.wait_for(lock, std::chrono::seconds(2),
                             [&]() { return received_frames > 0; }),
          "capture frame from oversized device block");
  }
  capture.Stop();
}

void TestDummyPlayback() {
  Check(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_AUDIODRIVER",
                                   "dummy", true),
        "select SDL dummy audio driver");
  Check(SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480"),
        "set dummy playback block");
  rtc_audio::AudioPlayback playback;
  rtc_audio::AudioPlaybackOptions options;
  std::string error;
  const bool started = playback.Start(options, &error);
  Check(started, std::string("start dummy playback: ") + error);

  std::vector<int16_t> samples(480, 0);
  rtc_audio::AudioFrameView frame;
  frame.bits_per_sample = 16;
  frame.sample_rate = 48000;
  frame.number_of_channels = 1;
  frame.number_of_frames = samples.size();
  frame.data = samples.data();
  frame.data_size = samples.size() * sizeof(samples[0]);
  for (int i = 0; i < 100; ++i) {
    Check(playback.PushFrame(frame), "queue dummy playback frame");
  }
  playback.Stop();
}

}  // 匿名命名空间

int main() {
  TestResolveDefaultDevice();
  TestResolveNamedDevice();
  TestRejectUnknownDevice();
  TestRejectFractionalCaptureFrame();
  TestDummyCaptureHandlesOversizedDeviceBlocks();
  TestDummyPlayback();
  std::cout << "rtc_audio_device_tests passed" << std::endl;
  return 0;
}
