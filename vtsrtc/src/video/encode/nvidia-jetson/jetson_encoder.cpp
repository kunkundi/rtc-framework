#include "jetson_encoder.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iterator>

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
const uint32_t OUTPUT_DRAIN_TIMEOUT_MS = 250;
const uint32_t CAPTURE_TASK_DRAIN_TIMEOUT_MS = 250;

int64_t SteadyTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool HasH264NalType(const uint8_t* data, size_t size, uint8_t nal_type) {
  if (!data) {
    return false;
  }

  for (size_t i = 0; i + 4 < size; ++i) {
    size_t header_offset = size;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      header_offset = i + 3;
    } else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
               data[i + 3] == 1) {
      header_offset = i + 4;
    }
    if (header_offset < size &&
        (data[header_offset] & 0x1f) == nal_type) {
      return true;
    }
  }
  return false;
}

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
      awaiting_resolution_idr_(false),
      force_idr_before_next_frame_(false),
      reconfigure_ready_(false),
      width_(width),
      height_(height),
      session_width_(width),
      session_height_(height),
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

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, session_width_,
                                        session_height_, CHUNK_SIZE);
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

  if (new_width <= 0 || new_height <= 0) {
    LOG_ERROR("[JetsonEnc][reconfigure] Invalid target size=%dx%d", new_width,
              new_height);
    return false;
  }

  if (new_width == width_ && new_height == height_) {
    return true;
  }

  if (new_width > session_width_ || new_height > session_height_) {
    LOG_WARN(
        "[JetsonEnc][reconfigure] Target size=%dx%d exceeds session "
        "limit=%dx%d, recreate encoder session",
        new_width, new_height, session_width_, session_height_);
    return false;
  }

  if (new_width > width_ || new_height > height_) {
    LOG_WARN(
        "[JetsonEnc][reconfigure] Jetson output-plane DRC only supports "
        "high-to-low changes, current=%dx%d target=%dx%d",
        width_, height_, new_width, new_height);
    return false;
  }

  if (!CanReconfigure()) {
    LOG_WARN(
        "[JetsonEnc][reconfigure] Session has not produced a frame since "
        "creation or the previous DRC, current=%dx%d target=%dx%d",
        width_, height_, new_width, new_height);
    return false;
  }

  const auto start_time = std::chrono::steady_clock::now();
  LOG_INFO(
      "[JetsonEnc][reconfigure] begin old=%dx%d new=%dx%d session=%dx%d",
      width_, height_, new_width, new_height, session_width_, session_height_);
  LogQueueState("reconfigure:before-output-drain");

  if (!DrainOutputPlane()) {
    return false;
  }
  LogQueueState("reconfigure:after-output-drain");

  if (!WaitForPendingTasks()) {
    return false;
  }
  LogQueueState("reconfigure:after-capture-drain");

  int ret = encoder_->output_plane.setStreamStatus(false);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not stream off output plane ret=%d "
        "errno=%d",
        ret, errno);
    return false;
  }
  LogQueueState("reconfigure:output-streamoff");

  encoder_->output_plane.deinitPlane();
  LogQueueState("reconfigure:output-plane-deinit");

  ret = encoder_->setOutputPlaneFormat(src_pix_fmt_, new_width, new_height);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not set output plane format ret=%d "
        "errno=%d",
        ret, errno);
    MarkUnhealthyAfterReconfigureFailure("set-output-format", ret);
    return false;
  }
  LogQueueState("reconfigure:output-format-set");

  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                          false);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Could not setup output plane ret=%d errno=%d",
        ret, errno);
    MarkUnhealthyAfterReconfigureFailure("setup-output-plane", ret);
    return false;
  }
  ResetAvailableOutputBuffers();
  LogQueueState("reconfigure:output-plane-setup");

  // 在重新启流前先进入 DRC pending。只有目标分辨率的 SPS+IDR 已经
  // 通过完整的 capture task 路径后，才允许同一会话继续下一次 DRC。
  reconfigure_ready_.store(false, std::memory_order_release);
  if (dst_pix_fmt_ == V4L2_PIX_FMT_H264) {
    awaiting_resolution_idr_.store(true, std::memory_order_release);
    force_idr_before_next_frame_.store(true, std::memory_order_release);
  }

  ret = encoder_->output_plane.setStreamStatus(true);
  if (ret < 0) {
    LOG_ERROR(
        "[JetsonEnc][reconfigure] Failed to stream on output plane ret=%d "
        "errno=%d",
        ret, errno);
    MarkUnhealthyAfterReconfigureFailure("streamon-output-plane", ret);
    return false;
  }
  LogQueueState("reconfigure:output-streamon");

  width_ = new_width;
  height_ = new_height;
  if (dst_pix_fmt_ != V4L2_PIX_FMT_H264) {
    ForceKeyFrame();
  }
  const auto duration_ms =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start_time)
          .count() /
      1000.0;
  LOG_INFO(
      "[JetsonEnc][reconfigure] output-plane DRC complete size=%dx%d "
      "duration=%.3f ms",
      width_, height_, duration_ms);
  LogQueueState("reconfigure:complete");
  return true;
}

