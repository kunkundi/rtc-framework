#include <absl/strings/match.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "log/log_manager.h"
#if defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
#include "nvidia/nvh264_decoder_impl.h"
#endif
#include "rtc_decoder_factory.h"

namespace webrtc {

std::vector<SdpVideoFormat> RtcDecoderFactory::GetSupportedFormats() const {
	// return webrtc::SupportedH264Codecs();

	return {
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileBaseline, H264::kLevel3_1, "0"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "1"),
		CreateH264Format(H264::kProfileConstrainedBaseline, H264::kLevel3_1, "0")
	};
}

std::unique_ptr<VideoDecoder> RtcDecoderFactory::CreateVideoDecoder(
	const SdpVideoFormat& format) {
	if (absl::EqualsIgnoreCase(format.name, cricket::kH264CodecName)) {
#if defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
		return std::make_unique<NvH264DecoderImpl>(cricket::VideoCodec(format));
#else
		LOG_ERROR("[WEBRTC] RtcDecoderFactory was built without CUDA/NVDEC support");
		return nullptr;
#endif
	}

	LOG_ERROR("[WEBRTC] Trying to created decoder of unsupported format %s",
		format.name.c_str());
	return nullptr;
}

}  // namespace webrtc
