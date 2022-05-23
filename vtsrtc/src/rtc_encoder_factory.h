# pragma once

#include <vector>
#include <memory>

#include <api/video_codecs/video_encoder_factory.h> // NOLINT

namespace webrtc {

class RtcEncoderFactory : public VideoEncoderFactory {
public:
	// Returns a list of supported video formats in order of preference, to use
	// for signaling etc.
	std::vector<SdpVideoFormat> GetSupportedFormats() const override;

	// Returns information about how this format will be encoded. The specified
	// format must be one of the supported formats by this factory.
	CodecInfo QueryVideoEncoder(const SdpVideoFormat& format) const override;

	// Creates a VideoEncoder for the specified format.
	std::unique_ptr<VideoEncoder> CreateVideoEncoder(
		const SdpVideoFormat& format) override;
};

}  // namespace webrtc