bool JetsonEncoder::CanReconfigure() const {
  return reconfigure_ready_.load(std::memory_order_acquire) &&
         !awaiting_resolution_idr_.load(std::memory_order_acquire);
}

bool JetsonEncoder::IsHealthy() const {
  return encoder_ && !abort_.load(std::memory_order_acquire) &&
         !stopping_.load(std::memory_order_acquire) && !encoder_->isInError();
}

void JetsonEncoder::MarkUnhealthyAfterReconfigureFailure(const char* stage,
                                                         int ret) {
  reconfigure_ready_.store(false, std::memory_order_release);
  abort_.store(true, std::memory_order_release);
  LOG_ERROR(
      "[JetsonEnc][reconfigure] Session is unusable after destructive "
      "output-plane failure stage=%s ret=%d",
      stage, ret);
  if (encoder_) {
    encoder_->abort();
  }
}

bool JetsonEncoder::DrainOutputPlane() {
  std::unique_lock<std::mutex> lock(output_buffers_mutex_);
  const size_t output_buffer_count = encoder_->output_plane.getNumBuffers();
  if (output_buffers_condition_.wait_for(
          lock, std::chrono::milliseconds(OUTPUT_DRAIN_TIMEOUT_MS),
          [this, output_buffer_count]() {
            return available_output_buffers_.size() == output_buffer_count;
          })) {
    return true;
  }

  LOG_ERROR(
      "[JetsonEnc][reconfigure] Timed out draining output plane, "
      "available=%zu total=%zu queued=%u",
      available_output_buffers_.size(), output_buffer_count,
      encoder_->output_plane.getNumQueuedBuffers());
  return false;
}

