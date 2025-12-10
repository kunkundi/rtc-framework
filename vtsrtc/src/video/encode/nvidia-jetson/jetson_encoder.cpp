#include "jetson_encoder.h"
#include "log/log_manager.h"

#include <cstring>
#include <linux/videodev2.h>

#include "/usr/src/jetson_multimedia_api/include/NvBuffer.h"
#include "/usr/src/jetson_multimedia_api/include/nvbufsurface.h"

#ifndef MAX_PLANES
#define MAX_PLANES 4
#endif

namespace webrtc {

const int KEY_FRAME_INTERVAL = 256;
const int BUFFER_NUM = 4;

std::unique_ptr<JetsonEncoder> JetsonEncoder::Create(int width, int height,
                                                     uint32_t dst_pix_fmt,
                                                     bool is_dma_src) {
  auto ptr = std::make_unique<JetsonEncoder>(width, height, dst_pix_fmt, is_dma_src);
  if (!ptr->CreateVideoEncoder()) {
    return nullptr;
  }
  ptr->Start();
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
  // Initialize packet buffers
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

  SendEOS();

  if (encoder_) {
    encoder_->capture_plane.waitForDQThread(-1);
    encoder_->capture_plane.deinitPlane();
    encoder_->output_plane.deinitPlane();

    delete encoder_;
    encoder_ = nullptr;
  }

  // Free packet buffers
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

  ret = encoder_->setCapturePlaneFormat(dst_pix_fmt_, width_, height_, CHUNK_SIZE);
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
    ret = encoder_->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder profile");
      return false;
    }

    ret = encoder_->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_1);
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

  ret = encoder_->setIFrameInterval(0);
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

  // Setup output plane
  ret = encoder_->output_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true, false);
  if (ret < 0) {
    LOG_ERROR("Could not setup output plane");
    return false;
  }

  // Setup capture plane
  ret = encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, BUFFER_NUM, true, false);
  if (ret < 0) {
    LOG_ERROR("Could not setup capture plane");
    return false;
  }

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
  if (framerate_ != adjusted_fps) {
    framerate_ = adjusted_fps;
    int ret = encoder_->setFrameRate(framerate_, 1);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder framerate to %d", framerate_);
    }
  }
}

void JetsonEncoder::SetBitrate(int adjusted_bitrate_bps) {
  if (bitrate_bps_ != adjusted_bitrate_bps) {
    bitrate_bps_ = adjusted_bitrate_bps;
    int ret = encoder_->setBitrate(adjusted_bitrate_bps);
    if (ret < 0) {
      LOG_ERROR("Could not set encoder bitrate");
    }
  }
}

void JetsonEncoder::ForceKeyFrame() {
  int ret = encoder_->forceIDR();
  if (ret < 0) {
    LOG_ERROR("Could not force set encoder to key frame");
  }
}

void JetsonEncoder::Start() {
  int e = encoder_->output_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("Failed to stream on output plane");
    return;
  }
  e = encoder_->capture_plane.setStreamStatus(true);
  if (e < 0) {
    LOG_ERROR("Failed to stream on capture plane");
    return;
  }

  encoder_->capture_plane.setDQThreadCallback(EncoderCapturePlaneDqCallback);
  encoder_->capture_plane.startDQThread(this);

  PrepareCaptureBuffer();

  abort_ = false;
}

void JetsonEncoder::EmplaceBuffer(
    rtc::scoped_refptr<I420BufferInterface> i420_buffer,
    std::function<void(const uint8_t* data, size_t size, bool is_keyframe,
                      uint64_t timestamp)>
        on_capture) {
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
    if (encoder_->output_plane.dqBuffer(v4l2_output_buf, &nv_buffer, NULL, 10) <
        0) {
      LOG_ERROR("Failed to dqBuffer at encoder output_plane");
      return;
    }
  } else {
    nv_buffer = encoder_->output_plane.getNthBuffer(
        encoder_->output_plane.getNumQueuedBuffers());
    v4l2_output_buf.index = nv_buffer->index;
  }

  ConvertI420ToYUV420M(nv_buffer, i420_buffer);

  {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    CaptureTask task;
    task.callback = on_capture;
    capturing_tasks_.push(task);
  }

  if (encoder_->output_plane.qBuffer(v4l2_output_buf, nullptr) < 0) {
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
    // EOS
    return false;
  }

  // Get metadata for keyframe detection
  v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
  bool is_keyframe = false;
  if (thiz->encoder_->getMetadata(v4l2_buf->index, enc_metadata) >= 0) {
    is_keyframe = enc_metadata.KeyFrame;
  }

  // Calculate timestamp
  uint64_t timestamp = (v4l2_buf->timestamp.tv_usec % 1000000) +
                       (v4l2_buf->timestamp.tv_sec * 1000000UL);

  // Reallocate buffer if needed
  if (thiz->packets_buf_size_ < buffer->planes[0].bytesused) {
    thiz->packets_buf_size_ = buffer->planes[0].bytesused;
    for (int i = 0; i < thiz->packets_num_; i++) {
      delete[] thiz->packets_[i];
      thiz->packets_[i] = new unsigned char[thiz->packets_buf_size_];
    }
  }

  // Copy encoded data
  int current_index = thiz->buf_index_;
  thiz->packets_size_[current_index] = buffer->planes[0].bytesused;
  memcpy(thiz->packets_[current_index], buffer->planes[0].data,
         buffer->planes[0].bytesused);
  thiz->packets_keyflag_[current_index] = is_keyframe;
  thiz->timestamp_[current_index] = timestamp;

  // Get callback and invoke it
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
    thiz->capturing_tasks_.pop();
  }

  // Invoke callback
  task.callback(thiz->packets_[current_index], thiz->packets_size_[current_index],
                is_keyframe, timestamp);

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
    NvBuffer* nv_buffer,
    rtc::scoped_refptr<I420BufferInterface> i420_buffer) {
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
  struct v4l2_buffer v4l2_buffer;
  struct v4l2_plane planes[MAX_PLANES];
  NvBuffer* buffer;

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

