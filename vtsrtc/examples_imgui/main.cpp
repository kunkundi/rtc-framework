#include "c_rtc.h"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl3.h>

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <alsa/asoundlib.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kDataChannelLabel = "datachannel";
constexpr const char* kExternalAudioSource = "external_audio";
constexpr const char* kExternalVideoSource = "merged_image";
constexpr int kMainWindowWidth = 1060;
constexpr int kMainWindowHeight = 910;
constexpr float kPanelLeft = 16.0f;
constexpr float kPanelWidth = 1028.0f;
constexpr float kPanelTop = 16.0f;
constexpr float kPanelGap = 10.0f;
constexpr float kControlPanelHeight = 260.0f;
constexpr float kVideoPanelHeight = 590.0f;
constexpr float kStatsPanelHeight = 120.0f;
constexpr float kLogPanelHeight = 90.0f;
constexpr float kControlPanelTop = kPanelTop;
constexpr float kVideoPanelTop = kControlPanelTop + kControlPanelHeight + kPanelGap;
constexpr float kStatsPanelTop = kVideoPanelTop + kVideoPanelHeight + kPanelGap;
constexpr float kLogPanelTopWithStats = kStatsPanelTop + kStatsPanelHeight + kPanelGap;
constexpr float kLogPanelTopWithoutStats = kVideoPanelTop + kVideoPanelHeight + kPanelGap;
constexpr float kVideoCardPadding = 14.0f;
constexpr float kVideoOverlayPadding = 8.0f;
constexpr float kFullscreenOverlayPadding = 20.0f;

struct NetStatsView {
  bool input = false;
  std::string source_id;
  unsigned long bitrate_bps = 0;
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int fps = 0;
  unsigned int loss_rate = 0;
  unsigned int delay_ms = 0;
  unsigned int key_frame_count = 0;
  unsigned int fir_count = 0;
  unsigned int pli_count = 0;
  unsigned int nack_count = 0;
  std::string codec_name;
};

struct VideoFrameView {
  std::string stream_key;
  std::string source_id;
  RtcSessionId remote_sessionid = 0;
  RtcMediaSourceType source_type = RtcMediaSourceType::Rtc;
  size_t width = 0;
  size_t height = 0;
  size_t dimension = 0;
  uint64_t frame_seq = 0;
  std::vector<unsigned char> buffer;
};

struct VideoTextureView {
  GLuint texture = 0;
  int width = 0;
  int height = 0;
  uint64_t uploaded_frame_seq = 0;
  bool uploaded_with_lr_interleave = false;
  std::vector<unsigned char> remapped_buffer;
};

size_t ScaleIndexNearest(size_t dst_index, size_t dst_count, size_t src_count) {
  if (src_count <= 1 || dst_count <= 1) {
    return 0;
  }

  const uint64_t numerator =
      static_cast<uint64_t>(dst_index) * static_cast<uint64_t>(src_count - 1);
  const uint64_t denominator = static_cast<uint64_t>(dst_count - 1);
  return static_cast<size_t>((numerator + denominator / 2) / denominator);
}

bool RasterizeFrameToRenderedHorizontalInterleave(
    const VideoFrameView& frame,
    size_t output_width,
    size_t output_height,
    std::vector<unsigned char>* output) {
  if (!output || output_width == 0 || output_height == 0 || frame.width < 2 ||
      (frame.width % 2) != 0 || frame.height == 0 || frame.dimension == 0) {
    return false;
  }

  const size_t frame_bytes = frame.width * frame.height * frame.dimension;
  if (frame.buffer.size() < frame_bytes) {
    return false;
  }

  const size_t half_width = frame.width / 2;
  const size_t pixel_bytes = frame.dimension;
  const size_t src_row_bytes = frame.width * pixel_bytes;
  const size_t dst_row_bytes = output_width * pixel_bytes;
  const size_t left_output_columns = (output_width + 1) / 2;
  const size_t right_output_columns = output_width / 2;
  output->resize(output_width * output_height * pixel_bytes);

  for (size_t y = 0; y < output_height; ++y) {
    const size_t src_y = ScaleIndexNearest(y, output_height, frame.height);
    const unsigned char* src_row = frame.buffer.data() + src_y * src_row_bytes;
    const unsigned char* left_row = src_row;
    const unsigned char* right_row = src_row + half_width * pixel_bytes;
    unsigned char* dst_row = output->data() + y * dst_row_bytes;

    for (size_t x = 0; x < output_width; ++x) {
      const bool use_left = (x % 2) == 0;
      const size_t half_index = x / 2;
      const size_t src_x = ScaleIndexNearest(
          half_index, use_left ? left_output_columns : right_output_columns,
          half_width);
      const unsigned char* src_pixel =
          (use_left ? left_row : right_row) + src_x * pixel_bytes;
      std::memcpy(dst_row + x * pixel_bytes, src_pixel, pixel_bytes);
    }
  }

  return true;
}

std::string JoinPath(const std::string& base, const std::string& leaf) {
  if (base.empty()) {
    return leaf;
  }
  if (base.back() == '/' || base.back() == '\\') {
    return base + leaf;
  }
#if defined(_WIN32)
  return base + "\\" + leaf;
#else
  return base + "/" + leaf;
#endif
}

bool FileExists(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return f.good();
}

std::string ExecutableDir() {
#if defined(_WIN32)
  char path[MAX_PATH] = {0};
  const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (len == 0 || len == MAX_PATH) {
    return ".";
  }
  std::string s(path, len);
  const size_t pos = s.find_last_of("\\/");
  if (pos == std::string::npos) {
    return ".";
  }
  return s.substr(0, pos);
#else
  char path[4096] = {0};
  const ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len <= 0) {
    return ".";
  }
  path[len] = '\0';
  std::string s(path);
  const size_t pos = s.find_last_of('/');
  if (pos == std::string::npos) {
    return ".";
  }
  return s.substr(0, pos);
#endif
}

const char* ServerStateText(RtcServerConnectionState state) {
  switch (state) {
    case ServerConnecting:
      return "Connecting";
    case ServerConnected:
      return "Connected";
    case ServerLogined:
      return "Logined";
    case ServerDisconnected:
      return "Disconnected";
    case ServerReconnecting:
      return "Reconnecting";
    default:
      return "Unknown";
  }
}

const char* P2PStateText(RtcP2PState state) {
  switch (state) {
    case P2PNew:
      return "New";
    case P2PConnecting:
      return "Connecting";
    case P2PConnected:
      return "Connected";
    case P2PDisconnected:
      return "Disconnected";
    case P2PFailed:
      return "Failed";
    case P2PClosed:
      return "Closed";
    default:
      return "Unknown";
  }
}

class RtcAudioPlayer {
 public:
  RtcAudioPlayer() {
    worker_ = std::thread([this]() { PlaybackLoop(); });
  }

  ~RtcAudioPlayer() {
    stop_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    CloseDevice();
  }

  void PushFrame(size_t bits_per_sample,
                 size_t sample_rate,
                 size_t number_of_channels,
                 const void* audio_data,
                 size_t sz_audio_data) {
    if (!audio_data || sz_audio_data == 0 || bits_per_sample == 0 ||
        number_of_channels == 0 || sample_rate == 0) {
      return;
    }

    AudioPacket packet;
    packet.bits_per_sample = bits_per_sample;
    packet.sample_rate = sample_rate;
    packet.number_of_channels = number_of_channels;
    const char* begin = static_cast<const char*>(audio_data);
    packet.data.assign(begin, begin + sz_audio_data);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.size() >= kMaxQueuedPackets) {
        queue_.pop_front();
      }
      queue_.push_back(std::move(packet));
    }
    cv_.notify_one();
  }

  void Clear() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.clear();
    }
#if defined(__linux__)
    if (pcm_device_) {
      snd_pcm_drop(pcm_device_);
      snd_pcm_prepare(pcm_device_);
    }
