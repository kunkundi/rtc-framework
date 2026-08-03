#include "jetson_encoder.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "/usr/src/jetson_multimedia_api/include/NvBuffer.h"
#include "/usr/src/jetson_multimedia_api/include/nvbufsurface.h"
#include "log/log_manager.h"

#ifndef MAX_PLANES
#define MAX_PLANES 4
#endif

namespace webrtc {

const int KEY_FRAME_INTERVAL = 3000;
const int BUFFER_NUM = 4;
const uint32_t DQ_THREAD_WAIT_TIMEOUT_MS = 1000;

const char* PixFmtName(uint32_t pix_fmt) {
  switch (pix_fmt) {
    case V4L2_PIX_FMT_YUV420M:
      return "YUV420M";
    case V4L2_PIX_FMT_NV12M:
      return "NV12M";
    case V4L2_PIX_FMT_H264:
      return "H264";
    case V4L2_PIX_FMT_H265:
      return "H265";
    default:
      return "unknown";
  }
}

void LogV4L2Buffer(const char* event, const struct v4l2_buffer& buffer) {
  LOG_INFO(
      "[JetsonEnc][buf] %s type=%u memory=%u index=%u flags=0x%x "
      "bytesused=%u length=%u planes=%u",
      event, buffer.type, buffer.memory, buffer.index, buffer.flags,
      buffer.bytesused, buffer.length, buffer.length);
  if (!buffer.m.planes) {
    return;
  }
  for (uint32_t i = 0; i < buffer.length && i < MAX_PLANES; ++i) {
    const v4l2_plane& plane = buffer.m.planes[i];
    LOG_INFO(
        "[JetsonEnc][buf] %s plane=%u bytesused=%u length=%u data_offset=%u "
        "mem_offset=%u fd=%d",
        event, i, plane.bytesused, plane.length, plane.data_offset,
        plane.m.mem_offset, plane.m.fd);
  }
}

void LogNvBuffer(const char* event, const NvBuffer* buffer) {
  if (!buffer) {
    LOG_INFO("[JetsonEnc][nvbuf] %s null", event);
    return;
  }
  LOG_INFO("[JetsonEnc][nvbuf] %s index=%u planes=%u", event, buffer->index,
           buffer->n_planes);
  for (uint32_t i = 0; i < buffer->n_planes && i < MAX_PLANES; ++i) {
    const auto& plane = buffer->planes[i];
    LOG_INFO(
        "[JetsonEnc][nvbuf] %s plane=%u fmt=%ux%u stride=%u bpp=%u "
        "bytesused=%u",
        event, i, plane.fmt.width, plane.fmt.height, plane.fmt.stride,
        plane.fmt.bytesperpixel, plane.bytesused);
  }
}

uint32_t SelectH264Level(int width, int height, int framerate) {
  const uint32_t safe_width = static_cast<uint32_t>(std::max(width, 1));
  const uint32_t safe_height = static_cast<uint32_t>(std::max(height, 1));
  const uint32_t safe_framerate =
      static_cast<uint32_t>(std::max(framerate, 1));
  const uint32_t macroblocks_per_frame =
      ((safe_width + 15) / 16) * ((safe_height + 15) / 16);
  const uint64_t macroblocks_per_second =
      static_cast<uint64_t>(macroblocks_per_frame) * safe_framerate;

  if (macroblocks_per_frame <= 3600 && macroblocks_per_second <= 108000) {
    return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
  }
  if (macroblocks_per_frame <= 8192 && macroblocks_per_second <= 245760) {
    return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
  }
  if (macroblocks_per_frame <= 8704 && macroblocks_per_second <= 522240) {
    return V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
  }
  return V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
}

const char* H264LevelName(uint32_t level) {
  switch (level) {
    case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:
      return "3.1";
    case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:
      return "4.0";
    case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:
      return "4.2";
    case V4L2_MPEG_VIDEO_H264_LEVEL_5_1:
      return "5.1";
    default:
      return "unknown";
  }
}

std::unique_ptr<JetsonEncoder> JetsonEncoder::Create(int width, int height,
                                                     uint32_t dst_pix_fmt,
                                                     bool is_dma_src,
                                                     const StrategyConfig& config,
                                                     int framerate,
                                                     int bitrate_bps) {
  auto ptr =
      std::make_unique<JetsonEncoder>(width, height, dst_pix_fmt, is_dma_src);
  ptr->framerate_ = std::max(1, framerate);
  ptr->bitrate_bps_ = std::max(1, bitrate_bps);
  ptr->SetStrategyConfig(config);
  if (!ptr->CreateVideoEncoder()) {
    return nullptr;
  }
  if (!ptr->Start()) {
    return nullptr;
  }
  return ptr;
}

JetsonEncoder::JetsonEncoder(int width, int height, uint32_t dst_pix_fmt,
                             bool is_dma_src)
    : encoder_(nullptr),
      abort_(true),
      stopping_(true),
      width_(width),
      height_(height),
      framerate_(30),
      bitrate_bps_(2 * 1024 * 1024),
      src_pix_fmt_(V4L2_PIX_FMT_YUV420M),
      dst_pix_fmt_(dst_pix_fmt),
      is_dma_src_(is_dma_src),
      packets_buf_size_(CHUNK_SIZE),
      packets_num_(BUFFER_NUM),
      buf_index_(0),
      strategy_config_{} {
  for (int i = 0; i < MAX_BUFFERS; i++) {
    packets_[i] = nullptr;
  }
  for (int i = 0; i < packets_num_; i++) {
    packets_[i] = new unsigned char[packets_buf_size_];
    packets_size_[i] = 0;
    packets_keyflag_[i] = false;
    timestamp_[i] = 0;
  }
}

JetsonEncoder::~JetsonEncoder() {
  abort_ = true;
  stopping_.store(true, std::memory_order_release);

  if (encoder_) {
    StopEncoderIo(false);

    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      capturing_tasks_.clear();
    }

    encoder_->capture_plane.deinitPlane();
    encoder_->output_plane.deinitPlane();

    delete encoder_;
    encoder_ = nullptr;
  }

