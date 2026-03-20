#pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>  // NOLINT
#include <common_video/h264/h264_bitstream_parser.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include <chrono>
#include <climits>
#include <memory>
#include <mutex>
#include <vector>

#include "jetson_encoder.h"
#include "rtc_types.h"

// 编码性能统计开关定义在 jetson_encoder.h 中

namespace webrtc {

class JetsonH264EncoderImpl : public VideoEncoder {
 public:
  explicit JetsonH264EncoderImpl(const cricket::VideoCodec& codec,
                                 const vts_rtc::RtcConfig& rtc_config);
  ~JetsonH264EncoderImpl() override;

  int InitEncode(const VideoCodec* codec_settings,
                 const VideoEncoder::Settings& settings) override;
  int32_t Release() override;

  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;

  void SetRates(const RateControlParameters& parameters) override;

  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;

  EncoderInfo GetEncoderInfo() const override;

  void SetFecControllerOverride(
      FecControllerOverride* fec_controller_override) override;

  void OnPacketLossRateUpdate(float packet_loss_rate) override;

  void OnRttUpdate(int64_t rtt_ms) override;

  void OnLossNotification(const LossNotification& loss_notification) override;

 private:
  void ReconfigureEncoderRates(uint32_t fps, uint32_t bitrate);
  void ReconfigureEncoderIDR();

  void ReportInit();
  void ReportError();

  void SendFrame(const VideoFrame& frame, const uint8_t* data, size_t size,
                 bool is_keyframe, int64_t encode_duration_us = 0);

 private:
  std::unique_ptr<JetsonEncoder> encoder_;

  EncodedImage encoded_image_;
  size_t encoded_image_capacity_ = 0;
  std::mutex encoded_image_mutex_;

  EncodedImageCallback* encoded_image_callback_ = nullptr;
  H264BitstreamParser h264_bitstream_parser_;

  VideoCodec codec_;
  H264PacketizationMode packetization_mode_ =
      H264PacketizationMode::SingleNalUnit;
  size_t max_payload_size_ = 0;

  bool has_reported_init_ = false;
  bool has_reported_error_ = false;

  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int fps_ = 30;
  unsigned int bitrate_ = 25000000;

#if ENABLE_ENCODE_PERF_STATS
  struct EncodeStats {
    int64_t total_encode_time_us = 0;
    int64_t max_encode_time_us = 0;
    int64_t min_encode_time_us = INT64_MAX;
    uint32_t frame_count = 0;
    uint32_t keyframe_count = 0;
    std::chrono::steady_clock::time_point last_log_time;
  } encode_stats_;
#endif
};

}  // namespace webrtc
