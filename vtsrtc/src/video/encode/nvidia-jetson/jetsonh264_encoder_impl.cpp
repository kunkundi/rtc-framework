#include "jetsonh264_encoder_impl.h"

#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include "jetson_encoder.h"
#include "log/log_manager.h"
#include "video/encode/playout_delay_config.h"

namespace webrtc {

static const int kLowH264QpThreshold = 37;
static const int kHighH264QpThreshold = 39;

enum class H264EncoderImplEvent {
  H264EncoderEventInit = 0,
  H264EncoderEventError = 1,
  H264EncoderEventMax = 16,
};

static inline unsigned int AlignToEven(unsigned int value) {
  return value & ~1u;
}

static inline unsigned int ClampQpValue(unsigned int value) {
  return std::min(value, 51u);
}

JetsonH264EncoderImpl::JetsonH264EncoderImpl(
    const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config)
    : rtc_config_(rtc_config) {
  RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));

  std::string packetization_mode_string;
  if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
                     &packetization_mode_string) &&
      packetization_mode_string == "1") {
    packetization_mode_ = H264PacketizationMode::NonInterleaved;
  }

  if (rtc_config_.encode_params.I_frame_interval != 0) {
    gop_size_ = rtc_config_.encode_params.I_frame_interval;
  }

  if (rtc_config_.encode_params.qp_threshold.first > 0 &&
      rtc_config_.encode_params.qp_threshold.second >
          rtc_config_.encode_params.qp_threshold.first) {
    qp_threshold_ = rtc_config_.encode_params.qp_threshold;
  }

  if (rtc_config_.encode_params.qp_range.first > 0 &&
      rtc_config_.encode_params.qp_range.second >=
          rtc_config_.encode_params.qp_range.first) {
    qp_range_.first = ClampQpValue(rtc_config_.encode_params.qp_range.first);
    qp_range_.second = ClampQpValue(rtc_config_.encode_params.qp_range.second);
  }

  if (!rtc_config_.encode_params.bitrate_mode.empty()) {
    if (absl::EqualsIgnoreCase(rtc_config_.encode_params.bitrate_mode, "vbr") ||
        absl::EqualsIgnoreCase(rtc_config_.encode_params.bitrate_mode, "cbr")) {
      bitrate_mode_ = rtc_config_.encode_params.bitrate_mode;
    } else {
      LOG_WARN(
          "[WEBRTC] Unsupported bitrate_mode=%s for jetson, fallback to cbr",
          rtc_config_.encode_params.bitrate_mode.c_str());
    }
  }

  if (rtc_config_.encode_params.bitrate_minmum != 0) {
    bitrate_floor_bps_ = rtc_config_.encode_params.bitrate_minmum;
  }

  if (rtc_config_.encode_params.bitrate_maxmum != 0) {
    bitrate_cap_bps_ = rtc_config_.encode_params.bitrate_maxmum;
  }

  if (bitrate_floor_bps_ > 0 && bitrate_cap_bps_ < bitrate_floor_bps_) {
    bitrate_cap_bps_ = bitrate_floor_bps_;
  }

  const auto configured_playout_delay = ResolveConfiguredPlayoutDelay(
      rtc_config_.encode_params, "jetson");
  has_configured_playout_delay_ = configured_playout_delay.enabled;
  configured_playout_delay_min_ms_ = configured_playout_delay.min_ms;
  configured_playout_delay_max_ms_ = configured_playout_delay.max_ms;
  if (has_configured_playout_delay_) {
    LOG_INFO("[WEBRTC] Enable playout delay for jetson: [%d, %d] ms",
             configured_playout_delay_min_ms_,
             configured_playout_delay_max_ms_);
  }

  InitializeResolutionBitrateLimits();
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() { Release(); }

void JetsonH264EncoderImpl::ApplyRatesToEncoder(JetsonEncoder* encoder) {
  if (!encoder) {
    return;
  }

  // NVENC frame rate is configured before STREAMON. Only bitrate is safe to
  // update while the encoder is streaming.
  if (bitrate_ > 0) {
    encoder->SetBitrate(bitrate_);
  }
}

