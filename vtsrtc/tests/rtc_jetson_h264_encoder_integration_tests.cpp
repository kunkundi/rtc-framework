#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "log/log_manager.h"
#include "media/base/codec.h"
#include "rtc_types.h"
#include "video/encode/nvidia-jetson/jetsonh264_encoder_impl.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Resolution {
  int width;
  int height;
};

bool ContainsNalType(const uint8_t* data, size_t size, uint8_t nal_type) {
  for (size_t i = 0; i + 4 < size; ++i) {
    size_t header = size;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      header = i + 3;
    } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
               data[i + 3] == 1) {
      header = i + 4;
    }
    if (header < size && (data[header] & 0x1f) == nal_type) {
      return true;
    }
  }
  return false;
}

void FillMovingPattern(const rtc::scoped_refptr<webrtc::I420Buffer>& buffer,
                       uint32_t frame_index) {
  for (int y = 0; y < buffer->height(); ++y) {
    uint8_t* row = buffer->MutableDataY() + y * buffer->StrideY();
    for (int x = 0; x < buffer->width(); ++x) {
      row[x] = static_cast<uint8_t>((x + y + frame_index * 7) & 0xff);
    }
  }

  const int chroma_width = (buffer->width() + 1) / 2;
  const int chroma_height = (buffer->height() + 1) / 2;
  for (int y = 0; y < chroma_height; ++y) {
    memset(buffer->MutableDataU() + y * buffer->StrideU(),
           96 + (frame_index & 0x1f), chroma_width);
    memset(buffer->MutableDataV() + y * buffer->StrideV(),
           128 + (frame_index & 0x1f), chroma_width);
  }
}

class EncodedCallback final : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(
      const webrtc::EncodedImage& image,
      const webrtc::CodecSpecificInfo*,
      const webrtc::RTPFragmentationHeader*) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++count_;
      width_ = image._encodedWidth;
      height_ = image._encodedHeight;
      callback_time_ = Clock::now();
      keyframe_ = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
      idr_ = ContainsNalType(image.data(), image.size(), 5);
      sps_ = ContainsNalType(image.data(), image.size(), 7);
      if (image.qp_ >= 0 && image.qp_ <= 51) {
        qp_sum_ += static_cast<uint64_t>(image.qp_);
        ++qp_count_;
        min_qp_ = std::min(min_qp_, image.qp_);
        max_qp_ = std::max(max_qp_, image.qp_);
      }
    }
    condition_.notify_all();
    return Result(Result::OK, image.Timestamp());
  }

  bool WaitForResolution(uint64_t previous_count, const Resolution& target,
                         std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout, [&]() {
      return count_ > previous_count && width_ == target.width &&
             height_ == target.height;
    });
  }

  uint64_t count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }

  Clock::time_point callback_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callback_time_;
  }

  bool has_decodable_keyframe() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keyframe_ && idr_ && sps_;
  }

  uint64_t qp_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return qp_count_;
  }

  double average_qp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return qp_count_ == 0
               ? -1.0
               : static_cast<double>(qp_sum_) / qp_count_;
  }

  int min_qp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return min_qp_;
  }

  int max_qp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_qp_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  uint64_t count_ = 0;
  int width_ = 0;
  int height_ = 0;
  Clock::time_point callback_time_;
  bool keyframe_ = false;
  bool idr_ = false;
  bool sps_ = false;
  uint64_t qp_sum_ = 0;
  uint64_t qp_count_ = 0;
  int min_qp_ = 52;
  int max_qp_ = -1;
};

class BlockingEncodedCallback final : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(
      const webrtc::EncodedImage& image,
      const webrtc::CodecSpecificInfo*,
      const webrtc::RTPFragmentationHeader*) override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this]() { return unblocked_; });
    return Result(Result::OK, image.Timestamp());
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this]() { return entered_; });
  }

  void Unblock() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      unblocked_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool unblocked_ = false;
};

