#include "jetsonh264_encoder_impl.h"

#include <absl/strings/match.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <system_wrappers/include/metrics.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

#include "jetson_encoder.h"
#include "log/log_manager.h"
#include "video/encode/playout_delay_config.h"

namespace webrtc {

namespace {

constexpr int kDeferredReleaseGraceMs = 500;
thread_local const JetsonH264EncoderImpl* g_active_callback_encoder = nullptr;

}  // namespace

enum class H264EncoderImplEvent {
  H264EncoderEventInit = 0,
  H264EncoderEventError = 1,
  H264EncoderEventMax = 16,
};

static inline unsigned int AlignToEven(unsigned int value) {
  return value & ~1u;
}

static inline unsigned int ClampQpValue(unsigned int value) {
  return std::min(value, 51u);
}

static inline int64_t SteadyTimeMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

JetsonH264EncoderImpl::JetsonH264EncoderImpl(
    const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config)
    : rtc_config_(rtc_config) {
  RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));

  std::string packetization_mode_string;
  if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
                     &packetization_mode_string) &&
      packetization_mode_string == "1") {
    packetization_mode_ = H264PacketizationMode::NonInterleaved;
  }

  if (rtc_config_.encode_params.I_frame_interval != 0) {
    gop_size_ = rtc_config_.encode_params.I_frame_interval;
  }

  if (rtc_config_.encode_params.qp_threshold.first > 0 &&
      rtc_config_.encode_params.qp_threshold.second >
          rtc_config_.encode_params.qp_threshold.first) {
    qp_threshold_ = rtc_config_.encode_params.qp_threshold;
  }

  if (rtc_config_.encode_params.qp_range.first > 0 &&
      rtc_config_.encode_params.qp_range.second >=
          rtc_config_.encode_params.qp_range.first) {
    qp_range_.first = ClampQpValue(rtc_config_.encode_params.qp_range.first);
    qp_range_.second = ClampQpValue(rtc_config_.encode_params.qp_range.second);
  }

  if (!rtc_config_.encode_params.bitrate_mode.empty()) {
    if (absl::EqualsIgnoreCase(rtc_config_.encode_params.bitrate_mode, "vbr") ||
        absl::EqualsIgnoreCase(rtc_config_.encode_params.bitrate_mode, "cbr")) {
      bitrate_mode_ = rtc_config_.encode_params.bitrate_mode;
    } else {
      LOG_WARN(
          "[WEBRTC] Unsupported bitrate_mode=%s for jetson, fallback to cbr",
          rtc_config_.encode_params.bitrate_mode.c_str());
    }
  }

  if (rtc_config_.encode_params.bitrate_maxmum != 0) {
    bitrate_cap_bps_ = rtc_config_.encode_params.bitrate_maxmum;
  }

  const auto configured_playout_delay = ResolveConfiguredPlayoutDelay(
      rtc_config_.encode_params, "jetson");
  has_configured_playout_delay_ = configured_playout_delay.enabled;
  configured_playout_delay_min_ms_ = configured_playout_delay.min_ms;
  configured_playout_delay_max_ms_ = configured_playout_delay.max_ms;
  if (has_configured_playout_delay_) {
    LOG_INFO("[WEBRTC] Enable playout delay for jetson: [%d, %d] ms",
             configured_playout_delay_min_ms_,
             configured_playout_delay_max_ms_);
  }

  InitializeResolutionBitrateLimits();
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() {
  ReleaseHardwareResources();
}

void JetsonH264EncoderImpl::ApplyRatesToEncoder(JetsonEncoder* encoder) {
  if (!encoder) {
    return;
  }

  // Jetson 编码会话运行中只更新码率。WebRTC 的输入帧率估计在启动和
  // 资源自适应阶段会快速摆动，频繁下发 VIDIOC_S_PARM 会增加多会话竞争。
  if (bitrate_ > 0) {
    encoder->SetBitrate(bitrate_);
  }
}

bool JetsonH264EncoderImpl::ShouldEnableStandby(unsigned int width,
                                                unsigned int height) {
  if (width == 0 || height == 0) {
    return false;
  }

  // 双目拼接主画面为 16:10；环视为 16:9。只为主画面保留热备，避免
  // 四路低码率环视同时创建活动和热备会话，将 NVENC 会话数翻倍。
  const uint64_t scaled_width = static_cast<uint64_t>(width) * 10u;
  const uint64_t scaled_height = static_cast<uint64_t>(height) * 16u;
  const uint64_t difference = scaled_width > scaled_height
                                  ? scaled_width - scaled_height
                                  : scaled_height - scaled_width;
  return difference <= 16u;
}

JetsonEncoder::StrategyConfig JetsonH264EncoderImpl::BuildStrategyConfig() const {
  JetsonEncoder::StrategyConfig config;
  config.bitrate_mode = absl::EqualsIgnoreCase(bitrate_mode_, "vbr")
                            ? V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
                            : V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
  config.gop_size = std::max(1u, gop_size_);
  if (qp_range_.first > 0 && qp_range_.second >= qp_range_.first) {
    config.has_qp_range = true;
    config.qp_min = qp_range_.first;
    config.qp_max = qp_range_.second;
  }
  return config;
}

std::unique_ptr<JetsonEncoder> JetsonH264EncoderImpl::CreateEncoder(
    unsigned int width, unsigned int height,
    const JetsonEncoder::StrategyConfig& config, unsigned int framerate,
    unsigned int bitrate) const {
  return JetsonEncoder::Create(
      width, height, V4L2_PIX_FMT_H264, false, config,
      static_cast<int>(std::max(1u, framerate)),
      static_cast<int>(std::max(1u, bitrate)));
}

bool JetsonH264EncoderImpl::WarmupEncoder(JetsonEncoder* encoder,
                                          unsigned int width,
                                          unsigned int height) const {
  if (!encoder || width == 0 || height == 0) {
    return false;
  }

  auto buffer = I420Buffer::Create(width, height);
  for (int row = 0; row < buffer->height(); ++row) {
    memset(buffer->MutableDataY() + row * buffer->StrideY(), 16,
           buffer->width());
  }
  const int chroma_width = (buffer->width() + 1) / 2;
  const int chroma_height = (buffer->height() + 1) / 2;
  for (int row = 0; row < chroma_height; ++row) {
    memset(buffer->MutableDataU() + row * buffer->StrideU(), 128,
           chroma_width);
    memset(buffer->MutableDataV() + row * buffer->StrideV(), 128,
           chroma_width);
  }

  struct WarmupState {
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
  };
  auto state = std::make_shared<WarmupState>();
  encoder->ForceKeyFrame();
  encoder->EmplaceBuffer(
      buffer, [state](const uint8_t*, size_t, bool, uint64_t) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->completed = true;
        }
        state->condition.notify_one();
      });

  std::unique_lock<std::mutex> lock(state->mutex);
  return state->condition.wait_for(
      lock, std::chrono::seconds(1),
      [state]() { return state->completed; });
}