  for (int i = 0; i < packets_num_; i++) {
    if (packets_[i]) {
      delete[] packets_[i];
      packets_[i] = nullptr;
    }
  }
}

bool JetsonEncoder::CreateVideoEncoder() {
  int ret = 0;

  LOG_INFO(
      "[JetsonEnc][create] begin size=%dx%d src=%s dst=%s fps=%d bitrate=%d "
      "buffers=%d dma_src=%d",
      width_, height_, PixFmtName(src_pix_fmt_), PixFmtName(dst_pix_fmt_),
      framerate_, bitrate_bps_, BUFFER_NUM, is_dma_src_);

  encoder_ = NvVideoEncoder::createVideoEncoder("enc0");
  if (!encoder_) {
    LOG_ERROR("Could not create encoder");
    return false;
  }
  LogQueueState("create:created");

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, width_, height_,
                                        CHUNK_SIZE);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][create] Could not set capture plane format ret=%d errno=%d",
              ret, errno);
    return false;
  }
  LogQueueState("create:capture-format-set");

  ret = encoder_->setOutputPlaneFormat(src_pix_fmt_, width_, height_);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][create] Could not set output plane format ret=%d errno=%d",
              ret, errno);
    return false;
  }
  LogQueueState("create:output-format-set");

  if (!ApplyCodecSettings()) {
    return false;
  }
  LogQueueState("create:codec-settings-applied");

  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                          false);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][create] Could not setup output plane ret=%d errno=%d",
              ret, errno);
    return false;
  }
  LogQueueState("create:output-plane-setup");

  ret = encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                           false);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][create] Could not setup capture plane ret=%d errno=%d",
              ret, errno);
    return false;
  }
  LogQueueState("create:capture-plane-setup");

  return true;
}

