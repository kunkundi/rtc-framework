#include <absl/strings/match.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "log/log_manager.h"
#include "rtc_encoder_factory.h"
#include "rtc_base/logging.h"

#if defined  __aarch64__
#include "nvidia-jetson/rtc_codec_pool.h"
#include "nvidia-jetson/jetsonh264_encoder_impl.h"
#else
#include "nvidia/nvh264_encoder_impl.h"
#endif

namespace webrtc {

std::vector<SdpVideoFormat> RtcEncoderFactory::GetSupportedFormats()
	const {
	// return webrtc::SupportedH264Codecs();

	return {
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "0"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "0"),
		CreateH264Format(H264::kProfileHigh, H264::kLevel5_1, "1"),
		CreateH264Format(H264::kProfileHigh, H264::kLevel5_1, "0")
	};
}

VideoEncoderFactory::CodecInfo RtcEncoderFactory::QueryVideoEncoder(
	const SdpVideoFormat& format) const {
	CodecInfo info;
	info.has_internal_source = false;
	info.is_hardware_accelerated = true;

	return info;
}

std::unique_ptr<VideoEncoder> RtcEncoderFactory::CreateVideoEncoder(
	const SdpVideoFormat& format) {
	if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
#if defined  __aarch64__
		if(rtc_config_.encode_params.use_codec_pool)
			CodecPool::GetInstance()->Init(rtc_config_);
		log_level = 0;
		// rtc::LogMessage::LogToDebug(rtc::LS_VERBOSE);
		return std::make_unique<JetsonH264EncoderImpl>(cricket::VideoCodec(format), rtc_config_);
#else
		return std::make_unique<NvH264EncoderImpl>(cricket::VideoCodec(format));
#endif
	}

	LOG_ERROR("[WEBRTC] Trying to created encoder of unsupported format %s",
		format.name.c_str());
	return nullptr;
}

}  // namespace webrtc