void JetsonH264EncoderImpl::ResetQpStatistics() {
  std::lock_guard<std::mutex> lock(qp_stats_mutex_);
  const unsigned int bitrate_bps = qp_stats_.bitrate_bps;
  const unsigned int fps = qp_stats_.fps;
  qp_stats_ = QpWindowStats();
  qp_stats_.bitrate_bps = bitrate_bps;
  qp_stats_.fps = fps;
}

void JetsonH264EncoderImpl::RecordQp(int qp, unsigned int width,
                                     unsigned int height) {
  const auto now = std::chrono::steady_clock::now();
  QpWindowStats snapshot;
  bool should_log = false;
  {
    std::lock_guard<std::mutex> lock(qp_stats_mutex_);
    if (!qp_stats_.initialized) {
      qp_stats_.initialized = true;
      qp_stats_.window_start = now;
      qp_stats_.width = width;
      qp_stats_.height = height;
    }

    if (qp >= 0 && qp <= 51) {
      qp_stats_.qp_sum += static_cast<uint64_t>(qp);
      ++qp_stats_.sample_count;
      qp_stats_.min_qp = std::min(qp_stats_.min_qp, qp);
      qp_stats_.max_qp = std::max(qp_stats_.max_qp, qp);
    } else {
      ++qp_stats_.missing_count;
    }
    qp_stats_.width = width;
    qp_stats_.height = height;

    if (now - qp_stats_.window_start >= std::chrono::seconds(1)) {
      snapshot = qp_stats_;
      should_log = true;
      qp_stats_.initialized = true;
      qp_stats_.qp_sum = 0;
      qp_stats_.sample_count = 0;
      qp_stats_.missing_count = 0;
      qp_stats_.min_qp = 52;
      qp_stats_.max_qp = -1;
      qp_stats_.window_start = now;
    }
  }

  if (!should_log) {
    return;
  }
  if (snapshot.sample_count == 0) {
    LOG_WARN(
        "[JetsonEnc][qp] encoder=%p size=%ux%u samples=0 missing=%u "
        "bitrate=%u fps=%u thresholds=%u/%u",
        static_cast<void*>(this), snapshot.width, snapshot.height,
        snapshot.missing_count, snapshot.bitrate_bps, snapshot.fps,
        qp_threshold_.first, qp_threshold_.second);
    return;
  }

  const double average_qp =
      static_cast<double>(snapshot.qp_sum) / snapshot.sample_count;
  LOG_INFO(
      "[JetsonEnc][qp] encoder=%p size=%ux%u avg=%.2f min=%d max=%d "
      "samples=%u missing=%u bitrate=%u fps=%u thresholds=%u/%u",
      static_cast<void*>(this), snapshot.width, snapshot.height, average_qp,
      snapshot.min_qp, snapshot.max_qp, snapshot.sample_count,
      snapshot.missing_count, snapshot.bitrate_bps, snapshot.fps,
      qp_threshold_.first, qp_threshold_.second);
}

EncodedImageCallback* JetsonH264EncoderImpl::BeginEncodeCallback() {
  std::unique_lock<std::mutex> lock(callback_lifecycle_mutex_);
  if (g_active_callback_encoder == this) {
    // 同一线程递归进入外部回调会破坏 Release 的单 lease 假设。硬件输出
    // 正常不会同步递归；若上层回调间接触发该路径，丢弃内层帧更安全。
    return nullptr;
  }
  const uint64_t entry_epoch = callback_epoch_;
  // 活跃会话和待退休/热备会话拥有独立 DQ 线程。分辨率切换边界可能同时
  // 到达 SendFrame，必须串行调用同一个 WebRTC callback，才能让回调内
  // Release() 只跳过当前 lease，而不会漏掉另一个仍持有旧指针的线程。
  callback_lifecycle_condition_.wait(
      lock, [this]() { return callback_inflight_ == 0; });
  if (entry_epoch != callback_epoch_) {
    // 该帧在 Release/Register 之前已经进入等待队列。即使新 callback 已
    // 注册，也不能把旧会话帧交给新生命周期。
    LOG_INFO(
        "[JetsonEnc][callback] Drop queued frame from previous lifecycle "
        "epoch=%llu current=%llu",
        static_cast<unsigned long long>(entry_epoch),
        static_cast<unsigned long long>(callback_epoch_));
    return nullptr;
  }
  EncodedImageCallback* callback =
      encoded_image_callback_.load(std::memory_order_acquire);
  if (callback) {
    callback_inflight_ = 1;
  }
  return callback;
}

void JetsonH264EncoderImpl::EndEncodeCallback() {
  {
    std::lock_guard<std::mutex> lock(callback_lifecycle_mutex_);
    if (callback_inflight_ > 0) {
      --callback_inflight_;
    }
  }
  callback_lifecycle_condition_.notify_all();
}

void JetsonH264EncoderImpl::ClearEncodeCallbackAndWait() {
  std::unique_lock<std::mutex> lock(callback_lifecycle_mutex_);
  ++callback_epoch_;
  encoded_image_callback_.store(nullptr, std::memory_order_release);
  if (g_active_callback_encoder == this) {
    // OnEncodedImage 内部可能同步触发 Release()；当前回调本身持有调用栈，
    // 不能等待自己退出。其他线程不会再取得已清空的 callback 指针。
    return;
  }
  callback_lifecycle_condition_.wait(
      lock, [this]() { return callback_inflight_ == 0; });
}

void JetsonH264EncoderImpl::StartStandbyWorker() {
  StopStandbyWorker();
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    standby_worker_stop_ = false;
    standby_request_pending_ = false;
    deferred_release_pending_ = false;
  }
  standby_thread_ =
      std::thread(&JetsonH264EncoderImpl::StandbyWorkerLoop, this);
}

void JetsonH264EncoderImpl::StopStandbyWorker() {
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    standby_worker_stop_ = true;
    standby_request_pending_ = false;
    deferred_release_pending_ = false;
  }
  standby_worker_condition_.notify_all();
  if (standby_thread_.joinable()) {
    standby_thread_.join();
  }
  std::lock_guard<std::mutex> lock(standby_worker_mutex_);
  retired_encoders_.clear();
}

void JetsonH264EncoderImpl::CancelDeferredRelease() {
  codec_released_.store(false, std::memory_order_release);
  release_sequence_.fetch_add(1, std::memory_order_acq_rel);
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    deferred_release_pending_ = false;
  }
  standby_worker_condition_.notify_all();
}

