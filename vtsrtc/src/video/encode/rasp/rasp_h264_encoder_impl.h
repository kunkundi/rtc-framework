#pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>  // NOLINT
#include <common_video/h264/h264_bitstream_parser.h>
#include <media/base/h264_profile_level_id.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include <chrono>
#include <climits>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
typedef struct _GstAppSink GstAppSink;
typedef struct _GstAppSrc GstAppSrc;
typedef struct _GstBus GstBus;
typedef struct _GstBufferPool GstBufferPool;
typedef struct _GstElement GstElement;
}

#include "rtc_types.h"

#ifndef ENABLE_ENCODE_PERF_STATS
#define ENABLE_ENCODE_PERF_STATS 0
#endif

namespace webrtc {

class RaspH264EncoderImpl : public VideoEncoder {
 public:
  explicit RaspH264EncoderImpl(const cricket::VideoCodec& codec,
                               const vts_rtc::RtcConfig& rtc_config);
  ~RaspH264EncoderImpl() override;

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
  struct PendingFrame {
    VideoFrame frame;
#if ENABLE_ENCODE_PERF_STATS
    std::chrono::steady_clock::time_point encode_start_time;
#endif
  };

  bool InitializePipelineLocked(unsigned int width, unsigned int height);
  bool InitializeInputBufferPoolLocked(size_t buffer_size);
  void DestroyPipelineLocked();
  bool ReinitializePipelineLocked(unsigned int width, unsigned int height);
  bool WriteFrameToPipeLocked(const I420BufferInterface& frame_buffer);
  int DrainPacketsLocked(int first_wait_timeout_ms);
  int DeliverPacketLocked(const VideoFrame& frame, const uint8_t* payload,
                          size_t payload_size,
                          int64_t encode_duration_us = 0);
  bool PullPacketFromSinkLocked(int timeout_ms, std::vector<uint8_t>* packet);
  bool RequestKeyFrameLocked();
  bool LaunchPipelineLocked(unsigned int width, unsigned int height);
  void MaybeLogHelperExitLocked(const char* context);
  void InitializeResolutionBitrateLimits();
  static int MapProfileToNvvLaunchProfileValue(H264::Profile profile);

  void ReportInit();
  void ReportError();
#if ENABLE_ENCODE_PERF_STATS
  void RecordEncodeLatencyStats(bool is_keyframe, size_t payload_size,
                                int64_t encode_duration_us);
  void LogEncodeLatencySummary(const char* reason);
#endif

 private:
  GstElement* pipeline_ = nullptr;
  GstElement* encoder_element_ = nullptr;
  GstAppSrc* appsrc_ = nullptr;
  GstAppSink* appsink_ = nullptr;
  GstBus* pipeline_bus_ = nullptr;
  GstBufferPool* input_buffer_pool_ = nullptr;

  std::mutex encoder_mutex_;
  EncodedImage encoded_image_;
  size_t encoded_image_capacity_ = 0;
  EncodedImageCallback* encoded_image_callback_ = nullptr;
  H264BitstreamParser h264_bitstream_parser_;

  vts_rtc::RtcConfig rtc_config_;
  VideoCodec codec_;
  H264PacketizationMode packetization_mode_ =
      H264PacketizationMode::SingleNalUnit;
  size_t max_payload_size_ = 0;
  std::string encoder_name_ = "h264_rasp_v4l2";
  bool is_hardware_encoder_ = true;
  bool bitrate_property_uses_kbps_ = false;

  bool has_reported_init_ = false;
  bool has_reported_error_ = false;
  bool has_reported_missing_callback_ = false;
  bool force_keyframe_after_callback_ = false;
  bool pending_pipeline_reconfigure_ = false;
  bool has_logged_restart_warning_ = false;
  uint64_t instance_id_ = 0;

  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int fps_ = 30;
  unsigned int bitrate_bps_ = 2500000;
  bool has_configured_playout_delay_ = false;
  int configured_playout_delay_min_ms_ = -1;
  int configured_playout_delay_max_ms_ = -1;
  unsigned int bitrate_floor_bps_ = 0;
  unsigned int gop_size_ = 3000;
  unsigned int bitrate_cap_bps_ = 100000000;
  std::pair<unsigned int, unsigned int> qp_range_ = {0u, 0u};
  std::pair<unsigned int, unsigned int> qp_threshold_ = {34u, 38u};
  std::string bitrate_mode_ = "cbr";
  uint64_t next_buffer_pts_ns_ = 0;
  std::vector<ResolutionBitrateLimits> resolution_bitrate_limits_;
  std::deque<std::vector<uint8_t>> ready_packets_;

  H264::Profile profile_ = H264::kProfileConstrainedBaseline;
  H264::Level level_ = H264::kLevel3_1;

  std::deque<PendingFrame> pending_frames_;

#if ENABLE_ENCODE_PERF_STATS
  struct EncodeStats {
    int64_t total_encode_time_us = 0;
    int64_t max_encode_time_us = 0;
    int64_t min_encode_time_us = INT64_MAX;
    uint32_t frame_count = 0;
    uint32_t keyframe_count = 0;
    std::chrono::steady_clock::time_point last_log_time{};
  } encode_stats_;
#endif
};

}  // namespace webrtc
