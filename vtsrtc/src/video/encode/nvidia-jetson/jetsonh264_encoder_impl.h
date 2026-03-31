#pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>  // NOLINT
#include <common_video/h264/h264_bitstream_parser.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
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
  struct EncoderSlot {
    std::unique_ptr<JetsonEncoder> encoder;
    unsigned int width = 0;
    unsigned int height = 0;
    uint64_t token = 0;
  };

  bool CreateEncoderSlot(unsigned int width,
                         unsigned int height,
                         EncoderSlot* slot);
  bool EnsureActiveEncoderForResolution(unsigned int width,
                                        unsigned int height);
  void ApplyRatesToEncoder(JetsonEncoder* encoder);
  bool PrewarmStandbyForActiveResolution(unsigned int active_width,
                                         unsigned int active_height);
  std::pair<unsigned int, unsigned int> SelectPrewarmResolution(
      unsigned int active_width,
      unsigned int active_height) const;
  void StartPrewarmWorker();
  void StopPrewarmWorker();
  void RequestAsyncPrewarm(unsigned int active_width,
                           unsigned int active_height);
  void PrewarmWorkerLoop();

  void ReconfigureEncoderRates(uint32_t fps, uint32_t bitrate);
  void ReconfigureEncoderIDR();

  void ReportInit();
  void ReportError();

  void SendFrame(const VideoFrame& frame, const uint8_t* data, size_t size,
                 bool is_keyframe, int64_t encode_duration_us = 0);

 private:
  EncoderSlot active_encoder_;
  EncoderSlot standby_encoder_;
  std::mutex encoder_slots_mutex_;
  std::atomic<uint64_t> next_encoder_token_{1};
  std::atomic<uint64_t> active_encoder_token_{0};

  std::thread prewarm_thread_;
  std::mutex prewarm_mutex_;
  std::condition_variable prewarm_cv_;
  bool prewarm_stop_ = true;
  bool prewarm_request_pending_ = false;
  unsigned int prewarm_request_active_width_ = 0;
  unsigned int prewarm_request_active_height_ = 0;

  EncodedImage encoded_image_;
  size_t encoded_image_capacity_ = 0;
  std::mutex encoded_image_mutex_;

  EncodedImageCallback* encoded_image_callback_ = nullptr;
  H264BitstreamParser h264_bitstream_parser_;

  vts_rtc::RtcConfig rtc_config_;
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