void JetsonH264EncoderImpl::ScheduleDeferredRelease() {
  codec_released_.store(true, std::memory_order_release);
  const uint64_t sequence =
      release_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    standby_request_pending_ = false;
    deferred_release_pending_ = true;
    deferred_release_sequence_ = sequence;
    deferred_release_deadline_ = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(
                                     kDeferredReleaseGraceMs);
  }
  standby_worker_condition_.notify_all();
}

void JetsonH264EncoderImpl::ReleaseHardwareResources() {
  codec_released_.store(true, std::memory_order_release);
  release_sequence_.fetch_add(1, std::memory_order_acq_rel);
  session_epoch_.fetch_add(1, std::memory_order_acq_rel);
  standby_enabled_.store(false, std::memory_order_release);
  encoder_generation_.fetch_add(1, std::memory_order_release);
  ClearEncodeCallbackAndWait();
  StopStandbyWorker();

  std::unique_ptr<JetsonEncoder> active;
  std::unique_ptr<JetsonEncoder> standby;
  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    active = std::move(encoder_);
    standby = std::move(standby_encoder_);
    width_ = 0;
    height_ = 0;
    session_width_ = 0;
    session_height_ = 0;
  }
  active.reset();
  standby.reset();

  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.ClearEncodedData();
    encoded_image_capacity_ = 0;
  }
  ResetQpStatistics();
}

void JetsonH264EncoderImpl::RequestStandbyPrewarm() {
  if (!standby_enabled_.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    if (standby_worker_stop_) {
      return;
    }
    standby_request_pending_ = true;
  }
  standby_worker_condition_.notify_one();
}

void JetsonH264EncoderImpl::RetireEncoder(
    std::unique_ptr<JetsonEncoder> encoder) {
  if (!encoder) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(standby_worker_mutex_);
    retired_encoders_.push_back(std::move(encoder));
  }
  standby_worker_condition_.notify_one();
}

void JetsonH264EncoderImpl::StandbyWorkerLoop() {
  while (true) {
    bool stop = false;
    bool create_standby = false;
    bool release_encoders = false;
    uint64_t release_sequence = 0;
    std::deque<std::unique_ptr<JetsonEncoder>> retired;
    {
      std::unique_lock<std::mutex> lock(standby_worker_mutex_);
      while (!standby_worker_stop_ && !standby_request_pending_ &&
             retired_encoders_.empty()) {
        if (deferred_release_pending_) {
          standby_worker_condition_.wait_until(
              lock, deferred_release_deadline_, [this]() {
                return standby_worker_stop_ || standby_request_pending_ ||
                       !retired_encoders_.empty() ||
                       !deferred_release_pending_;
              });
          if (deferred_release_pending_ &&
              std::chrono::steady_clock::now() >=
                  deferred_release_deadline_) {
            break;
          }
        } else {
          standby_worker_condition_.wait(lock, [this]() {
            return standby_worker_stop_ || standby_request_pending_ ||
                   !retired_encoders_.empty() || deferred_release_pending_;
          });
        }
      }
      stop = standby_worker_stop_;
      if (!stop) {
        create_standby = standby_request_pending_;
        if (deferred_release_pending_ &&
            std::chrono::steady_clock::now() >= deferred_release_deadline_) {
          release_encoders = true;
          release_sequence = deferred_release_sequence_;
          deferred_release_pending_ = false;
        }
      }
      standby_request_pending_ = false;
      retired.swap(retired_encoders_);
    }

    // 编码器销毁可能阻塞，必须在后台线程且不持有业务锁时执行。
    retired.clear();
    if (stop) {
      return;
    }

    if (release_encoders &&
        codec_released_.load(std::memory_order_acquire) &&
        release_sequence_.load(std::memory_order_acquire) ==
            release_sequence) {
      std::unique_ptr<JetsonEncoder> active;
      std::unique_ptr<JetsonEncoder> standby;
      {
        std::lock_guard<std::mutex> lock(encoder_mutex_);
        if (codec_released_.load(std::memory_order_acquire) &&
            release_sequence_.load(std::memory_order_acquire) ==
                release_sequence) {
          active = std::move(encoder_);
          standby = std::move(standby_encoder_);
          width_ = 0;
          height_ = 0;
          session_width_ = 0;
          session_height_ = 0;
        }
      }
      active.reset();
      standby.reset();
      LOG_INFO(
          "[JetsonEnc][release] Deferred hardware session cleanup complete");
    }
    if (!create_standby) {
      continue;
    }

    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int framerate = 0;
    unsigned int bitrate = 0;
    uint64_t epoch = 0;
    JetsonEncoder::StrategyConfig config;
    {
      std::lock_guard<std::mutex> lock(encoder_mutex_);
      if (standby_encoder_ || codec_released_.load(std::memory_order_acquire)) {
        continue;
      }
      width = session_width_;
      height = session_height_;
      framerate = hardware_framerate_;
      bitrate = bitrate_;
      epoch = session_epoch_.load(std::memory_order_acquire);
      config = BuildStrategyConfig();
    }

    auto standby =
        CreateEncoder(width, height, config, framerate, bitrate);
    if (!standby || !WarmupEncoder(standby.get(), width, height)) {
      LOG_WARN("[JetsonEnc][standby] Failed to prewarm encoder <%ux%u>",
               width, height);
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(encoder_mutex_);
      if (!standby_enabled_.load(std::memory_order_acquire) ||
          epoch != session_epoch_.load(std::memory_order_acquire) ||
          standby_encoder_) {
        continue;
      }
      standby_encoder_ = std::move(standby);
    }
    LOG_INFO("[JetsonEnc][standby] Prewarmed encoder <%ux%u>", width,
             height);
  }
}

std::pair<unsigned int, unsigned int>
JetsonH264EncoderImpl::ResolveSessionResolution(unsigned int width,
                                                unsigned int height) const {
  std::pair<unsigned int, unsigned int> result(width, height);
  if (!ShouldEnableStandby(width, height)) {
    return result;
  }

  uint64_t result_pixels = static_cast<uint64_t>(width) * height;
  for (const auto& limit : rtc_config_.resolution_limit) {
    if (limit.second.first <= 0 || limit.second.second <= 0) {
      continue;
    }
    const unsigned int candidate_width = static_cast<unsigned int>(
        std::min<long>(limit.second.first,
                       std::numeric_limits<unsigned int>::max()));
    const unsigned int candidate_height = static_cast<unsigned int>(
        std::min<long>(limit.second.second,
                       std::numeric_limits<unsigned int>::max()));
    if (!ShouldEnableStandby(candidate_width, candidate_height)) {
      continue;
    }
    const uint64_t candidate_pixels =
        static_cast<uint64_t>(candidate_width) * candidate_height;
    if (candidate_pixels > result_pixels) {
      result.first = candidate_width;
      result.second = candidate_height;
      result_pixels = candidate_pixels;
    }
  }
  return result;
}