webrtc::VideoFrame BuildFrame(const Resolution& resolution,
                              uint32_t frame_index) {
  auto buffer =
      webrtc::I420Buffer::Create(resolution.width, resolution.height);
  FillMovingPattern(buffer, frame_index);
  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_timestamp_rtp(frame_index * 3000)
      .set_timestamp_us(static_cast<int64_t>(frame_index) * 33333)
      .build();
}

class ReentrantReconfigureCallback final
    : public webrtc::EncodedImageCallback {
 public:
  ReentrantReconfigureCallback(
      webrtc::JetsonH264EncoderImpl* encoder,
      EncodedCallback* next_callback,
      webrtc::VideoCodec* codec_settings,
      const webrtc::VideoEncoder::Settings* settings,
      const Resolution& target)
      : encoder_(encoder),
        next_callback_(next_callback),
        codec_settings_(codec_settings),
        settings_(settings),
        target_(target) {}

  Result OnEncodedImage(
      const webrtc::EncodedImage& image,
      const webrtc::CodecSpecificInfo*,
      const webrtc::RTPFragmentationHeader*) override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this]() { return start_reconfigure_; });
    }

    const int32_t release_result = encoder_->Release();
    const int32_t register_result =
        encoder_->RegisterEncodeCompleteCallback(next_callback_);
    codec_settings_->width = target_.width;
    codec_settings_->height = target_.height;
    codec_settings_->startBitrate = 7000;
    const int32_t init_result = encoder_->InitEncode(codec_settings_, *settings_);

    {
      std::unique_lock<std::mutex> lock(mutex_);
      operation_ok_ = release_result == WEBRTC_VIDEO_CODEC_OK &&
                      register_result == WEBRTC_VIDEO_CODEC_OK &&
                      init_result == WEBRTC_VIDEO_CODEC_OK;
      operation_submitted_ = true;
      condition_.notify_all();
      condition_.wait(lock, [this]() { return allow_return_; });
    }
    return Result(Result::OK, image.Timestamp());
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, timeout,
                               [this]() { return entered_; });
  }

  void StartReconfigure() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      start_reconfigure_ = true;
    }
    condition_.notify_all();
  }

  bool WaitUntilOperationSubmitted(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(
        lock, timeout, [this]() { return operation_submitted_; });
  }

  bool operation_ok() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operation_ok_;
  }

  void AllowReturn() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      allow_return_ = true;
    }
    condition_.notify_all();
  }

 private:
  webrtc::JetsonH264EncoderImpl* encoder_;
  EncodedCallback* next_callback_;
  webrtc::VideoCodec* codec_settings_;
  const webrtc::VideoEncoder::Settings* settings_;
  Resolution target_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool start_reconfigure_ = false;
  bool operation_submitted_ = false;
  bool operation_ok_ = false;
  bool allow_return_ = false;
};

bool EncodeUntilOutput(webrtc::JetsonH264EncoderImpl* encoder,
                       EncodedCallback* callback,
                       const Resolution& resolution, uint32_t* frame_index,
                       double* callback_gap_ms) {
  const uint64_t previous_count = callback->count();
  const Clock::time_point previous_callback = callback->callback_time();
  const std::vector<webrtc::VideoFrameType> frame_types = {
      webrtc::VideoFrameType::kVideoFrameDelta};

  for (int attempt = 0; attempt < 12; ++attempt) {
    webrtc::VideoFrame frame = BuildFrame(resolution, (*frame_index)++);
    const int32_t result = encoder->Encode(frame, &frame_types);
    if (result != WEBRTC_VIDEO_CODEC_OK) {
      return false;
    }
    if (callback->WaitForResolution(previous_count, resolution,
                                    std::chrono::milliseconds(34))) {
      if (callback_gap_ms) {
        *callback_gap_ms =
            std::chrono::duration_cast<
                std::chrono::duration<double, std::milli>>(
                callback->callback_time() - previous_callback)
                .count();
      }
      return true;
    }
  }
  return false;
}

int ParseIterations(int argc, char** argv) {
  if (argc < 2) {
    return 10;
  }
  return std::max(1, std::atoi(argv[1]));
}

