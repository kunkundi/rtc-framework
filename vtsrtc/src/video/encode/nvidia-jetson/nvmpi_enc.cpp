#include <unistd.h>
#include <iostream>
#include <queue>
#include <thread>
#include <vector>
#include <mutex>

#include "/usr/src/jetson_multimedia_api/include/NvVideoEncoder.h"
#include "/usr/src/jetson_multimedia_api/include/nvbufsurface.h"
#include "/usr/src/jetson_multimedia_api/include/nvbufsurftransform.h"
#include "log/log_manager.h"
#include "nvmpi.h"

#define CHUNK_SIZE 2 * 1024 * 1024
#define MAX_BUFFERS 32
#define TEST_ERROR(condition, message, errorCode) \
  if (condition) {                                \
    std::cerr << message << " Error code: " << errorCode << std::endl; \
    return false; \
  }
#define TEST_ERROR_NULL(condition, message, errorCode) \
  if (condition) {                                \
    std::cerr << message << " Error code: " << errorCode << std::endl; \
    return nullptr; \
  }

using namespace std;

struct nvmpictx {
  NvVideoEncoder *enc;
  int index;
  std::queue<int> *packet_pools;
  uint32_t width;
  uint32_t height;
  uint32_t profile;
  bool enableLossless;
  uint32_t bitrate;
  uint32_t peak_bitrate;
  uint32_t raw_pixfmt;
  uint32_t encoder_pixfmt;
  enum v4l2_mpeg_video_bitrate_mode ratecontrol;
  enum v4l2_mpeg_video_h264_level level;
  enum v4l2_enc_hw_preset_type hw_preset_type;
  uint32_t iframe_interval;
  uint32_t idr_interval;
  uint32_t fps_n;
  uint32_t fps_d;
  bool enable_extended_colorformat;
  uint32_t qmax;
  uint32_t qmin;
  uint32_t num_b_frames;
  uint32_t num_reference_frames;
  bool insert_sps_pps_at_idr;

  uint32_t packets_buf_size;
  uint32_t packets_num;
  unsigned char *packets[MAX_BUFFERS];
  uint32_t packets_size[MAX_BUFFERS];
  bool packets_keyflag[MAX_BUFFERS];
  uint64_t timestamp[MAX_BUFFERS];
  int buf_index;

  std::mutex mtx;  // Mutex to protect shared resources
};

static bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf,
                                              NvBuffer *buffer,
                                              NvBuffer *shared_buffer,
                                              void *arg) {
  nvmpictx *ctx = (nvmpictx *)arg;
  NvVideoEncoder *enc = ctx->enc;
  
  // Lock the mutex to ensure thread safety when accessing shared resources
  std::lock_guard<std::mutex> lock(ctx->mtx);

  if (v4l2_buf == NULL || buffer == NULL) {
    std::cerr << "Error while dequeuing buffer from output plane" << std::endl;
    return false;
  }

  if (buffer->planes[0].bytesused == 0) {
    std::cerr << "Got 0 size buffer in capture" << std::endl;
    return false;
  }

  if (ctx->packets_buf_size < buffer->planes[0].bytesused) {
    ctx->packets_buf_size = buffer->planes[0].bytesused;

    // Reallocate buffers if the size changes
    for (int index = 0; index < ctx->packets_num; index++) {
      delete[] ctx->packets[index];
      ctx->packets[index] = new unsigned char[ctx->packets_buf_size];
    }
  }

  ctx->packets_size[ctx->buf_index] = buffer->planes[0].bytesused;
  memcpy(ctx->packets[ctx->buf_index], buffer->planes[0].data,
         buffer->planes[0].bytesused);

  ctx->timestamp[ctx->buf_index] = (v4l2_buf->timestamp.tv_usec % 1000000) +
                                   (v4l2_buf->timestamp.tv_sec * 1000000UL);

  // Get metadata and set keyframe flag before pushing to queue
  v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
  ctx->enc->getMetadata(v4l2_buf->index, enc_metadata);
  ctx->packets_keyflag[ctx->buf_index] = enc_metadata.KeyFrame ? true : false;

  // Push to queue after all data is set
  ctx->packet_pools->push(ctx->buf_index);

  // Update buffer index for next packet
  ctx->buf_index = (ctx->buf_index + 1) % ctx->packets_num;

  if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0) {
    std::cerr << "Error while queueing buffer at capture plane" << std::endl;
    return false;
  }

  return true;
}