JetsonEncoder::StrategyConfig JetsonH264EncoderImpl::BuildStrategyConfig() const {
  JetsonEncoder::StrategyConfig config;
  config.bitrate_mode = absl::EqualsIgnoreCase(bitrate_mode_, "vbr")
                            ? V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
                            : V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
  config.gop_size = std::max(1u, gop_size_);
  if (qp_range_.first > 0 && qp_range_.second >= qp_range_.first) {
    config.has_qp_range = true;
    config.qp_min = qp_range_.first;
    config.qp_max = qp_range_.second;
  }
  return config;
}

bool JetsonH264EncoderImpl::EnsureEncoderForResolution(unsigned int width,
                                                       unsigned int height) {
  std::lock_guard<std::mutex> lock(encoder_mutex_);

  if (encoder_ && width_ == width && height_ == height) {
    ApplyRatesToEncoder(encoder_.get());
    return true;
  }

  encoder_generation_.fetch_add(1, std::memory_order_acq_rel);
  const auto strategy_config = BuildStrategyConfig();

  bool switched_encoder = false;
  if (encoder_) {
    encoder_->SetStrategyConfig(strategy_config);
    if (!encoder_->Reconfigure(width, height)) {
      LOG_WARN(
          "Reconfigure Jetson encoder to <%ux%u> failed, recreate encoder",
          width, height);
      encoder_.reset();
    } else {
      switched_encoder = true;
    }
  }

  if (!encoder_) {
    encoder_ = JetsonEncoder::Create(width, height, V4L2_PIX_FMT_H264, false,
                                     strategy_config,
                                     static_cast<int>(hardware_framerate_),
                                     static_cast<int>(bitrate_));
    if (!encoder_) {
      LOG_ERROR("Failed to create Jetson encoder for <%ux%u>", width, height);
      return false;
    }
    switched_encoder = true;
  }

  width_ = width;
  height_ = height;
  ApplyRatesToEncoder(encoder_.get());

  if (switched_encoder) {
    encoder_->ForceKeyFrame();
  }

  return true;
}

void JetsonH264EncoderImpl::InitializeResolutionBitrateLimits() {
  resolution_bitrate_limits_.clear();

  if (rtc_config_.use_strategy) {
    for (const auto& item : rtc_config_.strategy) {
      if (item.second.size() < 3) {
        continue;
      }
      resolution_bitrate_limits_.emplace_back(
          static_cast<int>(item.first), static_cast<int>(item.second[0]),
          static_cast<int>(item.second[1]), static_cast<int>(item.second[2]));
    }
  }

  if (!resolution_bitrate_limits_.empty()) {
    return;
  }

  const int max_bitrate = static_cast<int>(
      std::min<unsigned int>(rtc_config_.encode_params.bitrate_maxmum == 0
                                 ? 100000000u
                                 : rtc_config_.encode_params.bitrate_maxmum,
                             100000000u));

  const auto append_limit = [this, max_bitrate](int width, int height) {
    const int pixels = width * height;
    for (const auto& limit : resolution_bitrate_limits_) {
      if (limit.frame_size_pixels == pixels) {
        return;
      }
    }
    resolution_bitrate_limits_.emplace_back(pixels, 1, 1, max_bitrate);
  };

  append_limit(320, 180);
  append_limit(480, 270);
  append_limit(640, 360);
  append_limit(960, 540);
  append_limit(1280, 720);

  for (const auto& codec_height : rtc_config_.encode_params.codecs) {
    if (codec_height == 0) {
      continue;
    }
    const unsigned int aligned_height = AlignToEven(codec_height);
    const unsigned int aligned_width = AlignToEven(static_cast<unsigned int>(
        (static_cast<uint64_t>(aligned_height) * 16 + 8) / 9));
    if (aligned_width >= 16 && aligned_height >= 16) {
      append_limit(static_cast<int>(aligned_width),
                   static_cast<int>(aligned_height));
    }
  }
}

