#include "rasp_h264_encoder_impl.h"

#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

extern "C" {
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
}

#include "log/log_manager.h"

namespace webrtc {

namespace {

constexpr int kFirstPacketWaitTimeoutMs = 50;
constexpr int kSubsequentPacketWaitTimeoutMs = 0;
constexpr size_t kMaxPendingFrames = 32;

enum class H264EncoderImplEvent {
  H264EncoderEventInit = 0,
  H264EncoderEventError = 1,
  H264EncoderEventMax = 16,
};

struct PipelineCandidate {
  std::string pipeline_desc;
  const char* name = "";
  bool is_hardware = true;
  bool bitrate_uses_kbps = false;
};

std::atomic<uint64_t> g_rasp_h264_encoder_instance_id{0};
std::once_flag g_rasp_gstreamer_init_once;
bool g_rasp_gstreamer_init_result = false;

void CopyPlane(uint8_t* dst, int dst_stride, const uint8_t* src, int src_stride,
               int width, int height) {
  for (int row = 0; row < height; ++row) {
    std::memcpy(dst + row * dst_stride, src + row * src_stride, width);
  }
}

bool EnsureGstreamerInitialized() {
  std::call_once(g_rasp_gstreamer_init_once, [] {
    GError* error = nullptr;
    g_rasp_gstreamer_init_result = gst_init_check(nullptr, nullptr, &error);
    if (!g_rasp_gstreamer_init_result) {
      const char* message = (error != nullptr && error->message != nullptr)
                                ? error->message
                                : "unknown";
      LOG_ERROR("[WEBRTC] gst_init_check failed: %s", message);
      if (error != nullptr) {
        g_error_free(error);
      }
    }
  });
  return g_rasp_gstreamer_init_result;
}

bool PacketContainsKeyframe(const uint8_t* payload, size_t payload_size) {
  auto nalu_indices = H264::FindNaluIndices(payload, payload_size);
  for (const auto& nalu : nalu_indices) {
    if (nalu.payload_size == 0 || nalu.payload_start_offset >= payload_size) {
      continue;
    }
    const uint8_t nal_type = payload[nalu.payload_start_offset] & 0x1f;
    if (nal_type == 5 || nal_type == 7) {
      return true;
    }
  }
  return false;
}

}  // namespace

RaspH264EncoderImpl::RaspH264EncoderImpl(
    const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config)
    : rtc_config_(rtc_config),
      instance_id_(g_rasp_h264_encoder_instance_id.fetch_add(1) + 1) {
  RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));

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

  if (rtc_config_.encode_params.I_frame_interval != 0) {
    gop_size_ = rtc_config_.encode_params.I_frame_interval;
  }

  if (rtc_config_.encode_params.qp_threshold.first > 0 &&
      rtc_config_.encode_params.qp_threshold.second >
          rtc_config_.encode_params.qp_threshold.first) {
    qp_threshold_ = rtc_config_.encode_params.qp_threshold;
  }

  if (!rtc_config_.encode_params.bitrate_mode.empty()) {
    bitrate_mode_ = rtc_config_.encode_params.bitrate_mode;
  }

  if (rtc_config_.encode_params.bitrate_maxmum != 0) {
    bitrate_cap_bps_ = rtc_config_.encode_params.bitrate_maxmum;
  }

  InitializeResolutionBitrateLimits();
}

RaspH264EncoderImpl::~RaspH264EncoderImpl() { Release(); }