nvmpictx *nvmpi_create_encoder(nvCodingType codingType, nvEncParam *param) {
  int ret;
  log_level = LOG_LEVEL_INFO;

  nvmpictx *ctx = new nvmpictx;
  ctx->index = 0;
  ctx->width = param->width;
  ctx->height = param->height;
  ctx->enableLossless = false;
  ctx->bitrate = param->bitrate;
  ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
  ctx->idr_interval = param->idr_interval;
  ctx->fps_n = param->fps_n;
  ctx->fps_d = param->fps_d;
  ctx->iframe_interval = param->iframe_interval;
  ctx->packet_pools = new std::queue<int>;
  ctx->buf_index = 0;
  ctx->enable_extended_colorformat = false;
  ctx->packets_num = param->capture_num;
  ctx->qmax = param->qmax;
  ctx->qmin = param->qmin;
  ctx->num_b_frames = param->max_b_frames;
  ctx->num_reference_frames = param->refs;
  ctx->insert_sps_pps_at_idr = (param->insert_spspps_idr == 1) ? true : false;

  // Profile selection
  switch (param->profile) {
    case 77:  // FF_PROFILE_H264_MAIN
      ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
      break;
    case 66:  // FF_PROFILE_H264_BASELINE
      ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
      break;
    case 100:  // FF_PROFILE_H264_HIGH
      ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
      break;
    default:
      ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
      break;
  }

  // Level selection
  switch (param->level) {
    case 10:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
      break;
    case 11:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
      break;
    case 12:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
      break;
    case 13:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
      break;
    case 20:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
      break;
    case 21:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
      break;
    case 22:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
      break;
    case 30:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
      break;
    case 31:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
      break;
    case 32:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
      break;
    case 40:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
      break;
    case 41:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
      break;
    case 42:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
      break;
    case 50:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
      break;
    case 51:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
      break;
    default:
      ctx->level = V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
      break;
  }

  // Preset type selection
  switch (param->hw_preset_type) {
    case 1:
      ctx->hw_preset_type = V4L2_ENC_HW_PRESET_ULTRAFAST;
      break;
    case 2:
      ctx->hw_preset_type = V4L2_ENC_HW_PRESET_FAST;
      break;
    case 3:
      ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
      break;
    case 4:
      ctx->hw_preset_type = V4L2_ENC_HW_PRESET_SLOW;
      break;
    default:
      ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
      break;
  }

  if (param->enableLossless) ctx->enableLossless = true;
  if (param->mode_vbr) ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;

  ctx->packets_buf_size = CHUNK_SIZE;

  // Allocate memory for the buffers
  for (int index = 0; index < ctx->packets_num; index++) {
    ctx->packets[index] = new unsigned char[ctx->packets_buf_size];
  }

  // Set encoding format
  if (codingType == NV_VIDEO_CodingH264) {
    ctx->encoder_pixfmt = V4L2_PIX_FMT_H264;
  } else if (codingType == NV_VIDEO_CodingHEVC) {
    ctx->encoder_pixfmt = V4L2_PIX_FMT_H265;
  }

  ctx->enc = NvVideoEncoder::createVideoEncoder("enc0");
  TEST_ERROR_NULL(!ctx->enc, "Could not create encoder", ret);

  ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width, ctx->height, CHUNK_SIZE);
  TEST_ERROR_NULL(ret < 0, "Could not set output plane format", ret);

  switch (ctx->profile) {
    case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10:
      ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
      break;
    case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN:
    default:
      ctx->raw_pixfmt = V4L2_PIX_FMT_YUV420M;
      break;
  }

  if (ctx->enableLossless && codingType == NV_VIDEO_CodingH264) {
    ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
    ret = ctx->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV444M, ctx->width, ctx->height);
  } else {
    ret = ctx->enc->setOutputPlaneFormat(ctx->raw_pixfmt, ctx->width, ctx->height);
  }

  TEST_ERROR_NULL(ret < 0, "Could not set output plane format", ret);

  // Set bitrate, preset, etc.
  ret = ctx->enc->setBitrate(ctx->bitrate);
  TEST_ERROR_NULL(ret < 0, "Could not set encoder bitrate", ret);

  ret = ctx->enc->setHWPresetType(ctx->hw_preset_type);
  TEST_ERROR_NULL(ret < 0, "Could not set encoder HW Preset Type", ret);

  if (ctx->num_reference_frames) {
    ret = ctx->enc->setNumReferenceFrames(ctx->num_reference_frames);
    TEST_ERROR_NULL(ret < 0, "Could not set num reference frames", ret);
  }

  if (ctx->num_b_frames != (uint32_t)-1 && codingType == NV_VIDEO_CodingH264) {
    ret = ctx->enc->setNumBFrames(ctx->num_b_frames);
    TEST_ERROR_NULL(ret < 0, "Could not set number of B Frames", ret);
  }

  if (codingType == NV_VIDEO_CodingH264 || codingType == NV_VIDEO_CodingHEVC) {
    ret = ctx->enc->setProfile(ctx->profile);
    TEST_ERROR_NULL(ret < 0, "Could not set encoder profile", ret);
  }

  if (codingType == NV_VIDEO_CodingH264) {
    ret = ctx->enc->setLevel(ctx->level);
    TEST_ERROR_NULL(ret < 0, "Could not set encoder level", ret);
  }

  // Rate control mode and QP range
  if (ctx->enableLossless) {
    ret = ctx->enc->setConstantQp(0);
    TEST_ERROR_NULL(ret < 0, "Could not set encoder constant qp=0", ret);
  } else {
    ret = ctx->enc->setRateControlMode(ctx->ratecontrol);
    TEST_ERROR_NULL(ret < 0, "Could not set encoder rate control mode", ret);

    if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) {
      uint32_t peak_bitrate;
      if (ctx->peak_bitrate < ctx->bitrate)
        peak_bitrate = 1.2f * ctx->bitrate;
      else
        peak_bitrate = ctx->peak_bitrate;
      ret = ctx->enc->setPeakBitrate(peak_bitrate);
      TEST_ERROR_NULL(ret < 0, "Could not set encoder peak bitrate", ret);
    }
  }

  ret = ctx->enc->setIDRInterval(ctx->idr_interval);
  TEST_ERROR_NULL(ret < 0, "Could not set encoder IDR interval", ret);

  if (ctx->qmax > 0 || ctx->qmin > 0) {
    ctx->enc->setQpRange(ctx->qmin, ctx->qmax, ctx->qmin, ctx->qmax, ctx->qmin, ctx->qmax);
  }

  ret = ctx->enc->setIFrameInterval(ctx->iframe_interval);
  TEST_ERROR_NULL(ret < 0, "Could not set encoder I-Frame interval", ret);

  if (ctx->insert_sps_pps_at_idr) {
    ret = ctx->enc->setInsertSpsPpsAtIdrEnabled(true);
    TEST_ERROR_NULL(ret < 0, "Could not set insertSPSPPSAtIDR", ret);
  }

  ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
  TEST_ERROR_NULL(ret < 0, "Could not set framerate", ret);

  // Setup output and capture plane
  ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
  TEST_ERROR_NULL(ret < 0, "Could not setup output plane", ret);

  ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
  TEST_ERROR_NULL(ret < 0, "Could not setup capture plane", ret);

  ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS, 0, 0);
  TEST_ERROR_NULL(ret < 0, "Could not subscribe EOS event", ret);

  ret = ctx->enc->output_plane.setStreamStatus(true);
  TEST_ERROR_NULL(ret < 0, "Error in output plane streamon", ret);

  ret = ctx->enc->capture_plane.setStreamStatus(true);
  TEST_ERROR_NULL(ret < 0, "Error in capture plane streamon", ret);

  ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);
  ctx->enc->capture_plane.startDQThread(ctx);

  // Enqueue all the empty capture plane buffers
  for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

    v4l2_buf.index = i;
    v4l2_buf.m.planes = planes;

    ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
    TEST_ERROR_NULL(ret < 0, "Error while queueing buffer at capture plane", ret);
  }

  return ctx;
}