int JetsonH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                      const VideoEncoder::Settings& settings) {
  LOG_INFO("[WebRTC] Init Jetson H264 encoder");

  ReportInit();

  if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->maxFramerate == 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->width < 1 || codec_settings->height < 1) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  encoder_generation_.fetch_add(1, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    encoder_.reset();
  }

  auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  codec_ = *codec_settings;
  hardware_framerate_ = codec_.maxFramerate;
  max_payload_size_ = settings.max_payload_size;

  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  const auto frame_width = codec_.simulcastStream[0].width;
  const auto frame_height = codec_.simulcastStream[0].height;

  width_ = frame_width;
  height_ = frame_height;

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, frame_width, frame_height);
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
    encoded_image_capacity_ = new_capacity;
    encoded_image_._completeFrame = true;
    encoded_image_._encodedWidth = frame_width;
    encoded_image_._encodedHeight = frame_height;
    encoded_image_.set_size(0);
  }

  SimulcastRateAllocator init_allocator(codec_);
  auto allocation = init_allocator.Allocate(VideoBitrateAllocationParameters(
      DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));

  if (!EnsureEncoderForResolution(frame_width, frame_height)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::Release() {
  encoder_generation_.fetch_add(1, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    encoder_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.ClearEncodedData();
    encoded_image_capacity_ = 0;
  }
  encoded_image_callback_ = nullptr;

  width_ = 0;
  height_ = 0;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;

  return WEBRTC_VIDEO_CODEC_OK;
}

void JetsonH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  const double framerate_fps = parameters.framerate_fps;
  const uint64_t bitrate_bps = parameters.bitrate.get_sum_bps();
  if (!std::isfinite(framerate_fps) || framerate_fps < 1.0 ||
      bitrate_bps < 1) {
    LOG_WARN(
        "[WEBRTC] jetson SetRates failed because framerate or bitrate is "
        "invalid (fps=%.3f, bitrate=%llu)",
        framerate_fps, static_cast<unsigned long long>(bitrate_bps));
    return;
  }

  const uint32_t fps = static_cast<uint32_t>(std::min<double>(
      std::round(framerate_fps),
      static_cast<double>(std::numeric_limits<uint32_t>::max())));
  const uint32_t bitrate = static_cast<uint32_t>(std::min<uint64_t>(
      bitrate_bps, std::numeric_limits<uint32_t>::max()));

  std::lock_guard<std::mutex> lock(encoder_mutex_);
  codec_.maxFramerate = fps;
  codec_.maxBitrate = bitrate;

  const auto previous_fps = fps_;
  const auto previous_bitrate = bitrate_;
  fps_ = fps;
  bitrate_ =
      static_cast<unsigned int>(std::min<uint64_t>(bitrate, bitrate_cap_bps_));
  if (bitrate_floor_bps_ > 0) {
    bitrate_ = std::max(bitrate_, bitrate_floor_bps_);
  }

  if (previous_fps == fps_ && previous_bitrate == bitrate_) {
    return;
  }

  if (previous_fps != fps_ && encoder_ && fps_ != hardware_framerate_) {
    LOG_WARN(
        "[JetsonEnc][rates] WebRTC requested fps=%u, keep NVENC hardware "
        "fps=%u; recreate encoder to change hardware fps",
        fps_, hardware_framerate_);
  }

  ApplyRatesToEncoder(encoder_.get());
}