bool JetsonEncoder::ApplyCodecSettings() {
  if (!encoder_) {
    LOG_ERROR("ApplyCodecSettings called with null encoder");
    return false;
  }

  int ret = 0;
  LOG_INFO(
      "[JetsonEnc][settings] begin bitrate=%d fps=%d gop=%u bitrate_mode=%d "
      "qp_range=%d qp_min=%u qp_max=%u",
      bitrate_bps_, framerate_, strategy_config_.gop_size,
      strategy_config_.bitrate_mode, strategy_config_.has_qp_range,
      strategy_config_.qp_min, strategy_config_.qp_max);

  ret = encoder_->setBitrate(bitrate_bps_);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set bitrate ret=%d errno=%d",
              ret, errno);
    return false;
  }

  if (dst_pix_fmt_ == V4L2_PIX_FMT_H264) {
    ret = encoder_->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
    if (ret < 0) {
      LOG_ERROR(
          "[JetsonEnc][settings] Could not set encoder profile ret=%d errno=%d",
          ret, errno);
      return false;
    }

    const uint32_t h264_level =
        SelectH264Level(width_, height_, framerate_);
    LOG_INFO("Jetson H264 level %s selected for %dx%d@%dfps",
             H264LevelName(h264_level), width_, height_, framerate_);
    ret = encoder_->setLevel(h264_level);
    if (ret < 0) {
      LOG_ERROR("[JetsonEnc][settings] Could not set encoder level ret=%d errno=%d",
                ret, errno);
      return false;
    }

    ret = encoder_->setNumBFrames(0);
    if (ret < 0) {
      LOG_ERROR("[JetsonEnc][settings] Could not set B frame number ret=%d errno=%d",
                ret, errno);
      return false;
    }

    ret = encoder_->setInsertSpsPpsAtIdrEnabled(true);
    if (ret < 0) {
      LOG_ERROR("Could not insert SPS PPS at every IDR");
      return false;
    }

    ret = encoder_->setInsertVuiEnabled(true);
    if (ret < 0) {
      LOG_ERROR("Could not insert Video Usability Information");
      return false;
    }
  }

  ret = encoder_->setRateControlMode(strategy_config_.bitrate_mode);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set rate control mode ret=%d errno=%d",
              ret, errno);
    return false;
  }

  if (strategy_config_.bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) {
    const uint32_t peak_bitrate = std::max<uint32_t>(
        static_cast<uint32_t>(bitrate_bps_),
        static_cast<uint32_t>(bitrate_bps_ + bitrate_bps_ / 2));
    ret = encoder_->setPeakBitrate(peak_bitrate);
    if (ret < 0) {
      LOG_WARN("[JetsonEnc][settings] Could not set encoder peak bitrate ret=%d errno=%d",
               ret, errno);
    }
  }

  const uint32_t key_frame_interval =
      std::max<uint32_t>(1, strategy_config_.gop_size);

  ret = encoder_->setIDRInterval(key_frame_interval);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set IDR interval ret=%d errno=%d",
              ret, errno);
    return false;
  }

  ret = encoder_->setIFrameInterval(key_frame_interval);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set I-frame interval ret=%d errno=%d",
              ret, errno);
    return false;
  }

  if (strategy_config_.has_qp_range) {
    ret = encoder_->setQpRange(strategy_config_.qp_min, strategy_config_.qp_max,
                               strategy_config_.qp_min, strategy_config_.qp_max,
                               strategy_config_.qp_min,
                               strategy_config_.qp_max);
    if (ret < 0) {
      LOG_WARN("[JetsonEnc][settings] Could not set encoder qp range ret=%d errno=%d",
               ret, errno);
    }
  }

  ret = encoder_->setFrameRate(framerate_, 1);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set encoder framerate ret=%d errno=%d",
              ret, errno);
    return false;
  }

  ret = encoder_->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
  if (ret < 0) {
    LOG_ERROR("[JetsonEnc][settings] Could not set encoder HW Preset ret=%d errno=%d",
              ret, errno);
    return false;
  }

  ret = encoder_->setMaxPerfMode(1);
  if (ret < 0) {
    LOG_WARN(
        "[JetsonEnc][settings] Could not set encoder max performance mode "
        "ret=%d errno=%d (may affect encoding speed)",
        ret, errno);
  }

  LOG_INFO("[JetsonEnc][settings] complete");
  return true;
}

void JetsonEncoder::SetStrategyConfig(const StrategyConfig& config) {
  strategy_config_ = config;
  if (strategy_config_.gop_size == 0) {
    strategy_config_.gop_size = KEY_FRAME_INTERVAL;
  }

  if (strategy_config_.has_qp_range &&
      strategy_config_.qp_max < strategy_config_.qp_min) {
    std::swap(strategy_config_.qp_min, strategy_config_.qp_max);
  }
}

