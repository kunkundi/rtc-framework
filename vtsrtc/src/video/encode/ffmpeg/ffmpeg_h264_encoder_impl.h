#pragma once

#include <api/video/i420_buffer.h>
#include <api/video_codecs/video_encoder.h>  // NOLINT
#include <common_video/h264/h264_bitstream_parser.h>
#include <media/base/h264_profile_level_id.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "rtc_types.h"

extern "C" {
#include "nvmpi.h"
}

namespace webrtc {

class FFmpegH264EncoderImpl : public VideoEncoder {
 public:
  explicit FFmpegH264EncoderImpl(const cricket::VideoCodec& codec,
                                 const vts_rtc::RtcConfig& rtc_config);
  ~FFmpegH264EncoderImpl() override;

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
  bool ReinitializeEncoder(unsigned int width, unsigned int height);
  bool InitializeEncoderLocked(unsigned int width, unsigned int height);
  void DestroyEncoderLocked();
  bool CopyFrameToEncoderLocked(const I420BufferInterface& frame_buffer);
  int DrainPacketsLocked(const VideoFrame& frame);
  int DeliverPacketLocked(const VideoFrame& frame, const uint8_t* payload,
                          size_t payload_size, bool is_keyframe);
  bool RequestKeyFrameLocked();
  void EnsureScratchBufferCapacityLocked(unsigned int width,
                                         unsigned int height);
#if defined(NVMPI_ENC_CHUNK_SIZE)
  bool InitializePacketPoolLocked();
  void ReleasePacketPoolLocked();
#endif
  void InitializeResolutionBitrateLimits();
  static unsigned int MapProfileToNvmpi(H264::Profile profile);

  void ReportInit();
  void ReportError();

 private:
  nvmpictx* encoder_ = nullptr;

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
  std::string encoder_name_ = "h264_nvmpi";
  bool is_hardware_encoder_ = true;

  bool has_reported_init_ = false;
  bool has_reported_error_ = false;
  bool has_reported_missing_callback_ = false;
  bool force_keyframe_after_callback_ = false;
  bool pending_encoder_reconfigure_ = false;
  uint64_t instance_id_ = 0;
  uint64_t empty_drains_ = 0;

  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int fps_ = 30;
  unsigned int bitrate_bps_ = 2500000;
  unsigned int gop_size_ = 3000;
  unsigned int bitrate_cap_bps_ = 100000000;
  std::string bitrate_mode_ = "cbr";
  std::pair<unsigned int, unsigned int> qp_threshold_ = {24, 37};
  std::vector<ResolutionBitrateLimits> resolution_bitrate_limits_;

  H264::Profile profile_ = H264::kProfileConstrainedBaseline;
  H264::Level level_ = H264::kLevel3_1;

  std::vector<uint8_t> y_plane_;
  std::vector<uint8_t> u_plane_;
  std::vector<uint8_t> v_plane_;

#if defined(NVMPI_ENC_CHUNK_SIZE)
  std::vector<std::unique_ptr<nvPacket>> packet_pool_;
  std::vector<std::unique_ptr<unsigned char[]>> packet_payload_pool_;
#endif
};

}  // namespace webrtc
