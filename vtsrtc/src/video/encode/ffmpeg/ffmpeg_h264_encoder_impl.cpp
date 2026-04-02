#include "ffmpeg_h264_encoder_impl.h"

#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "log/log_manager.h"

namespace webrtc {

namespace {

constexpr int kDefaultLowH264QpThreshold = 24;
constexpr int kDefaultHighH264QpThreshold = 37;
constexpr int kEncoderCaptureBufferCount = 10;
constexpr unsigned int kUltrafastPreset = 1;
constexpr auto kFirstPacketWaitTimeout = std::chrono::milliseconds(50);
constexpr auto kPacketPollInterval = std::chrono::milliseconds(1);

#if defined(NVMPI_ENC_CHUNK_SIZE)
constexpr size_t kNvmpiPacketBufferSize = NVMPI_ENC_CHUNK_SIZE;
#else
constexpr size_t kNvmpiPacketBufferSize = 2 * 1024 * 1024;
#endif

enum class H264EncoderImplEvent {
  H264EncoderEventInit = 0,
  H264EncoderEventError = 1,
  H264EncoderEventMax = 16,
};

std::atomic<uint64_t> g_ffmpeg_h264_encoder_instance_id{0};

void CopyPlane(uint8_t* dst, int dst_stride, const uint8_t* src, int src_stride,
               int width, int height) {
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst + row * dst_stride, src + row * src_stride, width);
  }
}

}  // namespace

FFmpegH264EncoderImpl::FFmpegH264EncoderImpl(
    const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config)
    : rtc_config_(rtc_config),
      instance_id_(g_ffmpeg_h264_encoder_instance_id.fetch_add(1) + 1) {
  RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));
  LOG_INFO("[WEBRTC] FFmpegH264EncoderImpl ctor, instance=%llu",
           static_cast<unsigned long long>(instance_id_));

  std::string packetization_mode_string;
  if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
                     &packetization_mode_string) &&
      packetization_mode_string == "1") {
    packetization_mode_ = H264PacketizationMode::NonInterleaved;
  }

  auto profile_level_id = H264::ParseSdpProfileLevelId(codec.params);
  if (profile_level_id.has_value()) {
    profile_ = profile_level_id->profile;
    level_ = profile_level_id->level;
  }

  if (rtc_config_.encode_params.qp_threshold.first != 0 &&
      rtc_config_.encode_params.qp_threshold.second != 0) {
    qp_threshold_ = rtc_config_.encode_params.qp_threshold;
  }

  if (rtc_config_.encode_params.I_frame_interval != 0) {
    gop_size_ = rtc_config_.encode_params.I_frame_interval;
  }

  if (!rtc_config_.encode_params.bitrate_mode.empty()) {
    bitrate_mode_ = rtc_config_.encode_params.bitrate_mode;
  }

  if (rtc_config_.encode_params.bitrate_maxmum != 0) {
    bitrate_cap_bps_ = rtc_config_.encode_params.bitrate_maxmum;
  }

  InitializeResolutionBitrateLimits();
}

FFmpegH264EncoderImpl::~FFmpegH264EncoderImpl() {
  LOG_INFO(
      "[WEBRTC] FFmpegH264EncoderImpl dtor, instance=%llu, encode_calls=%llu, "
      "delivered=%llu, empty_drains=%llu",
      static_cast<unsigned long long>(instance_id_),
      static_cast<unsigned long long>(encode_calls_),
      static_cast<unsigned long long>(delivered_packets_),
      static_cast<unsigned long long>(empty_drains_));
  Release();
}