void JetsonEncoder::LogQueueState(const char* event) {
  if (!encoder_) {
    LOG_INFO("[JetsonEnc][state] %s encoder=null", event);
    return;
  }

  LOG_INFO(
      "[JetsonEnc][state] %s size=%dx%d src=%s dst=%s fps=%d bitrate=%d "
      "out_queued=%u/%u cap_queued=%u/%u stopping=%d abort=%d error=%d",
      event, width_, height_, PixFmtName(src_pix_fmt_), PixFmtName(dst_pix_fmt_),
      framerate_, bitrate_bps_, encoder_->output_plane.getNumQueuedBuffers(),
      encoder_->output_plane.getNumBuffers(),
      encoder_->capture_plane.getNumQueuedBuffers(),
      encoder_->capture_plane.getNumBuffers(),
      stopping_.load(std::memory_order_acquire),
      abort_.load(std::memory_order_acquire), encoder_->isInError());
}

bool JetsonEncoder::Reconfigure(int new_width, int new_height) {
  if (!encoder_) {
    LOG_ERROR("Reconfigure called with null encoder");
    return false;
  }

  LOG_INFO("[JetsonEnc][reconfigure] begin old=%dx%d new=%dx%d", width_,
           height_, new_width, new_height);
  LogQueueState("reconfigure:before-stop");
  int ret = 0;
  abort_ = true;
  stopping_.store(true, std::memory_order_release);

  StopEncoderIo(false);
  LogQueueState("reconfigure:after-stop");
  encoder_->capture_plane.deinitPlane();
  encoder_->output_plane.deinitPlane();
  LogQueueState("reconfigure:after-deinit");

  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    capturing_tasks_.clear();
  }

  width_ = new_width;
  height_ = new_height;

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, width_, height_,
                                        CHUNK_SIZE);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not set capture plane format ret=%d "
        "errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:capture-format-set");

  ret = encoder_->setOutputPlaneFormat(src_pix_fmt_, width_, height_);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not set output plane format ret=%d "
        "errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:output-format-set");

  if (!ApplyCodecSettings()) {
    return false;
  }
  LogQueueState("reconfigure:codec-settings-applied");

  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                          false);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not setup output plane ret=%d errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:output-plane-setup");

  ret = encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                           false);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not setup capture plane ret=%d errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:capture-plane-setup");

  ret = encoder_->output_plane.setStreamStatus(true);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Failed to stream on output plane ret=%d "
        "errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:output-streamon");

  ret = encoder_->capture_plane.setStreamStatus(true);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Failed to stream on capture plane ret=%d "
        "errno=%d",
        ret, errno);
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }
  LogQueueState("reconfigure:capture-streamon");

  encoder_->capture_plane.setDQThreadCallback(EncoderCapturePlaneDqCallback);
  encoder_->capture_plane.startDQThread(this);
  LogQueueState("reconfigure:capture-dq-thread-started");

  if (!PrepareCaptureBuffer()) {
    LOG_ERROR("Failed to prepare capture buffers");
    StopEncoderIo(false);
    return false;
  }

  stopping_.store(false, std::memory_order_release);
  abort_ = false;
  ForceKeyFrame();
  LogQueueState("reconfigure:complete");
  return true;
}

bool JetsonEncoder::PrepareCaptureBuffer() {
  LogQueueState("prepare-capture:begin");
  for (uint32_t i = 0; i < encoder_->capture_plane.getNumBuffers(); i++) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];

    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

    v4l2_buf.index = i;
    v4l2_buf.m.planes = planes;

    LogNvBuffer("prepare-capture:before-qbuf",
                encoder_->capture_plane.getNthBuffer(i));
    if (encoder_->capture_plane.qBuffer(v4l2_buf, NULL) < 0) {
      LOG_ERROR(
          "[JetsonEnc][qbuf] Failed to queue capture buffer index=%u errno=%d",
          i, errno);
      LogV4L2Buffer("prepare-capture:qbuf-failed", v4l2_buf);
      LogQueueState("prepare-capture:qbuf-failed");
      return false;
    }
    LogV4L2Buffer("prepare-capture:qbuf-ok", v4l2_buf);
    LogQueueState("prepare-capture:qbuf-ok");
  }

  LogQueueState("prepare-capture:complete");
  return true;
}