int nvmpi_encoder_put_frame(nvmpictx *ctx, nvFrame *frame) {
  int ret;
  struct v4l2_buffer v4l2_buf;
  struct v4l2_plane planes[MAX_PLANES];
  NvBuffer *nvBuffer;

  memset(&v4l2_buf, 0, sizeof(v4l2_buf));
  memset(planes, 0, sizeof(planes));

  v4l2_buf.m.planes = planes;

  if (ctx->enc->isInError()) return -1;

  if (ctx->index < ctx->enc->output_plane.getNumBuffers()) {
    nvBuffer = ctx->enc->output_plane.getNthBuffer(ctx->index);
    v4l2_buf.index = ctx->index;
    ctx->index++;
  } else {
    ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
    if (ret < 0) {
      std::cerr << "Error DQing buffer at output plane" << std::endl;
      return -1;
    }
  }

  NvBufSurface *nvbuf_surf = nullptr;
  ret = NvBufSurfaceFromFd(nvBuffer->planes[0].fd, (void**)(&nvbuf_surf));
  if (ret < 0) return -1;

  // Validate frame payload
  if (!frame || !frame->payload[0] || !frame->payload[1] || !frame->payload[2]) {
    std::cerr << "Invalid frame payload" << std::endl;
    return -1;
  }

  // Copy data using payload_size (caller has set correct sizes for YUV420)
  memcpy(nvbuf_surf->surfaceList[0].dataPtr, frame->payload[0], frame->payload_size[0]);
  memcpy(nvbuf_surf->surfaceList[1].dataPtr, frame->payload[1], frame->payload_size[1]);
  memcpy(nvbuf_surf->surfaceList[2].dataPtr, frame->payload[2], frame->payload_size[2]);

  // Sync all planes for device
  for (uint32_t plane = 0; plane < nvbuf_surf->surfaceList[0].planeParams.num_planes; plane++) {
    ret = NvBufSurfaceSyncForDevice(nvbuf_surf, 0, plane);
    if (ret < 0) return -1;
  }

  ret = ctx->enc->output_plane.qBuffer(v4l2_buf, nvBuffer);
  if (ret < 0) {
    std::cerr << "Error while enqueuing buffer at output plane" << std::endl;
    return -1;
  }

  return 0;
}