bool JetsonEncoder::WaitForPendingTasks() {
  std::unique_lock<std::mutex> lock(tasks_mutex_);
  if (tasks_condition_.wait_for(
          lock, std::chrono::milliseconds(CAPTURE_TASK_DRAIN_TIMEOUT_MS),
          [this]() { return capturing_tasks_.empty(); })) {
    return true;
  }

  LOG_ERROR(
      "[JetsonEnc][reconfigure] Timed out waiting for capture tasks, "
      "pending=%zu",
      capturing_tasks_.size());
  return false;
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

void JetsonEncoder::ResetAvailableOutputBuffers() {
  std::lock_guard<std::mutex> lock(output_buffers_mutex_);
  available_output_buffers_.clear();
  for (uint32_t i = 0; i < encoder_->output_plane.getNumBuffers(); ++i) {
    available_output_buffers_.push_back(i);
  }
  output_buffers_condition_.notify_all();
}

bool JetsonEncoder::ReclaimOutputBuffer() {
  struct v4l2_buffer v4l2_output_buf;
  struct v4l2_plane output_planes[MAX_PLANES];
  NvBuffer* output_buffer = nullptr;
  memset(&v4l2_output_buf, 0, sizeof(v4l2_output_buf));
  memset(output_planes, 0, sizeof(output_planes));
  v4l2_output_buf.m.planes = output_planes;

  // 编码完成时对应的输入缓冲已经消费完毕。阻塞式 DQ 只允许发生在
  // Capture Plane 线程，禁止占用 WebRTC EncoderQueue。
  if (encoder_->output_plane.dqBuffer(v4l2_output_buf, &output_buffer, nullptr,
                                      10) < 0) {
    if (stopping_.load(std::memory_order_acquire)) {
      return false;
    }
    const int saved_errno = errno;
    abort_ = true;
    encoder_->abort();
    LOG_ERROR(
        "[JetsonEnc][dqbuf] Failed to reclaim output buffer errno=%d",
        saved_errno);
    LogQueueState("capture-dq:output-reclaim-failed");
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(output_buffers_mutex_);
    available_output_buffers_.push_back(v4l2_output_buf.index);
  }
  output_buffers_condition_.notify_one();
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

void JetsonEncoder::SetFramerate(int adjusted_framerate) {
  if (!encoder_ || adjusted_framerate < 1 || framerate_ == adjusted_framerate) {
    return;
  }

  const int ret = encoder_->setFrameRate(adjusted_framerate, 1);
  if (ret < 0) {
    LOG_ERROR("Could not set encoder framerate ret=%d errno=%d", ret, errno);
    return;
  }
  framerate_ = adjusted_framerate;
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

  ResetAvailableOutputBuffers();

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

  if (!i420_buffer || i420_buffer->width() != width_ ||
      i420_buffer->height() != height_) {
    LOG_ERROR(
        "[JetsonEnc][emplace] Input size mismatch, input=%dx%d encoder=%dx%d; "
        "drop frame before qBuffer",
        i420_buffer ? i420_buffer->width() : 0,
        i420_buffer ? i420_buffer->height() : 0, width_, height_);
    return;
  }

  struct v4l2_buffer v4l2_output_buf;
  struct v4l2_plane output_planes[MAX_PLANES];
  NvBuffer* nv_buffer = nullptr;

  memset(&v4l2_output_buf, 0, sizeof(v4l2_output_buf));
  memset(output_planes, 0, sizeof(output_planes));
  v4l2_output_buf.m.planes = output_planes;
  const uint64_t task_timestamp_us =
      next_task_timestamp_us_.fetch_add(1, std::memory_order_relaxed);
  v4l2_output_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
  v4l2_output_buf.timestamp.tv_sec = task_timestamp_us / 1000000u;
  v4l2_output_buf.timestamp.tv_usec = task_timestamp_us % 1000000u;

  {
    std::lock_guard<std::mutex> lock(output_buffers_mutex_);
    if (available_output_buffers_.empty()) {
      const int64_t now_ms = SteadyTimeMillis();
      int64_t last_log_ms =
          last_output_drop_log_ms_.load(std::memory_order_relaxed);
      if (now_ms - last_log_ms >= 1000 &&
          last_output_drop_log_ms_.compare_exchange_strong(
              last_log_ms, now_ms, std::memory_order_relaxed)) {
        LOG_WARN(
            "[JetsonEnc][emplace] No free output buffer, dropping input "
            "frame size=%dx%d queued=%u/%u",
            width_, height_, encoder_->output_plane.getNumQueuedBuffers(),
            encoder_->output_plane.getNumBuffers());
      }
      return;
    }
    v4l2_output_buf.index = available_output_buffers_.front();
    available_output_buffers_.pop_front();
  }

  nv_buffer = encoder_->output_plane.getNthBuffer(v4l2_output_buf.index);
  if (!nv_buffer) {
    {
      std::lock_guard<std::mutex> lock(output_buffers_mutex_);
      available_output_buffers_.push_front(v4l2_output_buf.index);
    }
    output_buffers_condition_.notify_one();
    LOG_ERROR("[JetsonEnc][emplace] Failed to get output buffer");
    LogQueueState("emplace:get-output-buffer-failed");
    return;
  }

#if ENABLE_ENCODE_PERF_STATS
  auto convert_start = std::chrono::steady_clock::now();
#endif
  if (!ConvertI420ToYUV420M(nv_buffer, i420_buffer)) {
    {
      std::lock_guard<std::mutex> lock(output_buffers_mutex_);
      available_output_buffers_.push_front(v4l2_output_buf.index);
    }
    output_buffers_condition_.notify_one();
    return;
  }
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
    task.timestamp_us = task_timestamp_us;
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
  }

#if ENABLE_ENCODE_PERF_STATS
  if (convert_duration_us > 1000) {
    LOG_WARN("[编码性能] 格式转换耗时较长: %ld us (%.2f ms), 可能影响整体性能",
             convert_duration_us, convert_duration_us / 1000.0f);
  }
#endif

  if (force_idr_before_next_frame_.exchange(false,
                                             std::memory_order_acq_rel)) {
    ForceKeyFrame();
  }

  if (encoder_->output_plane.qBuffer(v4l2_output_buf, nullptr) < 0) {
    const int saved_errno = errno;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      if (!capturing_tasks_.empty()) {
        capturing_tasks_.pop_back();
      }
    }
    tasks_condition_.notify_all();
    {
      std::lock_guard<std::mutex> lock(output_buffers_mutex_);
      available_output_buffers_.push_front(v4l2_output_buf.index);
    }
    output_buffers_condition_.notify_one();
    LOG_ERROR("[JetsonEnc][qbuf] Failed to qBuffer output_plane errno=%d",
              saved_errno);
    LogV4L2Buffer("emplace:output-qbuf-failed", v4l2_output_buf);
    LogNvBuffer("emplace:output-qbuf-failed-nvbuf", nv_buffer);
    LogQueueState("emplace:output-qbuf-failed");
    return;
  }
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

  v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
  bool is_keyframe = false;
  if (thiz->encoder_->getMetadata(v4l2_buf->index, enc_metadata) >= 0) {
    is_keyframe = enc_metadata.KeyFrame;
  }

  const bool has_h264_idr =
      thiz->dst_pix_fmt_ == V4L2_PIX_FMT_H264 &&
      HasH264NalType(buffer->planes[0].data, buffer->planes[0].bytesused, 5);
  const bool has_h264_sps =
      thiz->dst_pix_fmt_ == V4L2_PIX_FMT_H264 &&
      HasH264NalType(buffer->planes[0].data, buffer->planes[0].bytesused, 7);
  is_keyframe = is_keyframe || has_h264_idr;

  const uint64_t timestamp =
      static_cast<uint64_t>(v4l2_buf->timestamp.tv_usec) +
      static_cast<uint64_t>(v4l2_buf->timestamp.tv_sec) * 1000000u;

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

  const bool awaiting_resolution_idr =
      thiz->awaiting_resolution_idr_.load(std::memory_order_acquire);
  const bool deliver_frame =
      !awaiting_resolution_idr || (has_h264_idr && has_h264_sps);

  CaptureTask task;
  bool matched_task = false;
  size_t abandoned_tasks = 0;
  {
    std::lock_guard<std::mutex> lock(thiz->tasks_mutex_);
    auto task_it = std::find_if(
        thiz->capturing_tasks_.begin(), thiz->capturing_tasks_.end(),
        [timestamp](const CaptureTask& candidate) {
          return candidate.timestamp_us == timestamp;
        });
    if (task_it == thiz->capturing_tasks_.end()) {
      if (thiz->stopping_.load(std::memory_order_acquire)) {
        return false;
      }
      LOG_INFO(
          "[JetsonEnc][capture-dq] Extra coded buffer without matching "
          "capture task timestamp=%llu; requeue after DRC",
          static_cast<unsigned long long>(timestamp));
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
      // Jetson 在 DRC 后可能为同一个输入额外输出一块 SPS/图像数据。
      // 此时对应的输出面缓冲已随前一块捕获数据回收，不能再次阻塞 DQ。
      LogV4L2Buffer("capture-dq:no-task-requeue-ok", *v4l2_buf);
      thiz->LogQueueState("capture-dq:no-task-requeue-ok");
      return true;
    }

    // 时间戳已经前进到较新的输入时，排在它之前的任务不会再产生可交付
    // 码流。丢弃这些任务并在锁外逐个回收对应 output buffer。
    abandoned_tasks = static_cast<size_t>(
        std::distance(thiz->capturing_tasks_.begin(), task_it));
    for (size_t i = 0; i < abandoned_tasks; ++i) {
      thiz->capturing_tasks_.pop_front();
    }

    if (deliver_frame) {
      task = thiz->capturing_tasks_.front();
      thiz->capturing_tasks_.pop_front();
      matched_task = true;
    }
  }
  if (abandoned_tasks > 0 || matched_task) {
    thiz->tasks_condition_.notify_all();
  }

  if (!deliver_frame) {
    // Jetson DRC 的首帧可能仅带新 SPS 和非 IDR 图像。同一输入还可能
    // 继续输出 IDR，因此保留时间戳任务，不得让该 buffer 消费下一帧任务。
    thiz->force_idr_before_next_frame_.store(true,
                                             std::memory_order_release);
    LOG_WARN(
        "[JetsonEnc][reconfigure] Hold capture task for target IDR, "
        "timestamp=%llu idr=%d sps=%d",
        static_cast<unsigned long long>(timestamp), has_h264_idr,
        has_h264_sps);
  }

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

  for (size_t i = 0; i < abandoned_tasks; ++i) {
    if (!thiz->ReclaimOutputBuffer()) {
      return false;
    }
  }

  if (!deliver_frame) {
    return true;
  }

  if (!thiz->ReclaimOutputBuffer()) {
    return false;
  }

  thiz->buf_index_ = (thiz->buf_index_ + 1) % thiz->packets_num_;
  if (awaiting_resolution_idr) {
    thiz->awaiting_resolution_idr_.store(false, std::memory_order_release);
    is_keyframe = true;
  }
  // 无任务的额外 SPS 输出和 DRC 后不可交付的非 IDR 都不能解除 pending，
  // 否则快速弱网降档可能在硬件确认新尺寸前再次修改 output plane。
  thiz->reconfigure_ready_.store(true, std::memory_order_release);
  task.callback(thiz->packets_[current_index],
                thiz->packets_size_[current_index], is_keyframe, timestamp);
  return true;
}