JetsonH264EncoderImpl::EnsureEncoderResult
JetsonH264EncoderImpl::EnsureEncoderForResolution(unsigned int width,
                                                  unsigned int height) {
  std::lock_guard<std::mutex> lock(encoder_mutex_);

  if (encoder_ && !encoder_->IsHealthy()) {
    LOG_WARN("[JetsonEnc][reuse] Active encoder is unhealthy, recreate it");
    RetireEncoder(std::move(encoder_));
  }
  if (standby_encoder_ && !standby_encoder_->IsHealthy()) {
    LOG_WARN("[JetsonEnc][reuse] Standby encoder is unhealthy, recreate it");
    RetireEncoder(std::move(standby_encoder_));
  }

  if (encoder_ && encoder_->width() == static_cast<int>(width) &&
      encoder_->height() == static_cast<int>(height)) {
    ApplyRatesToEncoder(encoder_.get());
    return EnsureEncoderResult::kReady;
  }

  encoder_generation_.fetch_add(1, std::memory_order_acq_rel);
  const auto strategy_config = BuildStrategyConfig();

  if (!encoder_) {
    encoder_ = CreateEncoder(width, height, strategy_config,
                             hardware_framerate_, bitrate_);
    if (!encoder_) {
      LOG_ERROR("Failed to create Jetson encoder for <%ux%u>", width, height);
      return EnsureEncoderResult::kError;
    }
    ApplyRatesToEncoder(encoder_.get());
    encoder_->ForceKeyFrame();
    return EnsureEncoderResult::kReady;
  }

  const unsigned int active_width =
      encoder_ ? static_cast<unsigned int>(encoder_->width()) : 0;
  const unsigned int active_height =
      encoder_ ? static_cast<unsigned int>(encoder_->height()) : 0;
  const bool downscale = width <= active_width && height <= active_height;
  if (downscale) {
    encoder_->SetStrategyConfig(strategy_config);
    if (encoder_->Reconfigure(width, height)) {
      ApplyRatesToEncoder(encoder_.get());
      return EnsureEncoderResult::kReady;
    }
    LOG_WARN(
        "[JetsonEnc][switch] Active encoder downscale to <%ux%u> failed",
        width, height);
  }

  if (standby_encoder_) {
    standby_encoder_->SetStrategyConfig(strategy_config);
    bool standby_ready = true;
    if (standby_encoder_->width() != static_cast<int>(width) ||
        standby_encoder_->height() != static_cast<int>(height)) {
      standby_ready = standby_encoder_->Reconfigure(width, height);
    } else {
      standby_encoder_->ForceKeyFrame();
    }
    if (standby_ready) {
      ApplyRatesToEncoder(standby_encoder_.get());
      std::unique_ptr<JetsonEncoder> retired = std::move(encoder_);
      encoder_ = std::move(standby_encoder_);
      RetireEncoder(std::move(retired));
      RequestStandbyPrewarm();
      LOG_INFO("[JetsonEnc][switch] Switched to prewarmed encoder <%ux%u>",
               width, height);
      return EnsureEncoderResult::kReady;
    }
    LOG_WARN("[JetsonEnc][switch] Standby encoder DRC to <%ux%u> failed",
             width, height);
    RetireEncoder(std::move(standby_encoder_));
  }

  RequestStandbyPrewarm();
  if (!downscale && standby_enabled_.load(std::memory_order_acquire)) {
    LOG_WARN(
        "[JetsonEnc][switch] Standby encoder is not ready for upscale "
        "<%ux%u>, drop frame and wait for background prewarm",
        width, height);
    return EnsureEncoderResult::kPending;
  }

  std::unique_ptr<JetsonEncoder> replacement =
      CreateEncoder(width, height, strategy_config, hardware_framerate_,
                    bitrate_);
  if (!replacement) {
    return EnsureEncoderResult::kError;
  }
  RetireEncoder(std::move(encoder_));
  encoder_ = std::move(replacement);
  ApplyRatesToEncoder(encoder_.get());
  encoder_->ForceKeyFrame();
  RequestStandbyPrewarm();
  return EnsureEncoderResult::kReady;
}

