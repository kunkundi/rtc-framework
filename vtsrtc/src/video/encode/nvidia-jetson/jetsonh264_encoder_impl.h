# pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>
#include <modules/video_coding/codecs/h264/include/h264.h>
#include <common_video/h264/h264_bitstream_parser.h>

#include <vector>
#include <memory>
#include <fstream>

#include "rtc_types.h"
#include "NvVideoEncoder.h"

namespace webrtc {

class JetsonH264EncoderImpl : public VideoEncoder {
public:
	explicit JetsonH264EncoderImpl(const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config);
	~JetsonH264EncoderImpl() override;

	int InitEncode(const VideoCodec* codec_settings,
		const VideoEncoder::Settings& settings) override;
	int32_t Release() override;

	// Register an encode complete callback object.
	int32_t RegisterEncodeCompleteCallback(
		EncodedImageCallback* callback) override;

	// Sets rate control parameters: bitrate, framerate, etc.
	void SetRates(const RateControlParameters& parameters) override;

	// Encode an I420 image (as a part of a video stream). The encoded image
	// will be returned to the user through the encode complete callback.
	int32_t Encode(const VideoFrame& frame,
		const std::vector<VideoFrameType>* frame_types) override;

	// Returns meta-data about the encoder, such as implementation name.
	EncoderInfo GetEncoderInfo() const override;

	// Set a FecControllerOverride, through which the encoder may override
	// decisions made by FecController.
	void SetFecControllerOverride(
		FecControllerOverride* fec_controller_override) override;

	// Inform the encoder when the packet loss rate changes. [0.0 to 1.0]
	void OnPacketLossRateUpdate(float packet_loss_rate) override;

	// Inform the encoder when the round trip time changes. [in milliseconds]
	void OnRttUpdate(int64_t rtt_ms) override;

	// Called when a loss notification is received.
	void OnLossNotification(const LossNotification& loss_notification) override;

	NvVideoEncoder *GetJetsonH264Encoder() {return jetsonh264_encoder;}

	EncodedImageCallback *GetEncodedImageCallback() {return encoded_image_callback_;}

	EncodedImage *GetEncodedImage() {return &encoded_image_;}

	H264PacketizationMode GetH264PacketizationMode() {return packetization_mode_;}

private:

	// Reports statistics with histograms.
	void ReportInit();
	void ReportError();

	//Encoder capture-plane deque buffer callback function
	static bool CapturePlaneDqCallbackWithCodecPool(struct v4l2_buffer *v4l2_buf, 
						NvBuffer * buffer, NvBuffer * shared_buffer, void *data);
	static bool CapturePlaneDqCallbackWithoutCodecPool(struct v4l2_buffer *v4l2_buf, 
						NvBuffer * buffer, NvBuffer * shared_buffer, void *data);

	int InitEncodeWithCodecPool(const VideoCodec* codec_settings, const VideoEncoder::Settings& settings);
	int InitEncodeWithoutCodecPool(const VideoCodec* codec_settings, const VideoEncoder::Settings& settings);
	int32_t ReleaseWithCodecPool();
	int32_t ReleaseWithoutCodecPool();
	int32_t EncodeWithCodecPool(const VideoFrame& frame, const std::vector<VideoFrameType>* frame_types);
	int32_t EncodeWithoutCodecPool(const VideoFrame& frame, const std::vector<VideoFrameType>* frame_types);

private:
    NvVideoEncoder *jetsonh264_encoder = nullptr;
    EncodedImageCallback* encoded_image_callback_ = nullptr;
    VideoCodec codec_;
	H264PacketizationMode packetization_mode_ = H264PacketizationMode::NonInterleaved;
	EncodedImage encoded_image_;
    bool has_reported_init_ = false;
	bool has_reported_error_ = false;
	std::ofstream *stream_file_;
	bool save_stream_ = false;
	uint32_t fps_ = 0;
	uint32_t bitrate_ = 0;
	vts_rtc::RtcConfig rtc_config_;
	std::vector<ResolutionBitrateLimits> resolution_bitrate_limits_;
	int width_ = 0;
	int height_ = 0;
	bool release_flag_ = false;
	bool stop_flag_ = true;
};

}  // namespace webrtc