int32_t JetsonH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_) {
    LOG_ERROR(
        "InitEncode() has been called, but a callback function "
        "has not been set with RegisterEncodeCompleteCallback()");
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  const bool has_frame_type = frame_types && !frame_types->empty();
  if (has_frame_type && (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  auto frame_buffer = input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const unsigned int frame_width = frame_buffer->width();
  const unsigned int frame_height = frame_buffer->height();
  const bool resolution_changed =
      (frame_width != width_ || frame_height != height_);

  if (!EnsureEncoderForResolution(frame_width, frame_height)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (resolution_changed) {
    width_ = frame_width;
    height_ = frame_height;
    const size_t new_capacity =
        CalcBufferSize(VideoType::kI420, width_, height_);
    {
      std::lock_guard<std::mutex> lock(encoded_image_mutex_);
      encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
      encoded_image_capacity_ = new_capacity;
      encoded_image_._encodedWidth = width_;
      encoded_image_._encodedHeight = height_;
      encoded_image_.set_size(0);
    }
  }

  const bool request_keyframe =
      has_frame_type && (*frame_types)[0] == VideoFrameType::kVideoFrameKey;

#if ENABLE_ENCODE_PERF_STATS
  auto encode_start_time = std::chrono::steady_clock::now();
#endif

  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (!encoder_) {
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (request_keyframe) {
      encoder_->ForceKeyFrame();
    }

    const uint64_t encode_generation =
        encoder_generation_.load(std::memory_order_acquire);

    encoder_->EmplaceBuffer(
        frame_buffer,
#if ENABLE_ENCODE_PERF_STATS
        [this, input_frame, encode_start_time,
         encode_generation](const uint8_t* data, size_t size, bool is_keyframe,
                            uint64_t timestamp) {
          if (encode_generation !=
              encoder_generation_.load(std::memory_order_acquire)) {
            return;
          }

          auto encode_end_time = std::chrono::steady_clock::now();
          int64_t encode_duration_us =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  encode_end_time - encode_start_time)
                  .count();

          SendFrame(input_frame, data, size, is_keyframe, encode_duration_us);
        });
#else
        [this, input_frame, encode_generation](const uint8_t* data, size_t size,
                                               bool is_keyframe,
                                               uint64_t timestamp) {
          if (encode_generation !=
              encoder_generation_.load(std::memory_order_acquire)) {
            return;
          }

          SendFrame(input_frame, data, size, is_keyframe, 0);
        });
#endif
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

void JetsonH264EncoderImpl::SendFrame(const VideoFrame& frame,
                                      const uint8_t* data, size_t size,
                                      bool is_keyframe,
                                      int64_t encode_duration_us) {
  if (size == 0) {
    return;
  }

#if ENABLE_ENCODE_PERF_STATS
  if (encode_duration_us > 0) {
    encode_stats_.total_encode_time_us += encode_duration_us;
    encode_stats_.max_encode_time_us =
        std::max(encode_stats_.max_encode_time_us, encode_duration_us);
    encode_stats_.min_encode_time_us =
        std::min(encode_stats_.min_encode_time_us, encode_duration_us);
    encode_stats_.frame_count++;
    if (is_keyframe) {
      encode_stats_.keyframe_count++;
    }

    LOG_INFO(
        "[编码性能] 帧编码耗时: %ld us (%.2f ms), 大小: %zu bytes, 关键帧: %s",
        encode_duration_us, encode_duration_us / 1000.0f, size,
        is_keyframe ? "是" : "否");

    auto now = std::chrono::steady_clock::now();
    bool should_log_stats = false;
    if (encode_stats_.frame_count == 1) {
      encode_stats_.last_log_time = now;
      should_log_stats = false;
    } else {
      auto time_since_last_log =
          std::chrono::duration_cast<std::chrono::seconds>(
              now - encode_stats_.last_log_time)
              .count();
      if (encode_stats_.frame_count % 100 == 0 || time_since_last_log >= 5) {
        should_log_stats = true;
        encode_stats_.last_log_time = now;
      }
    }

    if (should_log_stats && encode_stats_.frame_count > 0) {
      int64_t avg_encode_time_us =
          encode_stats_.total_encode_time_us / encode_stats_.frame_count;
      float fps = codec_.maxFramerate;
      float frame_budget_ms = 1000.0f / fps;
      float utilization =
          (avg_encode_time_us / 1000.0f) / frame_budget_ms * 100.0f;

      LOG_INFO(
          "[编码性能统计] 总帧数: %u, 关键帧数: %u, 平均耗时: %ld us (%.2f "
          "ms), "
          "最大耗时: %ld us (%.2f ms), 最小耗时: %ld us (%.2f ms), "
          "帧率: %.1f fps, 时间预算: %.2f ms/帧, 占用率: %.1f%%",
          encode_stats_.frame_count, encode_stats_.keyframe_count,
          avg_encode_time_us, avg_encode_time_us / 1000.0f,
          encode_stats_.max_encode_time_us,
          encode_stats_.max_encode_time_us / 1000.0f,
          encode_stats_.min_encode_time_us == INT64_MAX
              ? 0
              : encode_stats_.min_encode_time_us,
          encode_stats_.min_encode_time_us == INT64_MAX
              ? 0.0f
              : encode_stats_.min_encode_time_us / 1000.0f,
          fps, frame_budget_ms, utilization);

      if (utilization > 80.0f) {
        LOG_WARN(
            "[编码性能警告] 编码耗时占用率过高 (%.1f%%), "
            "可能影响实时性能。建议："
            "1) 降低分辨率或帧率 2) 检查格式转换耗时 3) "
            "确认setMaxPerfMode已启用",
            utilization);
      } else if (utilization > 60.0f) {
        LOG_WARN("[编码性能提示] 编码耗时占用率较高 (%.1f%%), 建议监控性能",
                 utilization);
      } else if (utilization < 30.0f && avg_encode_time_us < 5000) {
        LOG_INFO(
            "[编码性能] ✓ 编码性能优秀！耗时: %.2f ms, 占用率: %.1f%%, "
            "有充足的时间余量",
            avg_encode_time_us / 1000.0f, utilization);
      } else if (utilization < 50.0f) {
        LOG_INFO("[编码性能] ✓ 编码性能良好，占用率: %.1f%%", utilization);
      }
    }
  }
#endif

  auto nalu_indices = H264::FindNaluIndices(data, size);
  if (nalu_indices.empty()) {
    return;
  }

  VideoFrameType frame_type = VideoFrameType::kVideoFrameDelta;
  if (is_keyframe) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x07) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x01) {
    frame_type = VideoFrameType::kVideoFrameDelta;
  }

  RTPFragmentationHeader frag_header;
  frag_header.VerifyAndAllocateFragmentationHeader(nalu_indices.size());
  for (size_t i = 0; i < nalu_indices.size(); ++i) {
    frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
    frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
  }

  CodecSpecificInfo codec_specific;
  codec_specific.codecType = kVideoCodecH264;
  codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
  codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
  codec_specific.codecSpecific.H264.idr_frame =
      (frame_type == VideoFrameType::kVideoFrameKey);
  codec_specific.codecSpecific.H264.base_layer_sync = false;

  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);

    if (size > encoded_image_capacity_) {
      encoded_image_.SetEncodedData(EncodedImageBuffer::Create(size));
      encoded_image_capacity_ = size;
    }

    encoded_image_._completeFrame = true;
    encoded_image_._encodedWidth = frame.width();
    encoded_image_._encodedHeight = frame.height();
    encoded_image_.set_size(size);
    if (has_configured_playout_delay_) {
      encoded_image_.playout_delay_.min_ms = configured_playout_delay_min_ms_;
      encoded_image_.playout_delay_.max_ms = configured_playout_delay_max_ms_;
    }
    encoded_image_.SetTimestamp(frame.timestamp());
    encoded_image_.ntp_time_ms_ = frame.ntp_time_ms();
    encoded_image_.capture_time_ms_ = frame.render_time_ms();
    encoded_image_.rotation_ = frame.rotation();
    encoded_image_.SetColorSpace(frame.color_space());
    encoded_image_.content_type_ = codec_.mode == VideoCodecMode::kScreensharing
                                       ? VideoContentType::SCREENSHARE
                                       : VideoContentType::UNSPECIFIED;
    encoded_image_.SetSpatialIndex(0);
    encoded_image_._frameType = frame_type;

    memcpy(encoded_image_.data(), data, size);

    h264_bitstream_parser_.ParseBitstream(encoded_image_.data(),
                                          encoded_image_.size());
    auto qp = h264_bitstream_parser_.GetLastSliceQp();
    if (qp.has_value()) {
      encoded_image_.qp_ = qp.value();
    }

    encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific,
                                            &frag_header);
  }
}

