#include <absl/strings/match.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "log/log_manager.h"
#include "rtc_base/logging.h"
#include "rtc_encoder_factory.h"

#if defined __aarch64__
#ifndef USE_DEFAULT_JETSON_ENCODER
#include "nvidia-jetson/jetsonh264_encoder_impl.h"
#ifdef VTSRTC_HAS_GSTREAMER
#include "rasp/rasp_h264_encoder_impl.h"
#endif
#endif
#elif defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
#include "nvidia/nvh264_encoder_impl.h"
#endif

namespace webrtc {

std::vector<SdpVideoFormat> RtcEncoderFactory::GetSupportedFormats() const {
  // return webrtc::SupportedH264Codecs();

  return {
      CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "1"),
      CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "0"),
      CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "1"),
      CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "0"),
      CreateH264Format(H264::kProfileHigh, H264::kLevel5_1, "1"),
      CreateH264Format(H264::kProfileHigh, H264::kLevel5_1, "0")};
}

VideoEncoderFactory::CodecInfo RtcEncoderFactory::QueryVideoEncoder(
    const SdpVideoFormat& format) const {
  CodecInfo info;
  info.has_internal_source = false;
#if !defined(__aarch64__) && (!defined(VTSRTC_HAS_CUDA_DRIVER) || !VTSRTC_HAS_CUDA_DRIVER)
  info.is_hardware_accelerated = false;
#else
  info.is_hardware_accelerated = true;
#endif

  return info;
}

std::unique_ptr<VideoEncoder> RtcEncoderFactory::CreateVideoEncoder(
    const SdpVideoFormat& format) {
  if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
#if defined __aarch64__
#ifdef USE_DEFAULT_JETSON_ENCODER
    LOG_ERROR("[WEBRTC] RtcEncoderFactory should not be used when "
              "USE_DEFAULT_JETSON_ENCODER is enabled");
    return nullptr;
#else
    const bool use_rasp =
        absl::EqualsIgnoreCase(rtc_config_.jetson_h264_encoder, "rasp");
    const bool use_jetson =
        absl::EqualsIgnoreCase(rtc_config_.jetson_h264_encoder, "jetson");

    if (use_rasp) {
#ifdef VTSRTC_HAS_GSTREAMER
      LOG_INFO(
          "[WEBRTC] Select runtime H264 encoder: "
          "rasp-gstreamer-v4l2");
      return std::make_unique<RaspH264EncoderImpl>(cricket::VideoCodec(format),
                                                   rtc_config_);
#else
      LOG_WARN(
          "[WEBRTC] Raspberry Pi encoder requested but GStreamer support is "
          "not built in, fallback to jetson");
#endif
    }

    if (use_jetson) {
      LOG_INFO("[WEBRTC] Select runtime Jetson H264 encoder: jetson");
      return std::make_unique<JetsonH264EncoderImpl>(
          cricket::VideoCodec(format), rtc_config_);
    }

    if (!use_rasp) {
      LOG_WARN(
          "[WEBRTC] Unknown jetson_h264_encoder value: %s, supported values "
          "are jetson/rasp. Fallback to jetson",
          rtc_config_.jetson_h264_encoder.c_str());
    }

    LOG_INFO("[WEBRTC] Select runtime Jetson H264 encoder: jetson");
    return std::make_unique<JetsonH264EncoderImpl>(
        cricket::VideoCodec(format), rtc_config_);
#endif
#else
#if defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
    LOG_INFO("[WEBRTC] Select runtime H264 encoder: nvidia-nvenc");
    return std::make_unique<NvH264EncoderImpl>(cricket::VideoCodec(format),
                                               rtc_config_);
#else
    LOG_ERROR("[WEBRTC] RtcEncoderFactory was built without CUDA/NVENC support");
    return nullptr;
#endif
#endif
  }

  LOG_ERROR("[WEBRTC] Trying to created encoder of unsupported format %s",
            format.name.c_str());
  return nullptr;
}

}  // namespace webrtc