void JetsonH264EncoderImpl::InitializeResolutionBitrateLimits() {
  resolution_bitrate_limits_.clear();

  const auto append_limit = [this](int frame_size_pixels,
                                   int min_start_bitrate_bps,
                                   int min_bitrate_bps,
                                   int max_bitrate_bps) {
    if (frame_size_pixels <= 0 || min_start_bitrate_bps <= 0 ||
        min_bitrate_bps <= 0 || max_bitrate_bps <= 0) {
      return;
    }

    const int configured_cap = static_cast<int>(std::min<unsigned int>(
        bitrate_cap_bps_, static_cast<unsigned int>(INT_MAX)));
    const int effective_max = std::min(max_bitrate_bps, configured_cap);
    const int effective_min = std::min(min_bitrate_bps, effective_max);
    const int effective_start = std::max(
        effective_min, std::min(min_start_bitrate_bps, effective_max));
    if (effective_max <= 0) {
      return;
    }

    for (const auto& limit : resolution_bitrate_limits_) {
      if (limit.frame_size_pixels == frame_size_pixels) {
        return;
      }
    }
    resolution_bitrate_limits_.emplace_back(
        frame_size_pixels, effective_start, effective_min, effective_max);
  };

  if (rtc_config_.use_strategy) {
    for (const auto& item : rtc_config_.strategy) {
      if (item.second.size() < 3) {
        continue;
      }
      append_limit(static_cast<int>(item.first),
                   static_cast<int>(item.second[0]),
                   static_cast<int>(item.second[1]),
                   static_cast<int>(item.second[2]));
    }
  }

  if (!resolution_bitrate_limits_.empty()) {
    return;
  }

  const auto append_resolution =
      [&append_limit](int width, int height, int min_start_bitrate_bps,
                      int min_bitrate_bps, int max_bitrate_bps) {
    append_limit(width * height, min_start_bitrate_bps, min_bitrate_bps,
                 max_bitrate_bps);
  };

  const int configured_adaptation_floor =
      rtc_config_.encode_params.bitrate_minmum > 0
          ? static_cast<int>(std::min<unsigned int>(
                rtc_config_.encode_params.bitrate_minmum,
                static_cast<unsigned int>(INT_MAX)))
          : 100000;
  const auto adaptive_min_bitrate = [configured_adaptation_floor](
                                        int quality_min_bitrate_bps) {
    return std::max(
        1, std::min(quality_min_bitrate_bps, configured_adaptation_floor));
  };

  // 升档门槛按动态道路画面 30 fps 标定。持续运行下限必须允许拥塞控制
  // 继续降低目标码率，否则网络在编码器后限速时 QP 不会上升，空间降档
  // 永远不会触发，发送端会持续产生远高于链路容量的码流。
  append_resolution(320, 180, 175000, adaptive_min_bitrate(100000), 450000);
  // 双目 16:10 QualityScaler 的实际低档会落到约 320x200、480x300 和
  // 720x450。WebRTC 按当前像素数的 5/3 查找升档门槛，不一定选择表中
  // 相邻项；每一档最大码率必须覆盖该门槛并留出余量，否则从极端弱网
  // 恢复后会长期停在低分辨率，即使 QP 已经显著低于升档阈值。
  append_resolution(320, 200, 175000, adaptive_min_bitrate(100000), 450000);
  append_resolution(480, 270, 400000, adaptive_min_bitrate(250000), 800000);
  append_resolution(480, 300, 350000, adaptive_min_bitrate(200000), 1200000);
  append_resolution(640, 360, 700000, adaptive_min_bitrate(450000), 1800000);
  append_resolution(720, 450, 1000000, adaptive_min_bitrate(600000),
                    2200000);
  append_resolution(960, 540, 1600000, adaptive_min_bitrate(1000000),
                    2500000);
  // 双目 16:10 输入需要独立阶梯，避免 960x600 直接跨到 1080p 门槛。
  append_resolution(960, 600, 1800000, adaptive_min_bitrate(1100000),
                    2800000);
  // 真实双目道路流在 1.0～1.6 Mbps、540p 下 QP 均值约为 23，允许在
  // 1.6 Mbps 以上尝试升到 720p；降档由实际 QP 和全局码率地板控制。
  append_resolution(1280, 720, 1600000, adaptive_min_bitrate(1200000),
                    6800000);
  append_resolution(1280, 800, 2400000, adaptive_min_bitrate(1600000),
                    6800000);
  append_resolution(1440, 900, 4200000, adaptive_min_bitrate(2800000),
                    7800000);
  append_resolution(1920, 1080, 6200000, adaptive_min_bitrate(3800000),
                    12000000);
  // 双目拼接源为 16:10，正常网络下允许恢复到摄像头原生 1920x1200。
  append_resolution(1920, 1200, 7000000, adaptive_min_bitrate(4200000),
                    12000000);

  for (const auto& codec_height : rtc_config_.encode_params.codecs) {
    if (codec_height == 0) {
      continue;
    }
    const unsigned int aligned_height = AlignToEven(codec_height);
    const unsigned int aligned_width = AlignToEven(static_cast<unsigned int>(
        (static_cast<uint64_t>(aligned_height) * 16 + 8) / 9));
    if (aligned_width >= 16 && aligned_height >= 16) {
      const uint64_t pixels =
          static_cast<uint64_t>(aligned_width) * aligned_height;
      const uint64_t min_bitrate = pixels * 30u * 6u / 100u;
      const uint64_t start_bitrate = pixels * 30u * 10u / 100u;
      const uint64_t max_bitrate = pixels * 30u * 16u / 100u;
      append_limit(
          static_cast<int>(std::min<uint64_t>(pixels, INT_MAX)),
          static_cast<int>(std::min<uint64_t>(start_bitrate, INT_MAX)),
          static_cast<int>(std::min<uint64_t>(min_bitrate, INT_MAX)),
          static_cast<int>(std::min<uint64_t>(max_bitrate, INT_MAX)));
    }
  }

  std::sort(resolution_bitrate_limits_.begin(),
            resolution_bitrate_limits_.end(),
            [](const ResolutionBitrateLimits& left,
               const ResolutionBitrateLimits& right) {
              return left.frame_size_pixels < right.frame_size_pixels;
            });
}