VideoEncoder::EncoderInfo JetsonH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "JetsonH264";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.has_internal_source = false;
  info.supports_simulcast = false;
  info.scaling_settings.min_pixels_per_frame = 360 * 180;
  info.requested_resolution_alignment = 2;
  info.resolution_bitrate_limits = resolution_bitrate_limits_;

  return info;
}

void JetsonH264EncoderImpl::SetFecControllerOverride(
    FecControllerOverride* fec_controller_override) {}

void JetsonH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {
  /* LOG_INFO("[WEBRTC] OnPacketLossRateUpdate: %f", packet_loss_rate); */
}

void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
  /* LOG_INFO("[WEBRTC] OnRttUpdate: %d", rtt_ms); */
}

void JetsonH264EncoderImpl::OnLossNotification(
    const LossNotification& loss_notification) {
  // auto delta = loss_notification.timestamp_of_last_decodable -
  //     loss_notification.timestamp_of_last_received;
  // LOG_INFO("[WEBRTC] OnLossNotification timestamp between"
  //     "last decodable and last received frame: %d", delta);
}

void JetsonH264EncoderImpl::ReportInit() {
  if (has_reported_init_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

  has_reported_init_ = true;
}

void JetsonH264EncoderImpl::ReportError() {
  if (has_reported_error_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventError),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

  has_reported_error_ = true;
}

}  // namespace webrtc
