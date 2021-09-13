# pragma once

#include <api/video_codecs/video_decoder.h>
#include <common_video/include/i420_buffer_pool.h>
#include <modules/video_coding/codecs/h264/include/h264.h>
#include <common_video/h264/h264_bitstream_parser.h>
#include <NvDecoder/NvDecoder.h>  // must be included after i420_buffer_pool.h

#include <memory>

namespace webrtc {

class NvH264DecoderImpl : public VideoDecoder {
public:
	explicit NvH264DecoderImpl(const cricket::VideoCodec& codec);
	~NvH264DecoderImpl() override;

	int32_t InitDecode(const VideoCodec* codec_settings,
		int32_t number_of_cores) override;
	int32_t Release() override;

	int32_t Decode(const EncodedImage& input_image,
		bool missing_frames,
		int64_t render_time_ms) override;

	int32_t RegisterDecodeCompleteCallback(
		DecodedImageCallback* callback) override;

	// Returns true if the decoder prefer to decode frames late.
	// That is, it can not decode infinite number of frames before the decoded
	// frame is consumed.
	bool PrefersLateDecoding() const override;

	const char* ImplementationName() const override;

private:
	// Reports statistics with histograms.
	void ReportInit();
	void ReportError();

private:
	CUcontext cuda_context_ = nullptr;
	std::unique_ptr<NvDecoder> nvh264_decoder_ = nullptr;
	I420BufferPool I420buffer_pool_ = I420BufferPool(true);

	DecodedImageCallback* decoded_image_callback_ = nullptr;
	H264BitstreamParser h264_bitstream_parser_;

	// To be improved
	// Init NvDecoder with max_width x max_height(for example: 4096x4096),
	// HandlePictureDisplay in NvDecoder will crash when resolution changed, why???
	uint32_t width_ = 0, height_ = 0;

	bool has_reported_init_ = false;
	bool has_reported_error_ = false;
};

}  // namespace webrtc