bool JetsonEncoder::ConvertI420ToYUV420M(
    NvBuffer* nv_buffer, rtc::scoped_refptr<I420BufferInterface> i420_buffer) {
  if (!nv_buffer || !i420_buffer || nv_buffer->n_planes < 3) {
    LOG_ERROR("[JetsonEnc][convert] Invalid I420 or NvBuffer");
    return false;
  }

  for (uint32_t p = 0; p < nv_buffer->n_planes; p++) {
    const uint8_t* src_addr;
    int stride;
    int visible_width;
    int visible_height;
    uint8_t padding_value;
    if (p == 0) {
      src_addr = i420_buffer->DataY();
      stride = i420_buffer->StrideY();
      visible_width = i420_buffer->width();
      visible_height = i420_buffer->height();
      padding_value = 16;
    } else if (p == 1) {
      src_addr = i420_buffer->DataU();
      stride = i420_buffer->StrideU();
      visible_width = (i420_buffer->width() + 1) / 2;
      visible_height = (i420_buffer->height() + 1) / 2;
      padding_value = 128;
    } else if (p == 2) {
      src_addr = i420_buffer->DataV();
      stride = i420_buffer->StrideV();
      visible_width = (i420_buffer->width() + 1) / 2;
      visible_height = (i420_buffer->height() + 1) / 2;
      padding_value = 128;
    } else {
      break;
    }

    auto& plane = nv_buffer->planes[p];
    const int bytes_per_pixel = std::max(1u, plane.fmt.bytesperpixel);
    const int row_size = bytes_per_pixel * visible_width;
    const uint64_t padded_size =
        static_cast<uint64_t>(plane.fmt.stride) * plane.fmt.height;
    if (!src_addr || stride < row_size ||
        plane.fmt.width < static_cast<uint32_t>(visible_width) ||
        plane.fmt.height < static_cast<uint32_t>(visible_height) ||
        plane.fmt.stride < static_cast<uint32_t>(row_size) ||
        padded_size > plane.length) {
      LOG_ERROR(
          "[JetsonEnc][convert] Plane mismatch p=%u visible=%dx%d "
          "src_stride=%d dst=%ux%u dst_stride=%u bpp=%u padded=%llu "
          "length=%u sizeimage=%u",
          p, visible_width, visible_height, stride, plane.fmt.width,
          plane.fmt.height, plane.fmt.stride, plane.fmt.bytesperpixel,
          static_cast<unsigned long long>(padded_size), plane.length,
          plane.fmt.sizeimage);
      return false;
    }

    uint8_t* dst_addr = plane.data;
    if (!dst_addr) {
      LOG_ERROR("[JetsonEnc][convert] Null destination plane p=%u", p);
      return false;
    }
    plane.bytesused = 0;
    memset(dst_addr, padding_value, static_cast<size_t>(padded_size));

    for (int row = 0; row < visible_height; row++) {
      memcpy(dst_addr, src_addr + stride * row, row_size);
      dst_addr += plane.fmt.stride;
    }

    plane.bytesused = static_cast<uint32_t>(padded_size);
  }
  return true;
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