#endif
  }

 private:
  struct AudioPacket {
    size_t bits_per_sample = 0;
    size_t sample_rate = 0;
    size_t number_of_channels = 0;
    std::vector<char> data;
  };

  void PlaybackLoop() {
    while (!stop_.load()) {
      AudioPacket packet;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock,
                 [this]() { return stop_.load() || !queue_.empty(); });
        if (stop_.load()) {
          break;
        }
        packet = std::move(queue_.front());
        queue_.pop_front();
      }
      PlayPacket(packet);
    }
  }

  void PlayPacket(const AudioPacket& packet) {
    if (packet.data.empty()) {
      return;
    }

#if defined(__linux__)
    if (!EnsureDevice(packet.bits_per_sample, packet.sample_rate,
                      packet.number_of_channels)) {
      return;
    }

    const size_t bytes_per_sample = packet.bits_per_sample / 8;
    const size_t bytes_per_frame = bytes_per_sample * packet.number_of_channels;
    if (bytes_per_frame == 0) {
      return;
    }

    const snd_pcm_uframes_t total_frames =
        static_cast<snd_pcm_uframes_t>(packet.data.size() / bytes_per_frame);
    snd_pcm_uframes_t sent_frames = 0;
    const char* data_ptr = packet.data.data();

    while (!stop_.load() && sent_frames < total_frames) {
      const snd_pcm_sframes_t written = snd_pcm_writei(
          pcm_device_, data_ptr + sent_frames * bytes_per_frame,
          total_frames - sent_frames);
      if (written > 0) {
        sent_frames += static_cast<snd_pcm_uframes_t>(written);
        continue;
      }
      if (written == -EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }

      const int recovered =
          snd_pcm_recover(pcm_device_, static_cast<int>(written), 1);
      if (recovered < 0) {
        snd_pcm_prepare(pcm_device_);
        break;
      }
    }
#else
    (void)packet;
#endif
  }

#if defined(__linux__)
  snd_pcm_format_t BitsToFormat(size_t bits_per_sample) {
    switch (bits_per_sample) {
      case 8:
        return SND_PCM_FORMAT_S8;
      case 16:
        return SND_PCM_FORMAT_S16_LE;
      case 24:
        return SND_PCM_FORMAT_S24_LE;
      case 32:
        return SND_PCM_FORMAT_S32_LE;
      default:
        return SND_PCM_FORMAT_UNKNOWN;
    }
  }

  bool EnsureDevice(size_t bits_per_sample,
                    size_t sample_rate,
                    size_t number_of_channels) {
    if (pcm_device_ && bits_per_sample == configured_bits_per_sample_ &&
        sample_rate == configured_sample_rate_ &&
        number_of_channels == configured_channels_) {
      return true;
    }

    CloseDevice();

    const snd_pcm_format_t format = BitsToFormat(bits_per_sample);
    if (format == SND_PCM_FORMAT_UNKNOWN) {
      return false;
    }

    if (snd_pcm_open(&pcm_device_, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
      pcm_device_ = nullptr;
      return false;
    }

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    if (snd_pcm_hw_params_any(pcm_device_, hw_params) < 0) {
      CloseDevice();
      return false;
    }
    if (snd_pcm_hw_params_set_access(pcm_device_, hw_params,
                                     SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
      CloseDevice();
      return false;
    }
    if (snd_pcm_hw_params_set_format(pcm_device_, hw_params, format) < 0) {
      CloseDevice();
      return false;
    }
    if (snd_pcm_hw_params_set_channels(
            pcm_device_, hw_params,
            static_cast<unsigned int>(number_of_channels)) < 0) {
      CloseDevice();
      return false;
    }

    unsigned int rate = static_cast<unsigned int>(sample_rate);
    if (snd_pcm_hw_params_set_rate_near(pcm_device_, hw_params, &rate, nullptr) <
        0) {
      CloseDevice();
      return false;
    }

    snd_pcm_uframes_t period_size = rate / 100;
    if (period_size < 80) {
      period_size = 80;
    }
    snd_pcm_hw_params_set_period_size_near(pcm_device_, hw_params, &period_size,
                                           nullptr);
    snd_pcm_uframes_t buffer_size = period_size * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm_device_, hw_params, &buffer_size);

    if (snd_pcm_hw_params(pcm_device_, hw_params) < 0) {
      CloseDevice();
      return false;
    }
    if (snd_pcm_prepare(pcm_device_) < 0) {
      CloseDevice();
      return false;
    }

    configured_bits_per_sample_ = bits_per_sample;
    configured_sample_rate_ = sample_rate;
    configured_channels_ = number_of_channels;
    return true;
  }
#endif

  void CloseDevice() {
#if defined(__linux__)
    if (pcm_device_) {
      snd_pcm_drop(pcm_device_);
      snd_pcm_close(pcm_device_);
      pcm_device_ = nullptr;
    }
    configured_bits_per_sample_ = 0;
    configured_sample_rate_ = 0;
    configured_channels_ = 0;
#endif
  }

  static constexpr size_t kMaxQueuedPackets = 200;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<AudioPacket> queue_;
  std::thread worker_;
  std::atomic<bool> stop_{false};

#if defined(__linux__)
  snd_pcm_t* pcm_device_ = nullptr;
  size_t configured_bits_per_sample_ = 0;
  size_t configured_sample_rate_ = 0;
  size_t configured_channels_ = 0;
#endif
};

class SDLOpenGLWindow {
 public:
  bool Init(const char* title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      return false;
    }
    initialized_ = true;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window_ = SDL_CreateWindow(title, width, height,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window_) {
      SDL_Quit();
      initialized_ = false;
      return false;
    }
    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_Quit();
      initialized_ = false;
      return false;
    }

    if (!SDL_GL_MakeCurrent(window_, gl_context_)) {
      SDL_GL_DestroyContext(gl_context_);
      gl_context_ = nullptr;
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_Quit();
      initialized_ = false;
      return false;
    }
    SDL_GL_SetSwapInterval(1);

    width_ = width;
    height_ = height;
    return true;
  }

  void Shutdown() {
    if (gl_context_) {
      SDL_GL_DestroyContext(gl_context_);
      gl_context_ = nullptr;
    }
    if (window_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    if (initialized_) {
      SDL_Quit();
      initialized_ = false;
    }
  }

  void PumpEvents(ImGuiIO& /*io*/, bool* should_close) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        *should_close = true;
      } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                 event.window.windowID == SDL_GetWindowID(window_)) {
        *should_close = true;
      }
    }
    SDL_GetWindowSize(window_, &width_, &height_);
  }

  void SwapBuffers() { SDL_GL_SwapWindow(window_); }

  bool SetFullscreen(bool fullscreen) {
    if (!window_) {
      return false;
    }
    if (!SDL_SetWindowFullscreen(window_, fullscreen)) {
      return false;
    }
    return SDL_SyncWindow(window_);
  }

  bool IsFullscreen() const {
    if (!window_) {
      return false;
    }
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
  }

  int width() const { return width_; }
  int height() const { return height_; }
  SDL_Window* window() const { return window_; }
  SDL_GLContext gl_context() const { return gl_context_; }

 private:
  SDL_Window* window_ = nullptr;
  SDL_GLContext gl_context_ = nullptr;
  bool initialized_ = false;
  int width_ = 0;
  int height_ = 0;
};

class RtcImguiApp {
 public:
  RtcImguiApp() {
    instance_ = this;
    std::memset(open_room_id_, 0, sizeof(open_room_id_));
    std::memset(srs_url_, 0, sizeof(srs_url_));
    std::memset(send_msg_, 0, sizeof(send_msg_));

    std::snprintf(open_room_id_, sizeof(open_room_id_), "%s", "zhejianglab");
    std::snprintf(srs_url_, sizeof(srs_url_), "%s",
                  "webrtc://47.96.251.52/AR/livestream");
    std::snprintf(send_msg_, sizeof(send_msg_), "%s", "hello world");
  }

