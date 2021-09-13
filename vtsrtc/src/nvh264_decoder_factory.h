# pragma once

#include <vector>
#include <memory>

#include <api/video_codecs/video_decoder_factory.h> // NOLINT

namespace webrtc {

class NvH264DecoderFactory : public VideoDecoderFactory {
public:
	// Returns a list of supported video formats in order of preference, to use
	// for signaling etc.
	std::vector<SdpVideoFormat> GetSupportedFormats() const override;

	// Creates a VideoDecoder for the specified format.
	std::unique_ptr<VideoDecoder> CreateVideoDecoder(
		const SdpVideoFormat& format) override;
};

}  // namespace webrtc