int FFmpegH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                      const VideoEncoder::Settings& settings) {
  LOG_INFO("[WEBRTC] Init jetson-ffmpeg H264 encoder");
  LOG_INFO("[WEBRTC] InitEncode enter, instance=%llu",
           static_cast<unsigned long long>(instance_id_));

  ReportInit();

  if (codec_settings == nullptr ||
      codec_settings->codecType != kVideoCodecH264) {
    LOG_ERROR("[WEBRTC] InitEncode failed: invalid codec settings");
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->maxFramerate == 0) {
    LOG_ERROR("[WEBRTC] InitEncode failed: maxFramerate is 0");
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->width < 1 || codec_settings->height < 1) {
    LOG_ERROR("[WEBRTC] InitEncode failed: invalid resolution <%dx%d>",
              codec_settings->width, codec_settings->height);
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  const auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    LOG_ERROR(
        "[WEBRTC] InitEncode failed: simulcast is not supported, streams=%zu",
        num_of_streams);
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  int32_t release_result = Release();
  if (release_result != WEBRTC_VIDEO_CODEC_OK) {
    LOG_ERROR("[WEBRTC] InitEncode failed: Release() returned %d",
              release_result);
    ReportError();
    return release_result;
  }

  codec_ = *codec_settings;
  max_payload_size_ = settings.max_payload_size;

  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  width_ = codec_.simulcastStream[0].width;
  height_ = codec_.simulcastStream[0].height;

  const size_t new_capacity = CalcBufferSize(VideoType::kI420, width_, height_);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_capacity_ = new_capacity;
  encoded_image_._completeFrame = true;
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.set_size(0);

  SimulcastRateAllocator init_allocator(codec_);
  const auto allocation =
      init_allocator.Allocate(VideoBitrateAllocationParameters(
          DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));

  std::lock_guard<std::mutex> lock(encoder_mutex_);
  if (!InitializeEncoderLocked(width_, height_)) {
    LOG_ERROR(
        "[WEBRTC] InitEncode failed: InitializeEncoderLocked(%u, %u) failed",
        width_, height_);
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  LOG_INFO(
      "[WEBRTC] InitEncode success: instance=%llu, frame=<%ux%u>, fps=%u, bitrate=%u, "
      "gop=%u, packetization_mode=%d, max_payload_size=%zu",
      static_cast<unsigned long long>(instance_id_), width_, height_, fps_,
      bitrate_bps_, gop_size_,
      static_cast<int>(packetization_mode_), max_payload_size_);

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t FFmpegH264EncoderImpl::Release() {
  LOG_INFO(
      "[WEBRTC] Release enter, instance=%llu, encode_calls=%llu, "
      "delivered=%llu, empty_drains=%llu",
      static_cast<unsigned long long>(instance_id_),
      static_cast<unsigned long long>(encode_calls_),
      static_cast<unsigned long long>(delivered_packets_),
      static_cast<unsigned long long>(empty_drains_));
  std::lock_guard<std::mutex> lock(encoder_mutex_);
  DestroyEncoderLocked();
  encoded_image_.ClearEncodedData();
  encoded_image_capacity_ = 0;
  has_reported_missing_callback_ = false;
  force_keyframe_after_callback_ = false;
  pending_encoder_reconfigure_ = false;
  width_ = 0;
  height_ = 0;
  y_plane_.clear();
  u_plane_.clear();
  v_plane_.clear();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t FFmpegH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(encoder_mutex_);
  encoded_image_callback_ = callback;
  LOG_INFO(
      "[WEBRTC] RegisterEncodeCompleteCallback called, instance=%llu, "
      "callback=%p",
      static_cast<unsigned long long>(instance_id_), callback);
  return WEBRTC_VIDEO_CODEC_OK;
}

void FFmpegH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  const auto fps = static_cast<uint32_t>(parameters.framerate_fps);
  const auto bitrate = parameters.bitrate.GetBitrate(0, 0);
  LOG_INFO(
      "[WEBRTC] SetRates called, instance=%llu, fps=%u, bitrate=%llu",
      static_cast<unsigned long long>(instance_id_), fps,
      static_cast<unsigned long long>(bitrate));
  codec_.maxFramerate = fps;
  codec_.maxBitrate = bitrate;

  if (fps < 1 || bitrate < 1) {
    LOG_WARN(
        "[WEBRTC] SetRates failed because framerate or bitrate is invalid");
    return;
  }

  std::lock_guard<std::mutex> lock(encoder_mutex_);
  fps_ = fps;
  bitrate_bps_ =
      static_cast<unsigned int>(std::min<uint64_t>(bitrate, bitrate_cap_bps_));

  if (encoder_ == nullptr) {
    return;
  }

#if defined(NVMPI_ENC_CHUNK_SIZE)
  pending_encoder_reconfigure_ = true;
#else
  const int fps_result = nvmpi_encoder_set_fps(encoder_, fps_);
  const int bitrate_result = nvmpi_encoder_set_bitrate(encoder_, bitrate_bps_);
  if (fps_result != 0 || bitrate_result != 0) {
    LOG_WARN(
        "[WEBRTC] nvmpi runtime rate update failed, encoder will be recreated");
    pending_encoder_reconfigure_ = true;
  }
#endif
}

int32_t FFmpegH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  ++encode_calls_;
  if (encode_calls_ == 1 || encode_calls_ % 90 == 0) {
    LOG_INFO(
        "[WEBRTC] Encode entry encoder=%s, frame=<%dx%d>, ts_us=%lld, "
        "ts_rtp=%u, encode_calls=%llu, instance=%llu",
        encoder_name_.c_str(), input_frame.width(), input_frame.height(),
        static_cast<long long>(input_frame.timestamp_us()),
        input_frame.timestamp(),
        static_cast<unsigned long long>(encode_calls_),
        static_cast<unsigned long long>(instance_id_));
  }
  const bool has_frame_type = frame_types != nullptr && !frame_types->empty();
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
  if (frame_width == 0 || frame_height == 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if ((frame_width & 1u) != 0 || (frame_height & 1u) != 0) {
    LOG_ERROR(
        "[WEBRTC] jetson-ffmpeg H264 encoder only supports even resolution, "
        "got <%ux%u>",
        frame_width, frame_height);
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  std::lock_guard<std::mutex> lock(encoder_mutex_);

  if (encoded_image_callback_ == nullptr) {
    if (!has_reported_missing_callback_) {
      LOG_WARN(
          "[WEBRTC] Encode() was called before "
          "RegisterEncodeCompleteCallback(); dropping frames until the "
          "callback is available");
      has_reported_missing_callback_ = true;
    }
    force_keyframe_after_callback_ = true;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  has_reported_missing_callback_ = false;

  if (encoder_ == nullptr || frame_width != width_ || frame_height != height_ ||
      pending_encoder_reconfigure_) {
    if (!ReinitializeEncoder(frame_width, frame_height)) {
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  const bool request_keyframe =
      force_keyframe_after_callback_ ||
      (has_frame_type && (*frame_types)[0] == VideoFrameType::kVideoFrameKey);

  if (request_keyframe && !RequestKeyFrameLocked()) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!CopyFrameToEncoderLocked(*frame_buffer)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const int drain_result = DrainPacketsLocked(input_frame);
  if (drain_result < 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

bool FFmpegH264EncoderImpl::ReinitializeEncoder(unsigned int width,
                                                unsigned int height) {
  width_ = width;
  height_ = height;

  const size_t new_capacity = CalcBufferSize(VideoType::kI420, width_, height_);
  if (new_capacity > encoded_image_capacity_) {
    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
    encoded_image_capacity_ = new_capacity;
  }
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.set_size(0);

  DestroyEncoderLocked();
  pending_encoder_reconfigure_ = false;
  return InitializeEncoderLocked(width_, height_);
}

bool FFmpegH264EncoderImpl::InitializeEncoderLocked(unsigned int width,
                                                    unsigned int height) {
  nvEncParam params = {};
  params.width = width;
  params.height = height;
  params.profile = MapProfileToNvmpi(profile_);
  params.level = static_cast<unsigned int>(level_);
  params.bitrate = bitrate_bps_;
  params.peak_bitrate = std::max(bitrate_bps_, bitrate_cap_bps_);
  params.enableLossless = 0;
  params.mode_vbr = bitrate_mode_ == "vbr" ? 1 : 0;
  params.insert_spspps_idr = 1;
  params.iframe_interval = std::max(1u, gop_size_);
  params.idr_interval = std::max(1u, gop_size_);
  params.fps_n = std::max(1u, fps_);
  params.fps_d = 1;
  params.capture_num = kEncoderCaptureBufferCount;
  params.max_b_frames = 0;
  params.refs = 0;
  params.qmax = 0;
  params.qmin = 0;
  params.hw_preset_type = kUltrafastPreset;

#if defined(NVMPI_ENC_CHUNK_SIZE)
  params.vbv_buffer_size = bitrate_bps_;
  params.codingType = NV_VIDEO_CodingH264;
  encoder_ = nvmpi_create_encoder(&params);
#else
  encoder_ = nvmpi_create_encoder(NV_VIDEO_CodingH264, &params);
#endif

  if (encoder_ == nullptr) {
    LOG_ERROR("[WEBRTC] Failed to create jetson-ffmpeg H264 encoder");
    return false;
  }

#if defined(NVMPI_ENC_CHUNK_SIZE)
  if (!InitializePacketPoolLocked()) {
    LOG_ERROR("[WEBRTC] Failed to initialize jetson-ffmpeg packet pool");
    DestroyEncoderLocked();
    return false;
  }
#endif

  is_hardware_encoder_ = true;
  encoder_name_ = "h264_nvmpi";
  LOG_INFO(
      "[WEBRTC] InitializeEncoderLocked success: encoder=%s, frame=<%ux%u>, "
      "fps=%u, bitrate=%u, gop=%u, instance=%llu",
      encoder_name_.c_str(), width, height, fps_, bitrate_bps_, gop_size_,
      static_cast<unsigned long long>(instance_id_));
  return true;
}

void FFmpegH264EncoderImpl::DestroyEncoderLocked() {
  if (encoder_ != nullptr) {
    nvmpi_encoder_close(encoder_);
    encoder_ = nullptr;
  }
#if defined(NVMPI_ENC_CHUNK_SIZE)
  ReleasePacketPoolLocked();
#endif
}

bool FFmpegH264EncoderImpl::RequestKeyFrameLocked() {
  if (encoder_ == nullptr) {
    return false;
  }

#if defined(NVMPI_ENC_CHUNK_SIZE)
  return ReinitializeEncoder(width_, height_);
#else
  return nvmpi_encoder_force_idr(encoder_) == 0;
#endif
}

void FFmpegH264EncoderImpl::EnsureScratchBufferCapacityLocked(
    unsigned int width, unsigned int height) {
  const size_t y_size =
      static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t uv_size = y_size / 4;

  if (y_plane_.size() != y_size) {
    y_plane_.resize(y_size);
  }
  if (u_plane_.size() != uv_size) {
    u_plane_.resize(uv_size);
  }
  if (v_plane_.size() != uv_size) {
    v_plane_.resize(uv_size);
  }
}

bool FFmpegH264EncoderImpl::CopyFrameToEncoderLocked(
    const I420BufferInterface& frame_buffer) {
  if (encoder_ == nullptr) {
    return false;
  }

  const unsigned int width = frame_buffer.width();
  const unsigned int height = frame_buffer.height();
  EnsureScratchBufferCapacityLocked(width, height);

  CopyPlane(y_plane_.data(), static_cast<int>(width), frame_buffer.DataY(),
            frame_buffer.StrideY(), static_cast<int>(width),
            static_cast<int>(height));
  CopyPlane(u_plane_.data(), static_cast<int>(width / 2), frame_buffer.DataU(),
            frame_buffer.StrideU(), static_cast<int>(width / 2),
            static_cast<int>(height / 2));
  CopyPlane(v_plane_.data(), static_cast<int>(width / 2), frame_buffer.DataV(),
            frame_buffer.StrideV(), static_cast<int>(width / 2),
            static_cast<int>(height / 2));

  nvFrame frame = {};
  frame.type = NV_PIX_YUV420;
  frame.width = width;
  frame.height = height;
  frame.payload[0] = y_plane_.data();
  frame.payload[1] = u_plane_.data();
  frame.payload[2] = v_plane_.data();
  frame.payload_size[0] = y_plane_.size();
  frame.payload_size[1] = u_plane_.size();
  frame.payload_size[2] = v_plane_.size();
  frame.linesize[0] = width;
  frame.linesize[1] = width / 2;
  frame.linesize[2] = width / 2;

  const int ret = nvmpi_encoder_put_frame(encoder_, &frame);
  if (ret != 0) {
    LOG_ERROR("[WEBRTC] nvmpi_encoder_put_frame failed");
    return false;
  }
  return true;
}

int FFmpegH264EncoderImpl::DrainPacketsLocked(const VideoFrame& frame) {
  bool delivered_packet = false;
  const auto wait_deadline =
      std::chrono::steady_clock::now() + kFirstPacketWaitTimeout;

  while (true) {
    const uint8_t* payload = nullptr;
    size_t payload_size = 0;
    bool is_keyframe = false;
    int ret = 0;

#if defined(NVMPI_ENC_CHUNK_SIZE)
    nvPacket* packet = nullptr;
    ret = nvmpi_encoder_get_packet(encoder_, &packet);
    if (ret != 0 || packet == nullptr) {
      if (delivered_packet ||
          std::chrono::steady_clock::now() >= wait_deadline) {
        if (!delivered_packet) {
          ++empty_drains_;
          if (empty_drains_ == 1 || empty_drains_ % 90 == 0) {
            LOG_WARN(
                "[WEBRTC] No encoded packet available after waiting %lld ms, "
                "empty_drains=%llu",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        kFirstPacketWaitTimeout)
                        .count()),
                static_cast<unsigned long long>(empty_drains_));
          }
        }
        return 0;
      }
      std::this_thread::sleep_for(kPacketPollInterval);
      continue;
    }

    payload = packet->payload;
    payload_size = static_cast<size_t>(packet->payload_size);
    is_keyframe = (packet->flags & 0x1UL) != 0;

    const int deliver_result =
        DeliverPacketLocked(frame, payload, payload_size, is_keyframe);
    delivered_packet = true;
    nvmpi_encoder_qEmptyPacket(encoder_, packet);
    if (deliver_result != WEBRTC_VIDEO_CODEC_OK) {
      return deliver_result;
    }
#else
    nvPacket packet = {};
    ret = nvmpi_encoder_get_packet(encoder_, &packet);
    if (ret != 0) {
      if (delivered_packet ||
          std::chrono::steady_clock::now() >= wait_deadline) {
        if (!delivered_packet) {
          ++empty_drains_;
          if (empty_drains_ == 1 || empty_drains_ % 90 == 0) {
            LOG_WARN(
                "[WEBRTC] No encoded packet available after waiting %lld ms, "
                "empty_drains=%llu",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        kFirstPacketWaitTimeout)
                        .count()),
                static_cast<unsigned long long>(empty_drains_));
          }
        }
        return 0;
      }
      std::this_thread::sleep_for(kPacketPollInterval);
      continue;
    }

    payload = packet.payload;
    payload_size = static_cast<size_t>(packet.payload_size);
    is_keyframe = (packet.flags & 0x1UL) != 0;

    const int deliver_result =
        DeliverPacketLocked(frame, payload, payload_size, is_keyframe);
    delivered_packet = true;
    if (deliver_result != WEBRTC_VIDEO_CODEC_OK) {
      return deliver_result;
    }
#endif
  }
}

int FFmpegH264EncoderImpl::DeliverPacketLocked(const VideoFrame& frame,
                                               const uint8_t* payload,
                                               size_t payload_size,
                                               bool is_keyframe) {
  if (payload == nullptr || payload_size == 0) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  auto nalu_indices = H264::FindNaluIndices(payload, payload_size);
  if (nalu_indices.empty()) {
    LOG_WARN("[WEBRTC] jetson-ffmpeg returned packet without Annex-B NALU");
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  RTPFragmentationHeader frag_header;
  frag_header.VerifyAndAllocateFragmentationHeader(nalu_indices.size());
  for (size_t i = 0; i < nalu_indices.size(); ++i) {
    frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
    frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
  }

  if (payload_size > encoded_image_capacity_) {
    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(payload_size));
    encoded_image_capacity_ = payload_size;
  }

  encoded_image_._completeFrame = true;
  encoded_image_._encodedWidth = frame.width();
  encoded_image_._encodedHeight = frame.height();
  encoded_image_.set_size(payload_size);
  encoded_image_.SetTimestamp(frame.timestamp());
  encoded_image_.ntp_time_ms_ = frame.ntp_time_ms();
  encoded_image_.capture_time_ms_ = frame.render_time_ms();
  encoded_image_.rotation_ = frame.rotation();
  encoded_image_.SetColorSpace(frame.color_space());
  encoded_image_.content_type_ = codec_.mode == VideoCodecMode::kScreensharing
                                     ? VideoContentType::SCREENSHARE
                                     : VideoContentType::UNSPECIFIED;
  encoded_image_.SetSpatialIndex(0);
  encoded_image_._frameType = is_keyframe ? VideoFrameType::kVideoFrameKey
                                          : VideoFrameType::kVideoFrameDelta;
  std::memcpy(encoded_image_.data(), payload, payload_size);

  h264_bitstream_parser_.ParseBitstream(encoded_image_.data(),
                                        encoded_image_.size());
  auto qp = h264_bitstream_parser_.GetLastSliceQp();
  if (qp.has_value()) {
    encoded_image_.qp_ = qp.value();
  }

  CodecSpecificInfo codec_specific;
  codec_specific.codecType = kVideoCodecH264;
  codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
  codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
  codec_specific.codecSpecific.H264.idr_frame = is_keyframe;
  codec_specific.codecSpecific.H264.base_layer_sync = false;
  if (is_keyframe) {
    force_keyframe_after_callback_ = false;
  }

  if (encoded_image_callback_ == nullptr) {
    LOG_ERROR("[WEBRTC] Encode callback is null while delivering packet");
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  ++delivered_packets_;
  if (delivered_packets_ == 1 || delivered_packets_ % 90 == 0) {
    LOG_INFO(
        "[WEBRTC] Deliver encoded packet encoder=%s, bytes=%zu, key=%d, "
        "ts_rtp=%u, width=%d, height=%d, delivered=%llu",
        encoder_name_.c_str(), payload_size, is_keyframe ? 1 : 0,
        frame.timestamp(), frame.width(), frame.height(),
        static_cast<unsigned long long>(delivered_packets_));
  }
  encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific,
                                          &frag_header);
  return WEBRTC_VIDEO_CODEC_OK;
}

#if defined(NVMPI_ENC_CHUNK_SIZE)
bool FFmpegH264EncoderImpl::InitializePacketPoolLocked() {
  packet_pool_.clear();
  packet_payload_pool_.clear();
  packet_pool_.reserve(kEncoderCaptureBufferCount);
  packet_payload_pool_.reserve(kEncoderCaptureBufferCount);

  for (int i = 0; i < kEncoderCaptureBufferCount; ++i) {
    auto payload = std::make_unique<unsigned char[]>(kNvmpiPacketBufferSize);
    auto packet = std::make_unique<nvPacket>();
    std::memset(packet.get(), 0, sizeof(nvPacket));
    packet->payload = payload.get();
    packet->payload_size = 0;
    packet->pts = 0;
    packet->flags = 0;
    packet->privData = nullptr;
    nvmpi_encoder_qEmptyPacket(encoder_, packet.get());
    packet_payload_pool_.push_back(std::move(payload));
    packet_pool_.push_back(std::move(packet));
  }

  return true;
}

void FFmpegH264EncoderImpl::ReleasePacketPoolLocked() {
  packet_pool_.clear();
  packet_payload_pool_.clear();
}
#endif

void FFmpegH264EncoderImpl::InitializeResolutionBitrateLimits() {
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

  // Use permissive defaults so WebRTC doesn't suspend this custom encoder
  // before it has a chance to produce frames under low startup bitrate.
  const int max_bitrate = static_cast<int>(
      std::min<unsigned int>(bitrate_cap_bps_, 100000000u));
  resolution_bitrate_limits_.emplace_back(320 * 180, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(480 * 270, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(640 * 360, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(960 * 540, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(1280 * 720, 1, 1, max_bitrate);
}

unsigned int FFmpegH264EncoderImpl::MapProfileToNvmpi(H264::Profile profile) {
  switch (profile) {
    case H264::kProfileBaseline:
      return 66;
    case H264::kProfileHigh:
      return 100;
    case H264::kProfileConstrainedBaseline:
    default:
      return 66;
  }
}

VideoEncoder::EncoderInfo FFmpegH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "jetson-ffmpeg-h264_nvmpi";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = is_hardware_encoder_;
  info.has_internal_source = false;
  info.supports_simulcast = false;
  info.requested_resolution_alignment = 2;
  info.resolution_bitrate_limits = resolution_bitrate_limits_;
  return info;
}

void FFmpegH264EncoderImpl::SetFecControllerOverride(
    FecControllerOverride* fec_controller_override) {}

void FFmpegH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {}

void FFmpegH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {}

void FFmpegH264EncoderImpl::OnLossNotification(
    const LossNotification& loss_notification) {}

void FFmpegH264EncoderImpl::ReportInit() {
  if (has_reported_init_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));
  has_reported_init_ = true;
}

void FFmpegH264EncoderImpl::ReportError() {
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