void JetsonEncoder::SetBitrate(int adjusted_bitrate_bps) {
  if (!encoder_) {
    return;
  }

  if (bitrate_bps_ != adjusted_bitrate_bps) {
    bitrate_bps_ = adjusted_bitrate_bps;
    int ret = encoder_->setBitrate(adjusted_bitrate_bps);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder bitrate ret=%d errno=%d", ret, errno);
    }

    if (strategy_config_.bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) {
      const uint32_t peak_bitrate = std::max<uint32_t>(
          static_cast<uint32_t>(bitrate_bps_),
          static_cast<uint32_t>(bitrate_bps_ + bitrate_bps_ / 2));
      ret = encoder_->setPeakBitrate(peak_bitrate);
      if (ret < 0) {
        LOG_WARN("Could not set encoder peak bitrate ret=%d errno=%d", ret,
                 errno);
      }
    }
  }
}

void JetsonEncoder::ForceKeyFrame() {
  if (!encoder_) {
    return;
  }

  int ret = encoder_->forceIDR();
  if (ret < 0) {
    LOG_ERROR("Could not force set encoder to key frame");
  }
}

bool JetsonEncoder::Start() {
  if (!encoder_) {
    LOG_ERROR("Start called with null encoder");
    return false;
  }

  LogQueueState("start:begin");
  int e = encoder_->output_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("[JetsonEnc][start] Failed to stream on output plane ret=%d errno=%d",
              e, errno);
    return false;
  }
  LogQueueState("start:output-streamon");

  e = encoder_->capture_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("[JetsonEnc][start] Failed to stream on capture plane ret=%d errno=%d",
              e, errno);
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }
  LogQueueState("start:capture-streamon");

  encoder_->capture_plane.setDQThreadCallback(EncoderCapturePlaneDqCallback);
  encoder_->capture_plane.startDQThread(this);
  LogQueueState("start:capture-dq-thread-started");

  if (!PrepareCaptureBuffer()) {
    LOG_ERROR("Failed to prepare capture buffers");
    StopEncoderIo(false);
    return false;
  }

  stopping_.store(false, std::memory_order_release);
  abort_ = false;
  LogQueueState("start:complete");
  return true;
}

void JetsonEncoder::EmplaceBuffer(
    rtc::scoped_refptr<I420BufferInterface> i420_buffer,
    std::function<void(const uint8_t* data, size_t size, bool is_keyframe,
                       uint64_t timestamp)>
        on_capture) {
  if (!encoder_) {
    LOG_WARN("[JetsonEnc][emplace] encoder is null, dropping frame");
    return;
  }

  if (encoder_->isInError()) {
    LOG_ERROR("ERROR in encoder");
    LogQueueState("emplace:encoder-error");
    return;
  }

  if (abort_ || stopping_.load(std::memory_order_acquire)) {
    LogQueueState("emplace:stopping-or-abort");
    return;
  }

  LOG_INFO("[JetsonEnc][emplace] frame begin i420=%dx%d stride=%d/%d/%d",
           i420_buffer->width(), i420_buffer->height(), i420_buffer->StrideY(),
           i420_buffer->StrideU(), i420_buffer->StrideV());
  LogQueueState("emplace:begin");

  struct v4l2_buffer v4l2_output_buf;
  struct v4l2_plane output_planes[MAX_PLANES];
  NvBuffer* nv_buffer = nullptr;

  memset(&v4l2_output_buf, 0, sizeof(v4l2_output_buf));
  memset(output_planes, 0, sizeof(output_planes));
  v4l2_output_buf.m.planes = output_planes;

  if (encoder_->output_plane.getNumQueuedBuffers() ==
      encoder_->output_plane.getNumBuffers()) {
    // Queue is full. For low latency, do not block; drop frame if no buffer can
    // be dequeued immediately.
    LogQueueState("emplace:output-full-before-dq");
    if (encoder_->output_plane.dqBuffer(v4l2_output_buf, &nv_buffer, NULL, 0) <
        0) {
      LOG_WARN(
          "[JetsonEnc][dqbuf] Encoder output queue full, dropping frame; "
          "dqBuffer failed errno=%d",
          errno);
      LogV4L2Buffer("emplace:output-dq-failed", v4l2_output_buf);
      LogQueueState("emplace:output-dq-failed");
      return;
    }
    LogV4L2Buffer("emplace:output-dq-ok", v4l2_output_buf);
    LogNvBuffer("emplace:output-dq-nvbuf", nv_buffer);
    LogQueueState("emplace:output-dq-ok");
  } else {
    nv_buffer = encoder_->output_plane.getNthBuffer(
        encoder_->output_plane.getNumQueuedBuffers());
    if (!nv_buffer) {
      LOG_ERROR("[JetsonEnc][emplace] Failed to get output buffer");
      LogQueueState("emplace:get-output-buffer-failed");
      return;
    }
    v4l2_output_buf.index = nv_buffer->index;
    LogNvBuffer("emplace:output-next-nvbuf", nv_buffer);
  }

#if ENABLE_ENCODE_PERF_STATS
  auto convert_start = std::chrono::steady_clock::now();
#endif
  ConvertI420ToYUV420M(nv_buffer, i420_buffer);
  LogNvBuffer("emplace:after-convert", nv_buffer);
#if ENABLE_ENCODE_PERF_STATS
  auto convert_end = std::chrono::steady_clock::now();
  int64_t convert_duration_us =
      std::chrono::duration_cast<std::chrono::microseconds>(convert_end -
                                                            convert_start)
          .count();
#endif

  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    CaptureTask task;
    task.callback = on_capture;
#if ENABLE_ENCODE_PERF_STATS
    auto now = std::chrono::steady_clock::now();
    static auto reference_time = std::chrono::steady_clock::now();
    task.encode_start_time_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                              reference_time)
            .count();
