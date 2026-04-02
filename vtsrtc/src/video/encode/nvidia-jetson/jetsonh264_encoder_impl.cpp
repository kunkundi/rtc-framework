#include "jetsonh264_encoder_impl.h"

#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include "jetson_encoder.h"
#include "log/log_manager.h"

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
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() { Release(); }

bool JetsonH264EncoderImpl::CreateEncoderSlot(unsigned int width,
                                              unsigned int height,
                                              EncoderSlot* slot) {
  if (!slot) {
    return false;
  }

  auto encoder = JetsonEncoder::Create(width, height, V4L2_PIX_FMT_H264, false);
  if (!encoder) {
    return false;
  }

  ApplyRatesToEncoder(encoder.get());

  slot->encoder = std::move(encoder);
  slot->width = width;
  slot->height = height;
  slot->token = next_encoder_token_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void JetsonH264EncoderImpl::ApplyRatesToEncoder(JetsonEncoder* encoder) {
  if (!encoder) {
    return;
  }

  if (fps_ > 0) {
    encoder->SetFps(fps_);
  }
  if (bitrate_ > 0) {
    encoder->SetBitrate(bitrate_);
  }
}

std::pair<unsigned int, unsigned int>
JetsonH264EncoderImpl::SelectPrewarmResolution(
    unsigned int active_width, unsigned int active_height) const {
  std::pair<unsigned int, unsigned int> best = {0, 0};
  uint64_t best_delta = std::numeric_limits<uint64_t>::max();

  for (const auto& codec_height : rtc_config_.encode_params.codecs) {
    if (codec_height <= 0) {
      continue;
    }

    unsigned int candidate_h =
        AlignToEven(static_cast<unsigned int>(codec_height));
    unsigned int candidate_w = AlignToEven(static_cast<unsigned int>(
        (static_cast<uint64_t>(candidate_h) * 16 + 8) / 9));

    if (candidate_w < 16 || candidate_h < 16) {
      continue;
    }

    if (candidate_w == active_width && candidate_h == active_height) {
      continue;
    }

    uint64_t delta = (candidate_h > active_height)
                         ? (candidate_h - active_height)
                         : (active_height - candidate_h);
    if (delta < best_delta) {
      best_delta = delta;
      best = {candidate_w, candidate_h};
    }
  }

  if (best.first != 0 && best.second != 0) {
    return best;
  }

  unsigned int half_w = AlignToEven(std::max(320u, active_width / 2));
  unsigned int half_h = AlignToEven(std::max(180u, active_height / 2));
  if ((half_w != active_width || half_h != active_height) && half_w >= 16 &&
      half_h >= 16) {
    return {half_w, half_h};
  }

  return {0, 0};
}

bool JetsonH264EncoderImpl::PrewarmStandbyForActiveResolution(
    unsigned int active_width, unsigned int active_height) {
  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    if (standby_encoder_.encoder) {
      return true;
    }
  }

  auto candidate = SelectPrewarmResolution(active_width, active_height);
  if (candidate.first == 0 || candidate.second == 0) {
    return false;
  }

  EncoderSlot warmed_slot;
  if (!CreateEncoderSlot(candidate.first, candidate.second, &warmed_slot)) {
    LOG_WARN("Prewarm standby encoder failed for <%ux%u>", candidate.first,
             candidate.second);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    if (standby_encoder_.encoder) {
      return true;
    }

    if (active_encoder_.encoder && active_encoder_.width == warmed_slot.width &&
        active_encoder_.height == warmed_slot.height) {
      return true;
    }

    standby_encoder_ = std::move(warmed_slot);
  }

  LOG_INFO("Prewarmed standby encoder <%ux%u>", candidate.first,
           candidate.second);
  return true;
}

void JetsonH264EncoderImpl::StartPrewarmWorker() {
  StopPrewarmWorker();

  {
    std::lock_guard<std::mutex> lock(prewarm_mutex_);
    prewarm_stop_ = false;
    prewarm_request_pending_ = false;
    prewarm_request_active_width_ = 0;
    prewarm_request_active_height_ = 0;
  }

  prewarm_thread_ =
      std::thread(&JetsonH264EncoderImpl::PrewarmWorkerLoop, this);
}

void JetsonH264EncoderImpl::StopPrewarmWorker() {
  {
    std::lock_guard<std::mutex> lock(prewarm_mutex_);
    prewarm_stop_ = true;
    prewarm_request_pending_ = false;
  }

  prewarm_cv_.notify_all();

  if (prewarm_thread_.joinable()) {
    prewarm_thread_.join();
  }
}

void JetsonH264EncoderImpl::RequestAsyncPrewarm(unsigned int active_width,
                                                unsigned int active_height) {
  if (active_width == 0 || active_height == 0) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(prewarm_mutex_);
    if (prewarm_stop_ || !prewarm_thread_.joinable()) {
      return;
    }

    prewarm_request_active_width_ = active_width;
    prewarm_request_active_height_ = active_height;
    prewarm_request_pending_ = true;
  }

  prewarm_cv_.notify_one();
}

void JetsonH264EncoderImpl::PrewarmWorkerLoop() {
  while (true) {
    unsigned int active_width = 0;
    unsigned int active_height = 0;

    {
      std::unique_lock<std::mutex> lock(prewarm_mutex_);
      prewarm_cv_.wait(
          lock, [this] { return prewarm_stop_ || prewarm_request_pending_; });

      if (prewarm_stop_) {
        return;
      }

      active_width = prewarm_request_active_width_;
      active_height = prewarm_request_active_height_;
      prewarm_request_pending_ = false;
    }

    PrewarmStandbyForActiveResolution(active_width, active_height);
  }
}