bool ValidateEncoderAdaptationInfo(
    const webrtc::VideoEncoder::EncoderInfo& info) {
  if (!info.scaling_settings.thresholds.has_value() ||
      info.scaling_settings.thresholds->low != 32 ||
      info.scaling_settings.thresholds->high != 36 ||
      info.scaling_settings.min_pixels_per_frame != 320 * 180 ||
      info.has_trusted_rate_controller) {
    std::cerr << "Jetson QualityScaler 配置不正确" << std::endl;
    return false;
  }

  const std::vector<webrtc::VideoEncoder::ResolutionBitrateLimits> expected = {
      {320 * 180, 175000, 100000, 450000},
      {320 * 200, 175000, 100000, 450000},
      {480 * 270, 400000, 150000, 800000},
      {480 * 300, 350000, 150000, 1200000},
      {640 * 360, 700000, 150000, 1800000},
      {720 * 450, 1000000, 150000, 2200000},
      {960 * 540, 1600000, 150000, 2500000},
      {960 * 600, 1800000, 150000, 2800000},
      {1280 * 720, 1600000, 150000, 6800000},
      {1280 * 800, 2400000, 150000, 6800000},
      {1440 * 900, 4200000, 150000, 7800000},
      {1920 * 1080, 6200000, 150000, 12000000},
      {1920 * 1200, 7000000, 150000, 12000000},
  };
  if (info.resolution_bitrate_limits != expected) {
    std::cerr << "Jetson 默认分辨率码率阶梯不正确" << std::endl;
    return false;
  }

  std::vector<int> probe_pixels = {320 * 180};
  for (const auto& limit : info.resolution_bitrate_limits) {
    const int pixels = limit.frame_size_pixels;
    probe_pixels.push_back(std::max(320 * 180, pixels - 1));
    probe_pixels.push_back(pixels);
    probe_pixels.push_back(pixels + 1);

    // WebRTC 以 floor(current_pixels * 5 / 3) 查询升档门槛；在每个
    // limit 反推的切换点前后取样，覆盖非标准 16:9/16:10 尺寸。
    const int inverse_pivot = static_cast<int>(
        (static_cast<int64_t>(pixels) * 3 + 4) / 5);
    probe_pixels.push_back(std::max(320 * 180, inverse_pivot - 1));
    probe_pixels.push_back(std::max(320 * 180, inverse_pivot));
    probe_pixels.push_back(std::max(320 * 180, inverse_pivot + 1));
  }
  std::sort(probe_pixels.begin(), probe_pixels.end());
  probe_pixels.erase(std::unique(probe_pixels.begin(), probe_pixels.end()),
                     probe_pixels.end());

  for (int pixels : probe_pixels) {
    const auto current =
        info.GetEncoderBitrateLimitsForResolution(pixels);
    const int higher_pixels = static_cast<int>(
        static_cast<int64_t>(pixels) * 5 / 3);
    const auto higher =
        info.GetEncoderBitrateLimitsForResolution(higher_pixels);
    if (current.has_value() && higher.has_value() &&
        current->max_bitrate_bps < higher->min_start_bitrate_bps) {
      std::cerr << "Jetson 码率阶梯存在升档死区: pixels=" << pixels
                << " current_max=" << current->max_bitrate_bps
                << " higher_pixels=" << higher_pixels
                << " higher_start=" << higher->min_start_bitrate_bps
                << std::endl;
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  LogInst->init("/tmp/rtc_jetson_h264_encoder_integration_tests");

  const int iterations = ParseIterations(argc, argv);
  const std::vector<Resolution> resolutions = {
      {1920, 1200}, {1280, 800}, {960, 600}, {640, 400},
      {960, 600},   {1280, 800}, {1920, 1200},
  };

  webrtc::SdpVideoFormat format(
      "H264", {{"level-asymmetry-allowed", "1"},
               {"packetization-mode", "1"},
               {"profile-level-id", "42e033"}});
  cricket::VideoCodec cricket_codec(format);
  vts_rtc::RtcConfig rtc_config;
  rtc_config.jetson_h264_encoder = "jetson";
  rtc_config.encode_params.bitrate_mode = "cbr";
  rtc_config.encode_params.bitrate_minmum = 150000;
  rtc_config.encode_params.bitrate_maxmum = 12000000;
  rtc_config.encode_params.I_frame_interval = 3000;
  rtc_config.resolution_limit["merged_image"] = {1920, 1200};

  webrtc::JetsonH264EncoderImpl encoder(cricket_codec, rtc_config);
  if (!ValidateEncoderAdaptationInfo(encoder.GetEncoderInfo())) {
    return 7;
  }
  EncodedCallback callback;
  encoder.RegisterEncodeCompleteCallback(&callback);

  webrtc::VideoCodec codec_settings;
  codec_settings.codecType = webrtc::kVideoCodecH264;
  codec_settings.width = resolutions.front().width;
  codec_settings.height = resolutions.front().height;
  codec_settings.startBitrate = 6000;
  codec_settings.maxBitrate = 12000;
  codec_settings.minBitrate = 100;
  codec_settings.maxFramerate = 30;
  codec_settings.active = true;
  codec_settings.numberOfSimulcastStreams = 0;
  codec_settings.mode = webrtc::VideoCodecMode::kRealtimeVideo;
  *codec_settings.H264() = webrtc::VideoEncoder::GetDefaultH264Settings();

  const webrtc::VideoEncoder::Settings settings(
      webrtc::VideoEncoder::Capabilities(false), 4, 1200);
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "初始化 Jetson H264 编码器失败" << std::endl;
    return 1;
  }

  uint32_t frame_index = 1;
  double ignored_gap = 0.0;
  const std::vector<Resolution> rapid_startup_targets = {
      {1440, 900}, {960, 600}, {720, 450}};
  for (const Resolution& target : rapid_startup_targets) {
    codec_settings.width = target.width;
    codec_settings.height = target.height;
    encoder.Release();
    encoder.RegisterEncodeCompleteCallback(&callback);
    if (encoder.InitEncode(&codec_settings, settings) !=
        WEBRTC_VIDEO_CODEC_OK) {
      std::cerr << "首帧前快速重配失败: " << target.width << "x"
                << target.height << std::endl;
      return 22;
    }
  }
  if (!EncodeUntilOutput(&encoder, &callback, rapid_startup_targets.back(),
                         &frame_index, &ignored_gap) ||
      !callback.has_decodable_keyframe()) {
    std::cerr << "首帧前快速重配后无法输出可独立解码帧" << std::endl;
    return 23;
  }
  std::cout << "RAPID_STARTUP_DRC target=720x450 decodable_keyframe=1"
            << std::endl;

  codec_settings.width = resolutions.front().width;
  codec_settings.height = resolutions.front().height;
  encoder.Release();
  encoder.RegisterEncodeCompleteCallback(&callback);
  if (encoder.InitEncode(&codec_settings, settings) !=
          WEBRTC_VIDEO_CODEC_OK ||
      !EncodeUntilOutput(&encoder, &callback, resolutions.front(),
                         &frame_index, &ignored_gap)) {
    std::cerr << "首帧前快速重配后恢复原始分辨率失败" << std::endl;
    return 24;
  }

  for (int frame = 0; frame < 8; ++frame) {
    if (!EncodeUntilOutput(&encoder, &callback, resolutions.front(),
                           &frame_index, &ignored_gap)) {
      std::cerr << "初始连续编码失败" << std::endl;
      return 2;
    }
  }

  webrtc::VideoBitrateAllocation rate_allocation;
  rate_allocation.SetBitrate(0, 0, 6000000);
  encoder.SetRates(
      webrtc::VideoEncoder::RateControlParameters(rate_allocation, 24.0));
  if (!EncodeUntilOutput(&encoder, &callback, resolutions.front(),
                         &frame_index, &ignored_gap)) {
    std::cerr << "动态帧率更新后的编码失败" << std::endl;
    return 6;
  }
  encoder.SetRates(
      webrtc::VideoEncoder::RateControlParameters(rate_allocation, 30.0));

  // 保持一个完整统计窗口，验证实际码流能够持续产出有效 QP。
  for (int frame = 0; frame < 35; ++frame) {
    if (!EncodeUntilOutput(&encoder, &callback, resolutions.front(),
                           &frame_index, &ignored_gap)) {
      std::cerr << "QP 统计窗口编码失败" << std::endl;
      return 10;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(34));
  }

  std::vector<double> callback_gaps;
  size_t keyframe_count = 0;
  Resolution current = resolutions.front();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    for (size_t i = 1; i < resolutions.size(); ++i) {
      const Resolution target = resolutions[i];
      codec_settings.width = target.width;
      codec_settings.height = target.height;
      // WebRTC 质量自适应的真实调用顺序是 Release 后立即重新 InitEncode。
      encoder.Release();
      encoder.RegisterEncodeCompleteCallback(&callback);
      const Clock::time_point init_start = Clock::now();
      if (encoder.InitEncode(&codec_settings, settings) !=
          WEBRTC_VIDEO_CODEC_OK) {
        std::cerr << "分辨率切换时重新初始化失败: " << current.width << "x"
                  << current.height << " -> " << target.width << "x"
                  << target.height << std::endl;
        return 8;
      }
      const double init_ms =
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              Clock::now() - init_start)
              .count();
      double callback_gap_ms = 0.0;
      if (!EncodeUntilOutput(&encoder, &callback, target, &frame_index,
                             &callback_gap_ms)) {
        std::cerr << "切换编码失败: " << current.width << "x"
                  << current.height << " -> " << target.width << "x"
                  << target.height << std::endl;
        return 3;
      }
      callback_gaps.push_back(callback_gap_ms);
      if (callback.has_decodable_keyframe()) {
        ++keyframe_count;
      }

      std::cout << std::fixed << std::setprecision(3)
                << "SAMPLE iteration=" << iteration << " from="
                << current.width << "x" << current.height << " to="
                << target.width << "x" << target.height
                << " init_ms=" << init_ms
                << " callback_gap_ms=" << callback_gap_ms
                << " decodable_keyframe="
                << (callback.has_decodable_keyframe() ? 1 : 0) << std::endl;

      // 连续发送数帧，为后台补充最高分辨率热备留出实际视频时间。
      for (int frame = 0; frame < 4; ++frame) {
        if (!EncodeUntilOutput(&encoder, &callback, target, &frame_index,
                               &ignored_gap)) {
          std::cerr << "切换后的连续编码失败" << std::endl;
          return 4;
        }
      }
      current = target;
    }
  }

  const double average =
      std::accumulate(callback_gaps.begin(), callback_gaps.end(), 0.0) /
      callback_gaps.size();
  std::sort(callback_gaps.begin(), callback_gaps.end());
  const size_t p95_index = static_cast<size_t>(
      0.95 * static_cast<double>(callback_gaps.size() - 1));
  std::cout << "SUMMARY samples=" << callback_gaps.size()
            << " avg_ms=" << average
            << " p95_ms=" << callback_gaps[p95_index]
            << " max_ms=" << callback_gaps.back()
            << " keyframes=" << keyframe_count
            << " qp_samples=" << callback.qp_count()
            << " qp_avg=" << callback.average_qp()
            << " qp_min=" << callback.min_qp()
            << " qp_max=" << callback.max_qp() << std::endl;

  if (callback.qp_count() == 0) {
    std::cerr << "Jetson H264 输出未解析到有效 QP" << std::endl;
    return 9;
  }
  if (keyframe_count != callback_gaps.size()) {
    return 5;
  }

  encoder.Release();
  encoder.RegisterEncodeCompleteCallback(&callback);
  codec_settings.width = 640;
  codec_settings.height = 400;
  codec_settings.startBitrate = 700;
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "低分辨率会话初始化失败" << std::endl;
    return 11;
  }
  for (int frame = 0; frame < 4; ++frame) {
    if (!EncodeUntilOutput(&encoder, &callback, {640, 400}, &frame_index,
                           &ignored_gap)) {
      std::cerr << "低分辨率会话连续编码失败" << std::endl;
      return 12;
    }
  }

  codec_settings.width = 960;
  codec_settings.height = 600;
  codec_settings.startBitrate = 1600;
  const Clock::time_point growth_init_start = Clock::now();
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "扩大会话分辨率初始化失败" << std::endl;
    return 13;
  }
  const double growth_init_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          Clock::now() - growth_init_start)
          .count();
  double growth_callback_gap_ms = 0.0;
  if (!EncodeUntilOutput(&encoder, &callback, {960, 600}, &frame_index,
                         &growth_callback_gap_ms)) {
    std::cerr << "后台扩大会话分辨率后编码失败" << std::endl;
    return 14;
  }
  std::cout << "SESSION_GROWTH from=640x400 to=960x600 init_ms="
            << growth_init_ms
            << " callback_gap_ms=" << growth_callback_gap_ms << std::endl;

  double encode_growth_callback_gap_ms = 0.0;
  if (!EncodeUntilOutput(&encoder, &callback, {1440, 900}, &frame_index,
                         &encode_growth_callback_gap_ms)) {
    std::cerr << "Encode 未经 InitEncode 扩大会话分辨率失败" << std::endl;
    return 15;
  }
  if (!callback.has_decodable_keyframe()) {
    std::cerr << "Encode 扩大会话后的首帧不可独立解码" << std::endl;
    return 16;
  }
  std::cout << "ENCODE_SESSION_GROWTH from=960x600 to=1440x900 "
            << "callback_gap_ms=" << encode_growth_callback_gap_ms
            << " decodable_keyframe=1" << std::endl;

  double second_encode_growth_gap_ms = 0.0;
  if (!EncodeUntilOutput(&encoder, &callback, {1920, 1200}, &frame_index,
                         &second_encode_growth_gap_ms)) {
    std::cerr << "Encode 连续扩大会话分辨率失败" << std::endl;
    return 17;
  }
  if (!callback.has_decodable_keyframe()) {
    std::cerr << "Encode 连续扩大会话后的首帧不可独立解码" << std::endl;
    return 18;
  }
  for (int frame = 0; frame < 40; ++frame) {
    if (!EncodeUntilOutput(&encoder, &callback, {1920, 1200}, &frame_index,
                           &ignored_gap)) {
      std::cerr << "Encode 连续扩容后的持续编码失败" << std::endl;
      return 19;
    }
  }
  std::cout << "ENCODE_MULTI_GROWTH from=1440x900 to=1920x1200 "
            << "callback_gap_ms=" << second_encode_growth_gap_ms
            << " sustained_frames=40 decodable_keyframe=1" << std::endl;

  encoder.Release();
  std::this_thread::sleep_for(std::chrono::milliseconds(650));
  encoder.RegisterEncodeCompleteCallback(&callback);
  codec_settings.width = 640;
  codec_settings.height = 400;
  codec_settings.startBitrate = 700;
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "延迟释放后的重新初始化失败" << std::endl;
    return 20;
  }
  if (!EncodeUntilOutput(&encoder, &callback, {640, 400}, &frame_index,
                         &ignored_gap)) {
    std::cerr << "延迟释放后的编码失败" << std::endl;
    return 21;
  }
  std::cout << "DEFERRED_RELEASE_RESTART wait_ms=650 result=ok" << std::endl;

  encoder.Release();
  BlockingEncodedCallback blocking_callback;
  encoder.RegisterEncodeCompleteCallback(&blocking_callback);
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "回调屏障测试初始化失败" << std::endl;
    return 25;
  }
  const std::vector<webrtc::VideoFrameType> frame_types = {
      webrtc::VideoFrameType::kVideoFrameDelta};
  webrtc::VideoFrame blocking_frame =
      BuildFrame({640, 400}, frame_index++);
  if (encoder.Encode(blocking_frame, &frame_types) != WEBRTC_VIDEO_CODEC_OK ||
      !blocking_callback.WaitUntilEntered(std::chrono::seconds(2))) {
    std::cerr << "编码回调未进入屏障测试" << std::endl;
    blocking_callback.Unblock();
    encoder.Release();
    return 26;
  }

  std::atomic<bool> release_done(false);
  std::thread release_thread([&]() {
    encoder.Release();
    release_done.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (release_done.load(std::memory_order_acquire)) {
    std::cerr << "Release 未等待在途编码回调" << std::endl;
    blocking_callback.Unblock();
    release_thread.join();
    return 27;
  }
  blocking_callback.Unblock();
  release_thread.join();
  if (!release_done.load(std::memory_order_acquire)) {
    std::cerr << "解除回调阻塞后 Release 未完成" << std::endl;
    return 28;
  }
  std::cout << "RELEASE_CALLBACK_BARRIER blocked_ms=50 result=ok"
            << std::endl;

  EncodedCallback reentrant_target_callback;
  const Resolution reentrant_source = {640, 400};
  const Resolution reentrant_target = {1920, 1200};
  codec_settings.width = reentrant_source.width;
  codec_settings.height = reentrant_source.height;
  codec_settings.startBitrate = 700;
  ReentrantReconfigureCallback reentrant_callback(
      &encoder, &reentrant_target_callback, &codec_settings, &settings,
      reentrant_target);
  encoder.RegisterEncodeCompleteCallback(&reentrant_callback);
  if (encoder.InitEncode(&codec_settings, settings) != WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "重入回调测试初始化失败" << std::endl;
    return 29;
  }
  webrtc::VideoFrame reentrant_frame =
      BuildFrame(reentrant_source, frame_index++);
  if (encoder.Encode(reentrant_frame, &frame_types) != WEBRTC_VIDEO_CODEC_OK ||
      !reentrant_callback.WaitUntilEntered(std::chrono::seconds(2))) {
    std::cerr << "重入回调测试未进入旧回调" << std::endl;
    reentrant_callback.StartReconfigure();
    reentrant_callback.AllowReturn();
    encoder.Release();
    return 30;
  }
  // 先让预热会话产生一个旧 epoch 的目标分辨率回调，并在单 lease 屏障上
  // 等待。随后旧回调内 Release/Register，验证该 waiter 不会误投给新回调。
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  webrtc::VideoFrame queued_old_epoch_frame =
      BuildFrame(reentrant_target, frame_index++);
  if (encoder.Encode(queued_old_epoch_frame, &frame_types) !=
      WEBRTC_VIDEO_CODEC_OK) {
    std::cerr << "旧 epoch 目标帧提交失败" << std::endl;
    reentrant_callback.StartReconfigure();
    reentrant_callback.AllowReturn();
    encoder.Release();
    return 31;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  reentrant_callback.StartReconfigure();
  if (!reentrant_callback.WaitUntilOperationSubmitted(
          std::chrono::seconds(3)) ||
      !reentrant_callback.operation_ok()) {
    std::cerr << "回调内 Release/Register/InitEncode 失败" << std::endl;
    reentrant_callback.AllowReturn();
    encoder.Release();
    return 32;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (reentrant_target_callback.count() != 0) {
    std::cerr << "新会话回调与旧回调发生并发" << std::endl;
    reentrant_callback.AllowReturn();
    encoder.Release();
    return 33;
  }
  reentrant_callback.AllowReturn();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (reentrant_target_callback.count() != 0) {
    std::cerr << "Release 前等待的旧帧穿越到新 callback" << std::endl;
    encoder.Release();
    return 34;
  }
  if (!EncodeUntilOutput(&encoder, &reentrant_target_callback,
                         reentrant_target, &frame_index, &ignored_gap)) {
    std::cerr << "重入切换解除屏障后未恢复编码" << std::endl;
    encoder.Release();
    return 35;
  }
  std::cout << "REENTRANT_CALLBACK_SERIALIZATION blocked_ms=50 result=ok"
            << std::endl;
  encoder.Release();
  return 0;
}
