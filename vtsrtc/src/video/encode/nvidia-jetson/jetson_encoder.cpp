#include "jetson_encoder.h"

#include <linux/videodev2.h>

#include <algorithm>
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

std::unique_ptr<JetsonEncoder> JetsonEncoder::Create(int width, int height,
                                                     uint32_t dst_pix_fmt,
                                                     bool is_dma_src) {
  auto ptr =
      std::make_unique<JetsonEncoder>(width, height, dst_pix_fmt, is_dma_src);
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
      width_(width),
      height_(height),
      framerate_(30),
      bitrate_bps_(2 * 1024 * 1024),
      src_pix_fmt_(V4L2_PIX_FMT_YUV420M),
      dst_pix_fmt_(dst_pix_fmt),
      is_dma_src_(is_dma_src),
      packets_buf_size_(CHUNK_SIZE),
      packets_num_(BUFFER_NUM),
      buf_index_(0) {
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

  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    capturing_tasks_.clear();
  }

  if (encoder_) {
    SendEOS();

    encoder_->capture_plane.stopDQThread();
    encoder_->capture_plane.waitForDQThread(-1);

    encoder_->output_plane.setStreamStatus(false);
    encoder_->capture_plane.setStreamStatus(false);

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

  encoder_ = NvVideoEncoder::createVideoEncoder("enc0");
  if (!encoder_) {
    LOG_ERROR("Could not create encoder");
    return false;
  }

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, width_, height_,
                                        CHUNK_SIZE);
  if (ret < 0) {
    LOG_ERROR("Could not set capture plane format");
    return false;
  }

  ret = encoder_->setOutputPlaneFormat(src_pix_fmt_, width_, height_);
  if (ret < 0) {
    LOG_ERROR("Could not set output plane format");
    return false;
  }

  ret = encoder_->setBitrate(bitrate_bps_);
  if (ret < 0) {
    LOG_ERROR("Could not set bitrate");
    return false;
  }

  if (dst_pix_fmt_ == V4L2_PIX_FMT_H264) {
    ret = encoder_->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder profile");
      return false;
    }

    ret = encoder_->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_3_1);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder level");
      return false;
    }

    ret = encoder_->setNumBFrames(0);
    if (ret < 0) {
      LOG_ERROR("Could not set B frame number");
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

  ret = encoder_->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
  if (ret < 0) {
    LOG_ERROR("Could not set rate control mode");
    return false;
  }

  ret = encoder_->setIDRInterval(KEY_FRAME_INTERVAL);
  if (ret < 0) {
    LOG_ERROR("Could not set IDR interval");
    return false;
  }

  ret = encoder_->setIFrameInterval(KEY_FRAME_INTERVAL);
  if (ret < 0) {
    LOG_ERROR("Could not set I-frame interval");
    return false;
  }

  ret = encoder_->setFrameRate(framerate_, 1);
  if (ret < 0) {
    LOG_ERROR("Could not set encoder framerate");
    return false;
  }

  ret = encoder_->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
  if (ret < 0) {
    LOG_ERROR("Could not set encoder HW Preset");
    return false;
  }

  ret = encoder_->setMaxPerfMode(1);
  if (ret < 0) {
    LOG_WARN(
        "Could not set encoder max performance mode (may affect encoding "
        "speed)");
  }

  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                          false);
  if (ret < 0) {
    LOG_ERROR("Could not setup output plane");
    return false;
  }

  ret = encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                           false);
  if (ret < 0) {
    LOG_ERROR("Could not setup capture plane");
    return false;
  }

  return true;
}