bool JetsonH264EncoderImpl::EnsureActiveEncoderForResolution(
    unsigned int width, unsigned int height) {
  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    if (active_encoder_.encoder && active_encoder_.width == width &&
        active_encoder_.height == height) {
      ApplyRatesToEncoder(active_encoder_.encoder.get());
      active_encoder_token_.store(active_encoder_.token,
                                  std::memory_order_release);
      return true;
    }

    if (standby_encoder_.encoder && standby_encoder_.width == width &&
        standby_encoder_.height == height) {
      std::swap(active_encoder_, standby_encoder_);
      ApplyRatesToEncoder(active_encoder_.encoder.get());
      active_encoder_.encoder->ForceKeyFrame();
      active_encoder_token_.store(active_encoder_.token,
                                  std::memory_order_release);
      return true;
    }
  }

  EncoderSlot new_slot;
  if (!CreateEncoderSlot(width, height, &new_slot)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);

    if (active_encoder_.encoder && active_encoder_.width == width &&
        active_encoder_.height == height) {
      ApplyRatesToEncoder(active_encoder_.encoder.get());
      active_encoder_token_.store(active_encoder_.token,
                                  std::memory_order_release);
      return true;
    }

    if (standby_encoder_.encoder && standby_encoder_.width == width &&
        standby_encoder_.height == height) {
      std::swap(active_encoder_, standby_encoder_);
      ApplyRatesToEncoder(active_encoder_.encoder.get());
      active_encoder_.encoder->ForceKeyFrame();
      active_encoder_token_.store(active_encoder_.token,
                                  std::memory_order_release);
      return true;
    }

    standby_encoder_ = std::move(active_encoder_);
    active_encoder_ = std::move(new_slot);
    ApplyRatesToEncoder(active_encoder_.encoder.get());
    active_encoder_.encoder->ForceKeyFrame();
    active_encoder_token_.store(active_encoder_.token,
                                std::memory_order_release);
    return true;
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

  StopPrewarmWorker();

  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    active_encoder_ = EncoderSlot();
    standby_encoder_ = EncoderSlot();
  }
  active_encoder_token_.store(0, std::memory_order_release);

  auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  codec_ = *codec_settings;
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

  if (!EnsureActiveEncoderForResolution(frame_width, frame_height)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Start standby prewarm in background so InitEncode stays non-blocking.
  StartPrewarmWorker();
  RequestAsyncPrewarm(frame_width, frame_height);

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::Release() {
  StopPrewarmWorker();

  active_encoder_token_.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    active_encoder_ = EncoderSlot();
    standby_encoder_ = EncoderSlot();
  }
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.ClearEncodedData();
    encoded_image_capacity_ = 0;
  }

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
  auto fps = static_cast<uint32_t>(parameters.framerate_fps);
  codec_.maxFramerate = fps;

  auto bitrate = parameters.bitrate.GetBitrate(0, 0);
  codec_.maxBitrate = bitrate;

  if (fps < 1 || bitrate < 1) {
    LOG_WARN("SetRates failed because framerate or bitrate is invalid");
    return;
  }

  fps_ = fps;
  bitrate_ = bitrate;

  std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
  ApplyRatesToEncoder(active_encoder_.encoder.get());
  ApplyRatesToEncoder(standby_encoder_.encoder.get());
}

int32_t JetsonH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  LOG_ERROR("33333333");
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

  if (!EnsureActiveEncoderForResolution(frame_width, frame_height)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (resolution_changed) {
    RequestAsyncPrewarm(frame_width, frame_height);
  }

  JetsonEncoder* active_encoder = nullptr;
  uint64_t encode_token = 0;
  {
    std::lock_guard<std::mutex> lock(encoder_slots_mutex_);
    active_encoder = active_encoder_.encoder.get();
    encode_token = active_encoder_.token;
  }

  if (!active_encoder) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (has_frame_type && (*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
    active_encoder->ForceKeyFrame();
  }

#if ENABLE_ENCODE_PERF_STATS
  auto encode_start_time = std::chrono::steady_clock::now();
#endif

  active_encoder->EmplaceBuffer(
      frame_buffer,
#if ENABLE_ENCODE_PERF_STATS
      [this, input_frame, encode_start_time, encode_token](
          const uint8_t* data, size_t size, bool is_keyframe,
          uint64_t timestamp) {
        if (encode_token !=
            active_encoder_token_.load(std::memory_order_acquire)) {
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
      [this, input_frame, encode_token](const uint8_t* data, size_t size,
                                        bool is_keyframe, uint64_t timestamp) {
        if (encode_token !=
            active_encoder_token_.load(std::memory_order_acquire)) {
          return;
        }

        SendFrame(input_frame, data, size, is_keyframe, 0);
      });
#endif

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
  info.scaling_settings =
      VideoEncoder::ScalingSettings(kLowH264QpThreshold, kHighH264QpThreshold);
  info.is_hardware_accelerated = true;
  info.has_internal_source = false;
  info.supports_simulcast = false;

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

void JetsonH264EncoderImpl::ReconfigureEncoderRates(uint32_t fps,
                                                    uint32_t bitrate) {}

void JetsonH264EncoderImpl::ReconfigureEncoderIDR() {}

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