#endif
    capturing_tasks_.push_back(task);
    LOG_INFO("[JetsonEnc][tasks] pushed pending_capture_tasks=%zu",
             capturing_tasks_.size());
  }

#if ENABLE_ENCODE_PERF_STATS
  if (convert_duration_us > 1000) {
    LOG_WARN("[编码性能] 格式转换耗时较长: %ld us (%.2f ms), 可能影响整体性能",
             convert_duration_us, convert_duration_us / 1000.0f);
  }
#endif

  if (encoder_->output_plane.qBuffer(v4l2_output_buf, nullptr) < 0) {
    const int saved_errno = errno;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      if (!capturing_tasks_.empty()) {
        capturing_tasks_.pop_back();
      }
    }
    LOG_ERROR("[JetsonEnc][qbuf] Failed to qBuffer output_plane errno=%d",
              saved_errno);
    LogV4L2Buffer("emplace:output-qbuf-failed", v4l2_output_buf);
    LogNvBuffer("emplace:output-qbuf-failed-nvbuf", nv_buffer);
    LogQueueState("emplace:output-qbuf-failed");
    return;
  }
  LogV4L2Buffer("emplace:output-qbuf-ok", v4l2_output_buf);
  LogQueueState("emplace:output-qbuf-ok");
}

bool JetsonEncoder::EncoderCapturePlaneDqCallback(struct v4l2_buffer* v4l2_buf,
                                                  NvBuffer* buffer,
                                                  NvBuffer* shared_buffer,
                                                  void* arg) {
  JetsonEncoder* thiz = static_cast<JetsonEncoder*>(arg);
  const bool stopping = thiz->stopping_.load(std::memory_order_acquire);

  if (!v4l2_buf || !buffer) {
    if (stopping) {
      return false;
    }
    thiz->abort_ = true;
    thiz->encoder_->abort();
    LOG_ERROR(
        "[JetsonEnc][capture-dq] Failed to dequeue buffer from encoder capture "
        "plane v4l2_buf=%p buffer=%p stopping=%d",
        v4l2_buf, buffer, stopping);
    thiz->LogQueueState("capture-dq:null-buffer");
    return false;
  }

  if (buffer->planes[0].bytesused == 0 || stopping) {
    LOG_INFO(
        "[JetsonEnc][capture-dq] stopping or empty capture buffer index=%u "
        "bytesused=%u stopping=%d",
        v4l2_buf->index, buffer->planes[0].bytesused, stopping);
    LogV4L2Buffer("capture-dq:empty-or-stopping", *v4l2_buf);
    LogNvBuffer("capture-dq:empty-or-stopping-nvbuf", buffer);
    thiz->LogQueueState("capture-dq:empty-or-stopping");
    return false;
  }

  LogV4L2Buffer("capture-dq:got-buffer", *v4l2_buf);
  LogNvBuffer("capture-dq:got-nvbuf", buffer);
  thiz->LogQueueState("capture-dq:got-buffer");

  v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
  bool is_keyframe = false;
  if (thiz->encoder_->getMetadata(v4l2_buf->index, enc_metadata) >= 0) {
    is_keyframe = enc_metadata.KeyFrame;
  }

  uint64_t timestamp = (v4l2_buf->timestamp.tv_usec % 1000000) +
                       (v4l2_buf->timestamp.tv_sec * 1000000UL);

  if (thiz->packets_buf_size_ < buffer->planes[0].bytesused) {
    uint32_t new_size = thiz->packets_buf_size_;
    while (new_size < buffer->planes[0].bytesused) {
      new_size *= 2;
    }
    thiz->packets_buf_size_ = new_size;
    for (int i = 0; i < thiz->packets_num_; i++) {
      delete[] thiz->packets_[i];
      thiz->packets_[i] = new unsigned char[thiz->packets_buf_size_];
    }
  }

  int current_index = thiz->buf_index_;
  thiz->packets_size_[current_index] = buffer->planes[0].bytesused;
  memcpy(thiz->packets_[current_index], buffer->planes[0].data,
         buffer->planes[0].bytesused);
  thiz->packets_keyflag_[current_index] = is_keyframe;
  thiz->timestamp_[current_index] = timestamp;

  CaptureTask task;
  {
    std::lock_guard<std::mutex> lock(thiz->tasks_mutex_);
    if (thiz->capturing_tasks_.empty()) {
      if (thiz->stopping_.load(std::memory_order_acquire)) {
        return false;
      }
      LOG_ERROR("No capture task available");
      if (thiz->encoder_->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
        const int saved_errno = errno;
        thiz->abort_ = true;
        thiz->encoder_->abort();
        LOG_ERROR(
            "[JetsonEnc][qbuf] Failed to requeue capture buffer without task "
            "index=%u errno=%d",
            v4l2_buf->index, saved_errno);
        LogV4L2Buffer("capture-dq:no-task-requeue-failed", *v4l2_buf);
        thiz->LogQueueState("capture-dq:no-task-requeue-failed");
        return false;
      }
      LogV4L2Buffer("capture-dq:no-task-requeue-ok", *v4l2_buf);
      thiz->LogQueueState("capture-dq:no-task-requeue-ok");
      return true;
    }
    LOG_INFO("[JetsonEnc][tasks] pop pending_capture_tasks_before=%zu",
             thiz->capturing_tasks_.size());
    task = thiz->capturing_tasks_.front();
    thiz->capturing_tasks_.pop_front();
  }

  task.callback(thiz->packets_[current_index],
                thiz->packets_size_[current_index], is_keyframe, timestamp);

  thiz->buf_index_ = (thiz->buf_index_ + 1) % thiz->packets_num_;

  if (thiz->encoder_->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
    const int saved_errno = errno;
    thiz->abort_ = true;
    thiz->encoder_->abort();
    LOG_ERROR(
        "[JetsonEnc][qbuf] Failed to requeue capture buffer index=%u errno=%d",
        v4l2_buf->index, saved_errno);
    LogV4L2Buffer("capture-dq:requeue-failed", *v4l2_buf);
    thiz->LogQueueState("capture-dq:requeue-failed");
    return false;
  }
  LogV4L2Buffer("capture-dq:requeue-ok", *v4l2_buf);
  thiz->LogQueueState("capture-dq:requeue-ok");

  return true;
}