int nvmpi_encoder_get_packet(nvmpictx *ctx, nvPacket *packet) {
  if (!ctx || !packet || !ctx->packet_pools) {
    return -1;
  }

  std::lock_guard<std::mutex> lock(ctx->mtx);

  if (ctx->packet_pools->empty()) {
    return -1;  // No packet available
  }

  int buf_index = ctx->packet_pools->front();
  ctx->packet_pools->pop();

  packet->payload = ctx->packets[buf_index];
  packet->payload_size = ctx->packets_size[buf_index];
  packet->pts = ctx->timestamp[buf_index];
  packet->flags = ctx->packets_keyflag[buf_index] ? 0x1 : 0x0;  // Key frame flag

  return 0;
}

int nvmpi_encoder_set_fps(nvmpictx *ctx, unsigned int fps) {
  if (!ctx || !ctx->enc || fps == 0) {
    return -1;
  }

  ctx->fps_n = fps;
  ctx->fps_d = 1;

  int ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
  if (ret < 0) {
    std::cerr << "Error setting framerate" << std::endl;
    return -1;
  }

  return 0;
}

int nvmpi_encoder_set_bitrate(nvmpictx *ctx, unsigned int bitrate) {
  if (!ctx || !ctx->enc || bitrate == 0) {
    return -1;
  }

  ctx->bitrate = bitrate;

  int ret = ctx->enc->setBitrate(ctx->bitrate);
  if (ret < 0) {
    std::cerr << "Error setting bitrate" << std::endl;
    return -1;
  }

  // Update peak bitrate if VBR mode
  if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) {
    uint32_t peak_bitrate;
    if (ctx->peak_bitrate < ctx->bitrate) {
      peak_bitrate = 1.2f * ctx->bitrate;
    } else {
      peak_bitrate = ctx->peak_bitrate;
    }
    ret = ctx->enc->setPeakBitrate(peak_bitrate);
    if (ret < 0) {
      std::cerr << "Error setting peak bitrate" << std::endl;
      return -1;
    }
  }

  return 0;
}

int nvmpi_encoder_force_idr(nvmpictx *ctx) {
  if (!ctx || !ctx->enc) {
    return -1;
  }

  int ret = ctx->enc->forceIDR();
  if (ret < 0) {
    std::cerr << "Error forcing IDR frame" << std::endl;
    return -1;
  }

  return 0;
}

int nvmpi_encoder_close(nvmpictx *ctx) {
  if (!ctx) {
    return -1;
  }

  if (ctx->enc) {
    // Stop DQ thread
    ctx->enc->capture_plane.stopDQThread();
    ctx->enc->capture_plane.waitForDQThread(-1);

    // Stop streaming
    ctx->enc->output_plane.setStreamStatus(false);
    ctx->enc->capture_plane.setStreamStatus(false);

    // Destroy encoder
    delete ctx->enc;
    ctx->enc = nullptr;
  }

  // Free packet buffers
  for (int i = 0; i < ctx->packets_num; i++) {
    if (ctx->packets[i]) {
      delete[] ctx->packets[i];
      ctx->packets[i] = nullptr;
    }
  }

  // Free packet pools queue
  if (ctx->packet_pools) {
    delete ctx->packet_pools;
    ctx->packet_pools = nullptr;
  }

  // Free context
  delete ctx;

  return 0;
}