  ~RtcImguiApp() {
    StopMediaFeed();
    DestroyRtc();
    FreeMediaBuffers();
    instance_ = nullptr;
  }

  bool Init() {
    ResolveAssetPaths();
    if (!InitRtc()) {
      return false;
    }
    return true;
  }

  void Run() {
    SDLOpenGLWindow window;
    if (!window.Init("rtc-solutions | p2p_imgui", kMainWindowWidth,
                     kMainWindowHeight)) {
      AppendLog("SDLOpenGLWindow init failed");
      return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window.window(), window.gl_context())) {
      AppendLog("ImGui_ImplSDL3_InitForOpenGL failed");
      ImGui::DestroyContext();
      window.Shutdown();
      return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
      AppendLog("ImGui_ImplOpenGL3_Init failed");
      ImGui_ImplSDL3_Shutdown();
      ImGui::DestroyContext();
      window.Shutdown();
      return;
    }

    bool should_close = false;
    auto last_tick = std::chrono::steady_clock::now();
    ui_window_ = &window;

    while (!should_close) {
      const auto now = std::chrono::steady_clock::now();
      float delta = std::chrono::duration<float>(now - last_tick).count();
      last_tick = now;
      if (delta <= 0.0f) {
        delta = 1.0f / 60.0f;
      }

      io.DisplaySize = ImVec2(static_cast<float>(window.width()),
                              static_cast<float>(window.height()));
      io.DeltaTime = delta;

      window.PumpEvents(io, &should_close);

      ImGui_ImplSDL3_NewFrame();
      ImGui_ImplOpenGL3_NewFrame();
      ImGui::NewFrame();

      DrawUi();

      ImGui::Render();
      glViewport(0, 0, window.width(), window.height());
      glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      window.SwapBuffers();

      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ReleaseVideoTextures();
    ui_window_ = nullptr;
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    window.Shutdown();
  }

 private:
  bool InitRtc() {
    RtcInitParams params;
    std::memset(&params, 0, sizeof(params));
    params.config_filepath = rtc_cfg_path_.c_str();
    params.room_handler = &RtcImguiApp::OnRoom;
    params.P2P_state_handler = &RtcImguiApp::OnP2PState;
    params.datachannel_state_handler = &RtcImguiApp::OnDataChannelState;
    params.serverconnection_state_handler = &RtcImguiApp::OnServerConnectionState;
    params.SRS_state_handler = &RtcImguiApp::OnSRSState;
    params.SRS_response_handler = &RtcImguiApp::OnSRSResponse;
    params.recv_msg_handler = &RtcImguiApp::OnRecvMessage;
    params.recv_audioframe_handler = &RtcImguiApp::OnRecvAudioFrame;
    params.recv_frame_handler = &RtcImguiApp::OnRecvFrame;
    params.channel_network_stats_handler = &RtcImguiApp::OnChannelNetworkStats;

    const RtcErrorCode init_code = RtcInitAgentV2(params);
    if (init_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcInitAgentV2", init_code);
      return false;
    }
    AppendLog("RtcInitAgentV2 success");

    const RtcErrorCode dc_code =
        RtcAddDataChannel(kDataChannelLabel, RtcPriorityType::High, true, -1);
    if (dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel", dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel success");

    rtc_inited_ = true;
    LoadVideoSourceList();
    return true;
  }

  void DestroyRtc() {
    if (rtc_inited_) {
      RtcDestoryAgent();
      rtc_inited_ = false;
    }
  }

  void ResolveAssetPaths() {
    const std::string exe_dir = ExecutableDir();
    std::vector<std::string> base_candidates;
    base_candidates.push_back(".");
    base_candidates.push_back("test_data");

    std::string current = exe_dir;
    for (int i = 0; i < 5; ++i) {
      base_candidates.push_back(current);
      current = JoinPath(current, "..");
    }

    rtc_cfg_path_ = FindAsset("rtc.cfg", base_candidates);
    pcm_path_ = FindAsset("8k16bit.pcm", base_candidates);
    yuv_path_ = FindAsset("zjlabs.yuv", base_candidates);
    message_file_path_ = FindAsset("messagefile.txt", base_candidates);

    AppendLog(std::string("rtc.cfg: ") + rtc_cfg_path_);
    AppendLog(std::string("8k16bit.pcm: ") + pcm_path_);
    AppendLog(std::string("zjlabs.yuv: ") + yuv_path_);
    AppendLog(std::string("messagefile.txt: ") + message_file_path_);
  }

  std::string FindAsset(const std::string& name,
                        const std::vector<std::string>& base_candidates) {
    for (const std::string& base : base_candidates) {
      if (base.empty()) {
        continue;
      }

      const std::string candidate_direct = JoinPath(base, name);
      if (FileExists(candidate_direct)) {
        return candidate_direct;
      }

      const std::string candidate_test_data =
          JoinPath(JoinPath(base, "test_data"), name);
      if (FileExists(candidate_test_data)) {
        return candidate_test_data;
      }
    }
    return name;
  }

  void LoadVideoSourceList() {
    video_sources_.clear();
    video_sources_.push_back("Local YUV420p");

    RtcVideoDevices devices = nullptr;
    size_t count = 0;
    const RtcErrorCode code = RtcGetVideoDevices(&devices, &count);
    if (code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcGetVideoDevices", code);
      return;
    }

    for (size_t i = 0; i < count; ++i) {
      const char* name = devices[i].device_name ? devices[i].device_name : "(unknown)";
      video_sources_.push_back(name);
    }
    RtcDestoryVideoDevices(devices, count);

    if (selected_video_source_ >= static_cast<int>(video_sources_.size())) {
      selected_video_source_ = 0;
    }
  }

  void DrawUi() {
    ApplyPendingUiActions();
    DrawControlPanel();
    DrawVideoPanel();
    if (show_netstats_) {
      DrawNetStatsHint();
    }
    if (show_eventlog_) {
      DrawEventLogHint();
    }
    DrawFullscreenVideoOverlay();
  }

  void DrawControlPanel() {
    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(kPanelLeft, kControlPanelTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kControlPanelHeight),
                             ImGuiCond_Always);

    ImGui::Begin("RTC Controls", nullptr, window_flags);