int JetsonH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
                                      const VideoEncoder::Settings& settings) {
  const auto init_start = std::chrono::steady_clock::now();
  LOG_INFO("[WebRTC] Init Jetson H264 encoder");

  ReportInit();

  if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->maxFramerate == 0) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  if (codec_settings->width < 1 || codec_settings->height < 1) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  auto num_of_streams =
      SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
  if (num_of_streams > 1) {
    return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
  }

  CancelDeferredRelease();

  VideoCodec next_codec = *codec_settings;
  if (next_codec.numberOfSimulcastStreams == 0) {
    next_codec.simulcastStream[0].width = next_codec.width;
    next_codec.simulcastStream[0].height = next_codec.height;
  }

  const unsigned int frame_width = next_codec.simulcastStream[0].width;
  const unsigned int frame_height = next_codec.simulcastStream[0].height;
  const bool enable_standby =
      ShouldEnableStandby(frame_width, frame_height);
  const auto session_resolution =
      ResolveSessionResolution(frame_width, frame_height);
  unsigned int previous_width = 0;
  unsigned int previous_height = 0;
  unsigned int previous_session_width = 0;
  unsigned int previous_session_height = 0;
  bool reuse_encoder = false;
  bool session_grows = false;
  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    reuse_encoder = static_cast<bool>(encoder_);
    previous_width = width_;
    previous_height = height_;
    previous_session_width = session_width_;
    previous_session_height = session_height_;
    if (reuse_encoder &&
        (frame_width > session_width_ || frame_height > session_height_)) {
      session_grows = true;
      session_width_ = std::max(session_width_, frame_width);
      session_height_ = std::max(session_height_, frame_height);
      session_epoch_.fetch_add(1, std::memory_order_acq_rel);
      RetireEncoder(std::move(standby_encoder_));
    }
  }

  if (!reuse_encoder) {
    session_epoch_.fetch_add(1, std::memory_order_acq_rel);
    StopStandbyWorker();
    encoder_generation_.fetch_add(1, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(encoder_mutex_);
      encoder_.reset();
      standby_encoder_.reset();
      session_width_ = session_resolution.first;
      session_height_ = session_resolution.second;
    }
  }

  standby_enabled_.store(enable_standby, std::memory_order_release);

  codec_ = next_codec;
  hardware_framerate_ = codec_.maxFramerate;
  max_payload_size_ = settings.max_payload_size;

  SimulcastRateAllocator init_allocator(codec_);
  auto allocation = init_allocator.Allocate(VideoBitrateAllocationParameters(
      DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
  SetRates(RateControlParameters(allocation, codec_.maxFramerate));

  EnsureEncoderResult ensure_result = EnsureEncoderResult::kPending;
  if (!reuse_encoder) {
    ensure_result = EnsureEncoderForResolution(frame_width, frame_height);
  } else {
    // WebRTC 在连接建立和带宽探测时可能在首帧前连续调用多次
    // Release()->InitEncode()。Jetson DRC 需要会话先实际产出过帧；这里
    // 只记录最终目标，等真实输入帧到达后一次性切换，避免连续空 DRC
    // 让硬件仍按旧分辨率解释新 Surface。
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (encoder_ && encoder_->IsHealthy() &&
        encoder_->width() == static_cast<int>(frame_width) &&
        encoder_->height() == static_cast<int>(frame_height)) {
      ApplyRatesToEncoder(encoder_.get());
      encoder_->ForceKeyFrame();
      ensure_result = EnsureEncoderResult::kReady;
    }
  }
  if (ensure_result == EnsureEncoderResult::kError) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, frame_width, frame_height);
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
    encoded_image_capacity_ = new_capacity;
    encoded_image_._completeFrame = true;
    encoded_image_._encodedWidth = frame_width;
    encoded_image_._encodedHeight = frame_height;
    encoded_image_.qp_ = -1;
    encoded_image_.set_size(0);
  }
  ResetQpStatistics();

  if (ensure_result == EnsureEncoderResult::kReady) {
    width_ = frame_width;
    height_ = frame_height;
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (encoder_) {
      // Release 后接收端可能已经丢失旧参考帧，重新初始化必须从 IDR 恢复。
      encoder_->ForceKeyFrame();
    }
  }

  if (!reuse_encoder) {
    // 辅助路也保留后台回收线程，避免分辨率变化时在编码线程同步销毁
    // Jetson 会话；standby_enabled_ 仅控制是否额外创建热备会话。
    StartStandbyWorker();
  }
  if (enable_standby) {
    RequestStandbyPrewarm();
  } else {
    LOG_INFO(
        "[JetsonEnc][standby] Disabled for auxiliary stream <%ux%u>",
        frame_width, frame_height);
  }

  const double init_elapsed_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          std::chrono::steady_clock::now() - init_start)
          .count();
  if (reuse_encoder) {
    LOG_INFO(
        "[JetsonEnc][init] reused old=%ux%u target=%ux%u "
        "session=%ux%u->%ux%u session_grows=%d ready=%d elapsed_ms=%.3f",
        previous_width, previous_height, frame_width, frame_height,
        previous_session_width, previous_session_height, session_width_,
        session_height_, session_grows ? 1 : 0,
        ensure_result == EnsureEncoderResult::kReady ? 1 : 0,
        init_elapsed_ms);
  } else {
    LOG_INFO(
        "[JetsonEnc][init] created target=%ux%u session=%ux%u "
        "elapsed_ms=%.3f",
        frame_width, frame_height, session_width_, session_height_,
        init_elapsed_ms);
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::Release() {
  session_epoch_.fetch_add(1, std::memory_order_acq_rel);
  standby_enabled_.store(false, std::memory_order_release);
  encoder_generation_.fetch_add(1, std::memory_order_release);
  ClearEncodeCallbackAndWait();
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);
    encoded_image_.ClearEncodedData();
    encoded_image_capacity_ = 0;
  }
  ResetQpStatistics();
  ScheduleDeferredRelease();

  LOG_INFO(
      "[JetsonEnc][release] Keep hardware sessions for %d ms reconfigure "
      "grace period",
      kDeferredReleaseGraceMs);

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  std::unique_lock<std::mutex> lock(callback_lifecycle_mutex_);
  if (g_active_callback_encoder != this) {
    callback_lifecycle_condition_.wait(
        lock, [this]() { return callback_inflight_ == 0; });
  }
  ++callback_epoch_;
  encoded_image_callback_.store(callback, std::memory_order_release);

  return WEBRTC_VIDEO_CODEC_OK;
}

void JetsonH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
  const double framerate_fps = parameters.framerate_fps;
  const uint64_t bitrate_bps = parameters.bitrate.get_sum_bps();
  if (!std::isfinite(framerate_fps) || framerate_fps < 1.0 ||
      bitrate_bps < 1) {
    LOG_WARN(
        "[WEBRTC] jetson SetRates failed because framerate or bitrate is "
        "invalid (fps=%.3f, bitrate=%llu)",
        framerate_fps, static_cast<unsigned long long>(bitrate_bps));
    return;
  }

  const uint32_t fps = static_cast<uint32_t>(std::min<double>(
      std::round(framerate_fps),
      static_cast<double>(std::numeric_limits<uint32_t>::max())));
  const uint32_t bitrate = static_cast<uint32_t>(std::min<uint64_t>(
      bitrate_bps, std::numeric_limits<uint32_t>::max()));

  std::lock_guard<std::mutex> lock(encoder_mutex_);
  codec_.maxFramerate = fps;
  codec_.maxBitrate = bitrate;

  const auto previous_fps = fps_;
  const auto previous_bitrate = bitrate_;
  fps_ = fps;
  bitrate_ =
      static_cast<unsigned int>(std::min<uint64_t>(bitrate, bitrate_cap_bps_));

  if (previous_fps == fps_ && previous_bitrate == bitrate_) {
    return;
  }

  const uint64_t bitrate_delta = previous_bitrate > bitrate_
                                     ? previous_bitrate - bitrate_
                                     : bitrate_ - previous_bitrate;
  if (previous_fps != fps_ || previous_bitrate == 0 ||
      bitrate_delta * 100u >= static_cast<uint64_t>(previous_bitrate) * 5u) {
    const int64_t callback_start_ms =
        callback_start_time_ms_.load(std::memory_order_acquire);
    const int64_t callback_inflight_ms =
        callback_start_ms > 0
            ? std::max<int64_t>(0, SteadyTimeMillis() - callback_start_ms)
            : 0;
    LOG_INFO(
        "[JetsonEnc][rates] target_bitrate=%u applied_bitrate=%u fps=%u "
        "callback_inflight_ms=%lld",
        bitrate, bitrate_, fps_,
        static_cast<long long>(callback_inflight_ms));
  }

  ApplyRatesToEncoder(encoder_.get());
  ApplyRatesToEncoder(standby_encoder_.get());

  {
    std::lock_guard<std::mutex> qp_lock(qp_stats_mutex_);
    qp_stats_.bitrate_bps = bitrate_;
    qp_stats_.fps = fps_;
  }
}

