#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <string>

#include "jetsonh264_encoder_impl.h"
#include "jetson_encoder.h"
#include "log/log_manager.h"

namespace webrtc {

// QP scaling thresholds.
static const int kLowH264QpThreshold = 37;
static const int kHighH264QpThreshold = 39;

enum class H264EncoderImplEvent {
  H264EncoderEventInit = 0,
  H264EncoderEventError = 1,
  H264EncoderEventMax = 16,
};

JetsonH264EncoderImpl::JetsonH264EncoderImpl(
    const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config) {
  RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));

  std::string packetization_mode_string;
  if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
                     &packetization_mode_string) &&
      packetization_mode_string == "1") {
    packetization_mode_ = H264PacketizationMode::NonInterleaved;
  }
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() { Release(); }

int JetsonH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                      const VideoEncoder::Settings& settings) {
  LOG_INFO("[WebRTC] Init Nvidia H264 encoder");

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

  // Release necessary in case of re-initializing.
  encoder_.reset();

  // TO DO: support SVC feature
  auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  codec_ = *codec_settings;
  max_payload_size_ = settings.max_payload_size;

  // Codec expects simulcastStream resolutions to be correct, make sure they are
  // filled even when there are no simulcast layers.
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
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._completeFrame = true;
  encoded_image_._encodedWidth = frame_width;
  encoded_image_._encodedHeight = frame_height;
  encoded_image_.set_size(0);

  // TO DO
  SimulcastRateAllocator init_allocator(codec_);
  auto allocation = init_allocator.Allocate(VideoBitrateAllocationParameters(
      DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::Release() {
  encoder_.reset();
  encoded_image_.ClearEncodedData();

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

  if (encoder_) {
    encoder_->SetFps(fps);
    encoder_->SetBitrate(bitrate);
  }
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

  if (frame_types && (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  auto frame_buffer = input_frame.video_frame_buffer()->ToI420();

  RTC_DCHECK_EQ(encoded_image_._encodedWidth, frame_buffer->width());
  RTC_DCHECK_EQ(encoded_image_._encodedHeight, frame_buffer->height());

  // Create encoder if not exists
  if (!encoder_) {
    encoder_ = JetsonEncoder::Create(width_, height_, V4L2_PIX_FMT_H264, false);
    if (!encoder_) {
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    encoder_->SetFps(fps_);
    encoder_->SetBitrate(bitrate_);
  }

  // Request Key frame
  if (frame_types && (*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
    encoder_->ForceKeyFrame();
  }

  // Encode frame using callback mechanism
  encoder_->EmplaceBuffer(
      frame_buffer,
      [this, input_frame](const uint8_t* data, size_t size, bool is_keyframe,
                          uint64_t timestamp) {
        SendFrame(input_frame, data, size, is_keyframe);
      });

  return WEBRTC_VIDEO_CODEC_OK;
}

void JetsonH264EncoderImpl::SendFrame(const VideoFrame& frame,
                                       const uint8_t* data, size_t size,
                                       bool is_keyframe) {
  if (size == 0) {
    return;
  }

  // Check for NAL units
  auto nalu_indices = H264::FindNaluIndices(data, size);
  if (nalu_indices.empty()) {
    return;
  }

  // Determine frame type
  VideoFrameType frame_type = VideoFrameType::kVideoFrameDelta;
  if (is_keyframe) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x07) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x01) {
    frame_type = VideoFrameType::kVideoFrameDelta;
  }

  // Prepare encoded image
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

  // Copy encoded data
  memcpy(encoded_image_.data(), data, size);

  // Parse bitstream for QP
  if (size > 0) {
    h264_bitstream_parser_.ParseBitstream(encoded_image_.data(),
                                          encoded_image_.size());
    auto qp = h264_bitstream_parser_.GetLastSliceQp();
    if (qp.has_value()) {
      encoded_image_.qp_ = qp.value();
    }
  }

  // Prepare fragmentation header
  RTPFragmentationHeader frag_header;
  frag_header.VerifyAndAllocateFragmentationHeader(nalu_indices.size());
  for (size_t i = 0; i < nalu_indices.size(); ++i) {
    frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
    frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
  }

  // Prepare codec specific info
  CodecSpecificInfo codec_specific;
  codec_specific.codecType = kVideoCodecH264;
  codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
  codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
  codec_specific.codecSpecific.H264.idr_frame =
      (frame_type == VideoFrameType::kVideoFrameKey);
  codec_specific.codecSpecific.H264.base_layer_sync = false;

  // Send encoded image
  encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific,
                                          &frag_header);
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
  /*	LOG_INFO("[WEBRTC] OnPacketLossRateUpdate: %f", packet_loss_rate);*/
}

void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
  /*	LOG_INFO("[WEBRTC] OnRttUpdate: %d", rtt_ms);*/
}

void JetsonH264EncoderImpl::OnLossNotification(
    const LossNotification& loss_notification) {
  // 	auto delta = loss_notification.timestamp_of_last_decodable -
  // 		loss_notification.timestamp_of_last_received;
  // 	LOG_INFO("[WEBRTC] OnLossNotification timestamp between"
  // 		"last decodable and last received frame: %d", delta);
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