    if (ImGui::BeginTable("ControlHeader", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_NoSavedSettings,
                          ImVec2(-FLT_MIN, 0.0f))) {
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Tools", ImGuiTableColumnFlags_WidthFixed, 260.0f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::Text("Server: %s", ServerStateText(server_state_.load()));
      ImGui::Text("Remote Frames   video=%llu   audio=%llu",
                  static_cast<unsigned long long>(remote_video_frames_.load()),
                  static_cast<unsigned long long>(remote_audio_frames_.load()));

      ImGui::TableSetColumnIndex(1);
      const float toggle_w = 124.0f;
      if (ImGui::Button(show_netstats_ ? "Hide NetStats" : "Show NetStats",
                        ImVec2(toggle_w, 0))) {
        show_netstats_ = !show_netstats_;
        if (show_netstats_) {
          focus_netstats_hint_ = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(show_eventlog_ ? "Hide EventLog" : "Show EventLog",
                        ImVec2(toggle_w, 0))) {
        show_eventlog_ = !show_eventlog_;
        if (show_eventlog_) {
          focus_eventlog_hint_ = true;
        }
      }
      ImGui::EndTable();
    }

    ImGui::Separator();
    const float body_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("ControlBody", 2,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoSavedSettings,
                          ImVec2(-FLT_MIN, body_height))) {
      ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
      if (ImGui::BeginChild("ControlLeftCard", ImVec2(0, 0), true,
                            ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        const float side_btn_w = 96.0f;
        const float action_btn_w = 108.0f;

        ImGui::TextDisabled("SOURCE");
        const float source_combo_w =
            std::max(120.0f, ImGui::GetContentRegionAvail().x - side_btn_w - 6.0f);
        ImGui::SetNextItemWidth(source_combo_w);
        if (ImGui::BeginCombo(
                "##video_source_combo",
                video_sources_.empty() ? "(none)"
                                       : video_sources_[selected_video_source_].c_str())) {
          for (int i = 0; i < static_cast<int>(video_sources_.size()); ++i) {
            const bool selected = (i == selected_video_source_);
            if (ImGui::Selectable(video_sources_[i].c_str(), selected)) {
              selected_video_source_ = i;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(side_btn_w, 0))) {
          LoadVideoSourceList();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("ROOM");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##open_room_input", open_room_id_, sizeof(open_room_id_));
        if (ImGui::Button("Open Room", ImVec2(action_btn_w, 0))) {
          const RtcErrorCode code =
              RtcOpenRoom(open_room_id_, RtcRoomType::VideoBroadcasting, false);
          AppendLogWithCode("RtcOpenRoom", code);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close Room", ImVec2(action_btn_w, 0))) {
          const RtcErrorCode code = RtcCloseRoom(open_room_id_);
          AppendLogWithCode("RtcCloseRoom", code);
        }

        const float room_combo_w =
            std::max(120.0f, ImGui::GetContentRegionAvail().x - 86.0f - 6.0f);
        ImGui::SetNextItemWidth(room_combo_w);
        if (ImGui::BeginCombo(
                "##rooms_combo",
                rooms_.empty() || selected_room_ < 0 ||
                        selected_room_ >= static_cast<int>(rooms_.size())
                    ? "(none)"
                    : rooms_[selected_room_].c_str())) {
          for (int i = 0; i < static_cast<int>(rooms_.size()); ++i) {
            const bool selected = (i == selected_room_);
            if (ImGui::Selectable(rooms_[i].c_str(), selected)) {
              selected_room_ = i;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Query", ImVec2(80.0f, 0))) {
          QueryRooms();
        }
        if (ImGui::Button("Join Room", ImVec2(action_btn_w, 0))) {
          JoinSelectedRoom();
        }
        ImGui::SameLine();
        if (ImGui::Button("Leave Room", ImVec2(action_btn_w, 0))) {
          const RtcErrorCode code = RtcLeaveRoom();
          AppendLogWithCode("RtcLeaveRoom", code);
          if (code == RtcErrorCode::OK) {
            ResetSessionStateOnUi();
          }
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);

      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
      if (ImGui::BeginChild("ControlRightCard", ImVec2(0, 0), true,
                            ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::TextDisabled("SRS");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##srs_url", srs_url_, sizeof(srs_url_));
        const float half_btn_w =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) *
            0.5f;
        if (ImGui::Button("Publish SRS", ImVec2(half_btn_w, 0))) {
          EnsureMediaSources();
          const RtcErrorCode code = RtcPublishToSRS(srs_url_);
          AppendLogWithCode("RtcPublishToSRS", code);
        }
        ImGui::SameLine();
        if (ImGui::Button("Unpublish SRS", ImVec2(half_btn_w, 0))) {
          const RtcErrorCode code = RtcUnpublishToSRS(srs_url_);
          AppendLogWithCode("RtcUnpublishToSRS", code);
        }
        if (ImGui::Button("Play SRS", ImVec2(half_btn_w, 0))) {
          const RtcErrorCode code = RtcPlayFromSRS(srs_url_);
          AppendLogWithCode("RtcPlayFromSRS", code);
        }
        ImGui::SameLine();
        if (ImGui::Button("Unplay SRS", ImVec2(half_btn_w, 0))) {
          const RtcErrorCode code = RtcUnplayFromSRS(srs_url_);
          AppendLogWithCode("RtcUnplayFromSRS", code);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("MESSAGE");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##message_input", send_msg_, sizeof(send_msg_));
        if (ImGui::Button("Broadcast", ImVec2(half_btn_w, 0))) {
          const size_t size = std::strlen(send_msg_);
          if (size == 0) {
            AppendLog("Broadcast skipped: empty message");
          } else {
            const RtcErrorCode code =
                RtcBroadcastData(kDataChannelLabel, send_msg_, size);
            AppendLogWithCode("RtcBroadcastData", code);
          }
        }
        ImGui::SameLine();
        if (ImGui::Button("Broadcast File", ImVec2(half_btn_w, 0))) {
          SendMessageFromFile();
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);

      ImGui::EndTable();
    }

    ImGui::End();
  }

  void DrawVideoPanel() {
    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(kPanelLeft, kVideoPanelTop), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPanelWidth, kVideoPanelHeight),
                             ImGuiCond_Always);

    ImGui::Begin("Video Preview", nullptr, window_flags);

    if (ImGui::Button(render_lr_pixel_interleave_
                          ? "LR Pixel Interleave: On"
                          : "LR Pixel Interleave: Off")) {
      render_lr_pixel_interleave_ = !render_lr_pixel_interleave_;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Side-by-side input -> alternating output columns");
    ImGui::SameLine();
    ImGui::TextDisabled("| Double-click a frame for full screen");
    ImGui::Separator();

    std::vector<std::shared_ptr<VideoFrameView>> frame_snapshot;
    {
      std::lock_guard<std::mutex> lock(video_mutex_);
      for (const auto& item : remote_video_frames_by_source_) {
        if (item.second) {
          frame_snapshot.push_back(item.second);
        }
      }
    }

    if (frame_snapshot.empty()) {
      ImGui::TextUnformatted("Waiting for remote video frame...");
      ImGui::End();
      return;
    }

    std::sort(frame_snapshot.begin(), frame_snapshot.end(),
              [](const std::shared_ptr<VideoFrameView>& a,
                 const std::shared_ptr<VideoFrameView>& b) {
                return a->stream_key < b->stream_key;
              });

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 panel_avail = ImGui::GetContentRegionAvail();

    if (frame_snapshot.size() == 1) {
      const ImVec2 start_pos = ImGui::GetCursorPos();
      const float max_box_width =
          std::max(1.0f, panel_avail.x - kVideoCardPadding * 2.0f);
      const float max_box_height =
          std::max(1.0f, panel_avail.y - kVideoCardPadding * 2.0f);
      const ImVec2 video_box_size =
          ComputeVideoBoxSize(*frame_snapshot.front(), max_box_width, max_box_height);
      const float y_offset =
          std::max(0.0f, (panel_avail.y - video_box_size.y) * 0.5f);
      ImGui::SetCursorPosY(start_pos.y + y_offset);
      DrawSingleVideoFrame(*frame_snapshot.front(), max_box_width, max_box_height,
                           panel_avail.x);
      ImGui::End();
      return;
    }

    const int columns = 2;
    const int rows =
        static_cast<int>((frame_snapshot.size() + static_cast<size_t>(columns) - 1) /
                         static_cast<size_t>(columns));
    const float cell_width =
        (panel_avail.x - style.ItemSpacing.x * static_cast<float>(columns - 1)) /
        static_cast<float>(columns);
    const float max_by_width =
        std::max(1.0f, cell_width - style.CellPadding.x * 2.0f - 12.0f);
    const float max_by_height = std::max(
        1.0f, (panel_avail.y - style.ItemSpacing.y * static_cast<float>(rows - 1)) /
                  static_cast<float>(rows) -
                  style.CellPadding.y * 2.0f - 12.0f);
    const float row_height =
        std::max(1.0f, max_by_height + style.CellPadding.y * 2.0f + 12.0f);
    if (ImGui::BeginTable("VideoTable", columns,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders,
                          ImVec2(0.0f, 0.0f))) {
      for (size_t i = 0; i < frame_snapshot.size(); ++i) {
        if (static_cast<int>(i % static_cast<size_t>(columns)) == 0) {
          ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        }
        ImGui::TableSetColumnIndex(
            static_cast<int>(i % static_cast<size_t>(columns)));
        DrawSingleVideoFrame(*frame_snapshot[i], max_by_width, max_by_height,
                             cell_width);
      }
      ImGui::EndTable();
    }

    ImGui::End();
  }

  ImVec2 ComputeVideoBoxSize(const VideoFrameView& frame,
                             float max_width,
                             float max_height) const {
    const float clamped_max_width = std::max(1.0f, max_width);
    const float clamped_max_height = std::max(1.0f, max_height);

    float aspect = 1.0f;
    if (frame.width > 0 && frame.height > 0) {
      aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
      if (aspect <= 0.0f) {
        aspect = 1.0f;
      }
    }

    float box_width = clamped_max_width;
    float box_height = box_width / aspect;
    if (box_height > clamped_max_height) {
      box_height = clamped_max_height;
      box_width = box_height * aspect;
    }

    box_width = std::max(1.0f, std::min(box_width, clamped_max_width));
    box_height = std::max(1.0f, std::min(box_height, clamped_max_height));
    return ImVec2(box_width, box_height);
  }

  void DrawSingleVideoFrame(const VideoFrameView& frame,
                            float max_box_width,
                            float max_box_height,
                            float content_width) {
    const ImVec2 video_box_size =
        ComputeVideoBoxSize(frame, max_box_width, max_box_height);
    const float start_x = ImGui::GetCursorPosX();
    const float x_offset = std::max(0.0f, (content_width - video_box_size.x) * 0.5f);
    if (x_offset > 0.0f) {
      ImGui::SetCursorPosX(start_x + x_offset);
    }
    DrawVideoFrameBox(frame, video_box_size, false);
  }

  void DrawVideoFrameBox(const VideoFrameView& frame,
                         const ImVec2& requested_box_size,
                         bool fullscreen_mode) {
    const ImVec2 box_size(std::max(1.0f, requested_box_size.x),
                          std::max(1.0f, requested_box_size.y));

    ImGui::PushID(frame.stream_key.c_str());
    ImGui::InvisibleButton(fullscreen_mode ? "video_frame_fullscreen"
                                           : "video_frame_preview",
                           box_size);
    const bool hovered = ImGui::IsItemHovered();
    const bool double_clicked =
        hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();
    ImGui::PopID();

    if (double_clicked) {
      if (fullscreen_mode) {
        CloseFullscreenVideo();
      } else {
        OpenFullscreenVideo(frame.stream_key);
      }
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 255));

    float draw_width = box_size.x;
    float draw_height = box_size.y;
    if (frame.width > 0 && frame.height > 0) {
      const float src_aspect =
          static_cast<float>(frame.width) / static_cast<float>(frame.height);
      const float box_aspect = box_size.x / box_size.y;
      if (src_aspect > box_aspect) {
        draw_height = box_size.x / src_aspect;
      } else {
        draw_width = box_size.y * src_aspect;
      }
    }

    int render_width_pixels = 0;
    int render_height_pixels = 0;
    if (render_lr_pixel_interleave_) {
      const ImGuiIO& io = ImGui::GetIO();
      const float fb_scale_x =
          io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
      const float fb_scale_y =
          io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
      render_width_pixels =
          std::max(1, static_cast<int>(std::lround(draw_width * fb_scale_x)));
      render_height_pixels =
          std::max(1, static_cast<int>(std::lround(draw_height * fb_scale_y)));
      draw_width = static_cast<float>(render_width_pixels) / fb_scale_x;
      draw_height = static_cast<float>(render_height_pixels) / fb_scale_y;
    }

    UpdateTextureFromFrame(frame, render_width_pixels, render_height_pixels);

    const auto tex_it = video_textures_.find(frame.stream_key);
    if (tex_it == video_textures_.end() || tex_it->second.texture == 0) {
      draw_list->AddRect(box_min, box_max,
                         hovered || fullscreen_mode
                             ? IM_COL32(90, 145, 230, 255)
                             : IM_COL32(80, 80, 80, 255));
      return;
    }

    const float offset_x = (box_size.x - draw_width) * 0.5f;
    const float offset_y = (box_size.y - draw_height) * 0.5f;
    const ImVec2 image_min(box_min.x + offset_x, box_min.y + offset_y);
    const ImVec2 image_max(image_min.x + draw_width, image_min.y + draw_height);
    draw_list->PushClipRect(box_min, box_max, true);
    draw_list->AddImage((ImTextureID)(intptr_t)tex_it->second.texture, image_min,
                        image_max, ImVec2(0, 0), ImVec2(1, 1));
    draw_list->PopClipRect();
    draw_list->AddRect(box_min, box_max,
                       hovered || fullscreen_mode
                           ? IM_COL32(90, 145, 230, 255)
                           : IM_COL32(80, 80, 80, 255));

    std::ostringstream title;
    title << "sid=" << frame.remote_sessionid << "  " << frame.source_id << "  "
          << frame.width << "x" << frame.height;
    const std::string title_text = title.str();
    const float overlay_height = ImGui::GetTextLineHeight() + kVideoOverlayPadding * 2.0f;
    const ImVec2 overlay_max(box_max.x, box_min.y + overlay_height);
    draw_list->AddRectFilled(box_min, overlay_max, IM_COL32(0, 0, 0, 170));
    draw_list->PushClipRect(box_min, overlay_max, true);
    draw_list->AddText(
        ImVec2(box_min.x + kVideoOverlayPadding, box_min.y + kVideoOverlayPadding),
        IM_COL32(235, 235, 235, 255), title_text.c_str());
    draw_list->PopClipRect();

    if (fullscreen_mode || hovered) {
      const char* hint_text = fullscreen_mode
                                  ? "Double-click or press Esc to exit full screen"
                                  : "Double-click to view full screen";
      const float hint_height =
          ImGui::GetTextLineHeight() + kVideoOverlayPadding * 2.0f;
      const ImVec2 hint_min(box_min.x, box_max.y - hint_height);
      draw_list->AddRectFilled(hint_min, box_max, IM_COL32(0, 0, 0, 148));
      draw_list->PushClipRect(hint_min, box_max, true);
      draw_list->AddText(
          ImVec2(hint_min.x + kVideoOverlayPadding,
                 hint_min.y + kVideoOverlayPadding),
          IM_COL32(235, 235, 235, 255), hint_text);
      draw_list->PopClipRect();
    }
  }

  void DrawFullscreenVideoOverlay() {
    if (fullscreen_video_stream_key_.empty()) {
      return;
    }

    std::shared_ptr<VideoFrameView> frame;
    {
      std::lock_guard<std::mutex> lock(video_mutex_);
      const auto it = remote_video_frames_by_source_.find(fullscreen_video_stream_key_);
      if (it != remote_video_frames_by_source_.end()) {
        frame = it->second;
      }
    }

    if (!frame) {
      CloseFullscreenVideo();
      return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      CloseFullscreenVideo();
      return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport) {
      return;
    }

    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (focus_fullscreen_video_) {
      ImGui::SetNextWindowFocus();
      focus_fullscreen_video_ = false;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("##VideoFullscreenOverlay", nullptr, window_flags)) {
      DrawVideoFrameBox(*frame, ImGui::GetContentRegionAvail(), true);

      ImDrawList* draw_list = ImGui::GetWindowDrawList();
      const char* exit_text = "Esc: exit full screen";
      const ImVec2 exit_size = ImGui::CalcTextSize(exit_text);
      const float label_padding = 8.0f;
      const ImVec2 overlay_origin = viewport->Pos;
      const ImVec2 top_right(
          overlay_origin.x + viewport->Size.x - kFullscreenOverlayPadding -
              exit_size.x,
          overlay_origin.y + kFullscreenOverlayPadding);
      const ImVec2 exit_min(top_right.x - label_padding, top_right.y - label_padding);
      const ImVec2 exit_max(top_right.x + exit_size.x + label_padding,
                            top_right.y + exit_size.y + label_padding);
      draw_list->AddRectFilled(exit_min, exit_max, IM_COL32(0, 0, 0, 170), 6.0f);
      draw_list->AddText(top_right, IM_COL32(235, 235, 235, 255), exit_text);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  void OpenFullscreenVideo(const std::string& stream_key) {
    EnsureWindowFullscreen(true);
    fullscreen_video_stream_key_ = stream_key;
    focus_fullscreen_video_ = true;
  }

  void CloseFullscreenVideo() {
    fullscreen_video_stream_key_.clear();
    focus_fullscreen_video_ = false;
    EnsureWindowFullscreen(false);
  }

  void EnsureWindowFullscreen(bool enable) {
    if (!ui_window_) {
      return;
    }

    if (enable) {
      if (!fullscreen_video_window_forced_) {
        fullscreen_video_window_was_fullscreen_ = ui_window_->IsFullscreen();
      }

      if (fullscreen_video_window_forced_ || fullscreen_video_window_was_fullscreen_) {
        fullscreen_video_window_forced_ = true;
        return;
      }

      if (!ui_window_->SetFullscreen(true)) {
        AppendLog(std::string("SDL_SetWindowFullscreen(true) failed: ") +
                  SDL_GetError());
        fullscreen_video_window_forced_ = false;
        return;
      }
      fullscreen_video_window_forced_ = true;
      return;
    }

    if (!fullscreen_video_window_forced_) {
      fullscreen_video_window_was_fullscreen_ = false;
      return;
    }

    const bool should_restore_windowed = !fullscreen_video_window_was_fullscreen_;
    fullscreen_video_window_forced_ = false;
    fullscreen_video_window_was_fullscreen_ = false;
    if (!should_restore_windowed) {
      return;
    }

    if (!ui_window_->SetFullscreen(false)) {
      AppendLog(std::string("SDL_SetWindowFullscreen(false) failed: ") +
                SDL_GetError());
    }
  }

  void UpdateTextureFromFrame(const VideoFrameView& frame,
                              int render_width_pixels,
                              int render_height_pixels) {
    if (frame.buffer.empty() || frame.width == 0 || frame.height == 0 ||
        frame.dimension < 4) {
      return;
    }

    const size_t expected_size = frame.width * frame.height * frame.dimension;
    if (frame.buffer.size() < expected_size) {
      return;
    }

    VideoTextureView& texture_view = video_textures_[frame.stream_key];
    if (texture_view.texture == 0) {
      glGenTextures(1, &texture_view.texture);
      texture_view.width = 0;
      texture_view.height = 0;
      texture_view.uploaded_frame_seq = 0;
      texture_view.uploaded_with_lr_interleave = false;
    }

    const unsigned char* upload_data = frame.buffer.data();
    bool uploaded_with_lr_interleave = false;
    int upload_width = static_cast<int>(frame.width);
    int upload_height = static_cast<int>(frame.height);
    if (render_lr_pixel_interleave_ &&
        render_width_pixels > 0 && render_height_pixels > 0 &&
        RasterizeFrameToRenderedHorizontalInterleave(
            frame, static_cast<size_t>(render_width_pixels),
            static_cast<size_t>(render_height_pixels),
            &texture_view.remapped_buffer)) {
      upload_data = texture_view.remapped_buffer.data();
      uploaded_with_lr_interleave = true;
      upload_width = render_width_pixels;
      upload_height = render_height_pixels;
    }

    if (texture_view.uploaded_frame_seq == frame.frame_seq &&
        texture_view.uploaded_with_lr_interleave == uploaded_with_lr_interleave &&
        texture_view.width == upload_width && texture_view.height == upload_height) {
      return;
    }

    glBindTexture(GL_TEXTURE_2D, texture_view.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    uploaded_with_lr_interleave ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    uploaded_with_lr_interleave ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (texture_view.width != upload_width || texture_view.height != upload_height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, upload_width, upload_height, 0, GL_BGRA,
                   GL_UNSIGNED_INT_8_8_8_8, upload_data);
      texture_view.width = upload_width;
      texture_view.height = upload_height;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, upload_width, upload_height, GL_BGRA,
                      GL_UNSIGNED_INT_8_8_8_8, upload_data);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    texture_view.uploaded_frame_seq = frame.frame_seq;
    texture_view.uploaded_with_lr_interleave = uploaded_with_lr_interleave;
  }

  void ReleaseVideoTextures() {
    for (auto& item : video_textures_) {
      if (item.second.texture != 0) {
        glDeleteTextures(1, &item.second.texture);
        item.second.texture = 0;
      }
    }
    video_textures_.clear();
  }

  void DrawNetStatsHint() {
    std::vector<NetStatsView> stats_snapshot;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      for (const auto& it : net_stats_) {
        stats_snapshot.push_back(it.second);
      }
    }

    const ImGuiWindowFlags hint_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::SetNextWindowPos(
        ImVec2(kPanelLeft + kPanelWidth - 412.0f, kControlPanelTop + 58.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(392.0f, 118.0f), ImGuiCond_Always);
    if (focus_netstats_hint_) {
      ImGui::SetNextWindowFocus();
      focus_netstats_hint_ = false;
    }
    ImGui::Begin("##NetStatsHint", nullptr, hint_flags);

    ImGui::TextDisabled("NET STATS");
    if (stats_snapshot.empty()) {
      ImGui::TextUnformatted("No stats yet");
      ImGui::End();
      return;
    }

    if (ImGui::BeginTable("NetStatsHintTable", 5,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV,
                          ImVec2(-FLT_MIN, 0.0f))) {
      ImGui::TableSetupColumn("Source");
      ImGui::TableSetupColumn("Type");
      ImGui::TableSetupColumn("Bitrate");
      ImGui::TableSetupColumn("FPS");
      ImGui::TableSetupColumn("Delay");
      ImGui::TableHeadersRow();

      for (const NetStatsView& s : stats_snapshot) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(s.source_id.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(s.input ? "in" : "out");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%lu", s.bitrate_bps);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", s.fps);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u ms", s.delay_ms);
      }
      ImGui::EndTable();
    }

    ImGui::End();
  }

  void DrawEventLogHint() {
    const ImGuiWindowFlags hint_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::SetNextWindowPos(
        ImVec2(kPanelLeft + kPanelWidth - 452.0f, kControlPanelTop + 184.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(432.0f, 176.0f), ImGuiCond_Always);
    if (focus_eventlog_hint_) {
      ImGui::SetNextWindowFocus();
      focus_eventlog_hint_ = false;
    }

    ImGui::Begin("##EventLogHint", nullptr, hint_flags);
    ImGui::TextDisabled("EVENT LOG");
    ImGui::Separator();
    {
      std::lock_guard<std::mutex> lock(log_mutex_);
      if (logs_.empty()) {
        ImGui::TextUnformatted("No logs yet");
      } else {
        const size_t max_lines = 7;
        const size_t begin =
            logs_.size() > max_lines ? logs_.size() - max_lines : 0;
        for (size_t i = begin; i < logs_.size(); ++i) {
          ImGui::TextWrapped("%s", logs_[i].c_str());
        }
      }
    }
    ImGui::End();
  }

  void AppendLog(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    logs_.push_back(message);
    if (logs_.size() > 2000) {
      logs_.erase(logs_.begin(), logs_.begin() + 500);
    }
  }

  void AppendLogWithCode(const char* action, RtcErrorCode code) {
    std::ostringstream oss;
    oss << action << ": " << RtcErrorMessage(code) << " (" << static_cast<int>(code)
        << ")";
    AppendLog(oss.str());
  }

  void ApplyPendingUiActions() {
    if (pending_reset_session_state_.exchange(false)) {
      ResetSessionStateOnUi();
    }
  }

  void RequestSessionStateReset() { pending_reset_session_state_.store(true); }

  void ResetSessionStateOnUi() {
    StopMediaFeed();
    remote_audio_player_.Clear();
    CloseFullscreenVideo();
    video_source_added_ = false;
    audio_source_added_ = false;

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      net_stats_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(video_mutex_);
      remote_video_frames_by_source_.clear();
    }

    remote_video_frames_.store(0);
    remote_audio_frames_.store(0);
    remote_video_frame_seq_.store(0);
    ReleaseVideoTextures();
  }

  void QueryRooms() {
    RtcRooms rooms = nullptr;
    size_t count = 0;
    const RtcErrorCode code = RtcQueryRooms(&rooms, &count);
    AppendLogWithCode("RtcQueryRooms", code);
    if (code != RtcErrorCode::OK) {
      return;
    }

    rooms_.clear();
    for (size_t i = 0; i < count; ++i) {
      rooms_.push_back(rooms[i].roomid ? rooms[i].roomid : "");
    }
    RtcDestoryRooms(rooms, count);

    if (!rooms_.empty()) {
      selected_room_ = 0;
    } else {
      selected_room_ = -1;
    }
  }

  void JoinSelectedRoom() {
    if (selected_room_ < 0 || selected_room_ >= static_cast<int>(rooms_.size())) {
      AppendLog("Join skipped: no room selected");
      return;
    }

    EnsureMediaSources();
    const RtcErrorCode code =
        RtcJoinRoom(const_cast<char*>(rooms_[selected_room_].c_str()));
    AppendLogWithCode("RtcJoinRoom", code);
  }

  void EnsureMediaSources() {
    if (!video_source_added_) {
      AddVideoSource();
    }
    if (!audio_source_added_) {
      AddAudioSource();
    }
  }

  void AddAudioSource() {
    const RtcErrorCode code =
        RtcAddExternalAudioSource(kExternalAudioSource, RtcPriorityType::High);
    AppendLogWithCode("RtcAddExternalAudioSource", code);
    if (code != RtcErrorCode::OK) {
      return;
    }

    audio_source_added_ = true;
    if (pcm_frames_.empty()) {
      LoadPcmData();
    }
    StartAudioFeed();
  }

  void AddVideoSource() {
    if (selected_video_source_ == 0) {
      const RtcErrorCode code =
          RtcAddExternalVideoSource(kExternalVideoSource, RtcPriorityType::High);
      AppendLogWithCode("RtcAddExternalVideoSource", code);
      if (code == RtcErrorCode::OK) {
        video_source_added_ = true;
        if (yuv_frames_.empty()) {
          LoadYuvFrames();
        }
        StartVideoFeed();
      }
      return;
    }

    const size_t device_index = static_cast<size_t>(selected_video_source_ - 1);
    RtcVideoDeviceCapability capability;
    capability.width = 1280;
    capability.height = 720;
    capability.max_fps = 30;

    const RtcErrorCode code =
        RtcAddDeviceVideoSource(device_index, &capability, RtcPriorityType::High);
    AppendLogWithCode("RtcAddDeviceVideoSource", code);
    if (code == RtcErrorCode::OK) {
      video_source_added_ = true;
    }
  }

  void LoadPcmData() {
    std::ifstream file(pcm_path_, std::ios::binary);
    if (!file.good()) {
      AppendLog(std::string("Cannot open PCM file: ") + pcm_path_);
      return;
    }

    const size_t frame_bytes = 160;
    const size_t max_frames = 1000;
    while (pcm_frames_.size() < max_frames) {
      RtcPCMData frame;
      frame.bits_per_sample = 16;
      frame.sample_rate = 8000;
      frame.number_of_channels = 1;
      frame.number_of_frames = 80;
      frame.sz_buffer = frame_bytes;
      frame.buffer = new char[frame_bytes];
      file.read(static_cast<char*>(frame.buffer), static_cast<std::streamsize>(frame_bytes));
      if (file.gcount() != static_cast<std::streamsize>(frame_bytes)) {
        delete[] static_cast<char*>(frame.buffer);
        break;
      }
      pcm_frames_.push_back(frame);
    }

    std::ostringstream oss;
    oss << "Loaded PCM frames: " << pcm_frames_.size();
    AppendLog(oss.str());
  }

  void LoadYuvFrames() {
    std::ifstream file(yuv_path_, std::ios::binary);
    if (!file.good()) {
      AppendLog(std::string("Cannot open YUV file: ") + yuv_path_);
      return;
    }

    const size_t width = 1280;
    const size_t height = 720;
    const size_t frame_bytes = width * height * 3 / 2;
    const size_t max_frames = 300;

    while (yuv_frames_.size() < max_frames) {
      RtcYUV420pFrame frame;
      frame.width = width;
      frame.height = height;
      frame.stride_Y = width;
      frame.stride_U = width / 2;
      frame.stride_V = width / 2;
      frame.sz_buffer = frame_bytes;
      frame.buffer = new unsigned char[frame_bytes];
      file.read(reinterpret_cast<char*>(frame.buffer),
                static_cast<std::streamsize>(frame_bytes));
      if (file.gcount() != static_cast<std::streamsize>(frame_bytes)) {
        delete[] frame.buffer;
        break;
      }
      yuv_frames_.push_back(frame);
    }

    std::ostringstream oss;
    oss << "Loaded YUV frames: " << yuv_frames_.size();
    AppendLog(oss.str());
  }

  void StartAudioFeed() {
    if (audio_thread_.joinable() || pcm_frames_.empty()) {
      return;
    }

    stop_media_feed_.store(false);
    audio_thread_ = std::thread([this]() {
      size_t idx = 0;
      while (!stop_media_feed_.load()) {
        if (idx >= pcm_frames_.size()) {
          idx = 0;
        }
        RtcSendAudioFrame(kExternalAudioSource, &pcm_frames_[idx]);
        ++idx;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }

  void StartVideoFeed() {
    if (video_thread_.joinable() || yuv_frames_.empty()) {
      return;
    }

    stop_media_feed_.store(false);
    video_thread_ = std::thread([this]() {
      size_t idx = 0;
      while (!stop_media_feed_.load()) {
        if (idx >= yuv_frames_.size()) {
          idx = 0;
        }
        RtcSendFrame(kExternalVideoSource, &yuv_frames_[idx]);
        ++idx;
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
      }
    });
  }

  void StopMediaFeed() {
    stop_media_feed_.store(true);
    if (audio_thread_.joinable()) {
      audio_thread_.join();
    }
    if (video_thread_.joinable()) {
      video_thread_.join();
    }
  }

  void FreeMediaBuffers() {
    for (RtcPCMData& frame : pcm_frames_) {
      delete[] static_cast<char*>(frame.buffer);
      frame.buffer = nullptr;
    }
    pcm_frames_.clear();

    for (RtcYUV420pFrame& frame : yuv_frames_) {
      delete[] frame.buffer;
      frame.buffer = nullptr;
    }
    yuv_frames_.clear();
  }

  void SendMessageFromFile() {
    std::ifstream file(message_file_path_, std::ios::binary);
    if (!file.good()) {
      AppendLog(std::string("Cannot open message file: ") + message_file_path_);
      return;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size <= 0) {
      AppendLog("Message file is empty");
      return;
    }

    std::vector<char> content(static_cast<size_t>(size));
    file.read(content.data(), static_cast<std::streamsize>(size));

    const RtcErrorCode code =
        RtcBroadcastData(kDataChannelLabel, content.data(), content.size());
    AppendLogWithCode("RtcBroadcastData(file)", code);
  }

  static void OnRoom(RtcRoomOperation op, RtcRoomId roomid) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "Room event: op=" << static_cast<int>(op)
        << " room=" << (roomid ? roomid : "");
    instance_->AppendLog(oss.str());
  }

  static void OnP2PState(RtcSessionId sessionid, RtcP2PState state) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "P2P: session=" << sessionid << " state=" << P2PStateText(state);
    instance_->AppendLog(oss.str());

    if (state == RtcP2PState::P2PDisconnected || state == RtcP2PState::P2PClosed ||
        state == RtcP2PState::P2PFailed) {
      instance_->RequestSessionStateReset();
    }
  }

  static void OnDataChannelState(RtcSessionId sessionid, RtcDataChannelLabel label,
                                 RtcDataChannelState state) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "DataChannel: session=" << sessionid << " label="
        << (label ? label : "") << " state=" << static_cast<int>(state);
    instance_->AppendLog(oss.str());
  }

  static void OnServerConnectionState(RtcServerConnectionState state) {
    if (!instance_) {
      return;
    }
    instance_->server_state_.store(state);
    std::ostringstream oss;
    oss << "Server state: " << ServerStateText(state);
    instance_->AppendLog(oss.str());
  }

  static void OnSRSState(RtcSRSStreamurl streamurl, RtcP2PState state) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "SRS state: " << (streamurl ? streamurl : "")
        << " => " << P2PStateText(state);
    instance_->AppendLog(oss.str());
  }

  static void OnSRSResponse(RtcSRSStreamurl streamurl, RtcSRSResponse response) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "SRS response: " << (streamurl ? streamurl : "")
        << " => " << static_cast<int>(response);
    instance_->AppendLog(oss.str());
  }

  static void OnRecvMessage(RtcSessionId remote_sessionid, RtcDataChannelLabel label,
                            const char* msg, size_t msg_size) {
    if (!instance_) {
      return;
    }
    std::ostringstream oss;
    oss << "Recv msg from " << remote_sessionid << " [" << (label ? label : "")
        << "] bytes=" << msg_size;
    if (msg && msg_size > 0) {
      const size_t preview = std::min<size_t>(msg_size, 128);
      oss << " preview='" << std::string(msg, msg + preview) << "'";
    }
    instance_->AppendLog(oss.str());
  }

  static void OnRecvAudioFrame(RtcSessionId,
                               RtcAudioSourceId,
                               RtcMediaSourceType,
                               size_t bits_per_sample,
                               size_t sample_rate,
                               size_t number_of_channels,
                               size_t,
                               const void* audio_data,
                               size_t sz_audio_data) {
    if (!instance_) {
      return;
    }
    instance_->remote_audio_frames_.fetch_add(1);
    instance_->remote_audio_player_.PushFrame(bits_per_sample, sample_rate,
                                              number_of_channels, audio_data,
                                              sz_audio_data);
  }

  static void OnRecvFrame(RtcSessionId remote_sessionid,
                          RtcVideoSourceId sourceid,
                          RtcMediaSourceType source_type,
                          size_t width,
                          size_t height,
                          size_t dimension,
                          const unsigned char* buffer,
                          size_t sz_buffer) {
    if (!instance_) {
      return;
    }

    instance_->remote_video_frames_.fetch_add(1);
    if (!sourceid || !buffer || width == 0 || height == 0 || sz_buffer == 0) {
      return;
    }

    auto view = std::make_shared<VideoFrameView>();
    std::ostringstream key;
    key << remote_sessionid << ":" << sourceid;
    view->stream_key = key.str();
    view->source_id = sourceid;
    view->remote_sessionid = remote_sessionid;
    view->source_type = source_type;
    view->width = width;
    view->height = height;
    view->dimension = dimension;
    view->frame_seq = instance_->remote_video_frame_seq_.fetch_add(1) + 1;
    view->buffer.assign(buffer, buffer + sz_buffer);

    {
      std::lock_guard<std::mutex> lock(instance_->video_mutex_);
      instance_->remote_video_frames_by_source_[view->stream_key] = view;
    }
  }

  static void OnChannelNetworkStats(RtcSessionId, RtcNetStats stats) {
    if (!instance_) {
      return;
    }

    NetStatsView view;
    view.input = stats.input;
    if (stats.video_stats.sourceid) {
      view.source_id = stats.video_stats.sourceid;
    }
    view.bitrate_bps = stats.video_stats.bitrate_bps;
    view.width = stats.video_stats.width;
    view.height = stats.video_stats.height;
    view.fps = stats.video_stats.fps;
    view.loss_rate = stats.video_stats.loss_rate;
    view.delay_ms = stats.video_stats.delay_ms;
    view.key_frame_count = stats.video_stats.key_frame_count;
    view.fir_count = stats.video_stats.fir_count;
    view.pli_count = stats.video_stats.pli_count;
    view.nack_count = stats.video_stats.nack_count;
    if (stats.video_stats.codec_name) {
      view.codec_name = stats.video_stats.codec_name;
    }

    if (view.source_id.empty() && stats.audio_stats.sourceid) {
      view.source_id = stats.audio_stats.sourceid;
      view.bitrate_bps = stats.audio_stats.bitrate_bps;
      view.codec_name = "audio";
    }

    if (!view.source_id.empty()) {
      const std::string source_key =
          view.source_id + (view.input ? "|in" : "|out");
      std::lock_guard<std::mutex> lock(instance_->stats_mutex_);
      instance_->net_stats_[source_key] = view;
    }
  }

 private:
  static RtcImguiApp* instance_;

  std::atomic<RtcServerConnectionState> server_state_{ServerDisconnected};
  std::atomic<uint64_t> remote_video_frames_{0};
  std::atomic<uint64_t> remote_audio_frames_{0};
  std::atomic<uint64_t> remote_video_frame_seq_{0};
  std::atomic<bool> pending_reset_session_state_{false};
  RtcAudioPlayer remote_audio_player_;
  SDLOpenGLWindow* ui_window_ = nullptr;

  std::mutex log_mutex_;
  std::vector<std::string> logs_;
  bool auto_scroll_ = true;
  bool show_netstats_ = false;
  bool show_eventlog_ = false;
  bool focus_netstats_hint_ = false;
  bool focus_eventlog_hint_ = false;

  std::mutex stats_mutex_;
  std::map<std::string, NetStatsView> net_stats_;

  std::mutex video_mutex_;
  std::map<std::string, std::shared_ptr<VideoFrameView>> remote_video_frames_by_source_;
  std::map<std::string, VideoTextureView> video_textures_;
  bool render_lr_pixel_interleave_ = false;
  std::string fullscreen_video_stream_key_;
  bool focus_fullscreen_video_ = false;
  bool fullscreen_video_window_forced_ = false;
  bool fullscreen_video_window_was_fullscreen_ = false;

  bool rtc_inited_ = false;
  bool video_source_added_ = false;
  bool audio_source_added_ = false;

  std::vector<std::string> video_sources_ = {"Local YUV420p"};
  int selected_video_source_ = 0;

  std::vector<std::string> rooms_;
  int selected_room_ = -1;

  char open_room_id_[128];
  char srs_url_[256];
  char send_msg_[1024];

  std::string rtc_cfg_path_;
  std::string pcm_path_;
  std::string yuv_path_;
  std::string message_file_path_;

  std::atomic<bool> stop_media_feed_{false};
  std::thread audio_thread_;
  std::thread video_thread_;

  std::vector<RtcPCMData> pcm_frames_;
  std::vector<RtcYUV420pFrame> yuv_frames_;
};

RtcImguiApp* RtcImguiApp::instance_ = nullptr;

}  // namespace

int main() {
  RtcImguiApp app;
  if (!app.Init()) {
    return 1;
  }
  app.Run();
  return 0;
}