bool JetsonEncoder::Reconfigure(int new_width, int new_height) {
  if (!encoder_) {
    LOG_ERROR("Reconfigure called with null encoder");
    return false;
  }

  int ret = 0;
  abort_ = true;

  encoder_->capture_plane.stopDQThread();
  encoder_->capture_plane.waitForDQThread(-1);
  encoder_->output_plane.setStreamStatus(false);
  encoder_->capture_plane.setStreamStatus(false);
  encoder_->capture_plane.deinitPlane();
  encoder_->output_plane.deinitPlane();

  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    capturing_tasks_.clear();
  }

  width_ = new_width;
  height_ = new_height;

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, width_, height_,
                                        CHUNK_SIZE);
  if (ret < 0) {
    LOG_ERROR("Could not set capture plane format");
    return false;
  }

  ret = encoder_->setOutputPlaneFormat(src_pix_fmt_, width_, height_);
  if (ret < 0) {
    LOG_ERROR("Could not set output plane format");
    return false;
  }

  ret = encoder_->setFrameRate(framerate_, 1);
  if (ret < 0) {
    LOG_ERROR("Could not set encoder framerate");
    return false;
  }

  ret = encoder_->setBitrate(bitrate_bps_);
  if (ret < 0) {
    LOG_ERROR("Could not set bitrate");
    return false;
  }

  // Re-apply performance related settings after format changes.
  ret = encoder_->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
  if (ret < 0) {
    LOG_WARN("Could not set rate control mode during reconfigure");
  }

  ret = encoder_->setIDRInterval(KEY_FRAME_INTERVAL);
  if (ret < 0) {
    LOG_WARN("Could not set IDR interval during reconfigure");
  }

  ret = encoder_->setIFrameInterval(KEY_FRAME_INTERVAL);
  if (ret < 0) {
    LOG_WARN("Could not set I-frame interval during reconfigure");
  }

  ret = encoder_->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
  if (ret < 0) {
    LOG_WARN("Could not set encoder HW preset during reconfigure");
  }

  ret = encoder_->setMaxPerfMode(1);
  if (ret < 0) {
    LOG_WARN("Could not set encoder max performance mode during reconfigure");
  }

  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                          false);
  if (ret < 0) {
    LOG_ERROR("Could not setup output plane");
    return false;
  }

  ret = encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true,
                                           false);
  if (ret < 0) {
    LOG_ERROR("Could not setup capture plane");
    return false;
  }

  ret = encoder_->output_plane.setStreamStatus(true);
  if (ret < 0) {
    LOG_ERROR("Failed to stream on output plane");
    return false;
  }

  ret = encoder_->capture_plane.setStreamStatus(true);
  if (ret < 0) {
    LOG_ERROR("Failed to stream on capture plane");
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }

  encoder_->capture_plane.setDQThreadCallback(EncoderCapturePlaneDqCallback);
  encoder_->capture_plane.startDQThread(this);

  if (!PrepareCaptureBuffer()) {
    LOG_ERROR("Failed to prepare capture buffers");
    encoder_->capture_plane.stopDQThread();
    encoder_->capture_plane.waitForDQThread(-1);
    encoder_->capture_plane.setStreamStatus(false);
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }

  abort_ = false;
  ForceKeyFrame();
  return true;
}

bool JetsonEncoder::PrepareCaptureBuffer() {
  for (uint32_t i = 0; i < encoder_->capture_plane.getNumBuffers(); i++) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];

    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

    v4l2_buf.index = i;
    v4l2_buf.m.planes = planes;

    if (encoder_->capture_plane.qBuffer(v4l2_buf, NULL) < 0) {
      LOG_ERROR("Failed to queue buffers into encoder capture plane");
      return false;
    }
  }

  return true;
}

void JetsonEncoder::SetFps(int adjusted_fps) {
  if (!encoder_) {
    return;
  }

  if (framerate_ != adjusted_fps) {
    framerate_ = adjusted_fps;
    int ret = encoder_->setFrameRate(framerate_, 1);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder framerate to %d", framerate_);
    }
  }
}

void JetsonEncoder::SetBitrate(int adjusted_bitrate_bps) {
  if (!encoder_) {
    return;
  }

  if (bitrate_bps_ != adjusted_bitrate_bps) {
    bitrate_bps_ = adjusted_bitrate_bps;
    int ret = encoder_->setBitrate(adjusted_bitrate_bps);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder bitrate");
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

  int e = encoder_->output_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("Failed to stream on output plane");
    return false;
  }

  e = encoder_->capture_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("Failed to stream on capture plane");
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }

  encoder_->capture_plane.setDQThreadCallback(EncoderCapturePlaneDqCallback);
  encoder_->capture_plane.startDQThread(this);

  if (!PrepareCaptureBuffer()) {
    LOG_ERROR("Failed to prepare capture buffers");
    encoder_->capture_plane.stopDQThread();
    encoder_->capture_plane.waitForDQThread(-1);
    encoder_->capture_plane.setStreamStatus(false);
    encoder_->output_plane.setStreamStatus(false);
    return false;
  }

  abort_ = false;
  return true;
}

