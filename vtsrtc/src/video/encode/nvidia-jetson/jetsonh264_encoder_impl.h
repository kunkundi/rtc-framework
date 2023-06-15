#pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>  // NOLINT
#include <common_video/h264/h264_bitstream_parser.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include <memory>
#include <vector>

#include "nvmpi.h"
#include "rtc_types.h"

namespace webrtc {

class JetsonH264EncoderImpl : public VideoEncoder {
 public:
  explicit JetsonH264EncoderImpl(const cricket::VideoCodec& codec,
                                 const vts_rtc::RtcConfig& rtc_config);
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

 private:
  // Reconfigure encoder
  void ReconfigureEncoderRates(uint32_t fps, uint32_t bitrate);
  void ReconfigureEncoderIDR();

  // Reports statistics with histograms.
  void ReportInit();
  void ReportError();

 private:
  nvmpictx* ctx_ = nullptr;
  nvEncParam* param_ = nullptr;

  //   std::unique_ptr<NvEncoder> nvh264_encoder_ = nullptr;
  std::vector<std::vector<uint8_t>> encoded_packets_;
  EncodedImage encoded_image_;

  EncodedImageCallback* encoded_image_callback_ = nullptr;
  H264BitstreamParser h264_bitstream_parser_;

  VideoCodec codec_;
  H264PacketizationMode packetization_mode_ =
      H264PacketizationMode::SingleNalUnit;
  // The maximum size each payload is allowed to have. Usually MTU - overhead.
  size_t max_payload_size_ = 0;

  bool has_reported_init_ = false;
  bool has_reported_error_ = false;

  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int fps_ = 30;
  unsigned int bitrate_ = 25000000;
};

}  // namespace webrtc