void JetsonEncoder::ConvertI420ToYUV420M(
    NvBuffer* nv_buffer, rtc::scoped_refptr<I420BufferInterface> i420_buffer) {
  for (uint32_t p = 0; p < nv_buffer->n_planes; p++) {
    const uint8_t* src_addr;
    int stride;
    if (p == 0) {
      src_addr = i420_buffer->DataY();
      stride = i420_buffer->StrideY();
    } else if (p == 1) {
      src_addr = i420_buffer->DataU();
      stride = i420_buffer->StrideU();
    } else if (p == 2) {
      src_addr = i420_buffer->DataV();
      stride = i420_buffer->StrideV();
    } else {
      break;
    }

    auto& plane = nv_buffer->planes[p];
    int row_size = plane.fmt.bytesperpixel * plane.fmt.width;
    uint8_t* dst_addr = plane.data;
    plane.bytesused = 0;

    for (uint32_t row = 0; row < plane.fmt.height; row++) {
      memcpy(dst_addr, src_addr + stride * row, row_size);
      dst_addr += plane.fmt.stride;
    }

    plane.bytesused = plane.fmt.stride * plane.fmt.height;
  }
}

void JetsonEncoder::SendEOS() {
  if (!encoder_) {
    return;
  }

  LogQueueState("send-eos:begin");
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[MAX_PLANES];
  NvBuffer* buffer = nullptr;

  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
  v4l2_buffer.m.planes = planes;

  if (encoder_->output_plane.getNumQueuedBuffers() ==
      encoder_->output_plane.getNumBuffers()) {
    if (encoder_->output_plane.dqBuffer(v4l2_buffer, &buffer, NULL, 10) < 0) {
      LOG_ERROR(
          "[JetsonEnc][dqbuf] Failed to dqBuffer output while sending eos "
          "errno=%d",
          errno);
      LogV4L2Buffer("send-eos:output-dq-failed", v4l2_buffer);
      LogQueueState("send-eos:output-dq-failed");
      return;
    }
    LogV4L2Buffer("send-eos:output-dq-ok", v4l2_buffer);
    LogNvBuffer("send-eos:output-dq-nvbuf", buffer);
  } else {
    buffer = encoder_->output_plane.getNthBuffer(
        encoder_->output_plane.getNumQueuedBuffers());
    if (!buffer) {
      LOG_ERROR("Failed to get output buffer while sending eos");
      return;
    }
    v4l2_buffer.index = buffer->index;
    LogNvBuffer("send-eos:output-next-nvbuf", buffer);
  }

  planes[0].bytesused = 0;
  if (encoder_->output_plane.qBuffer(v4l2_buffer, NULL) < 0) {
    LOG_ERROR("[JetsonEnc][qbuf] Failed to qBuffer output eos errno=%d",
              errno);
    LogV4L2Buffer("send-eos:output-qbuf-failed", v4l2_buffer);
    LogQueueState("send-eos:output-qbuf-failed");
    return;
  }
  LogV4L2Buffer("send-eos:output-qbuf-ok", v4l2_buffer);
  LogQueueState("send-eos:complete");
}

