#ifndef JETSON_ENCODER_H_
#define JETSON_ENCODER_H_

#include <api/video/i420_buffer.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <mutex>

#include "/usr/src/jetson_multimedia_api/include/NvVideoEncoder.h"
#include "/usr/src/jetson_multimedia_api/include/nvbufsurface.h"

#ifndef ENABLE_ENCODE_PERF_STATS
#define ENABLE_ENCODE_PERF_STATS 0
#endif

namespace webrtc {

class JetsonEncoder {
 public:
  JetsonEncoder(int width, int height, uint32_t dst_pix_fmt, bool is_dma_src);
  ~JetsonEncoder();

  static std::unique_ptr<JetsonEncoder> Create(int width, int height,
                                                uint32_t dst_pix_fmt,
                                                bool is_dma_src);

  void EmplaceBuffer(rtc::scoped_refptr<I420BufferInterface> i420_buffer,
                     std::function<void(const uint8_t* data, size_t size,
                                        bool is_keyframe, uint64_t timestamp)>
                         on_capture);
  void ForceKeyFrame();
  void SetFps(int fps);
  void SetBitrate(int bitrate_bps);

 private:
  NvVideoEncoder* encoder_;
  std::atomic<bool> abort_;
  int width_;
  int height_;
  int framerate_;
  int bitrate_bps_;
  uint32_t src_pix_fmt_;
  uint32_t dst_pix_fmt_;
  bool is_dma_src_;

  struct CaptureTask {
    std::function<void(const uint8_t* data, size_t size, bool is_keyframe,
                       uint64_t timestamp)>
        callback;
#if ENABLE_ENCODE_PERF_STATS
    int64_t encode_start_time_us;  // 编码开始时间（微秒）
#endif
  };

  std::queue<CaptureTask> capturing_tasks_;
  std::mutex tasks_mutex_;

  // Packet buffers
  static const int MAX_BUFFERS = 32;
  static const int CHUNK_SIZE = 2 * 1024 * 1024;
  unsigned char* packets_[MAX_BUFFERS];
  uint32_t packets_size_[MAX_BUFFERS];
  bool packets_keyflag_[MAX_BUFFERS];
  uint64_t timestamp_[MAX_BUFFERS];
  uint32_t packets_buf_size_;
  uint32_t packets_num_;
  int buf_index_;

  bool CreateVideoEncoder();
  bool PrepareCaptureBuffer();
  void Start();
  void SendEOS();
  static bool EncoderCapturePlaneDqCallback(struct v4l2_buffer* v4l2_buf,
                                            NvBuffer* buffer,
                                            NvBuffer* shared_buffer, void* arg);
  void ConvertI420ToYUV420M(NvBuffer* nv_buffer,
                             rtc::scoped_refptr<I420BufferInterface> i420_buffer);
};

}  // namespace webrtc

#endif  // JETSON_ENCODER_H_