int32_t JetsonH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_.load(std::memory_order_acquire)) {
    LOG_ERROR(
        "InitEncode() has been called, but a callback function "
        "has not been set with RegisterEncodeCompleteCallback()");
    ReportError();
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  const bool has_frame_type = frame_types && !frame_types->empty();
  if (has_frame_type && (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
    return WEBRTC_VIDEO_CODEC_OK;
  }

  auto frame_buffer = input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const unsigned int frame_width = frame_buffer->width();
  const unsigned int frame_height = frame_buffer->height();
  const bool resolution_changed =
      (frame_width != width_ || frame_height != height_);

  unsigned int previous_session_width = 0;
  unsigned int previous_session_height = 0;
  unsigned int expanded_session_width = 0;
  unsigned int expanded_session_height = 0;
  bool session_grows = false;
  std::unique_ptr<JetsonEncoder> incompatible_standby;
  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (encoder_ &&
        (frame_width > session_width_ || frame_height > session_height_)) {
      previous_session_width = session_width_;
      previous_session_height = session_height_;
      session_width_ = std::max(session_width_, frame_width);
      session_height_ = std::max(session_height_, frame_height);
      expanded_session_width = session_width_;
      expanded_session_height = session_height_;
      session_epoch_.fetch_add(1, std::memory_order_acq_rel);
      incompatible_standby = std::move(standby_encoder_);
      session_grows = true;
    }
  }
  if (incompatible_standby) {
    RetireEncoder(std::move(incompatible_standby));
  }
  if (session_grows) {
    LOG_INFO(
        "[JetsonEnc][session] Encode expanded session <%ux%u> -> <%ux%u> "
        "for frame <%ux%u>",
        previous_session_width, previous_session_height,
        expanded_session_width, expanded_session_height, frame_width,
        frame_height);
    RequestStandbyPrewarm();
  }

  const EnsureEncoderResult ensure_result =
      EnsureEncoderForResolution(frame_width, frame_height);
  if (ensure_result == EnsureEncoderResult::kPending) {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  if (ensure_result != EnsureEncoderResult::kReady) {
    ReportError();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (resolution_changed) {
    width_ = frame_width;
    height_ = frame_height;
    const size_t new_capacity =
        CalcBufferSize(VideoType::kI420, width_, height_);
    {
      std::lock_guard<std::mutex> lock(encoded_image_mutex_);
      encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
      encoded_image_capacity_ = new_capacity;
      encoded_image_._encodedWidth = width_;
      encoded_image_._encodedHeight = height_;
      encoded_image_.qp_ = -1;
      encoded_image_.set_size(0);
    }
    ResetQpStatistics();
  }

  const bool request_keyframe =
      has_frame_type && (*frame_types)[0] == VideoFrameType::kVideoFrameKey;

#if ENABLE_ENCODE_PERF_STATS
  auto encode_start_time = std::chrono::steady_clock::now();
#endif

  {
    std::lock_guard<std::mutex> lock(encoder_mutex_);
    if (!encoder_) {
      ReportError();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    if (request_keyframe) {
      encoder_->ForceKeyFrame();
    }

    const uint64_t encode_generation =
        encoder_generation_.load(std::memory_order_acquire);

    encoder_->EmplaceBuffer(
        frame_buffer,
#if ENABLE_ENCODE_PERF_STATS
        [this, input_frame, encode_start_time,
         encode_generation](const uint8_t* data, size_t size, bool is_keyframe,
                            uint64_t timestamp) {
          if (encode_generation !=
              encoder_generation_.load(std::memory_order_acquire)) {
            return;
          }

          auto encode_end_time = std::chrono::steady_clock::now();
          int64_t encode_duration_us =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  encode_end_time - encode_start_time)
                  .count();

          SendFrame(input_frame, data, size, is_keyframe, encode_duration_us);
        });
#else
        [this, input_frame, encode_generation](const uint8_t* data, size_t size,
                                               bool is_keyframe,
                                               uint64_t timestamp) {
          if (encode_generation !=
              encoder_generation_.load(std::memory_order_acquire)) {
            return;
          }

          SendFrame(input_frame, data, size, is_keyframe, 0);
        });
#endif
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

void JetsonH264EncoderImpl::SendFrame(const VideoFrame& frame,
                                      const uint8_t* data, size_t size,
                                      bool is_keyframe,
                                      int64_t encode_duration_us) {
  if (size == 0) {
    return;
  }

#if ENABLE_ENCODE_PERF_STATS
  if (encode_duration_us > 0) {
    encode_stats_.total_encode_time_us += encode_duration_us;
    encode_stats_.max_encode_time_us =
        std::max(encode_stats_.max_encode_time_us, encode_duration_us);
    encode_stats_.min_encode_time_us =
        std::min(encode_stats_.min_encode_time_us, encode_duration_us);
    encode_stats_.frame_count++;
    if (is_keyframe) {
      encode_stats_.keyframe_count++;
    }

    LOG_INFO(
        "[编码性能] 帧编码耗时: %ld us (%.2f ms), 大小: %zu bytes, 关键帧: %s",
        encode_duration_us, encode_duration_us / 1000.0f, size,
        is_keyframe ? "是" : "否");

    auto now = std::chrono::steady_clock::now();
    bool should_log_stats = false;
    if (encode_stats_.frame_count == 1) {
      encode_stats_.last_log_time = now;
      should_log_stats = false;
    } else {
      auto time_since_last_log =
          std::chrono::duration_cast<std::chrono::seconds>(
              now - encode_stats_.last_log_time)
              .count();
      if (encode_stats_.frame_count % 100 == 0 || time_since_last_log >= 5) {
        should_log_stats = true;
        encode_stats_.last_log_time = now;
      }
    }

    if (should_log_stats && encode_stats_.frame_count > 0) {
      int64_t avg_encode_time_us =
          encode_stats_.total_encode_time_us / encode_stats_.frame_count;
      float fps = codec_.maxFramerate;
      float frame_budget_ms = 1000.0f / fps;
      float utilization =
          (avg_encode_time_us / 1000.0f) / frame_budget_ms * 100.0f;

      LOG_INFO(
          "[编码性能统计] 总帧数: %u, 关键帧数: %u, 平均耗时: %ld us (%.2f "
          "ms), "
          "最大耗时: %ld us (%.2f ms), 最小耗时: %ld us (%.2f ms), "
          "帧率: %.1f fps, 时间预算: %.2f ms/帧, 占用率: %.1f%%",
          encode_stats_.frame_count, encode_stats_.keyframe_count,
          avg_encode_time_us, avg_encode_time_us / 1000.0f,
          encode_stats_.max_encode_time_us,
          encode_stats_.max_encode_time_us / 1000.0f,
          encode_stats_.min_encode_time_us == INT64_MAX
              ? 0
              : encode_stats_.min_encode_time_us,
          encode_stats_.min_encode_time_us == INT64_MAX
              ? 0.0f
              : encode_stats_.min_encode_time_us / 1000.0f,
          fps, frame_budget_ms, utilization);

      if (utilization > 80.0f) {
        LOG_WARN(
            "[编码性能警告] 编码耗时占用率过高 (%.1f%%), "
            "可能影响实时性能。建议："
            "1) 降低分辨率或帧率 2) 检查格式转换耗时 3) "
            "确认setMaxPerfMode已启用",
            utilization);
      } else if (utilization > 60.0f) {
        LOG_WARN("[编码性能提示] 编码耗时占用率较高 (%.1f%%), 建议监控性能",
                 utilization);
      } else if (utilization < 30.0f && avg_encode_time_us < 5000) {
        LOG_INFO(
            "[编码性能] ✓ 编码性能优秀！耗时: %.2f ms, 占用率: %.1f%%, "
            "有充足的时间余量",
            avg_encode_time_us / 1000.0f, utilization);
      } else if (utilization < 50.0f) {
        LOG_INFO("[编码性能] ✓ 编码性能良好，占用率: %.1f%%", utilization);
      }
    }
  }
#endif

  auto nalu_indices = H264::FindNaluIndices(data, size);
  if (nalu_indices.empty()) {
    return;
  }

  VideoFrameType frame_type = VideoFrameType::kVideoFrameDelta;
  if (is_keyframe) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x07) {
    frame_type = VideoFrameType::kVideoFrameKey;
  } else if (size > 4 && (data[4] & 0x1f) == 0x01) {
    frame_type = VideoFrameType::kVideoFrameDelta;
  }

  RTPFragmentationHeader frag_header;
  frag_header.VerifyAndAllocateFragmentationHeader(nalu_indices.size());
  for (size_t i = 0; i < nalu_indices.size(); ++i) {
    frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
    frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
  }

  CodecSpecificInfo codec_specific;
  codec_specific.codecType = kVideoCodecH264;
  codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
  codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
  codec_specific.codecSpecific.H264.idr_frame =
      (frame_type == VideoFrameType::kVideoFrameKey);
  codec_specific.codecSpecific.H264.base_layer_sync = false;

  EncodedImage callback_image;
  EncodedImageCallback* callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(encoded_image_mutex_);

    if (size > encoded_image_capacity_) {
      encoded_image_.SetEncodedData(EncodedImageBuffer::Create(size));
      encoded_image_capacity_ = size;
    }

    encoded_image_._completeFrame = true;
    encoded_image_._encodedWidth = frame.width();
    encoded_image_._encodedHeight = frame.height();
    encoded_image_.set_size(size);
    if (has_configured_playout_delay_) {
      encoded_image_.playout_delay_.min_ms = configured_playout_delay_min_ms_;
      encoded_image_.playout_delay_.max_ms = configured_playout_delay_max_ms_;
    }
    encoded_image_.SetTimestamp(frame.timestamp());
    encoded_image_.ntp_time_ms_ = frame.ntp_time_ms();
    encoded_image_.capture_time_ms_ = frame.render_time_ms();
    encoded_image_.rotation_ = frame.rotation();
    encoded_image_.SetColorSpace(frame.color_space());
    encoded_image_.content_type_ = codec_.mode == VideoCodecMode::kScreensharing
                                       ? VideoContentType::SCREENSHARE
                                       : VideoContentType::UNSPECIFIED;
    encoded_image_.SetSpatialIndex(0);
    encoded_image_._frameType = frame_type;

    memcpy(encoded_image_.data(), data, size);

    encoded_image_.qp_ = -1;
    h264_bitstream_parser_.ParseBitstream(encoded_image_.data(),
                                          encoded_image_.size());
    auto qp = h264_bitstream_parser_.GetLastSliceQp();
    if (qp.has_value()) {
      encoded_image_.qp_ = qp.value();
    }
    RecordQp(encoded_image_.qp_, frame.width(), frame.height());

    // WebRTC 可能在编码完成回调中触发质量自适应和 InitEncode()。外部回调
    // 不得持有 encoded_image_mutex_，否则分辨率升档时会与编码队列互锁。
    callback_image = encoded_image_;
  }

  callback = BeginEncodeCallback();
  if (!callback) {
    return;
  }

  callback_start_time_ms_.store(SteadyTimeMillis(), std::memory_order_release);
  const JetsonH264EncoderImpl* previous_callback_encoder =
      g_active_callback_encoder;
  g_active_callback_encoder = this;
  const EncodedImageCallback::Result result =
      callback->OnEncodedImage(callback_image, &codec_specific, &frag_header);
  g_active_callback_encoder = previous_callback_encoder;
  callback_start_time_ms_.store(0, std::memory_order_release);
  EndEncodeCallback();
  if (result.error != EncodedImageCallback::Result::OK) {
    LOG_WARN("[JetsonEnc][callback] failed to deliver encoded frame error=%d",
             static_cast<int>(result.error));
  }
}

VideoEncoder::EncoderInfo JetsonH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "JetsonH264";
  info.scaling_settings = VideoEncoder::ScalingSettings(
      static_cast<int>(qp_threshold_.first),
      static_cast<int>(qp_threshold_.second), 320 * 180);
  // Jetson CBR 在极端码率变化时不保证主动丢帧并快速贴合目标值，保留
  // WebRTC 通用帧丢弃器，避免 pacer 队列持续积压并放大弱网延迟。
  info.has_trusted_rate_controller = false;
  info.is_hardware_accelerated = true;
  info.has_internal_source = false;
  info.supports_simulcast = false;
  info.requested_resolution_alignment = 2;
  info.resolution_bitrate_limits = resolution_bitrate_limits_;

  return info;
}

void JetsonH264EncoderImpl::SetFecControllerOverride(
    FecControllerOverride* fec_controller_override) {}

void JetsonH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {
  /* LOG_INFO("[WEBRTC] OnPacketLossRateUpdate: %f", packet_loss_rate); */
}

void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
  /* LOG_INFO("[WEBRTC] OnRttUpdate: %d", rtt_ms); */
}

void JetsonH264EncoderImpl::OnLossNotification(
    const LossNotification& loss_notification) {
  // auto delta = loss_notification.timestamp_of_last_decodable -
  //     loss_notification.timestamp_of_last_received;
  // LOG_INFO("[WEBRTC] OnLossNotification timestamp between"
  //     "last decodable and last received frame: %d", delta);
}

void JetsonH264EncoderImpl::ReportInit() {
  if (has_reported_init_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

  has_reported_init_ = true;
}

void JetsonH264EncoderImpl::ReportError() {
  if (has_reported_error_) {
    return;
  }

  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.Video.H264EncoderImpl.Event",
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventError),
      static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

  has_reported_error_ = true;
}

}  // namespace webrtc