void JetsonEncoder::StopEncoderIo(bool send_eos) {
  if (!encoder_) {
    return;
  }

  LogQueueState("stop:begin");
  stopping_.store(true, std::memory_order_release);

  if (send_eos) {
    SendEOS();
  }

  // In blocking mode stopDQThread() does not actively stop the thread; streamoff
  // is required to unblock dqBuffer() and let the callback thread exit.
  encoder_->capture_plane.stopDQThread();
  LogQueueState("stop:capture-dq-thread-stop-requested");

  if (encoder_->output_plane.setStreamStatus(false) < 0) {
    LOG_WARN(
        "[JetsonEnc][stop] Failed to streamoff output plane while stopping "
        "encoder IO errno=%d",
        errno);
  }
  LogQueueState("stop:output-streamoff");
  if (encoder_->capture_plane.setStreamStatus(false) < 0) {
    LOG_WARN(
        "[JetsonEnc][stop] Failed to streamoff capture plane while stopping "
        "encoder IO errno=%d",
        errno);
  }
  LogQueueState("stop:capture-streamoff");

  if (encoder_->capture_plane.waitForDQThread(DQ_THREAD_WAIT_TIMEOUT_MS) < 0) {
    LOG_WARN(
        "Timed out waiting for capture DQ thread to stop, forcing encoder "
        "abort");
    encoder_->abort();
    if (encoder_->capture_plane.waitForDQThread(DQ_THREAD_WAIT_TIMEOUT_MS) <
        0) {
      LOG_WARN("Capture DQ thread still running after abort");
    }
  }
  LogQueueState("stop:complete");
}

}  // namespace webrtc