int RaspH264EncoderImpl::InitEncode(
    const VideoCodec* codec_settings, const VideoEncoder::Settings& settings) {
  LOG_INFO("[WEBRTC] Init rasp-gstreamer H264 encoder");

  ReportInit();

  if (codec_settings == nullptr ||
      codec_settings->codecType != kVideoCodecH264) {
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

  const auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  int32_t release_result = Release();
  if (release_result != WEBRTC_VIDEO_CODEC_OK) {
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
  if (!InitializePipelineLocked(width_, height_)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t RaspH264EncoderImpl::Release() {
  std::lock_guard<std::mutex> lock(encoder_mutex_);
  DestroyPipelineLocked();
  encoded_image_.ClearEncodedData();
  encoded_image_capacity_ = 0;
  has_reported_missing_callback_ = false;
  force_keyframe_after_callback_ = false;
  pending_pipeline_reconfigure_ = false;
  has_logged_restart_warning_ = false;
  width_ = 0;
  height_ = 0;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t RaspH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  std::lock_guard<std::mutex> lock(encoder_mutex_);
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

void RaspH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  const double framerate_fps = parameters.framerate_fps;
  const uint64_t bitrate_bps = parameters.bitrate.get_sum_bps();
  if (!std::isfinite(framerate_fps) || framerate_fps < 1.0 ||
      bitrate_bps < 1) {
    LOG_WARN(
        "[WEBRTC] rasp-gstreamer SetRates failed because framerate or "
        "bitrate is invalid (fps=%.3f, bitrate=%llu)",
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
  const auto previous_bitrate_bps = bitrate_bps_;
  fps_ = fps;
  bitrate_bps_ =
      static_cast<unsigned int>(std::min<uint64_t>(bitrate, bitrate_cap_bps_));

  const bool fps_changed = previous_fps != fps_;
  const bool bitrate_changed = previous_bitrate_bps != bitrate_bps_;
  if (!fps_changed && !bitrate_changed) {
    return;
  }

  if (pipeline_ == nullptr || appsrc_ == nullptr ||
      encoder_element_ == nullptr) {
    return;
  }

  if (bitrate_changed) {
    const guint runtime_bitrate =
        bitrate_property_uses_kbps_
            ? std::max(1u, static_cast<unsigned int>(bitrate_bps_ / 1000u))
            : bitrate_bps_;
    GObjectClass* encoder_class = G_OBJECT_GET_CLASS(encoder_element_);
    if (g_object_class_find_property(encoder_class, "bitrate") != nullptr) {
      g_object_set(G_OBJECT(encoder_element_), "bitrate",
                   runtime_bitrate, nullptr);
    } else if (g_object_class_find_property(encoder_class, "extra-controls") !=
               nullptr) {
      GstStructure* controls = gst_structure_new(
          "controls", "video_bitrate", G_TYPE_INT,
          static_cast<int>(bitrate_bps_), nullptr);
      g_object_set(G_OBJECT(encoder_element_), "extra-controls", controls,
                   nullptr);
      gst_structure_free(controls);
    } else if (!has_logged_restart_warning_) {
      LOG_WARN(
          "[WEBRTC] rasp-gstreamer encoder has no runtime bitrate property; "
          "bitrate update will apply on pipeline reinitialization");
      has_logged_restart_warning_ = true;
    }
  }

  if (fps_changed) {
    // Keep pipeline caps stable to avoid re-negotiation interruptions.
    // Runtime fps updates are reflected by frame PTS/DURATION pacing only.
  }
}

int32_t RaspH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
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
        "[WEBRTC] rasp-gstreamer H264 encoder only supports even "
        "resolution, got <%ux%u>",
        frame_width, frame_height);
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  std::lock_guard<std::mutex> lock(encoder_mutex_);
  const unsigned int bitrate_bps_snapshot = bitrate_bps_;
  const unsigned int fps_snapshot = fps_;
  LOG_INFO(
      "[WEBRTC] Encode enter: instance=%llu, frame=<%ux%u>, bitrate=%u, fps=%u",
      static_cast<unsigned long long>(instance_id_), frame_width, frame_height,
      bitrate_bps_snapshot, fps_snapshot);

  if (encoded_image_callback_ == nullptr) {
    if (!has_reported_missing_callback_) {
      LOG_WARN(
          "[WEBRTC] rasp-gstreamer Encode() was called before "
          "RegisterEncodeCompleteCallback(); dropping frames until the "
          "callback is available");
      has_reported_missing_callback_ = true;
    }
    force_keyframe_after_callback_ = true;
    return WEBRTC_VIDEO_CODEC_OK;
  }
  has_reported_missing_callback_ = false;

  if (pipeline_ == nullptr || appsrc_ == nullptr || appsink_ == nullptr ||
      frame_width != width_ || frame_height != height_ ||
      pending_pipeline_reconfigure_) {
    if (!ReinitializePipelineLocked(frame_width, frame_height)) {
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }

  if (DrainPacketsLocked(kSubsequentPacketWaitTimeoutMs) < 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const bool request_keyframe =
      force_keyframe_after_callback_ ||
      (has_frame_type && (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (request_keyframe && !RequestKeyFrameLocked()) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!WriteFrameToPipeLocked(*frame_buffer)) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  pending_frames_.push_back(PendingFrame{input_frame});
  if (pending_frames_.size() > kMaxPendingFrames) {
    LOG_WARN(
        "[WEBRTC] rasp-gstreamer pending frame queue overflow (%zu), "
        "dropping oldest pending metadata",
        pending_frames_.size());
    pending_frames_.pop_front();
  }

  if (DrainPacketsLocked(kFirstPacketWaitTimeoutMs) < 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo RaspH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "rasp-gstreamer-h264";
  info.scaling_settings =
      VideoEncoder::ScalingSettings(static_cast<int>(qp_threshold_.first),
                                    static_cast<int>(qp_threshold_.second));
  info.is_hardware_accelerated = is_hardware_encoder_;
  info.has_internal_source = false;
  info.supports_simulcast = false;
  info.scaling_settings.min_pixels_per_frame = 360 * 180;
  info.requested_resolution_alignment = 2;
  info.resolution_bitrate_limits = resolution_bitrate_limits_;
  return info;
}

void RaspH264EncoderImpl::SetFecControllerOverride(
    FecControllerOverride* fec_controller_override) {}

void RaspH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {}

void RaspH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {}

void RaspH264EncoderImpl::OnLossNotification(
    const LossNotification& loss_notification) {}

bool RaspH264EncoderImpl::InitializePipelineLocked(unsigned int width,
                                                        unsigned int height) {
  if (!EnsureGstreamerInitialized()) {
    return false;
  }

  width_ = width;
  height_ = height;
  pending_frames_.clear();
  ready_packets_.clear();
  pending_pipeline_reconfigure_ = false;
  has_logged_restart_warning_ = false;
  next_buffer_pts_ns_ = 0;
  bitrate_property_uses_kbps_ = false;

  const size_t new_capacity = CalcBufferSize(VideoType::kI420, width_, height_);
  if (new_capacity > encoded_image_capacity_) {
    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
    encoded_image_capacity_ = new_capacity;
  }
  encoded_image_._encodedWidth = width_;
  encoded_image_._encodedHeight = height_;
  encoded_image_.set_size(0);

  return LaunchPipelineLocked(width, height);
}

void RaspH264EncoderImpl::DestroyPipelineLocked() {
  pending_frames_.clear();
  ready_packets_.clear();

  if (appsrc_ != nullptr) {
    gst_app_src_end_of_stream(appsrc_);
  }

  if (pipeline_ != nullptr) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_element_get_state(pipeline_, nullptr, nullptr, GST_MSECOND * 500);
  }

  if (pipeline_bus_ != nullptr) {
    gst_object_unref(pipeline_bus_);
    pipeline_bus_ = nullptr;
  }
  if (encoder_element_ != nullptr) {
    gst_object_unref(encoder_element_);
    encoder_element_ = nullptr;
  }
  if (appsink_ != nullptr) {
    gst_object_unref(appsink_);
    appsink_ = nullptr;
  }
  if (appsrc_ != nullptr) {
    gst_object_unref(appsrc_);
    appsrc_ = nullptr;
  }
  if (pipeline_ != nullptr) {
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
  }

  next_buffer_pts_ns_ = 0;
}

bool RaspH264EncoderImpl::ReinitializePipelineLocked(unsigned int width,
                                                          unsigned int height) {
  DestroyPipelineLocked();
  return InitializePipelineLocked(width, height);
}

bool RaspH264EncoderImpl::WriteFrameToPipeLocked(
    const I420BufferInterface& frame_buffer) {
  if (appsrc_ == nullptr) {
    return false;
  }

  const unsigned int width = frame_buffer.width();
  const unsigned int height = frame_buffer.height();
  const size_t total_size =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2;
  input_frame_bytes_.resize(total_size);

  const int y_plane_size = static_cast<int>(width * height);
  const int uv_width = static_cast<int>(width / 2);
  const int uv_height = static_cast<int>(height / 2);
  const int uv_plane_size = uv_width * uv_height;

  CopyPlane(input_frame_bytes_.data(), static_cast<int>(width),
            frame_buffer.DataY(), frame_buffer.StrideY(),
            static_cast<int>(width), static_cast<int>(height));
  CopyPlane(input_frame_bytes_.data() + y_plane_size, uv_width,
            frame_buffer.DataU(), frame_buffer.StrideU(), uv_width, uv_height);
  CopyPlane(input_frame_bytes_.data() + y_plane_size + uv_plane_size, uv_width,
            frame_buffer.DataV(), frame_buffer.StrideV(), uv_width, uv_height);

  GstBuffer* gst_buffer = gst_buffer_new_allocate(nullptr, total_size, nullptr);
  if (gst_buffer == nullptr) {
    LOG_ERROR("[WEBRTC] gst_buffer_new_allocate failed");
    return false;
  }

  GstMapInfo map_info = {};
  if (!gst_buffer_map(gst_buffer, &map_info, GST_MAP_WRITE)) {
    LOG_ERROR("[WEBRTC] gst_buffer_map failed for appsrc input");
    gst_buffer_unref(gst_buffer);
    return false;
  }
  std::memcpy(map_info.data, input_frame_bytes_.data(), total_size);
  gst_buffer_unmap(gst_buffer, &map_info);

  const uint64_t duration_ns =
      gst_util_uint64_scale_int(1, GST_SECOND, std::max(1u, fps_));
  GST_BUFFER_PTS(gst_buffer) = next_buffer_pts_ns_;
  GST_BUFFER_DTS(gst_buffer) = next_buffer_pts_ns_;
  GST_BUFFER_DURATION(gst_buffer) = duration_ns;
  next_buffer_pts_ns_ += duration_ns;

  const GstFlowReturn push_result =
      gst_app_src_push_buffer(appsrc_, gst_buffer);
  if (push_result != GST_FLOW_OK) {
    LOG_ERROR("[WEBRTC] gst_app_src_push_buffer failed: %d",
              static_cast<int>(push_result));
    MaybeLogHelperExitLocked("WriteFrameToPipeLocked");
    return false;
  }

  return true;
}

bool RaspH264EncoderImpl::PullPacketFromSinkLocked(
    int timeout_ms, std::vector<uint8_t>* packet) {
  if (appsink_ == nullptr || packet == nullptr) {
    return false;
  }

  const GstClockTime timeout =
      timeout_ms < 0 ? GST_CLOCK_TIME_NONE
                     : static_cast<GstClockTime>(timeout_ms) * GST_MSECOND;
  GstSample* sample = gst_app_sink_try_pull_sample(appsink_, timeout);
  if (sample == nullptr) {
    return false;
  }

  GstBuffer* gst_buffer = gst_sample_get_buffer(sample);
  if (gst_buffer == nullptr) {
    gst_sample_unref(sample);
    return false;
  }

  GstMapInfo map_info = {};
  if (!gst_buffer_map(gst_buffer, &map_info, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return false;
  }
  packet->assign(map_info.data, map_info.data + map_info.size);
  gst_buffer_unmap(gst_buffer, &map_info);
  gst_sample_unref(sample);
  return !packet->empty();
}

int RaspH264EncoderImpl::DrainPacketsLocked(int first_wait_timeout_ms) {
  if (appsink_ == nullptr) {
    return -1;
  }

  if (!ready_packets_.empty()) {
    first_wait_timeout_ms = kSubsequentPacketWaitTimeoutMs;
  }

  if (ready_packets_.empty()) {
    std::vector<uint8_t> packet;
    if (PullPacketFromSinkLocked(first_wait_timeout_ms, &packet)) {
      ready_packets_.push_back(std::move(packet));
    } else {
      MaybeLogHelperExitLocked("DrainPacketsLocked");
      return 0;
    }
  }

  while (true) {
    std::vector<uint8_t> packet;
    if (!PullPacketFromSinkLocked(kSubsequentPacketWaitTimeoutMs, &packet)) {
      break;
    }
    ready_packets_.push_back(std::move(packet));
  }

  if (ready_packets_.empty()) {
    MaybeLogHelperExitLocked("DrainPacketsLocked");
    return 0;
  }

  int delivered_packets = 0;
  while (!ready_packets_.empty() && !pending_frames_.empty()) {
    PendingFrame pending = pending_frames_.front();
    pending_frames_.pop_front();

    std::vector<uint8_t> packet = std::move(ready_packets_.front());
    ready_packets_.pop_front();

    const int deliver_result =
        DeliverPacketLocked(pending.frame, packet.data(), packet.size());
    if (deliver_result != WEBRTC_VIDEO_CODEC_OK) {
      return -1;
    }
    delivered_packets++;
  }

  return delivered_packets;
}

int RaspH264EncoderImpl::DeliverPacketLocked(const VideoFrame& frame,
                                                  const uint8_t* payload,
                                                  size_t payload_size) {
  if (payload == nullptr || payload_size == 0) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  auto nalu_indices = H264::FindNaluIndices(payload, payload_size);
  if (nalu_indices.empty()) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  const bool is_keyframe = PacketContainsKeyframe(payload, payload_size);

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
    LOG_ERROR(
        "[WEBRTC] rasp-gstreamer encode callback is null while delivering "
        "packet");
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific,
                                          &frag_header);
  return WEBRTC_VIDEO_CODEC_OK;
}

bool RaspH264EncoderImpl::RequestKeyFrameLocked() {
  // Keep this best-effort mode simple in Raspberry Pi gstreamer path.
  // Keyframe hints are currently satisfied by GOP policy.
  return true;
}

bool RaspH264EncoderImpl::LaunchPipelineLocked(unsigned int width,
                                                    unsigned int height) {
  const unsigned int fps = std::max(1u, fps_);
  const unsigned int profile = MapProfileToNvvLaunchProfileValue(profile_);
  const unsigned int control_rate =
      absl::EqualsIgnoreCase(bitrate_mode_, "vbr") ? 0u : 1u;
  const unsigned int x264_bitrate_kbps = std::max(1u, bitrate_bps_ / 1000u);
  const unsigned int key_interval = std::max(1u, gop_size_);
  const auto has_element = [](const char* element_name) {
    GstElementFactory* factory = gst_element_factory_find(element_name);
    if (factory == nullptr) {
      return false;
    }
    gst_object_unref(factory);
    return true;
  };

  const bool has_v4l2h264enc = has_element("v4l2h264enc");
  const bool has_v4l2convert = has_element("v4l2convert");
  const bool has_videoconvert = has_element("videoconvert");
  const bool has_nvv4l2h264enc = has_element("nvv4l2h264enc");
  const bool has_nvvidconv = has_element("nvvidconv");
  const bool has_x264enc = has_element("x264enc");

  std::vector<PipelineCandidate> pipeline_candidates;
  if (has_v4l2h264enc && has_v4l2convert) {
    std::ostringstream pipeline_desc;
    pipeline_desc << "appsrc name=src is-live=true block=true format=time "
                     "do-timestamp=true "
                  << "caps=video/x-raw,format=I420,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! v4l2convert"
                  << " ! video/x-raw,format=NV12,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! v4l2h264enc name=enc"
                  << " ! video/x-h264,stream-format=byte-stream,alignment=au"
                  << " ! appsink name=sink sync=false drop=false max-buffers=16";
    pipeline_candidates.push_back(
        PipelineCandidate{pipeline_desc.str(), "v4l2h264enc+v4l2convert", true,
                          false});
  }
  if (has_v4l2h264enc && has_videoconvert) {
    std::ostringstream pipeline_desc;
    pipeline_desc << "appsrc name=src is-live=true block=true format=time "
                     "do-timestamp=true "
                  << "caps=video/x-raw,format=I420,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! videoconvert"
                  << " ! video/x-raw,format=NV12,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! v4l2h264enc name=enc"
                  << " ! video/x-h264,stream-format=byte-stream,alignment=au"
                  << " ! appsink name=sink sync=false drop=false max-buffers=16";
    pipeline_candidates.push_back(
        PipelineCandidate{pipeline_desc.str(), "v4l2h264enc+videoconvert", true,
                          false});
  }
  if (has_nvv4l2h264enc && has_nvvidconv) {
    std::ostringstream pipeline_desc;
    pipeline_desc << "appsrc name=src is-live=true block=true format=time "
                     "do-timestamp=true "
                  << "caps=video/x-raw,format=I420,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! nvvidconv"
                  << " ! video/x-raw(memory:NVMM),format=NV12,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! nvv4l2h264enc name=enc bitrate=" << bitrate_bps_
                  << " control-rate=" << control_rate
                  << " iframeinterval=" << key_interval
                  << " idrinterval=" << key_interval
                  << " insert-sps-pps=true insert-vui=true insert-aud=true"
                  << " maxperf-enable=true preset-level=1 profile=" << profile
                  << " num-B-Frames=0"
                  << " ! video/x-h264,stream-format=byte-stream,alignment=au"
                  << " ! appsink name=sink sync=false drop=false max-buffers=16";
    pipeline_candidates.push_back(
        PipelineCandidate{pipeline_desc.str(), "nvv4l2h264enc+nvvidconv", true,
                          false});
  }
  if (has_x264enc && has_videoconvert) {
    std::ostringstream pipeline_desc;
    pipeline_desc << "appsrc name=src is-live=true block=true format=time "
                     "do-timestamp=true "
                  << "caps=video/x-raw,format=I420,width=" << width
                  << ",height=" << height << ",framerate=" << fps << "/1"
                  << " ! videoconvert"
                  << " ! x264enc name=enc tune=zerolatency speed-preset=ultrafast "
                  << "bitrate=" << x264_bitrate_kbps
                  << " key-int-max=" << key_interval
                  << " bframes=0 byte-stream=true aud=true"
                  << " ! video/x-h264,stream-format=byte-stream,alignment=au"
                  << " ! appsink name=sink sync=false drop=false max-buffers=16";
    pipeline_candidates.push_back(
        PipelineCandidate{pipeline_desc.str(), "x264enc+videoconvert", false,
                          true});
  }

  if (pipeline_candidates.empty()) {
    LOG_ERROR(
        "[WEBRTC] no usable gstreamer H264 encoder pipeline candidates found "
        "(need one of v4l2h264enc/nvv4l2h264enc/x264enc)");
    return false;
  }

  const PipelineCandidate* selected_candidate = nullptr;
  for (size_t i = 0; i < pipeline_candidates.size(); ++i) {
    GError* error = nullptr;
    pipeline_ =
        gst_parse_launch(pipeline_candidates[i].pipeline_desc.c_str(), &error);
    if (pipeline_ != nullptr && error == nullptr) {
      selected_candidate = &pipeline_candidates[i];
      if (i > 0) {
        LOG_WARN(
            "[WEBRTC] rasp-gstreamer fallback pipeline selected (index=%zu)",
            i);
      }
      break;
    }

    const char* message = (error != nullptr && error->message != nullptr)
                              ? error->message
                              : "unknown";
    LOG_WARN("[WEBRTC] rasp-gstreamer gst_parse_launch failed (index=%zu): %s",
             i, message);
    if (error != nullptr) {
      g_error_free(error);
    }
    if (pipeline_ != nullptr) {
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
  }

  if (pipeline_ == nullptr) {
    LOG_ERROR("[WEBRTC] failed to build Raspberry Pi gstreamer H264 pipeline");
    DestroyPipelineLocked();
    return false;
  }
  if (selected_candidate != nullptr) {
    is_hardware_encoder_ = selected_candidate->is_hardware;
    bitrate_property_uses_kbps_ = selected_candidate->bitrate_uses_kbps;
    encoder_name_ = selected_candidate->name;
    LOG_INFO("[WEBRTC] rasp-gstreamer selected pipeline: %s (hardware=%d)",
             selected_candidate->name, selected_candidate->is_hardware);
  }

  GstElement* appsrc_element = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
  GstElement* appsink_element = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
  GstElement* encoder_element = gst_bin_get_by_name(GST_BIN(pipeline_), "enc");
  if (appsrc_element == nullptr || appsink_element == nullptr ||
      encoder_element == nullptr) {
    LOG_ERROR(
        "[WEBRTC] failed to fetch appsrc/appsink/encoder from gstreamer "
        "pipeline");
    if (appsrc_element != nullptr) {
      gst_object_unref(appsrc_element);
    }
    if (appsink_element != nullptr) {
      gst_object_unref(appsink_element);
    }
    if (encoder_element != nullptr) {
      gst_object_unref(encoder_element);
    }
    DestroyPipelineLocked();
    return false;
  }

  appsrc_ = GST_APP_SRC(appsrc_element);
  appsink_ = GST_APP_SINK(appsink_element);
  encoder_element_ = encoder_element;
  pipeline_bus_ = gst_element_get_bus(pipeline_);

  gst_app_sink_set_emit_signals(appsink_, FALSE);
  gst_app_sink_set_drop(appsink_, FALSE);
  gst_app_sink_set_max_buffers(appsink_, 16);
  gst_app_sink_set_wait_on_eos(appsink_, FALSE);

  const guint runtime_bitrate =
      bitrate_property_uses_kbps_
          ? std::max(1u, static_cast<unsigned int>(bitrate_bps_ / 1000u))
          : bitrate_bps_;
  GObjectClass* encoder_class = G_OBJECT_GET_CLASS(encoder_element_);
  if (g_object_class_find_property(encoder_class, "bitrate") != nullptr) {
    g_object_set(G_OBJECT(encoder_element_), "bitrate",
                 runtime_bitrate, nullptr);
  }
  if (g_object_class_find_property(encoder_class, "keyframe-period") !=
      nullptr) {
    g_object_set(G_OBJECT(encoder_element_), "keyframe-period",
                 static_cast<guint>(std::max(1u, gop_size_)), nullptr);
  }
  if (g_object_class_find_property(encoder_class, "key-int-max") != nullptr) {
    g_object_set(G_OBJECT(encoder_element_), "key-int-max",
                 static_cast<guint>(std::max(1u, gop_size_)), nullptr);
  }
  if (g_object_class_find_property(encoder_class, "repeat-sequence-header") !=
      nullptr) {
    g_object_set(G_OBJECT(encoder_element_), "repeat-sequence-header", TRUE,
                 nullptr);
  }
  if (g_object_class_find_property(encoder_class, "extra-controls") !=
      nullptr) {
    GstStructure* controls = gst_structure_new(
        "controls", "video_bitrate", G_TYPE_INT, static_cast<int>(bitrate_bps_),
        "h264_i_frame_period", G_TYPE_INT, static_cast<int>(std::max(1u, gop_size_)),
        "repeat_sequence_header", G_TYPE_INT, 1, nullptr);
    g_object_set(G_OBJECT(encoder_element_), "extra-controls", controls,
                 nullptr);
    gst_structure_free(controls);
  }

  const GstStateChangeReturn state_ret =
      gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (state_ret == GST_STATE_CHANGE_FAILURE) {
    LOG_ERROR("[WEBRTC] failed to set gstreamer pipeline to PLAYING");
    DestroyPipelineLocked();
    return false;
  }

  return true;
}

void RaspH264EncoderImpl::MaybeLogHelperExitLocked(const char* context) {
  if (pipeline_bus_ == nullptr) {
    return;
  }

  while (true) {
    GstMessage* message = gst_bus_pop_filtered(
        pipeline_bus_,
        static_cast<GstMessageType>(GST_MESSAGE_WARNING | GST_MESSAGE_ERROR |
                                    GST_MESSAGE_EOS));
    if (message == nullptr) {
      break;
    }

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
      GError* error = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_warning(message, &error, &debug_info);
      LOG_WARN("[WEBRTC] gstreamer warning in %s: %s (%s)", context,
               (error != nullptr && error->message != nullptr) ? error->message
                                                               : "unknown",
               debug_info != nullptr ? debug_info : "");
      if (error != nullptr) {
        g_error_free(error);
      }
      if (debug_info != nullptr) {
        g_free(debug_info);
      }
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr;
      gchar* debug_info = nullptr;
      gst_message_parse_error(message, &error, &debug_info);
      LOG_ERROR("[WEBRTC] gstreamer error in %s: %s (%s)", context,
                (error != nullptr && error->message != nullptr) ? error->message
                                                                : "unknown",
                debug_info != nullptr ? debug_info : "");
      if (error != nullptr) {
        g_error_free(error);
      }
      if (debug_info != nullptr) {
        g_free(debug_info);
      }
      pending_pipeline_reconfigure_ = true;
    } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
      LOG_WARN("[WEBRTC] gstreamer pipeline reached EOS in %s", context);
      pending_pipeline_reconfigure_ = true;
    }

    gst_message_unref(message);
  }
}

void RaspH264EncoderImpl::InitializeResolutionBitrateLimits() {
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

  const int max_bitrate =
      static_cast<int>(std::min<unsigned int>(bitrate_cap_bps_, 100000000u));
  resolution_bitrate_limits_.emplace_back(320 * 180, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(480 * 270, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(640 * 360, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(960 * 540, 1, 1, max_bitrate);
  resolution_bitrate_limits_.emplace_back(1280 * 720, 1, 1, max_bitrate);
}

int RaspH264EncoderImpl::MapProfileToNvvLaunchProfileValue(
    H264::Profile profile) {
  switch (profile) {
    case H264::kProfileBaseline:
    case H264::kProfileConstrainedBaseline:
      return 0;
    case H264::kProfileMain:
      return 2;
    case H264::kProfileHigh:
    case H264::kProfileConstrainedHigh:
      return 4;
    default:
      return 0;
  }
}

void RaspH264EncoderImpl::ReportInit() {
  if (has_reported_init_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));
  has_reported_init_ = true;
}

void RaspH264EncoderImpl::ReportError() {
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
