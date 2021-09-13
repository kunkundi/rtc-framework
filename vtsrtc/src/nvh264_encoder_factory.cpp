#include <absl/strings/match.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "log_manager.h"
#include "nvh264_encoder_impl.h"
#include "nvh264_encoder_factory.h"

namespace webrtc {

std::vector<SdpVideoFormat> NvH264EncoderFactory::GetSupportedFormats()
	const {
	// return webrtc::SupportedH264Codecs();

	return {
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "0"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "0")
	};
}

VideoEncoderFactory::CodecInfo NvH264EncoderFactory::QueryVideoEncoder(
	const SdpVideoFormat& format) const {
	CodecInfo info;
	info.has_internal_source = false;
	info.is_hardware_accelerated = true;

	return info;
}

std::unique_ptr<VideoEncoder> NvH264EncoderFactory::CreateVideoEncoder(
	const SdpVideoFormat& format) {
	if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
		return std::make_unique<NvH264EncoderImpl>(cricket::VideoCodec(format));
	}

	LOG_ERROR("[WEBRTC] Trying to created encoder of unsupported format %s",
		format.name.c_str());
	return nullptr;
}

}  // namespace webrtc