void JetsonEncoder::EmplaceBuffer(
    rtc::scoped_refptr<I420BufferInterface> i420_buffer,
    std::function<void(const uint8_t* data, size_t size, bool is_keyframe,
                       uint64_t timestamp)>
        on_capture) {
  if (!encoder_) {
    return;
  }

  if (encoder_->isInError()) {
    LOG_ERROR("ERROR in encoder");
    return;
  }

  if (abort_) {
    return;
  }

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
    if (encoder_->output_plane.dqBuffer(v4l2_output_buf, &nv_buffer, NULL, 0) <
        0) {
      LOG_WARN("Encoder output queue full, dropping frame to keep latency low");
      return;
    }
  } else {
    nv_buffer = encoder_->output_plane.getNthBuffer(
        encoder_->output_plane.getNumQueuedBuffers());
    v4l2_output_buf.index = nv_buffer->index;
  }

#if ENABLE_ENCODE_PERF_STATS
  auto convert_start = std::chrono::steady_clock::now();
#endif
  ConvertI420ToYUV420M(nv_buffer, i420_buffer);
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
  }

#if ENABLE_ENCODE_PERF_STATS
  if (convert_duration_us > 1000) {
    LOG_WARN("[编码性能] 格式转换耗时较长: %ld us (%.2f ms), 可能影响整体性能",
             convert_duration_us, convert_duration_us / 1000.0f);
  }
#endif

  if (encoder_->output_plane.qBuffer(v4l2_output_buf, nullptr) < 0) {
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      if (!capturing_tasks_.empty()) {
        capturing_tasks_.pop_back();
      }
    }
    LOG_ERROR("Failed to qBuffer at encoder output_plane");
    return;
  }
}

bool JetsonEncoder::EncoderCapturePlaneDqCallback(struct v4l2_buffer* v4l2_buf,
                                                  NvBuffer* buffer,
                                                  NvBuffer* shared_buffer,
                                                  void* arg) {
  JetsonEncoder* thiz = static_cast<JetsonEncoder*>(arg);

  if (!v4l2_buf || !buffer) {
    thiz->abort_ = true;
    thiz->encoder_->abort();
    LOG_ERROR("Failed to dequeue buffer from encoder capture plane");
    return false;
  }

  if (buffer->planes[0].bytesused == 0) {
    return false;
  }

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
      LOG_ERROR("No capture task available");
      if (thiz->encoder_->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
        thiz->abort_ = true;
        thiz->encoder_->abort();
        LOG_ERROR("Failed to enqueue buffer to encoder capture plane");
        return false;
      }
      return true;
    }
    task = thiz->capturing_tasks_.front();
    thiz->capturing_tasks_.pop_front();
  }

  task.callback(thiz->packets_[current_index],
                thiz->packets_size_[current_index], is_keyframe, timestamp);

  thiz->buf_index_ = (thiz->buf_index_ + 1) % thiz->packets_num_;

  if (thiz->encoder_->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
    thiz->abort_ = true;
    thiz->encoder_->abort();
    LOG_ERROR("Failed to enqueue buffer to encoder capture plane");
    return false;
  }

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

  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[MAX_PLANES];
  NvBuffer* buffer = nullptr;

  memset(&v4l2_buffer, 0, sizeof(v4l2_buffer));
  memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
  v4l2_buffer.m.planes = planes;

  if (encoder_->output_plane.getNumQueuedBuffers() ==
      encoder_->output_plane.getNumBuffers()) {
    if (encoder_->output_plane.dqBuffer(v4l2_buffer, &buffer, NULL, 10) < 0) {
      LOG_ERROR("Failed to dqBuffer at encoder while sending eos");
    }
  }

  planes[0].bytesused = 0;
  if (encoder_->output_plane.qBuffer(v4l2_buffer, NULL) < 0) {
    LOG_ERROR("Failed to qBuffer at encoder while sending eos");
  }
}

}  // namespace webrtc
