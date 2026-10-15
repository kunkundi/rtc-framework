#include "c_rtc.h"
#include "rtc_audio/audio_device.h"
#include "rtc_runtime/device_identity.h"
#include "rtc_console_options.h"
#include "device_slot_registry.h"
#include "video_composite_layout.h"
#include "rtc_vehicle_protocol/vehicle_control_protocol.h"
#include "rtc_vision/vision_detection_codec.h"
#include "rtc_vision/view_control_protocol.h"
#include "video_view_state.h"
#include "vehicle_control_sender.h"
#include "vehicle_state_log_limiter.h"
#include "local_yolo.h"
#include "rtc_vision/coco_labels.h"

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
#include <csignal>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kDataChannelLabel = "datachannel";
constexpr const char* kExternalAudioSource = "console_audio";
constexpr const char* kExternalVideoSource = "stereo";
constexpr const char* kSurroundFrontVideoSource = "surround_front";
constexpr const char* kSurroundRearVideoSource = "surround_rear";
constexpr const char* kSurroundLeftVideoSource = "surround_left";
constexpr const char* kSurroundRightVideoSource = "surround_right";
constexpr int kAutoOpenRetryMs = 3000;
constexpr int kNoRenderStatusIntervalMs = 5000;
constexpr uint64_t kVehicleDriveIntervalMs = 20;
constexpr float kVehicleThrottleStep = 0.05f;
constexpr float kGamepadAxisDeadzone = 0.25f;
constexpr float kGamepadTriggerThreshold = 0.45f;
constexpr uint64_t kGamepadThrottleRepeatMs = 160;
constexpr int kMainWindowWidth = 1060;
constexpr int kMainWindowHeight = 950;
constexpr int kMinimumWindowWidth = 760;
constexpr int kMinimumWindowHeight = 600;
constexpr float kPanelMargin = 16.0f;
constexpr float kPanelGap = 10.0f;
constexpr float kPreferredControlPanelHeight = 370.0f;
constexpr float kMinimumControlPanelHeight = 180.0f;
constexpr float kMinimumVideoPanelHeight = 180.0f;
constexpr float kVideoOverlayPadding = 8.0f;
constexpr float kFullscreenOverlayPadding = 20.0f;

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) {
  g_stop_requested = 1;
}

struct NetStatsView {
  RtcSessionId session_id = 0;
  RtcVideoDelayStatsV1 delay{sizeof(RtcVideoDelayStatsV1), -1, -1, -1, -1};
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

struct VisionDetectionOverlayView {
  vts_rtc::vision::FrameDetections frame_detections;
  std::chrono::steady_clock::time_point updated_at;
};

struct VisionOverlaySnapshot {
  VisionDetectionOverlayView overlay;
  std::map<uint32_t, std::string> class_labels_by_id;
};

struct EdgeFeedbackView {
  uint64_t envelope_seq = 0;
  vts_rtc::vehicle::VehicleState state;
  std::chrono::steady_clock::time_point updated_at;
};

struct AudioDeviceChoice {
  bool enabled = false;
  uint32_t device_id = rtc_audio::kSystemDefaultDeviceId;
  std::string label;
};

struct NormalizedRect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct UiLayout {
  ImVec2 viewport_pos;
  ImVec2 viewport_size;
  ImVec2 control_pos;
  ImVec2 control_size;
  ImVec2 video_pos;
  ImVec2 video_size;
};

UiLayout ComputeUiLayout(const ImGuiViewport& viewport) {
  UiLayout layout;
  layout.viewport_pos = viewport.WorkPos;
  layout.viewport_size = viewport.WorkSize;

  const float margin_x = std::min(
      kPanelMargin, std::max(0.0f, (layout.viewport_size.x - 1.0f) * 0.5f));
  const float margin_y = std::min(
      kPanelMargin,
      std::max(0.0f,
               (layout.viewport_size.y - kPanelGap - 2.0f) * 0.5f));
  const float panel_width =
      std::max(1.0f, layout.viewport_size.x - margin_x * 2.0f);
  const float available_height = std::max(
      2.0f, layout.viewport_size.y - margin_y * 2.0f - kPanelGap);
  float control_height = std::min(
      std::min(kPreferredControlPanelHeight, available_height * 0.38f),
      std::max(kMinimumControlPanelHeight,
               available_height - kMinimumVideoPanelHeight));
  control_height =
      std::min(control_height, std::max(1.0f, available_height - 1.0f));
  const float video_height = std::max(1.0f, available_height - control_height);

    layout.video_pos =
      ImVec2(layout.viewport_pos.x + margin_x, layout.viewport_pos.y + margin_y);
  layout.video_size = ImVec2(panel_width, video_height);
    layout.control_pos = ImVec2(layout.video_pos.x,
                  layout.video_pos.y + video_height + kPanelGap);
    layout.control_size = ImVec2(panel_width, control_height);
  return layout;
}

std::string MakeVideoStreamKey(RtcSessionId remote_sessionid,
                               const std::string& source_id) {
  std::ostringstream key;
  key << remote_sessionid << ":" << source_id;
  return key.str();
}

const char* VideoSourceIdForTile(rtc_console::VideoTileId tile) {
  switch (tile) {
    case rtc_console::VideoTileId::Stereo:
      return kExternalVideoSource;
    case rtc_console::VideoTileId::SurroundFront:
      return kSurroundFrontVideoSource;
    case rtc_console::VideoTileId::SurroundRear:
      return kSurroundRearVideoSource;
    case rtc_console::VideoTileId::SurroundLeft:
      return kSurroundLeftVideoSource;
    case rtc_console::VideoTileId::SurroundRight:
      return kSurroundRightVideoSource;
  }
  return "unknown";
}

bool VideoTileForSourceId(const std::string& source_id,
                          rtc_console::VideoTileId* tile) {
  if (tile == nullptr) {
    return false;
  }
  if (source_id == kExternalVideoSource) {
    *tile = rtc_console::VideoTileId::Stereo;
    return true;
  }
  if (source_id == kSurroundFrontVideoSource) {
    *tile = rtc_console::VideoTileId::SurroundFront;
    return true;
  }
  if (source_id == kSurroundRearVideoSource) {
    *tile = rtc_console::VideoTileId::SurroundRear;
    return true;
  }
  if (source_id == kSurroundLeftVideoSource) {
    *tile = rtc_console::VideoTileId::SurroundLeft;
    return true;
  }
  if (source_id == kSurroundRightVideoSource) {
    *tile = rtc_console::VideoTileId::SurroundRight;
    return true;
  }
  return false;
}

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

const char* VehicleGearText(vts_rtc::vehicle::VehicleGear gear) {
  switch (gear) {
    case vts_rtc::vehicle::VehicleGear::Neutral:
      return "Neutral";
    case vts_rtc::vehicle::VehicleGear::Forward:
      return "Forward";
    case vts_rtc::vehicle::VehicleGear::Reverse:
      return "Reverse";
    default:
      return "Unknown";
  }
}

const char* DriveDirectionText(vts_rtc::vehicle::DriveDirection direction) {
  switch (direction) {
    case vts_rtc::vehicle::DriveDirection::Stop:
      return "Stop";
    case vts_rtc::vehicle::DriveDirection::Forward:
      return "Forward";
    case vts_rtc::vehicle::DriveDirection::Reverse:
      return "Reverse";
    default:
      return "Unknown";
  }
}

const char* SteeringDirectionText(
    vts_rtc::vehicle::SteeringDirection direction) {
  switch (direction) {
    case vts_rtc::vehicle::SteeringDirection::Center:
      return "Center";
    case vts_rtc::vehicle::SteeringDirection::Left:
      return "Left";
    case vts_rtc::vehicle::SteeringDirection::Right:
      return "Right";
    default:
      return "Unknown";
  }
}

const char* DogActionText(vts_rtc::vehicle::DogActionType action) {
  switch (action) {
    case vts_rtc::vehicle::DogActionType::LateralLeft:
      return "lateral_left";
    case vts_rtc::vehicle::DogActionType::LateralRight:
      return "lateral_right";
    case vts_rtc::vehicle::DogActionType::LateralStop:
      return "lateral_stop";
    case vts_rtc::vehicle::DogActionType::Stand:
      return "stand";
    case vts_rtc::vehicle::DogActionType::LieDown:
      return "lie_down";
    case vts_rtc::vehicle::DogActionType::Unknown:
    default:
      return "Unknown";
  }
}

const char* VehicleErrorCodeText(
    vts_rtc::vehicle::VehicleErrorCode error_code) {
  switch (error_code) {
    case vts_rtc::vehicle::VehicleErrorCode::None:
      return "none";
    case vts_rtc::vehicle::VehicleErrorCode::InvalidArgument:
      return "invalid_argument";
    case vts_rtc::vehicle::VehicleErrorCode::InvalidState:
      return "invalid_state";
    case vts_rtc::vehicle::VehicleErrorCode::Internal:
      return "internal";
    default:
      return "unknown";
  }
}

uint64_t SteadyTimeMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class SDLOpenGLWindow {
 public:
  bool Init(const char* title, int width, int height) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
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
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
      initialized_ = false;
      return false;
    }
    SDL_SetWindowMinimumSize(window_, kMinimumWindowWidth,
                             kMinimumWindowHeight);
    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
      initialized_ = false;
      return false;
    }

    if (!SDL_GL_MakeCurrent(window_, gl_context_)) {
      SDL_GL_DestroyContext(gl_context_);
      gl_context_ = nullptr;
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
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
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
      initialized_ = false;
    }
  }

  void PumpEvents(ImGuiIO& /*io*/, bool* should_close) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_TEXT_INPUT ||
          event.type == SDL_EVENT_TEXT_EDITING ||
          event.type == SDL_EVENT_TEXT_EDITING_CANDIDATES) {
        continue;
      }
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

class RtcConsoleApp {
 public:
  explicit RtcConsoleApp(const rtc_console::AppOptions& options)
      : options_(options) {
    instance_ = this;
    std::memset(open_room_id_, 0, sizeof(open_room_id_));

    std::snprintf(open_room_id_, sizeof(open_room_id_), "%s",
                  options_.room_id.c_str());
  }

  ~RtcConsoleApp() {
    local_yolo_.Stop();
    StopMediaFeed();
    remote_audio_player_.Stop();
    DestroyRtc();
    FreeMediaBuffers();
    instance_ = nullptr;
  }

  bool Init() {
    ResolveAssetPaths();
    const rtc_console::AppConfig config =
        rtc_console::LoadConfig(rtc_cfg_path_);
    if (!options_.room_id_from_command_line) {
      options_.room_id = config.room_id;
      std::snprintf(open_room_id_, sizeof(open_room_id_), "%s",
                    options_.room_id.c_str());
    }
    auto_open_room_ = config.auto_open_room;
    for (const auto& action : config.key_bindings) {
      for (const auto& name : action.second) {
        ImGuiKey resolved = ImGuiKey_None;
        for (int key = ImGuiKey_Tab; key < ImGuiKey_COUNT; ++key) {
          if (name == ImGui::GetKeyName(static_cast<ImGuiKey>(key))) {
            resolved = static_cast<ImGuiKey>(key);
            break;
          }
        }
        if (resolved == ImGuiKey_None) {
          throw std::runtime_error("unsupported console key: " + name);
        }
        key_bindings_[action.first].push_back(resolved);
      }
    }
    steering_wheel_range_deg_ = config.steering_wheel_range_deg;
    yolo_enabled_ = config.yolo_enabled;
    if (yolo_enabled_) {
      for (size_t index = 0; index < sizeof(rtc_camera_headless::kCocoLabels) /
                                        sizeof(rtc_camera_headless::kCocoLabels[0]); ++index) {
        vision_class_labels_by_id_[static_cast<uint32_t>(index)] =
            rtc_camera_headless::kCocoLabels[index];
      }
      std::string error;
      if (!local_yolo_.Start(config.yolo_model_path, config.yolo_max_fps,
          [this](const rtc_console::LocalYoloFrame& frame,
                 const vts_rtc::vision::FrameDetections& detections) {
            VisionDetectionOverlayView overlay;
            overlay.frame_detections = detections;
            overlay.updated_at = frame.received;
            std::lock_guard<std::mutex> lock(vision_detection_mutex_);
            vision_detections_by_source_[frame.key] = std::move(overlay);
          }, &error, [this](const std::string& error) {
            AppendLog("Local YOLO inference failed: " + error);
          })) {
        AppendLog("Local YOLO initialization failed: " + error);
        return false;
      }
      AppendLog("Local YOLO enabled (ONNX Runtime CPU)");
    }
    device_slots_.Configure(config.device_slots);
    AppendLog(std::string("Target room: ") + options_.room_id);
    AppendLog(std::string("Room auto-open: ") +
              (auto_open_room_ ? "enabled" : "disabled"));
    if (!InitRtc()) {
      return false;
    }
    return true;
  }

  void Run() {
    if (options_.no_render) {
      RunWithoutRenderer();
      return;
    }

    SDLOpenGLWindow window;
    if (!window.Init("rtc-framework | rtc_console", kMainWindowWidth,
                     kMainWindowHeight)) {
      AppendLog("SDLOpenGLWindow init failed");
      return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const char* font_paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "C:/Windows/Fonts/msyh.ttc"};
    for (const char* font_path : font_paths) {
      SDL_IOStream* font_file = SDL_IOFromFile(font_path, "rb");
      if (!font_file) {
        continue;
      }
      SDL_CloseIO(font_file);
      if (io.Fonts->AddFontFromFileTTF(font_path, 18.0f)) {
        break;
      }
    }
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
      MaybeOpenConfiguredRoom(now);
      float delta = std::chrono::duration<float>(now - last_tick).count();
      last_tick = now;
      if (delta <= 0.0f) {
        delta = 1.0f / 60.0f;
      }

      window.PumpEvents(io, &should_close);

      io.DisplaySize = ImVec2(static_cast<float>(window.width()),
                              static_cast<float>(window.height()));
      io.DeltaTime = delta;

      ImGui_ImplSDL3_NewFrame();
      ImGui_ImplOpenGL3_NewFrame();
      ImGui::NewFrame();

      DrawUi();
      TickVehicleControl(SteadyTimeMs());

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
    CloseGamepad();
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    window.Shutdown();
  }

 private:
  void RunWithoutRenderer() {
    g_stop_requested = 0;
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
    AppendLog(std::string("No-render mode started; room=") + options_.room_id);
    auto next_status_at = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              kNoRenderStatusIntervalMs);

    while (!g_stop_requested) {
      const auto now = std::chrono::steady_clock::now();
      MaybeOpenConfiguredRoom(now);
      if (now >= next_status_at) {
        AppendNoRenderStatus();
        next_status_at = now +
                         std::chrono::milliseconds(
                             kNoRenderStatusIntervalMs);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    AppendLog("No-render mode stopped");
  }

  void AppendNoRenderStatus() {
    std::ostringstream oss;
    oss << "No-render status: server=" << ServerStateText(server_state_.load())
        << " room_opened=" << (auto_room_opened_.load() ? "yes" : "no")
        << " recv_video=" << remote_video_frames_.load()
        << " recv_audio=" << remote_audio_frames_.load();
    AppendLog(oss.str());
  }

  void MaybeOpenConfiguredRoom(std::chrono::steady_clock::time_point now) {
    if (!auto_open_room_ || server_state_.load() != ServerLogined ||
        auto_room_opened_.load()) {
      return;
    }
    if (last_auto_open_attempt_.time_since_epoch().count() != 0 &&
        now - last_auto_open_attempt_ <
            std::chrono::milliseconds(kAutoOpenRetryMs)) {
      return;
    }

    last_auto_open_attempt_ = now;
    EnsureAudioSource();
    const RtcErrorCode code =
        RtcOpenRoom(const_cast<char*>(options_.room_id.c_str()),
                    RtcRoomType::VideoBroadcasting, false);
    AppendLogWithCode("RtcOpenRoom(auto)", code);
    if (code == RtcErrorCode::OK ||
        code == RtcErrorCode::AgentAlreadyInRoom) {
      auto_room_opened_.store(true);
    }
  }

  bool InitRtc() {
    RtcInitParams params;
    std::memset(&params, 0, sizeof(params));
    params.config_filepath = rtc_cfg_path_.c_str();
    params.room_handler = &RtcConsoleApp::OnRoom;
    params.P2P_state_handler = &RtcConsoleApp::OnP2PState;
    params.datachannel_state_handler = &RtcConsoleApp::OnDataChannelState;
    params.serverconnection_state_handler = &RtcConsoleApp::OnServerConnectionState;
    params.recv_msg_handler = &RtcConsoleApp::OnRecvMessage;
    params.recv_audioframe_handler = &RtcConsoleApp::OnRecvAudioFrame;
    params.recv_frame_handler = &RtcConsoleApp::OnRecvFrame;
    params.channel_network_stats_handler = &RtcConsoleApp::OnChannelNetworkStats;

    const RtcErrorCode init_code = RtcInitAgentV2(params);
    if (init_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcInitAgentV2", init_code);
      return false;
    }
    AppendLog("RtcInitAgentV2 success");
    RtcSetVideoDelayStatsHandlerV1(&RtcConsoleApp::OnVideoDelayStats);

    const RtcErrorCode dc_code =
        RtcAddDataChannel(kDataChannelLabel, RtcPriorityType::High, true, -1);
    if (dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(datachannel)", dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(datachannel) success");

    const RtcErrorCode vision_dc_code = RtcAddDataChannel(
        vts_rtc::vision::kVisionDetectionChannelLabel, RtcPriorityType::Medium,
        false, 0);
    if (vision_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(vision.detect.v1)", vision_dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(vision.detect.v1) success");

    const RtcErrorCode vehicle_control_dc_code = RtcAddDataChannel(
        vts_rtc::vehicle::kVehicleControlChannelLabel, RtcPriorityType::High,
        false, 0);
    if (vehicle_control_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(vehicle.control.v1)",
                        vehicle_control_dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(vehicle.control.v1) success");

    const RtcErrorCode vehicle_event_dc_code = RtcAddDataChannel(
        vts_rtc::vehicle::kVehicleEventChannelLabel, RtcPriorityType::High,
        true, -1);
    if (vehicle_event_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(vehicle.event.v1)",
                        vehicle_event_dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(vehicle.event.v1) success");

    const RtcErrorCode vehicle_state_dc_code = RtcAddDataChannel(
        vts_rtc::vehicle::kVehicleStateChannelLabel, RtcPriorityType::Medium,
        false, 0);
    if (vehicle_state_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(vehicle.state.v1)",
                        vehicle_state_dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(vehicle.state.v1) success");

    const RtcErrorCode video_view_control_dc_code = RtcAddDataChannel(
        vts_rtc::vision::kVideoViewControlChannelLabel, RtcPriorityType::High,
        true, -1);
    if (video_view_control_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(video.view_control.v1)",
                        video_view_control_dc_code);
      return false;
    }
    AppendLog("RtcAddDataChannel(video.view_control.v1) success");

    const auto identity_dc_code = RtcAddDataChannel(
        rtc_runtime::kDeviceIdentityChannelLabel, RtcPriorityType::Medium,
        true, -1);
    if (identity_dc_code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcAddDataChannel(device.identity.v1)",
                        identity_dc_code);
      return false;
    }

    rtc_inited_ = true;
    if (!options_.no_render) {
      LoadVideoSourceList();
      LoadAudioDeviceLists();
      ApplyAudioOutputSelection();
    }
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

    std::string current = exe_dir;
    for (int i = 0; i < 5; ++i) {
      base_candidates.push_back(current);
      current = JoinPath(current, "..");
    }

    rtc_cfg_path_ = FindAsset("rtc.cfg", "config", base_candidates);
    yuv_path_ = FindAsset("zjlabs.yuv", "test_data", base_candidates);

    AppendLog(std::string("rtc.cfg: ") + rtc_cfg_path_);
    AppendLog(std::string("zjlabs.yuv: ") + yuv_path_);
  }

  std::string FindAsset(const std::string& name,
                        const std::string& directory,
                        const std::vector<std::string>& base_candidates) {
    for (const std::string& base : base_candidates) {
      if (base.empty()) {
        continue;
      }

      const std::string candidate_direct = JoinPath(base, name);
      if (FileExists(candidate_direct)) {
        return candidate_direct;
      }

      const std::string candidate_in_directory =
          JoinPath(JoinPath(base, directory), name);
      if (FileExists(candidate_in_directory)) {
        return candidate_in_directory;
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

  void LoadAudioDeviceLists() {
    LoadAudioDeviceList(true, &audio_input_choices_,
                        &selected_audio_input_);
    LoadAudioDeviceList(false, &audio_output_choices_,
                        &selected_audio_output_);
  }

  void LoadAudioDeviceList(bool capture,
                           std::vector<AudioDeviceChoice>* choices,
                           int* selected_index) {
    if (choices == nullptr || selected_index == nullptr) {
      return;
    }
    bool selected_enabled = choices->empty() ? *selected_index > 0 : false;
    uint32_t selected_device_id = rtc_audio::kSystemDefaultDeviceId;
    std::string selected_label;
    if (*selected_index >= 0 &&
        *selected_index < static_cast<int>(choices->size())) {
      selected_enabled = (*choices)[*selected_index].enabled;
      selected_device_id = (*choices)[*selected_index].device_id;
      selected_label = (*choices)[*selected_index].label;
    }

    choices->clear();
    AudioDeviceChoice disabled;
    disabled.label = "Disabled";
    choices->push_back(disabled);
    AudioDeviceChoice system_default;
    system_default.enabled = true;
    system_default.label = "System default";
    choices->push_back(system_default);

    std::vector<rtc_audio::AudioDeviceInfo> devices;
    std::string error_message;
    const bool enumerated =
        capture ? rtc_audio::EnumerateCaptureDevices(&devices, &error_message)
                : rtc_audio::EnumeratePlaybackDevices(&devices,
                                                      &error_message);
    if (!enumerated) {
      AppendLog(error_message);
    } else {
      for (const rtc_audio::AudioDeviceInfo& device : devices) {
        AudioDeviceChoice choice;
        choice.enabled = true;
        choice.device_id = device.id;
        choice.label = device.name;
        choices->push_back(choice);
      }
    }

    *selected_index = selected_enabled ? 1 : 0;
    if (selected_enabled &&
        selected_device_id != rtc_audio::kSystemDefaultDeviceId) {
      bool restored = false;
      for (size_t i = 2; i < choices->size(); ++i) {
        if ((*choices)[i].device_id == selected_device_id) {
          *selected_index = static_cast<int>(i);
          restored = true;
          break;
        }
      }
      if (!restored && !selected_label.empty()) {
        for (size_t i = 2; i < choices->size(); ++i) {
          if ((*choices)[i].label == selected_label) {
            *selected_index = static_cast<int>(i);
            break;
          }
        }
      }
    }
  }

  bool DrawAudioDeviceCombo(const char* id,
                            const std::vector<AudioDeviceChoice>& choices,
                            int* selected_index) {
    if (selected_index == nullptr || choices.empty()) {
      return false;
    }
    if (*selected_index < 0 ||
        *selected_index >= static_cast<int>(choices.size())) {
      *selected_index = 0;
    }
    bool changed = false;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo(id, choices[*selected_index].label.c_str())) {
      for (int i = 0; i < static_cast<int>(choices.size()); ++i) {
        const bool selected = i == *selected_index;
        ImGui::PushID(i);
        if (ImGui::Selectable(choices[i].label.c_str(), selected)) {
          *selected_index = i;
          changed = true;
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    return changed;
  }

  void DrawUi() {
    ApplyPendingUiActions();
    ImGuiViewport content_viewport = *ImGui::GetMainViewport();
    const float navigation_height = ImGui::GetFrameHeightWithSpacing() + 16.0f;
    ImGui::SetNextWindowPos(content_viewport.WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(content_viewport.WorkSize.x, navigation_height), ImGuiCond_Always);
    const int previous_page = active_page_;
    const ImGuiWindowFlags navigation_flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##ConsolePages", nullptr, navigation_flags);
    if (ImGui::BeginTabBar("ConsolePages")) {
      const char* pages[] = {"Driving", "Connection & Devices", "Diagnostics"};
      for (int page = 0; page < 3; ++page) {
        if (ImGui::BeginTabItem(pages[page])) {
          active_page_ = page;
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
    ImGui::End();
    if (previous_page == 0 && active_page_ != 0) {
      gamepad_dog_actions_.StopForDisconnect(
          [this](vts_rtc::vehicle::DogActionType action, float speed, bool log_success) {
            return SendVehicleDogAction(action, speed, log_success);
          });
      SetVehicleEmergencyStop();
      SendVehicleDriveCommand(true);
      ClearVehicleInput();
      vehicle_control_enabled_ = false;
      CloseFullscreenVideo();
    }
    content_viewport.WorkPos.y += navigation_height;
    content_viewport.WorkSize.y = std::max(1.0f, content_viewport.WorkSize.y - navigation_height);
    UiLayout layout = ComputeUiLayout(content_viewport);
    if (active_page_ == 0) {
      UpdateVehicleInputFromKeyboard();
      MergeVehicleInputs();
      layout.video_pos.x = layout.viewport_pos.x;
      layout.video_size.x = std::max(1.0f, layout.viewport_size.x);
      layout.video_size.y = std::max(1.0f, layout.viewport_size.y - 2 * kPanelMargin);
      DrawVideoPanel(layout);
      DrawFullscreenVideoOverlay();
    } else if (active_page_ == 1) {
      layout.control_pos = layout.video_pos;
      layout.control_size = ImVec2(layout.video_size.x, std::max(1.0f, layout.viewport_size.y - 2 * kPanelMargin));
      DrawControlPanel(layout);
    } else {
      layout.control_pos = layout.video_pos;
      layout.control_size = ImVec2(layout.video_size.x, std::max(1.0f, layout.viewport_size.y - 2 * kPanelMargin));
      const float settings_height = ImGui::GetFrameHeightWithSpacing() + 16.0f;
      ImGui::SetNextWindowPos(layout.control_pos, ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(layout.control_size.x, settings_height), ImGuiCond_Always);
      ImGui::Begin("##DebugSettings", nullptr, navigation_flags);
      ImGui::Checkbox("Debug", &video_debug_enabled_);
      ImGui::End();
      layout.control_pos.y += settings_height + kPanelGap;
      layout.control_size.y = std::max(1.0f, layout.control_size.y - settings_height - kPanelGap);
      const float bottom = DrawNetStatsHint(layout);
      DrawEventLogHint(layout, bottom + kPanelGap);
    }
  }

  void DrawControlPanel(const UiLayout& layout) {
    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(layout.control_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.control_size, ImGuiCond_Always);

    ImGui::Begin("Connection & Devices", nullptr, window_flags);

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
      ImGui::Text("Local Audio     sent=%llu",
                  static_cast<unsigned long long>(local_audio_frames_.load()));
      ImGui::SameLine();
      DrawVehicleControlTargetSelector();

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted("Connection & Devices");
      ImGui::EndTable();
    }

    ImGui::Separator();
    const float body_height = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("ControlBody", 2,
                          ImGuiTableFlags_SizingStretchSame |
                              ImGuiTableFlags_NoSavedSettings,
                          ImVec2(-FLT_MIN, body_height))) {
      ImGui::TableSetupColumn("Controls", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);
      ImGui::TableSetupColumn("Feedback", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
      if (ImGui::BeginChild("ControlLeftCard", ImVec2(0, 0), false)) {
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
        ImGui::TextDisabled("AUDIO");
        ImGui::SameLine();
        const float audio_refresh_x =
            ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
            side_btn_w;
        ImGui::SetCursorPosX(audio_refresh_x);
        if (ImGui::Button("Refresh Audio", ImVec2(side_btn_w, 0))) {
          LoadAudioDeviceLists();
          ApplyAudioInputSelection();
          ApplyAudioOutputSelection();
        }
        if (ImGui::BeginTable("AudioDeviceSelectors", 2,
                              ImGuiTableFlags_SizingStretchSame |
                                  ImGuiTableFlags_NoSavedSettings)) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextDisabled("Input");
          if (DrawAudioDeviceCombo("##audio_input_combo",
                                   audio_input_choices_,
                                   &selected_audio_input_)) {
            ApplyAudioInputSelection();
          }
          ImGui::TableSetColumnIndex(1);
          ImGui::TextDisabled("Output");
          if (DrawAudioDeviceCombo("##audio_output_combo",
                                   audio_output_choices_,
                                   &selected_audio_output_)) {
            ApplyAudioOutputSelection();
          }
          ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextDisabled("ROOM");
        ImGui::TextUnformatted(open_room_id_);
        if (ImGui::Button("Open Room", ImVec2(action_btn_w, 0))) {
          EnsureAudioSource();
          const RtcErrorCode code =
              RtcOpenRoom(open_room_id_, RtcRoomType::VideoBroadcasting, false);
          AppendLogWithCode("RtcOpenRoom", code);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close Room", ImVec2(action_btn_w, 0))) {
          const RtcErrorCode code = RtcCloseRoom(open_room_id_);
          AppendLogWithCode("RtcCloseRoom", code);
          if (code == RtcErrorCode::OK) {
            ResetSessionStateOnUi();
            ResetVehicleControlOnUi(true);
          }
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
            ResetVehicleControlOnUi(true);
          }
        }
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);

      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
      if (ImGui::BeginChild("EdgeFeedbackCard", ImVec2(0, 0), false)) {
        DrawEdgeFeedbackPanel();
      }
      ImGui::EndChild();
      ImGui::PopStyleVar(2);

      ImGui::EndTable();
    }

    ImGui::End();
  }

  std::mutex device_identity_mutex_;
  rtc_console::DeviceSlotRegistry device_slots_;

  std::array<rtc_console::DeviceSlot, 4> SnapshotDeviceSlots() {
    std::lock_guard<std::mutex> lock(device_identity_mutex_);
    return device_slots_.Snapshot();
  }

  bool IsBoundSession(RtcSessionId sessionid) {
    if (sessionid == 0) return false;
    const auto slots = SnapshotDeviceSlots();
    return std::any_of(slots.begin(), slots.end(),
        [sessionid](const rtc_console::DeviceSlot& slot) {
          return slot.session_id == sessionid;
        });
  }

  std::string DeviceLabel(RtcSessionId sessionid) {
    std::lock_guard<std::mutex> lock(device_identity_mutex_);
    return device_slots_.Label(sessionid);
  }

  void DrawVehicleControlTargetSelector() {
    const uint32_t selected =
        vehicle_control_targets_.selected_target();
    const std::string preview =
        selected == 0 ? "(none)"
              : DeviceLabel(selected);

    ImGui::TextDisabled("Vehicle target");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", preview.c_str());
  }

  void DrawEdgeFeedbackPanel(bool compact = false) {
    if (!compact) {
      ImGui::TextDisabled("EDGE FEEDBACK");
    }

    const uint32_t selected_target =
        vehicle_control_targets_.selected_target();
    const bool target_ready =
        vehicle_control_targets_.selected_target_ready();
    EdgeFeedbackView feedback;
    bool has_feedback = false;
    if (selected_target != 0) {
      std::lock_guard<std::mutex> lock(edge_feedback_mutex_);
      const auto it = edge_feedback_by_session_.find(selected_target);
      if (it != edge_feedback_by_session_.end()) {
        feedback = it->second;
        has_feedback = true;
      }
    }

    if (compact) {
      ImGui::TextColored(vehicle_takeover_active_ ? ImVec4(0.35f, 0.80f, 0.50f, 1.0f)
                     : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
             "%s", vehicle_takeover_active_ ? "TAKEOVER" : "STANDBY");
      if (ImGui::BeginTable("DrivingStatus", 4,
                            ImGuiTableFlags_SizingStretchSame |
                            ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("LINK");
        ImGui::TextColored(target_ready ? ImVec4(0.35f, 0.80f, 0.50f, 1.0f)
                                        : ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                           "%s", target_ready ? "Ready" : "Not ready");
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("GEAR");
        ImGui::TextUnformatted(has_feedback ? VehicleGearText(feedback.state.active_gear) : "-");
        ImGui::TableSetColumnIndex(2);
        ImGui::TextDisabled("WATCHDOG");
        ImGui::TextUnformatted(!has_feedback ? "Waiting" : feedback.state.watchdog_stopped ? "Stopped" : "Active");
        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("FEEDBACK AGE");
        if (has_feedback) {
          const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - feedback.updated_at).count();
          ImGui::Text("%lld ms", static_cast<long long>(age));
        } else {
          ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
      }
      return;
    }
    if (ImGui::BeginTable("EdgeFeedbackTable", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_NoSavedSettings)) {
      ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed,
                              118.0f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Device");
      ImGui::TableSetColumnIndex(1);
      if (selected_target == 0) {
        ImGui::TextDisabled("Not selected");
      } else {
        ImGui::TextWrapped("%s", DeviceLabel(selected_target).c_str());
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Link");
      ImGui::TableSetColumnIndex(1);
      if (target_ready) {
        ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.50f, 1.0f), "Ready");
      } else {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "Not ready");
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Feedback seq");
      ImGui::TableSetColumnIndex(1);
      if (has_feedback) {
        ImGui::Text("%llu",
                    static_cast<unsigned long long>(feedback.envelope_seq));
      } else {
        ImGui::TextDisabled("Waiting");
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Gear");
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(
          has_feedback ? VehicleGearText(feedback.state.active_gear) : "-");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Last drive seq");
      ImGui::TableSetColumnIndex(1);
      if (has_feedback) {
        ImGui::Text("%llu", static_cast<unsigned long long>(
                                feedback.state.last_received_drive_seq));
      } else {
        ImGui::TextUnformatted("-");
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Watchdog");
      ImGui::TableSetColumnIndex(1);
      if (!has_feedback) {
        ImGui::TextUnformatted("-");
      } else if (feedback.state.watchdog_stopped) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Stopped");
      } else {
        ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.50f, 1.0f), "Active");
      }

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("Updated");
      ImGui::TableSetColumnIndex(1);
      if (has_feedback) {
        const auto now = std::chrono::steady_clock::now();
        const auto age = now >= feedback.updated_at
                             ? now - feedback.updated_at
                             : std::chrono::steady_clock::duration::zero();
        const auto age_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
        ImGui::Text("%lld ms ago", static_cast<long long>(age_ms));
      } else {
        ImGui::TextUnformatted("-");
      }

      ImGui::EndTable();
    }
  }

  void ClearVehicleInput() {
    vehicle_takeover_active_ = false;
    steering_wheel_angle_deg_ = 0.0;
    vehicle_keyboard_input_ = rtc_console::VehicleControlInput();
    vehicle_gamepad_input_ = rtc_console::VehicleControlInput();
    vehicle_input_ = rtc_console::VehicleControlInput();
    vehicle_keyboard_active_ = false;
    vehicle_gamepad_active_ = false;
  }

  void SetVehicleEmergencyStop() {
    vehicle_keyboard_input_ = rtc_console::VehicleControlInput();
    vehicle_gamepad_input_ = rtc_console::VehicleControlInput();
    vehicle_input_ = rtc_console::VehicleControlInput();
    vehicle_input_.emergency_stop = true;
    vehicle_keyboard_active_ = false;
    vehicle_gamepad_active_ = false;
  }

  void AdjustVehicleThrottle(float delta) {
    const float previous = vehicle_throttle_;
    vehicle_throttle_ =
        rtc_console::ClampVehicleThrottle(vehicle_throttle_ + delta);
    if (vehicle_throttle_ != previous) {
      std::ostringstream oss;
      oss << "Vehicle speed set to " << vehicle_throttle_;
      AppendLog(oss.str());
    }
  }

  bool ActionPressed(const std::string& action, bool repeat = false) const {
    for (const auto key : key_bindings_.at(action)) {
      if (ImGui::IsKeyPressed(key, repeat)) return true;
    }
    return false;
  }

  bool ActionDown(const std::string& action) const {
    for (const auto key : key_bindings_.at(action)) {
      if (ImGui::IsKeyDown(key)) return true;
    }
    return false;
  }

  void UpdateVehicleInputFromKeyboard() {
    if (ActionPressed("toggle_interleave")) {
      render_lr_pixel_interleave_ = !render_lr_pixel_interleave_;
    }

    const std::string selection_actions[] = {
      "select_1", "select_2", "select_3", "select_4"};
    if (viewed_session_ != 0 && ActionPressed("toggle_view")) {
      const auto mode = video_view_state_.Get(viewed_session_);
      SendVideoViewControl(mode == vts_rtc::vision::ViewMode::Binocular
                               ? vts_rtc::vision::ViewMode::Surround
                               : vts_rtc::vision::ViewMode::Binocular,
                           viewed_session_);
    }
    for (size_t slot = 0; slot < 4; ++slot) {
      if (!ActionPressed(selection_actions[slot])) {
        continue;
      }
      if (viewed_session_ != 0 &&
          video_view_state_.Get(viewed_session_) != vts_rtc::vision::ViewMode::Binocular) {
        const auto requested_mode = static_cast<vts_rtc::vision::ViewMode>(
          static_cast<uint32_t>(vts_rtc::vision::ViewMode::SurroundFront) + slot);
        SendVideoViewControl(video_view_state_.Get(viewed_session_) == requested_mode
                     ? vts_rtc::vision::ViewMode::Surround
                     : requested_mode,
                             viewed_session_);
        break;
      }
      const auto slots = SnapshotDeviceSlots();
      if (slots[slot].session_id == 0) {
        AppendLog("Vehicle takeover skipped: empty video slot");
        return;
      }
      const auto target = slots[slot].session_id;
      const auto next_view = viewed_session_ == target ? 0 : target;
      if (!vehicle_takeover_active_) {
        viewed_session_ = next_view;
        return;
      }
      const bool lateral_stopped = gamepad_dog_actions_.StopForDisconnect(
          [this](vts_rtc::vehicle::DogActionType action, float speed, bool log_success) {
            return SendVehicleDogAction(action, speed, log_success);
          });
      SetVehicleEmergencyStop();
      const bool previous_ready = vehicle_control_targets_.selected_target_ready();
      const bool previous_stopped = SendVehicleDriveCommand(true);
      ClearVehicleInput();
      vehicle_control_enabled_ = false;
      if ((previous_ready && !previous_stopped) ||
          (!lateral_stopped && gamepad_dog_actions_.lateral_active())) {
        AppendLog("Vehicle takeover cancelled: previous target stop failed");
        return;
      }
      viewed_session_ = next_view;
      return;
    }

    if (ActionPressed("release_takeover")) {
      if (vehicle_takeover_active_) {
        gamepad_dog_actions_.StopForDisconnect(
            [this](vts_rtc::vehicle::DogActionType action, float speed, bool log_success) {
              return SendVehicleDogAction(action, speed, log_success);
            });
        SetVehicleEmergencyStop();
        SendVehicleDriveCommand(true);
        ClearVehicleInput();
        vehicle_control_enabled_ = false;
      }
      return;
    }
    const bool brake_takeover = !ImGui::GetIO().WantTextInput &&
      ActionPressed("brake_takeover");
    if ((ActionPressed("takeover") || brake_takeover) &&
      !vehicle_takeover_active_) {
      const auto targets = vehicle_control_targets_.AvailableTargets();
      if (!IsBoundSession(viewed_session_) ||
          std::find(targets.begin(), targets.end(), viewed_session_) == targets.end()) {
        AppendLog("Vehicle takeover skipped: viewed target is not ready");
        return;
      }
      if (vehicle_control_targets_.SelectTarget(viewed_session_) &&
          vehicle_control_targets_.selected_target_ready()) {
        ClearVehicleInput();
        if (brake_takeover) {
          vehicle_keyboard_input_.emergency_stop = true;
          vehicle_keyboard_active_ = true;
          MergeVehicleInputs();
        }
        if (SendVehicleDriveCommand(true)) {
          vehicle_takeover_active_ = true;
          vehicle_control_enabled_ = true;
          AppendLog("Vehicle takeover activated: " + std::to_string(viewed_session_));
        }
      }
      return;
    }

    if (!vehicle_takeover_active_) {
      return;
    }
    if (ImGui::GetIO().WantTextInput) {
      vehicle_keyboard_input_ = rtc_console::VehicleControlInput();
      vehicle_keyboard_active_ = false;
      return;
    }
    if (ActionPressed("gear_forward")) {
      SendVehicleGear(vts_rtc::vehicle::VehicleGear::Forward);
    } else if (ActionPressed("gear_neutral")) {
      SendVehicleGear(vts_rtc::vehicle::VehicleGear::Neutral);
    } else if (ActionPressed("gear_reverse")) {
      SendVehicleGear(vts_rtc::vehicle::VehicleGear::Reverse);
    }
    if (ActionPressed("throttle_up", true)) {
      AdjustVehicleThrottle(kVehicleThrottleStep);
    }
    if (ActionPressed("throttle_down", true)) {
      AdjustVehicleThrottle(-kVehicleThrottleStep);
    }
    if (ActionPressed("stop_and_release")) {
      SetVehicleEmergencyStop();
      SendVehicleDriveCommand(true);
      ClearVehicleInput();
      vehicle_control_enabled_ = false;
      return;
    }

    const bool forward = ActionDown("forward");
    const bool reverse = ActionDown("reverse");
    const bool left = ActionDown("left");
    const bool right = ActionDown("right");
    steering_wheel_angle_deg_ = rtc_console::UpdateSteeringAngle(
      steering_wheel_angle_deg_, left, right, ImGui::GetIO().DeltaTime,
      steering_wheel_range_deg_);
    const bool emergency_stop = ActionDown("brake_takeover") ||
      ActionDown("emergency_stop");
    const bool any_key =
        forward || reverse || left || right || emergency_stop;
    if (!any_key) {
      if (vehicle_keyboard_active_) {
        vehicle_keyboard_input_ = rtc_console::VehicleControlInput();
        vehicle_keyboard_active_ = false;
        MergeVehicleInputs();
        SendVehicleDriveCommand(true);
        vehicle_control_enabled_ = vehicle_gamepad_active_;
      }
      return;
    }

    vehicle_keyboard_input_.forward = forward;
    vehicle_keyboard_input_.reverse = reverse;
    vehicle_keyboard_input_.left = left;
    vehicle_keyboard_input_.right = right;
    vehicle_keyboard_input_.emergency_stop = emergency_stop;
    vehicle_keyboard_active_ = true;
    vehicle_control_enabled_ = true;
  }

  float NormalizeGamepadAxis(Sint16 value) const {
    const float normalized =
        value < 0 ? static_cast<float>(value) / 32768.0f
                  : static_cast<float>(value) / 32767.0f;
    return std::fabs(normalized) < kGamepadAxisDeadzone ? 0.0f : normalized;
  }

  float NormalizeGamepadTrigger(Sint16 value) const {
    const float normalized =
        value <= 0 ? 0.0f : static_cast<float>(value) / 32767.0f;
    return normalized < kGamepadTriggerThreshold ? 0.0f : normalized;
  }

  void CloseGamepad() {
    if (gamepad_) {
      SDL_CloseGamepad(gamepad_);
      gamepad_ = nullptr;
    }
  }

  bool EnsureGamepad() {
    if (gamepad_ && SDL_GamepadConnected(gamepad_)) {
      return true;
    }
    CloseGamepad();

    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (!gamepads || count <= 0) {
      if (gamepads) {
        SDL_free(gamepads);
      }
      return false;
    }

    gamepad_ = SDL_OpenGamepad(gamepads[0]);
    SDL_free(gamepads);
    if (!gamepad_) {
      return false;
    }

    const char* name = SDL_GetGamepadName(gamepad_);
    AppendLog(std::string("Gamepad connected: ") +
              (name ? name : "unknown"));
    return true;
  }

  bool IsGamepadButtonDown(SDL_GamepadButton button) const {
    return gamepad_ && SDL_GetGamepadButton(gamepad_, button);
  }

  void UpdateVehicleInputFromGamepad() {
    if (!vehicle_takeover_active_) {
      return;
    }
    if (!EnsureGamepad()) {
      const bool drive_was_active = vehicle_gamepad_active_;
      const bool lateral_was_active = gamepad_dog_actions_.lateral_active();
      const bool lateral_stopped = gamepad_dog_actions_.StopForDisconnect(
          [this](vts_rtc::vehicle::DogActionType action, float speed,
                 bool log_success) {
            return SendVehicleDogAction(action, speed, log_success);
          });
      if (vehicle_gamepad_active_) {
        vehicle_gamepad_input_ = rtc_console::VehicleControlInput();
        vehicle_gamepad_active_ = false;
        MergeVehicleInputs();
        SendVehicleDriveCommand(true);
        vehicle_control_enabled_ = vehicle_keyboard_active_;
      }
      if (drive_was_active ||
          (lateral_was_active && lateral_stopped)) {
        AppendLog("Gamepad disconnected; gamepad input cleared");
      }
      return;
    }

    const bool dpad_up =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_DPAD_UP);
    const bool dpad_down =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    const bool dpad_left =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    const bool dpad_right =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    const bool stand =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_NORTH);
    const bool lie_down =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_SOUTH);
    const bool brake =
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ||
        IsGamepadButtonDown(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    const float rotation =
        NormalizeGamepadAxis(SDL_GetGamepadAxis(gamepad_,
                                                SDL_GAMEPAD_AXIS_RIGHTX));
    const float speed_up =
        NormalizeGamepadTrigger(SDL_GetGamepadAxis(gamepad_,
                                                   SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    const float speed_down =
        NormalizeGamepadTrigger(SDL_GetGamepadAxis(gamepad_,
                                                   SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    const uint64_t now_ms = SteadyTimeMs();
    if (speed_up > 0.0f &&
        now_ms - last_gamepad_throttle_change_ms_ >=
            kGamepadThrottleRepeatMs) {
      AdjustVehicleThrottle(kVehicleThrottleStep);
      last_gamepad_throttle_change_ms_ = now_ms;
    }
    if (speed_down > 0.0f &&
        now_ms - last_gamepad_throttle_change_ms_ >=
            kGamepadThrottleRepeatMs) {
      AdjustVehicleThrottle(-kVehicleThrottleStep);
      last_gamepad_throttle_change_ms_ = now_ms;
    }

    gamepad_dog_actions_.Update(
        dpad_left, dpad_right, stand, lie_down, vehicle_throttle_, now_ms,
        [this](vts_rtc::vehicle::DogActionType action, float speed,
               bool log_success) {
          return SendVehicleDogAction(action, speed, log_success);
        });

    const bool active =
        dpad_up || dpad_down || brake || rotation != 0.0f;
    if (!active) {
      if (vehicle_gamepad_active_) {
        vehicle_gamepad_input_ = rtc_console::VehicleControlInput();
        vehicle_gamepad_active_ = false;
        MergeVehicleInputs();
        SendVehicleDriveCommand(true);
        vehicle_control_enabled_ = vehicle_keyboard_active_;
      }
      return;
    }

    vehicle_gamepad_input_ = rtc_console::VehicleControlInput();
    vehicle_gamepad_input_.forward = dpad_up && !dpad_down;
    vehicle_gamepad_input_.reverse = dpad_down && !dpad_up;
    vehicle_gamepad_input_.left = rotation < 0.0f;
    vehicle_gamepad_input_.right = rotation > 0.0f;
    vehicle_gamepad_input_.emergency_stop = brake;
    vehicle_gamepad_active_ = true;
    vehicle_control_enabled_ = true;
  }

  void MergeVehicleInputs() {
    if (vehicle_keyboard_input_.emergency_stop || vehicle_gamepad_input_.emergency_stop) {
      vehicle_input_ = rtc_console::VehicleControlInput();
      vehicle_input_.emergency_stop = true;
      return;
    }
    if (vehicle_gamepad_active_) {
      vehicle_input_ = vehicle_gamepad_input_;
      return;
    }
    if (vehicle_keyboard_active_) {
      vehicle_input_ = vehicle_keyboard_input_;
      return;
    }
    vehicle_input_ = rtc_console::VehicleControlInput();
  }

  vts_rtc::vehicle::DriveCommand CurrentVehicleDriveCommand() const {
    rtc_console::VehicleControlInput input = vehicle_input_;
    input.throttle = vehicle_throttle_;
    return rtc_console::MakeVehicleDriveCommand(input);
  }

  bool IsSameVehicleCommand(
      const vts_rtc::vehicle::DriveCommand& lhs,
      const vts_rtc::vehicle::DriveCommand& rhs) const {
    return lhs.drive_direction == rhs.drive_direction &&
           lhs.steering_direction == rhs.steering_direction &&
           lhs.throttle == rhs.throttle && lhs.brake == rhs.brake;
  }

  void TickVehicleControl(uint64_t now_ms) {
    if (!vehicle_control_enabled_ && !vehicle_takeover_active_) {
      return;
    }
    if (!vehicle_control_targets_.selected_target_ready()) {
      if (vehicle_takeover_active_) {
        ClearVehicleInput();
        vehicle_control_enabled_ = false;
      }
      return;
    }
    if (now_ms - last_vehicle_control_sent_ms_ < kVehicleDriveIntervalMs) {
      return;
    }
    last_vehicle_control_sent_ms_ = now_ms;
    SendVehicleDriveCommand(false);
  }

  bool SendVehicleDriveCommand(bool force_log) {
    const RtcSessionId target =
        vehicle_control_targets_.selected_target();
    if (!IsBoundSession(target)) {
      if (force_log) {
        AppendLog("Vehicle control skipped: select a vehicle target");
      }
      return false;
    }
    if (!vehicle_control_targets_.selected_target_ready()) {
      if (force_log) {
        AppendLog("Vehicle control skipped: selected target is not ready");
      }
      return false;
    }

    const vts_rtc::vehicle::DriveCommand command =
        CurrentVehicleDriveCommand();
    const vts_rtc::vehicle::EncodeResult encoded =
        vts_rtc::vehicle::EncodeDriveCommand(vehicle_control_seq_++, command);
    if (!encoded) {
      AppendLog(std::string("Vehicle control encode failed: ") +
                encoded.error_message);
      return false;
    }

    const RtcErrorCode code = RtcSendData(
        target, vts_rtc::vehicle::kVehicleControlChannelLabel,
        reinterpret_cast<const char*>(encoded.payload.data()),
        encoded.payload.size());
    const bool ok = code == RtcErrorCode::OK;
    const bool command_changed =
        !has_last_vehicle_command_ ||
        !IsSameVehicleCommand(command, last_vehicle_command_);
    if (!ok) {
      const uint64_t now_ms = SteadyTimeMs();
      if (force_log || now_ms - last_vehicle_send_error_log_ms_ >= 1000) {
        AppendLogWithCode("RtcSendData(vehicle.control.v1)", code);
        last_vehicle_send_error_log_ms_ = now_ms;
      }
      return false;
    }

    if (force_log || command_changed) {
      std::ostringstream oss;
      oss << "Vehicle control sent to " << target
          << " drive=" << DriveDirectionText(command.drive_direction)
          << " steering=" << SteeringDirectionText(command.steering_direction)
          << " throttle=" << command.throttle
          << " brake=" << command.brake;
      AppendLog(oss.str());
    }
    last_vehicle_command_ = command;
    has_last_vehicle_command_ = true;
    return true;
  }

  bool SendVehicleGear(vts_rtc::vehicle::VehicleGear gear) {
    const RtcSessionId target = vehicle_control_targets_.selected_target();
    if (!vehicle_takeover_active_ || !IsBoundSession(target) ||
        !vehicle_control_targets_.selected_target_ready()) {
      AppendLog("Gear change skipped: takeover target is not ready");
      return false;
    }

    vts_rtc::vehicle::SetGear command;
    command.request_id = vehicle_event_request_id_++;
    command.gear = gear;
    const auto encoded =
        vts_rtc::vehicle::EncodeSetGear(vehicle_control_seq_++, command);
    if (!encoded) {
      AppendLog(std::string("Gear change encode failed: ") + encoded.error_message);
      return false;
    }
    const RtcErrorCode code = RtcSendData(
        target, vts_rtc::vehicle::kVehicleEventChannelLabel,
        reinterpret_cast<const char*>(encoded.payload.data()),
        encoded.payload.size());
    if (code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcSendData(vehicle.event.v1 set_gear)", code);
      return false;
    }
    AppendLog(std::string("Gear change queued: ") + VehicleGearText(gear));
    return true;
  }

  bool SendVehicleDogAction(vts_rtc::vehicle::DogActionType action,
                            float speed,
                            bool log_success = true) {
    const RtcSessionId target =
        vehicle_control_targets_.selected_target();
    if (!IsBoundSession(target)) {
      AppendLog("Dog action skipped: select a vehicle target");
      return false;
    }
    if (!vehicle_control_targets_.selected_target_ready()) {
      AppendLog("Dog action skipped: selected target is not ready");
      return false;
    }

    vts_rtc::vehicle::DogAction dog_action;
    dog_action.request_id = vehicle_event_request_id_++;
    dog_action.action = action;
    dog_action.speed = speed;
    const vts_rtc::vehicle::EncodeResult encoded =
        vts_rtc::vehicle::EncodeDogAction(vehicle_control_seq_++, dog_action);
    if (!encoded) {
      AppendLog(std::string("Dog action encode failed: ") +
                encoded.error_message);
      return false;
    }

    const RtcErrorCode code = RtcSendData(
        target, vts_rtc::vehicle::kVehicleEventChannelLabel,
        reinterpret_cast<const char*>(encoded.payload.data()),
        encoded.payload.size());
    if (code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcSendData(vehicle.event.v1 dog_action)", code);
      return false;
    }

    if (log_success) {
      AppendLog(std::string("Dog action queued: ") + DogActionText(action));
    }
    vehicle_event_acks_.TrackDogAction(dog_action.request_id, action,
                                       log_success);
    return true;
  }

  bool SendVideoViewControl(vts_rtc::vision::ViewMode enable_view,
                            RtcSessionId target = 0) {
    if (target == 0) target = vehicle_control_targets_.selected_target();
    if (!IsBoundSession(target)) return false;
    if (target == 0) {
      AppendLog("Video view control skipped: select a vehicle target");
      return false;
    }
    const vts_rtc::vision::ViewControlEncodeResult encoded =
        vts_rtc::vision::EncodeViewControl(video_view_control_seq_++,
                                           enable_view);
    if (!encoded) {
      AppendLog(std::string("Video view control encode failed: ") +
                encoded.error_message);
      return false;
    }

    const RtcErrorCode code = RtcSendData(
        target, vts_rtc::vision::kVideoViewControlChannelLabel,
        reinterpret_cast<const char*>(encoded.payload.data()),
        encoded.payload.size());
    if (code != RtcErrorCode::OK) {
      AppendLogWithCode("RtcSendData(video.view_control.v1)", code);
      return false;
    }

    video_view_state_.Set(target, enable_view);
    video_view_frame_thresholds_[target] = remote_video_frame_seq_.load();
    std::ostringstream oss;
    oss << "Video view control sent to " << target
        << " enable_view=" << vts_rtc::vision::ViewModeText(enable_view);
    AppendLog(oss.str());
    return true;
  }

  using VideoFramesByTile =
      std::map<rtc_console::VideoTileId, std::shared_ptr<VideoFrameView>>;

  struct VideoSessionSnapshot {
    RtcSessionId remote_sessionid = 0;
    std::string device_id;
    bool identity_conflict = false;
    vts_rtc::vision::ViewMode mode = vts_rtc::vision::ViewMode::Binocular;
    VideoFramesByTile frames_by_tile;
    std::shared_ptr<VideoFrameView> surround_grid;
  };

  std::map<RtcSessionId, VideoSessionSnapshot> displayed_video_sessions_;
  std::map<RtcSessionId, ImVec2> surround_window_positions_;

  std::vector<VideoSessionSnapshot> SnapshotVideoSessions() {
    std::map<RtcSessionId, VideoFramesByTile> frames_by_session;
    std::map<RtcSessionId, std::shared_ptr<VideoFrameView>> grids;
    {
      std::lock_guard<std::mutex> lock(video_mutex_);
      for (const auto& item : remote_video_frames_by_source_) {
        if (!item.second) {
          continue;
        }
        rtc_console::VideoTileId tile;
        const auto sessionid = item.second->remote_sessionid;
        frames_by_session[sessionid];
        const auto threshold = video_view_frame_thresholds_.find(sessionid);
        if (threshold != video_view_frame_thresholds_.end() &&
            item.second->frame_seq <= threshold->second) {
          continue;
        }
        const auto mode = video_view_state_.Get(sessionid);
        if (item.second->source_id == "surround_grid") {
          if (mode == vts_rtc::vision::ViewMode::Binocular ||
              mode == vts_rtc::vision::ViewMode::Surround) {
            grids[sessionid] = item.second;
          }
          continue;
        }
        if (!VideoTileForSourceId(item.second->source_id, &tile)) {
          continue;
        }
        if (tile == rtc_console::VideoTileId::Stereo) {
          if (mode != vts_rtc::vision::ViewMode::Binocular) continue;
        } else if (static_cast<uint32_t>(mode) != static_cast<uint32_t>(tile) + 2) {
          continue;
        }
        frames_by_session[item.second->remote_sessionid][tile] = item.second;
      }
    }

    std::vector<VideoSessionSnapshot> sessions;
    for (const auto& item : frames_by_session) {
      VideoSessionSnapshot session;
      session.remote_sessionid = item.first;
      session.mode = video_view_state_.Get(item.first);
      session.frames_by_tile = item.second;
      session.surround_grid = grids[item.first];
      const bool ready = session.mode == vts_rtc::vision::ViewMode::Binocular
          ? session.surround_grid && session.frames_by_tile.count(
                rtc_console::VideoTileId::Stereo) != 0
          : session.mode == vts_rtc::vision::ViewMode::Surround
              ? static_cast<bool>(session.surround_grid)
              : !session.frames_by_tile.empty();
      const auto previous = displayed_video_sessions_.find(item.first);
      if (ready) {
        displayed_video_sessions_[item.first] = session;
      } else if (previous != displayed_video_sessions_.end()) {
        session = previous->second;
      }
      sessions.push_back(session);
    }
    for (auto previous = displayed_video_sessions_.begin();
         previous != displayed_video_sessions_.end();) {
      if (frames_by_session.count(previous->first) == 0) {
        surround_window_positions_.erase(previous->first);
        previous = displayed_video_sessions_.erase(previous);
      } else {
        ++previous;
      }
    }

    const auto slots = SnapshotDeviceSlots();
    std::vector<VideoSessionSnapshot> fixed_sessions(4);
    for (size_t index = 0; index < slots.size(); ++index) {
      const auto found = std::find_if(sessions.begin(), sessions.end(),
          [&slots, index](const VideoSessionSnapshot& session) {
            return slots[index].session_id != 0 &&
                   session.remote_sessionid == slots[index].session_id;
          });
      if (found != sessions.end()) fixed_sessions[index] = *found;
      fixed_sessions[index].remote_sessionid = slots[index].session_id;
      fixed_sessions[index].device_id = slots[index].device_id;
      fixed_sessions[index].identity_conflict = slots[index].conflict;
    }
    return fixed_sessions;
  }

  const std::shared_ptr<VideoFrameView>* FindVideoFrame(
      const VideoFramesByTile& frames,
      rtc_console::VideoTileId tile) const {
    const auto it = frames.find(tile);
    return it == frames.end() ? nullptr : &it->second;
  }

  float StereoAspect(const VideoFramesByTile& frames) const {
    const std::shared_ptr<VideoFrameView>* frame =
        FindVideoFrame(frames, rtc_console::VideoTileId::Stereo);
    if (frame != nullptr && *frame && (*frame)->width > 0 &&
        (*frame)->height > 0) {
      return static_cast<float>((*frame)->width) /
             static_cast<float>((*frame)->height);
    }
    return 32.0f / 9.0f;
  }

  void DrawVideoPanel(const UiLayout& layout) {
    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(layout.video_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(layout.video_size, ImGuiCond_Always);

    ImGui::Begin("Driving Monitor", nullptr, window_flags);

    const std::vector<VideoSessionSnapshot> sessions = SnapshotVideoSessions();

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 panel_avail = ImGui::GetContentRegionAvail();
    const auto selected_session = std::find_if(
      sessions.begin(), sessions.end(),
      [this](const VideoSessionSnapshot& session) {
          return session.remote_sessionid == viewed_session_;
      });
    const bool focus_takeover = viewed_session_ != 0 &&
      selected_session != sessions.end();
    const int columns = 2;
    const int rows = std::max(2,
        static_cast<int>((sessions.size() + static_cast<size_t>(columns) - 1) /
               static_cast<size_t>(columns)));
    const float cell_width = std::max(
        1.0f, (panel_avail.x -
               style.ItemSpacing.x * static_cast<float>(columns - 1)) /
                  static_cast<float>(columns));
    const float cell_height = std::max(
        1.0f, (panel_avail.y -
               style.ItemSpacing.y * static_cast<float>(rows - 1)) /
                  static_cast<float>(rows));
    const ImVec2 grid_origin = ImGui::GetCursorScreenPos();

    const size_t slot_count = std::max(size_t(4), sessions.size());
    for (size_t i = 0; i < slot_count; ++i) {
      if (focus_takeover &&
          (i >= sessions.size() || sessions[i].remote_sessionid != selected_session->remote_sessionid)) {
        continue;
      }
      const int column = focus_takeover ? 0 : static_cast<int>(i % static_cast<size_t>(columns));
      const int row = focus_takeover ? 0 : static_cast<int>(i / static_cast<size_t>(columns));
      ImGui::SetCursorScreenPos(ImVec2(
          grid_origin.x +
              static_cast<float>(column) * (cell_width + style.ItemSpacing.x),
          grid_origin.y +
              static_cast<float>(row) * (cell_height + style.ItemSpacing.y)));
      const auto& session = sessions[i];
      const ImVec2 device_origin = ImGui::GetCursorScreenPos();
      const ImVec2 device_size = focus_takeover ? panel_avail : ImVec2(cell_width, cell_height);
      const bool has_video = session.remote_sessionid != 0 &&
          (!session.frames_by_tile.empty() || session.surround_grid);
      ImDrawList* draw_list = ImGui::GetWindowDrawList();
      if (has_video) {
        DrawVideoComposite(session, device_size);
      } else {
        draw_list->AddRectFilled(device_origin,
            ImVec2(device_origin.x + device_size.x, device_origin.y + device_size.y),
            IM_COL32(24, 26, 28, 255));
      }
      const bool takeover = vehicle_takeover_active_ &&
          session.remote_sessionid != 0 &&
          session.remote_sessionid == vehicle_control_targets_.selected_target();
      EdgeFeedbackView feedback;
      bool has_feedback = false;
      {
        std::lock_guard<std::mutex> lock(edge_feedback_mutex_);
        const auto found = edge_feedback_by_session_.find(session.remote_sessionid);
        if (found != edge_feedback_by_session_.end()) {
          feedback = found->second;
          has_feedback = true;
        }
      }
      const auto ready_targets = vehicle_control_targets_.AvailableTargets();
      const bool ready = session.remote_sessionid != 0 &&
          std::find(ready_targets.begin(), ready_targets.end(),
                    session.remote_sessionid) != ready_targets.end();
      std::ostringstream feedback_age;
      if (has_feedback) {
        feedback_age << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - feedback.updated_at).count()
                    << " ms";
      } else {
        feedback_age << "-";
      }
      const std::string label = session.device_id.empty() ? "-" : session.device_id;
      const float badge_width = std::max(1.0f, (device_size.x - 20.0f) * 0.5f);
      draw_list->PushClipRect(device_origin,
          ImVec2(device_origin.x + device_size.x, device_origin.y + device_size.y), true);
      const auto draw_badge = [&](const char* text, bool right, ImU32 color) {
        const ImVec2 measured = ImGui::CalcTextSize(text);
        const float text_width = std::max(1.0f, badge_width - 8.0f);
        const float scale = std::min(1.0f, std::min(
          text_width / std::max(1.0f, measured.x),
          std::max(1.0f, device_size.y - 12.0f) / std::max(1.0f, measured.y)));
        const float width = measured.x * scale + 8.0f;
        const float height = measured.y * scale + 4.0f;
        const ImVec2 position(
            right ? device_origin.x + device_size.x - 4.0f - width
                  : device_origin.x + 4.0f,
            device_origin.y + 4.0f);
        draw_list->AddRectFilled(position,
            ImVec2(position.x + width, position.y + height), IM_COL32(0, 0, 0, 180));
        draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
            ImVec2(position.x + 4.0f, position.y + 2.0f), color, text);
      };
      draw_badge(label.c_str(), false, IM_COL32(255, 255, 255, 255));
        const bool selected = session.remote_sessionid != 0 &&
          session.remote_sessionid == viewed_session_;
        if (selected || takeover) {
        const float row_height = ImGui::GetFontSize() + 7.0f;
        const float panel_height = row_height * 9.0f + 28.0f;
        const float panel_scale = std::min(1.0f, std::min(
          badge_width / 200.0f,
          std::max(1.0f, device_size.y - 12.0f) / panel_height));
        const float panel_width = 200.0f * panel_scale;
        const ImVec2 panel_origin(device_origin.x + device_size.x - panel_width - 6.0f,
                    device_origin.y + 6.0f);
        const ImVec2 panel_end(panel_origin.x + panel_width,
                   panel_origin.y + panel_height * panel_scale);
        const ImU32 muted = IM_COL32(160, 168, 176, 255);
        const ImU32 normal = IM_COL32(235, 239, 242, 255);
        const ImU32 green = IM_COL32(107, 218, 154, 255);
        const ImU32 warning = IM_COL32(245, 187, 91, 255);
        const ImU32 danger = IM_COL32(255, 115, 115, 255);
        const ImU32 blue = IM_COL32(100, 190, 255, 255);
        draw_list->AddRectFilled(panel_origin, panel_end, IM_COL32(17, 20, 24, 72),
                     4.0f * panel_scale);
        draw_list->AddRect(panel_origin, panel_end, IM_COL32(110, 122, 134, 45),
                 4.0f * panel_scale);
        const auto panel_text = [&](const char* text, float top, bool right,
                      ImU32 color, float inset) {
        const float available = std::max(1.0f, 200.0f - inset - 12.0f);
        const float text_scale = std::min(1.0f,
          available / std::max(1.0f, ImGui::CalcTextSize(text).x));
        const float font_size = ImGui::GetFontSize() * panel_scale * text_scale;
        const float width = ImGui::CalcTextSize(text).x * panel_scale * text_scale;
        draw_list->AddText(ImGui::GetFont(), font_size,
          ImVec2(right ? panel_end.x - 12.0f * panel_scale - width
                 : panel_origin.x + inset * panel_scale,
               panel_origin.y + top * panel_scale), color, text);
        };
        const ImU32 connection_color = ready ? green : danger;
        draw_list->AddCircleFilled(
          ImVec2(panel_origin.x + 15.0f * panel_scale,
             panel_origin.y + (12.0f + ImGui::GetFontSize() * 0.5f) * panel_scale),
          3.0f * panel_scale, connection_color);
        panel_text(ready ? "CONTROL READY" : "CONTROL UNAVAILABLE",
             12.0f, false, connection_color, 25.0f);
          panel_text(takeover ? "TAKEOVER" : "STANDBY", 12.0f + row_height,
               false, takeover ? blue : muted, 12.0f);
        const float divider_y = panel_origin.y + (12.0f + row_height * 2.0f) * panel_scale;
        draw_list->AddLine(ImVec2(panel_origin.x + 12.0f * panel_scale, divider_y),
          ImVec2(panel_end.x - 12.0f * panel_scale, divider_y),
          IM_COL32(110, 122, 134, 70));
        const std::string age = feedback_age.str();
        const bool brake_requested = takeover && vehicle_input_.emergency_stop;
        const std::string throttle =
          std::to_string(static_cast<int>(vehicle_throttle_ * 100.0f + 0.5f)) + "%";
        const auto steering = CurrentVehicleDriveCommand().steering_direction;
        const char* steering_text = !takeover ? "-"
          : steering == vts_rtc::vehicle::SteeringDirection::Left ? "Left"
          : steering == vts_rtc::vehicle::SteeringDirection::Right ? "Right" : "Center";
        double video_delay_ms = -1;
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          for (const auto& item : net_stats_) {
            const auto& stats = item.second;
            if (stats.input && stats.session_id == session.remote_sessionid &&
                stats.delay.estimated_receive_ms >= 0) {
              video_delay_ms = std::max(video_delay_ms, stats.delay.estimated_receive_ms);
            }
          }
        }
        std::ostringstream video_delay;
        if (video_delay_ms >= 0) {
          video_delay << std::fixed << std::setprecision(1) << video_delay_ms << " ms";
        } else {
          video_delay << "--";
        }
        const std::string video_delay_text = video_delay.str();
        const char* names[] = {"GEAR", "SAFETY", "THROTTLE SET", "WHEEL SIM", "BRAKE INPUT", "FEEDBACK AGE", "VIDEO DELAY"};
        const char* values[] = {
          has_feedback ? VehicleGearText(feedback.state.active_gear) : "-",
          !has_feedback ? "Unknown" : feedback.state.watchdog_stopped ? "Protected" : "Clear",
          throttle.c_str(),
          steering_text,
          brake_requested ? "Applied" : "Released",
          age.c_str(), video_delay_text.c_str()};
        const ImU32 colors[] = {normal,
          !has_feedback ? muted : feedback.state.watchdog_stopped ? warning : green,
          takeover ? blue : muted,
          takeover ? blue : muted,
          brake_requested ? warning : muted, muted,
          video_delay_ms >= 0 ? normal : muted};
        for (size_t row = 0; row < 7; ++row) {
        const float top = 20.0f + row_height * static_cast<float>(row + 2);
        panel_text(names[row], top, false, muted, 12.0f);
        if (row == 3) {
          const float track_left = panel_origin.x + 112.0f * panel_scale;
          const float track_right = panel_end.x - 12.0f * panel_scale;
          const float track_center = (track_left + track_right) * 0.5f;
          const float track_y = panel_origin.y +
              (top + ImGui::GetFontSize() * 0.5f) * panel_scale;
          const float tick_height = 5.0f * panel_scale;
          draw_list->AddRectFilled(
              ImVec2(track_left, track_y - 2.0f * panel_scale),
              ImVec2(track_right, track_y + 2.0f * panel_scale),
              IM_COL32(160, 168, 176, 90), 2.0f * panel_scale);
          for (const float tick_x : {track_left, track_center, track_right}) {
            draw_list->AddLine(ImVec2(tick_x, track_y - tick_height),
                ImVec2(tick_x, track_y + tick_height),
                tick_x == track_center ? normal : muted,
                std::max(1.0f, panel_scale));
          }
          if (takeover) {
            const float cursor_x = track_center +
              static_cast<float>(steering_wheel_angle_deg_ / steering_wheel_range_deg_) *
              (track_right - track_left);
            draw_list->AddRectFilled(
                ImVec2(cursor_x - 2.0f * panel_scale, track_y - 7.0f * panel_scale),
                ImVec2(cursor_x + 2.0f * panel_scale, track_y + 7.0f * panel_scale),
                blue, panel_scale);
          }
        } else {
          panel_text(values[row], top, true, colors[row], 112.0f);
        }
        }
      }
      draw_list->PopClipRect();
    }
    ImGui::SetCursorScreenPos(grid_origin);
    ImGui::Dummy(ImVec2(std::max(1.0f, panel_avail.x),
                        std::max(1.0f, panel_avail.y)));
    if (ActionPressed("escape", true)) {
      for (const auto& session : sessions) {
        if (video_view_state_.Get(session.remote_sessionid) >= vts_rtc::vision::ViewMode::SurroundFront) {
          SendVideoViewControl(vts_rtc::vision::ViewMode::Binocular, session.remote_sessionid);
        }
      }
    }

    ImGui::End();
  }

  void DrawVideoComposite(const VideoSessionSnapshot& session,
                          const ImVec2& requested_size) {
    const bool takeover = vehicle_takeover_active_ &&
        session.remote_sessionid == vehicle_control_targets_.selected_target();
    if (!takeover) {
      DrawVideoCompositeContent(session, requested_size);
      return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(1.0f, requested_size.x),
                      std::max(1.0f, requested_size.y));
    const float inset = std::min(8.0f,
        std::max(0.0f, (std::min(size.x, size.y) - 1.0f) * 0.5f));
    const ImVec2 video_max(origin.x + size.x, origin.y + size.y);
    DrawVideoCompositeContent(session, requested_size);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (int layer = 0; layer < 8; ++layer) {
      const float fraction = static_cast<float>(layer) / 8.0f;
      const float offset = inset * fraction + inset / 16.0f;
      const int alpha = static_cast<int>(255.0f * (1.0f - fraction));
      draw_list->AddRect(
          ImVec2(origin.x + offset, origin.y + offset),
          ImVec2(video_max.x - offset, video_max.y - offset),
          IM_COL32(45, 150, 255, alpha), 0.0f, 0,
          std::max(0.5f, inset / 8.0f));
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(size);
  }

  void DrawVideoCompositeContent(const VideoSessionSnapshot& session,
                                const ImVec2& requested_size) {
    const ImVec2 size(std::max(1.0f, requested_size.x),
                      std::max(1.0f, requested_size.y));
    const auto mode = session.mode;
    const bool binocular_mode = mode ==
        vts_rtc::vision::ViewMode::Binocular;
    if (mode >= vts_rtc::vision::ViewMode::SurroundFront &&
      mode <= vts_rtc::vision::ViewMode::SurroundRight) {
      rtc_console::VideoTilePlacement placement;
      placement.tile = static_cast<rtc_console::VideoTileId>(
        static_cast<uint32_t>(mode) - 2);
      placement.rect = rtc_console::VideoRect{0, 0, size.x, size.y};
      ImGui::PushID(static_cast<int>(session.remote_sessionid));
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      DrawVideoTile(session, placement, origin, false);
      ImGui::SetCursorScreenPos(origin);
      ImGui::Dummy(size);
      ImGui::PopID();
      return;
    }
    rtc_console::VideoCompositeLayout composite_layout =
        rtc_console::ComputeVideoCompositeLayout(
            binocular_mode, size.x, size.y,
            StereoAspect(session.frames_by_tile));

    ImGui::PushID(static_cast<int>(session.remote_sessionid));
    const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
    if (binocular_mode) {
      auto& bounds = composite_layout.picture_in_picture_bounds;
      const float max_x = std::max(0.0f, size.x - bounds.width - 3.0f);
      const float max_y = std::max(0.0f, size.y - bounds.height - 3.0f);
      const float min_x = std::min(3.0f, max_x);
      const float min_y = std::min(3.0f, max_y);
      const float range_x = max_x - min_x;
      const float range_y = max_y - min_y;
      const auto saved = surround_window_positions_.find(session.remote_sessionid);
      float next_x = saved == surround_window_positions_.end()
          ? bounds.x : min_x + saved->second.x * range_x;
      float next_y = saved == surround_window_positions_.end()
          ? bounds.y : min_y + saved->second.y * range_y;
      ImGui::SetCursorScreenPos(ImVec2(canvas_min.x + next_x,
                                      canvas_min.y + next_y));
      ImGui::InvisibleButton("surround_window_drag",
                             ImVec2(bounds.width, bounds.height));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
      }
      if (ImGui::IsItemActive() &&
          ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        next_x += ImGui::GetIO().MouseDelta.x;
        next_y += ImGui::GetIO().MouseDelta.y;
        next_x = std::max(min_x, std::min(max_x, next_x));
        next_y = std::max(min_y, std::min(max_y, next_y));
        surround_window_positions_[session.remote_sessionid] = ImVec2(
            range_x > 0.0f ? (next_x - min_x) / range_x : 0.0f,
            range_y > 0.0f ? (next_y - min_y) / range_y : 0.0f);
      }
      next_x = std::max(min_x, std::min(max_x, next_x));
      next_y = std::max(min_y, std::min(max_y, next_y));
      const float offset_x = next_x - bounds.x;
      const float offset_y = next_y - bounds.y;
      for (auto& placement : composite_layout.placements) {
        if (placement.picture_in_picture) {
          placement.rect.x += offset_x;
          placement.rect.y += offset_y;
        }
      }
      bounds.x = next_x;
      bounds.y = next_y;
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_min,
                            ImVec2(canvas_min.x + size.x,
                                   canvas_min.y + size.y),
                            IM_COL32(0, 0, 0, 255));

    for (const rtc_console::VideoTilePlacement& placement :
         composite_layout.placements) {
      if (!placement.picture_in_picture) {
        DrawVideoTile(session, placement, canvas_min, !binocular_mode);
      }
    }

    const rtc_console::VideoRect& pip =
        composite_layout.picture_in_picture_bounds;
    const ImVec2 pip_min(canvas_min.x + pip.x, canvas_min.y + pip.y);
    const ImVec2 pip_max(pip_min.x + pip.width, pip_min.y + pip.height);
    if (pip.width > 0 && pip.height > 0) {
      draw_list->AddRectFilled(ImVec2(pip_min.x - 3.0f, pip_min.y - 3.0f),
                             ImVec2(pip_max.x + 3.0f, pip_max.y + 3.0f),
                             IM_COL32(8, 8, 10, 245));
    draw_list->AddRect(ImVec2(pip_min.x - 3.0f, pip_min.y - 3.0f),
                       ImVec2(pip_max.x + 3.0f, pip_max.y + 3.0f),
                       IM_COL32(220, 220, 220, 230), 0.0f, 0, 2.0f);
              }

    for (const rtc_console::VideoTilePlacement& placement :
         composite_layout.placements) {
      if (placement.picture_in_picture) {
        DrawVideoTile(session, placement, canvas_min, true);
      }
    }

    draw_list->AddRect(canvas_min,
                       ImVec2(canvas_min.x + size.x, canvas_min.y + size.y),
               session.remote_sessionid == vehicle_control_targets_.selected_target()
                 ? IM_COL32(45, 190, 145, 255) : IM_COL32(80, 80, 80, 255));
    ImGui::SetCursorScreenPos(canvas_min);
    ImGui::Dummy(size);
    ImGui::PopID();
  }

  void DrawVideoTile(const VideoSessionSnapshot& session,
                     const rtc_console::VideoTilePlacement& placement,
                     const ImVec2& canvas_min,
                     bool compact_mode) {
    const ImVec2 tile_size(std::max(1.0f, placement.rect.width),
                           std::max(1.0f, placement.rect.height));
    ImGui::SetCursorScreenPos(
        ImVec2(canvas_min.x + placement.rect.x,
               canvas_min.y + placement.rect.y));
    const std::shared_ptr<VideoFrameView>* frame =
        FindVideoFrame(session.frames_by_tile, placement.tile);
    const auto mode = session.mode;
    const bool surround_tile = placement.tile != rtc_console::VideoTileId::Stereo;
    const bool grid_mode = mode == vts_rtc::vision::ViewMode::Binocular ||
                           mode == vts_rtc::vision::ViewMode::Surround;
    if (surround_tile && grid_mode && session.surround_grid) {
      const int index = static_cast<int>(placement.tile) - 1;
      const ImVec2 uv_min((index % 2) * 0.5f, (index / 2) * 0.5f);
      DrawVideoFrameBox(*session.surround_grid, tile_size, false, true,
                        uv_min, ImVec2(uv_min.x + 0.5f, uv_min.y + 0.5f),
                        VideoSourceIdForTile(placement.tile));
    } else if (frame != nullptr && *frame) {
      DrawVideoFrameBox(**frame, tile_size, false, compact_mode,
                        ImVec2(0, 0), ImVec2(1, 1), nullptr);
    } else {
      DrawMissingVideoFrame(session.remote_sessionid, placement.tile, tile_size);
    }
  }

  void DrawMissingVideoFrame(RtcSessionId remote_sessionid,
                             rtc_console::VideoTileId tile,
                             const ImVec2& requested_size) {
    const ImVec2 size(std::max(1.0f, requested_size.x),
                      std::max(1.0f, requested_size.y));
    const char* source_id = VideoSourceIdForTile(tile);
    ImGui::PushID(static_cast<int>(remote_sessionid));
    ImGui::PushID(source_id);
    ImGui::Dummy(size);
    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();
    ImGui::PopID();
    ImGui::PopID();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(12, 12, 14, 255));
    draw_list->AddRect(box_min, box_max, IM_COL32(70, 70, 74, 255));
    const ImVec2 text_size = ImGui::CalcTextSize(source_id);
    const ImVec2 text_pos(
        box_min.x + std::max(4.0f, (size.x - text_size.x) * 0.5f),
        box_min.y + std::max(4.0f, (size.y - text_size.y) * 0.5f));
    draw_list->PushClipRect(box_min, box_max, true);
    draw_list->AddText(text_pos, IM_COL32(150, 150, 155, 255), source_id);
    draw_list->PopClipRect();
  }

  void DrawVideoFrameBox(const VideoFrameView& frame,
                         const ImVec2& requested_box_size,
                         bool fullscreen_mode,
                         bool compact_mode = false,
                         ImVec2 uv_min = ImVec2(0, 0),
                         ImVec2 uv_max = ImVec2(1, 1),
                         const char* tile_label = nullptr) {
    const ImVec2 box_size(std::max(1.0f, requested_box_size.x),
                          std::max(1.0f, requested_box_size.y));

    ImGui::PushID(frame.stream_key.c_str());
    ImGui::PushID(tile_label ? tile_label : "frame");
    ImGui::Dummy(box_size);
    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();
    ImGui::PopID();
    ImGui::PopID();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 255));

    const float title_padding = compact_mode ? 2.0f : 4.0f;
    const float title_height = video_debug_enabled_ ? std::min(box_size.y - 1.0f,
      ImGui::GetTextLineHeight() + title_padding * 2.0f) : 0.0f;
    const float image_area_height = std::max(1.0f, box_size.y - title_height);
    float draw_width = box_size.x;
    float draw_height = image_area_height;
    if (frame.width > 0 && frame.height > 0) {
      const float src_aspect =
          static_cast<float>(frame.width) * (uv_max.x - uv_min.x) /
          (static_cast<float>(frame.height) * (uv_max.y - uv_min.y));
      const float box_aspect = box_size.x / image_area_height;
      if (src_aspect > box_aspect) {
        draw_height = box_size.x / src_aspect;
      } else {
        draw_width = image_area_height * src_aspect;
      }
    }

    const bool render_lr_pixel_interleave =
        render_lr_pixel_interleave_ && frame.source_id == kExternalVideoSource;
    int render_width_pixels = 0;
    int render_height_pixels = 0;
    if (render_lr_pixel_interleave) {
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

    UpdateTextureFromFrame(frame, render_lr_pixel_interleave,
                           render_width_pixels, render_height_pixels);

    const auto tex_it = video_textures_.find(frame.stream_key);
    if (tex_it == video_textures_.end() || tex_it->second.texture == 0) {
      draw_list->AddRect(box_min, box_max,
                 IM_COL32(80, 80, 80, 255));
      return;
    }

    const float offset_x = (box_size.x - draw_width) * 0.5f;
    const float offset_y = title_height + (image_area_height - draw_height) * 0.5f;
    const ImVec2 image_min(box_min.x + offset_x, box_min.y + offset_y);
    const ImVec2 image_max(image_min.x + draw_width, image_min.y + draw_height);
    const ImGuiIO& io = ImGui::GetIO();
    std::ostringstream render_state;
    render_state << std::fixed << std::setprecision(3)
                 << "sid=" << frame.remote_sessionid
                 << " source=" << frame.source_id
                 << " stream=" << frame.stream_key
                 << " tile=" << (tile_label ? tile_label : "frame")
                 << " decoded=" << frame.width << "x" << frame.height
                 << " aspect=" << (frame.height > 0
                     ? static_cast<double>(frame.width) / frame.height : 0.0)
                 << " box=" << box_min.x << "," << box_min.y << ","
                 << box_max.x << "," << box_max.y
                 << " image=" << image_min.x << "," << image_min.y << ","
                 << image_max.x << "," << image_max.y
                 << " draw=" << draw_width << "x" << draw_height
                 << " uv=" << uv_min.x << "," << uv_min.y << ","
                 << uv_max.x << "," << uv_max.y
                 << " source_crop_x=" << frame.width * uv_min.x << ","
                 << frame.width * uv_max.x
                 << " stereo_mid_x=" << frame.width / 2.0
                 << " framebuffer_scale=" << io.DisplayFramebufferScale.x
                 << "," << io.DisplayFramebufferScale.y
                 << " interleave=" << render_lr_pixel_interleave
                 << " render_pixels=" << render_width_pixels << "x"
                 << render_height_pixels
                 << " fullscreen=" << fullscreen_mode
                 << " compact=" << compact_mode
                 << " debug=" << video_debug_enabled_;
    const std::string render_key = frame.stream_key + ":" +
        (tile_label ? tile_label : "frame") +
        (fullscreen_mode ? ":fullscreen" : ":panel");
    const std::string state = render_state.str();
    auto& previous_state = video_render_log_states_[render_key];
    if (previous_state != state) {
      previous_state = state;
      const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      const std::string message = "[VideoRender] unix_ms=" +
          std::to_string(timestamp_ms) + " " + state;
      AppendLog(message);
      if (video_render_log_file_) {
        video_render_log_file_ << message << std::endl;
      } else if (!video_render_log_error_reported_) {
        video_render_log_error_reported_ = true;
        AppendLog("[VideoRender] cannot write rtc_console_video_render.log");
        std::cerr << "[VideoRender] cannot write rtc_console_video_render.log"
                  << std::endl;
      }
    }
    draw_list->PushClipRect(box_min, box_max, true);
    draw_list->AddImage((ImTextureID)(intptr_t)tex_it->second.texture, image_min,
                        image_max, uv_min, uv_max);
    DrawVisionDetectionOverlay(frame, image_min, image_max, draw_list,
                               tex_it->second.uploaded_with_lr_interleave);
    draw_list->PopClipRect();
    draw_list->AddRect(box_min, box_max,
               IM_COL32(80, 80, 80, 255));

    if (video_debug_enabled_) {
    std::ostringstream title;
    if (compact_mode) {
      std::string source = tile_label ? tile_label : frame.source_id;
      if (source == "surround_front") source = "Front";
      else if (source == "surround_rear") source = "Rear";
      else if (source == "surround_left") source = "Left";
      else if (source == "surround_right") source = "Right";
      title << source;
    } else {
      title << "sid=" << frame.remote_sessionid << "  " << frame.source_id
            << "  " << frame.width << "x" << frame.height;
    }
    const std::string title_text = title.str();
    const ImVec2 overlay_max(box_max.x, box_min.y + title_height);
    draw_list->AddRectFilled(box_min, overlay_max, IM_COL32(24, 26, 28, 255));
    draw_list->PushClipRect(box_min, overlay_max, true);
    draw_list->AddText(
        ImVec2(box_min.x + title_padding, box_min.y + title_padding),
        IM_COL32(235, 235, 235, 255), title_text.c_str());
    draw_list->PopClipRect();

    }
  }

  bool DetectionToNormalizedRect(
      const vts_rtc::vision::FrameDetections& frame_detections,
      const vts_rtc::vision::Detection& detection,
      const VideoFrameView& frame,
      NormalizedRect* rect) const {
    if (!rect) {
      return false;
    }

    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    if (frame_detections.coord_type ==
        vts_rtc::vision::CoordType::NormU16Xywh) {
      left = static_cast<float>(detection.x) / 65535.0f;
      top = static_cast<float>(detection.y) / 65535.0f;
      right =
          static_cast<float>(std::min<uint64_t>(
              static_cast<uint64_t>(detection.x) + detection.w, 65535u)) /
          65535.0f;
      bottom =
          static_cast<float>(std::min<uint64_t>(
              static_cast<uint64_t>(detection.y) + detection.h, 65535u)) /
          65535.0f;
    } else if (frame_detections.coord_type ==
               vts_rtc::vision::CoordType::NormU16Xyxy) {
      left = static_cast<float>(detection.x) / 65535.0f;
      top = static_cast<float>(detection.y) / 65535.0f;
      right = static_cast<float>(detection.w) / 65535.0f;
      bottom = static_cast<float>(detection.h) / 65535.0f;
    } else if (frame_detections.coord_type ==
               vts_rtc::vision::CoordType::PixelI32Xywh) {
      const float source_width = static_cast<float>(
          frame_detections.frame_width > 0 ? frame_detections.frame_width
                                           : frame.width);
      const float source_height = static_cast<float>(
          frame_detections.frame_height > 0 ? frame_detections.frame_height
                                            : frame.height);
      if (source_width <= 0.0f || source_height <= 0.0f) {
        return false;
      }
      left = static_cast<float>(detection.x) / source_width;
      top = static_cast<float>(detection.y) / source_height;
      right = static_cast<float>(detection.x + detection.w) / source_width;
      bottom = static_cast<float>(detection.y + detection.h) / source_height;
    } else {
      return false;
    }

    if (right < left) {
      std::swap(left, right);
    }
    if (bottom < top) {
      std::swap(top, bottom);
    }

    left = std::max(0.0f, std::min(left, 1.0f));
    top = std::max(0.0f, std::min(top, 1.0f));
    right = std::max(0.0f, std::min(right, 1.0f));
    bottom = std::max(0.0f, std::min(bottom, 1.0f));
    if (right <= left || bottom <= top) {
      return false;
    }

    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
    return true;
  }

  bool NormalizedRectInImage(const NormalizedRect& source_rect,
                             const ImVec2& image_min,
                             const ImVec2& image_max,
                             ImVec2* rect_min,
                             ImVec2* rect_max) const {
    if (!rect_min || !rect_max || image_max.x <= image_min.x ||
        image_max.y <= image_min.y || source_rect.right <= source_rect.left ||
        source_rect.bottom <= source_rect.top) {
      return false;
    }

    const float image_width = image_max.x - image_min.x;
    const float image_height = image_max.y - image_min.y;
    const float left = std::max(0.0f, std::min(source_rect.left, 1.0f));
    const float top = std::max(0.0f, std::min(source_rect.top, 1.0f));
    const float right = std::max(0.0f, std::min(source_rect.right, 1.0f));
    const float bottom = std::max(0.0f, std::min(source_rect.bottom, 1.0f));
    if (right <= left || bottom <= top) {
      return false;
    }

    rect_min->x = image_min.x + left * image_width;
    rect_min->y = image_min.y + top * image_height;
    rect_max->x = image_min.x + right * image_width;
    rect_max->y = image_min.y + bottom * image_height;
    return true;
  }

  bool MapSideBySideRectToInterleavedEye(const NormalizedRect& source_rect,
                                         bool left_eye,
                                         NormalizedRect* eye_rect) const {
    if (!eye_rect || source_rect.right <= source_rect.left ||
        source_rect.bottom <= source_rect.top) {
      return false;
    }

    const float source_half_left = left_eye ? 0.0f : 0.5f;
    const float source_half_right = left_eye ? 0.5f : 1.0f;
    const float overlap_left = std::max(source_rect.left, source_half_left);
    const float overlap_right = std::min(source_rect.right, source_half_right);
    if (overlap_right <= overlap_left) {
      return false;
    }

    eye_rect->left = (overlap_left - source_half_left) * 2.0f;
    eye_rect->right = (overlap_right - source_half_left) * 2.0f;
    eye_rect->top = source_rect.top;
    eye_rect->bottom = source_rect.bottom;
    return eye_rect->right > eye_rect->left && eye_rect->bottom > eye_rect->top;
  }

  void DrawInterleavedFilledRect(ImDrawList* draw_list,
                                const ImVec2& area_min,
                                const ImVec2& area_max,
                                const ImVec2& image_min,
                                const ImVec2& image_max,
                                int column_phase,
                                ImU32 color) const {
    if (!draw_list || area_max.x <= area_min.x || area_max.y <= area_min.y ||
        image_max.x <= image_min.x || image_max.y <= image_min.y) {
      return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float fb_scale_x =
        io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const int first_column = std::max(
        0, static_cast<int>(std::floor((area_min.x - image_min.x) * fb_scale_x)));
    const int last_column = std::max(
        first_column,
        static_cast<int>(std::ceil((area_max.x - image_min.x) * fb_scale_x)));

    const float clipped_y_min = std::max(area_min.y, image_min.y);
    const float clipped_y_max = std::min(area_max.y, image_max.y);
    if (clipped_y_max <= clipped_y_min) {
      return;
    }

    const int phase = column_phase & 1;
    for (int column = first_column; column < last_column; ++column) {
      if ((column & 1) != phase) {
        continue;
      }

      const float column_min_x = image_min.x + static_cast<float>(column) / fb_scale_x;
      const float column_max_x =
          image_min.x + static_cast<float>(column + 1) / fb_scale_x;
      const float clipped_x_min =
          std::max(area_min.x, std::max(column_min_x, image_min.x));
      const float clipped_x_max =
          std::min(area_max.x, std::min(column_max_x, image_max.x));
      if (clipped_x_max <= clipped_x_min) {
        continue;
      }

      draw_list->AddRectFilled(ImVec2(clipped_x_min, clipped_y_min),
                               ImVec2(clipped_x_max, clipped_y_max), color);
    }
  }

  void DrawInterleavedRect(ImDrawList* draw_list,
                           const ImVec2& rect_min,
                           const ImVec2& rect_max,
                           const ImVec2& image_min,
                           const ImVec2& image_max,
                           int column_phase,
                           ImU32 color) const {
    if (!draw_list || rect_max.x <= rect_min.x || rect_max.y <= rect_min.y) {
      return;
    }

    const float thickness = 2.0f;
    DrawInterleavedFilledRect(
        draw_list, rect_min, ImVec2(rect_max.x, std::min(rect_max.y, rect_min.y + thickness)),
        image_min, image_max, column_phase, color);
    DrawInterleavedFilledRect(
        draw_list, ImVec2(rect_min.x, std::max(rect_min.y, rect_max.y - thickness)),
        rect_max, image_min, image_max, column_phase, color);
    DrawInterleavedFilledRect(
        draw_list, rect_min, ImVec2(std::min(rect_max.x, rect_min.x + thickness), rect_max.y),
        image_min, image_max, column_phase, color);
    DrawInterleavedFilledRect(
        draw_list, ImVec2(std::max(rect_min.x, rect_max.x - thickness), rect_min.y),
        rect_max, image_min, image_max, column_phase, color);
  }

  void DrawInterleavedText(ImDrawList* draw_list,
                           const ImVec2& text_pos,
                           ImU32 color,
                           const char* text,
                           const ImVec2& clip_min,
                           const ImVec2& clip_max,
                           const ImVec2& image_min,
                           const ImVec2& image_max,
                           int column_phase) const {
    if (!draw_list || !text || text[0] == '\0' || clip_max.x <= clip_min.x ||
        clip_max.y <= clip_min.y) {
      return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const float fb_scale_x =
        io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const int first_column = std::max(
        0, static_cast<int>(std::floor((clip_min.x - image_min.x) * fb_scale_x)));
    const int last_column = std::max(
        first_column,
        static_cast<int>(std::ceil((clip_max.x - image_min.x) * fb_scale_x)));
    const int phase = column_phase & 1;

    for (int column = first_column; column < last_column; ++column) {
      if ((column & 1) != phase) {
        continue;
      }

      const float column_min_x = image_min.x + static_cast<float>(column) / fb_scale_x;
      const float column_max_x =
          image_min.x + static_cast<float>(column + 1) / fb_scale_x;
      const float clipped_x_min =
          std::max(clip_min.x, std::max(column_min_x, image_min.x));
      const float clipped_x_max =
          std::min(clip_max.x, std::min(column_max_x, image_max.x));
      const float clipped_y_min = std::max(clip_min.y, image_min.y);
      const float clipped_y_max = std::min(clip_max.y, image_max.y);
      if (clipped_x_max <= clipped_x_min || clipped_y_max <= clipped_y_min) {
        continue;
      }

      draw_list->PushClipRect(ImVec2(clipped_x_min, clipped_y_min),
                              ImVec2(clipped_x_max, clipped_y_max), true);
      draw_list->AddText(text_pos, color, text);
      draw_list->PopClipRect();
    }
  }

  void DrawDetectionLabel(ImDrawList* draw_list,
                          const ImVec2& image_min,
                          const ImVec2& image_max,
                          const ImVec2& rect_min,
                          ImU32 text_color,
                          ImU32 label_bg,
                          const char* label,
                          bool interleaved,
                          int column_phase) const {
    if (!draw_list || !label || label[0] == '\0') {
      return;
    }

    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 label_min(
        rect_min.x, std::max(image_min.y, rect_min.y - text_size.y - 4.0f));
    const ImVec2 label_max(
        std::min(image_max.x, label_min.x + text_size.x + 6.0f),
        std::min(image_max.y, label_min.y + text_size.y + 4.0f));
    if (label_max.x <= label_min.x || label_max.y <= label_min.y) {
      return;
    }

    if (interleaved) {
      DrawInterleavedFilledRect(draw_list, label_min, label_max, image_min,
                                image_max, column_phase, label_bg);
      DrawInterleavedText(draw_list,
                          ImVec2(label_min.x + 3.0f, label_min.y + 2.0f),
                          text_color, label, label_min, label_max, image_min,
                          image_max, column_phase);
    } else {
      draw_list->AddRectFilled(label_min, label_max, label_bg);
      draw_list->AddText(ImVec2(label_min.x + 3.0f, label_min.y + 2.0f),
                         text_color, label);
    }
  }

  void DrawVisionDetectionOverlay(const VideoFrameView& frame,
                                  const ImVec2& image_min,
                                  const ImVec2& image_max,
                                  ImDrawList* draw_list,
                                  bool interleaved_overlay) {
    if (!draw_list) {
      return;
    }

    VisionOverlaySnapshot snapshot;
    {
      std::lock_guard<std::mutex> lock(vision_detection_mutex_);
      const auto it = vision_detections_by_source_.find(frame.stream_key);
      if (it == vision_detections_by_source_.end()) {
        return;
      }
      snapshot.overlay = it->second;
      snapshot.class_labels_by_id = vision_class_labels_by_id_;
    }

    const VisionDetectionOverlayView& overlay = snapshot.overlay;
    const auto age = std::chrono::steady_clock::now() - overlay.updated_at;
    if (age > std::chrono::seconds(2)) {
      return;
    }

    const ImU32 box_color = IM_COL32(0, 255, 102, 255);
    const ImU32 label_bg = IM_COL32(0, 0, 0, 190);
    for (const vts_rtc::vision::Detection& detection :
         overlay.frame_detections.detections) {
      NormalizedRect source_rect;
      if (!DetectionToNormalizedRect(overlay.frame_detections, detection, frame,
                                     &source_rect)) {
        continue;
      }

      char label[64] = {0};
      const auto label_it =
          snapshot.class_labels_by_id.find(detection.class_id);
      if (label_it != snapshot.class_labels_by_id.end() &&
          !label_it->second.empty()) {
        std::snprintf(label, sizeof(label), "%s %.1f%%",
                      label_it->second.c_str(),
                      static_cast<double>(detection.confidence) / 10.0);
      } else {
        std::snprintf(label, sizeof(label), "cls %u %.1f%%",
                      detection.class_id,
                      static_cast<double>(detection.confidence) / 10.0);
      }

      if (interleaved_overlay) {
        for (int eye = 0; eye < 2; ++eye) {
          const bool left_eye = eye == 0;
          NormalizedRect eye_rect;
          if (!MapSideBySideRectToInterleavedEye(source_rect, left_eye,
                                                 &eye_rect)) {
            continue;
          }

          ImVec2 rect_min;
          ImVec2 rect_max;
          if (!NormalizedRectInImage(eye_rect, image_min, image_max, &rect_min,
                                     &rect_max)) {
            continue;
          }

          const int column_phase = left_eye ? 0 : 1;
          DrawInterleavedRect(draw_list, rect_min, rect_max, image_min,
                              image_max, column_phase, box_color);
          DrawDetectionLabel(draw_list, image_min, image_max, rect_min,
                             box_color, label_bg, label, true, column_phase);
        }
      } else {
        ImVec2 rect_min;
        ImVec2 rect_max;
        if (!NormalizedRectInImage(source_rect, image_min, image_max, &rect_min,
                                   &rect_max)) {
          continue;
        }

        draw_list->AddRect(rect_min, rect_max, box_color, 0.0f, 0, 2.0f);
        DrawDetectionLabel(draw_list, image_min, image_max, rect_min, box_color,
                           label_bg, label, false, 0);
      }
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

    if (ActionPressed("escape", true)) {
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
                              bool render_lr_pixel_interleave,
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
    if (render_lr_pixel_interleave &&
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
    video_render_log_states_.clear();
  }

  float DrawNetStatsHint(const UiLayout& layout) {
    std::vector<NetStatsView> stats_snapshot;
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      for (const auto& it : net_stats_) {
        stats_snapshot.push_back(it.second);
      }
    }

    const ImGuiWindowFlags hint_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
    const float hint_width = std::max(1.0f, layout.control_size.x);
    const float hint_x = layout.control_pos.x;
    const float preferred_y = layout.control_pos.y;
    const float minimum_y = layout.viewport_pos.y + kPanelMargin;
    const float viewport_bottom =
        layout.viewport_pos.y + layout.viewport_size.y - kPanelMargin;
    const float hint_y = std::max(minimum_y, preferred_y);
    const float available_height = std::max(1.0f, viewport_bottom - hint_y);
    const float hint_height = std::min(available_height,
      std::max(1.0f, layout.control_size.y * 0.45f));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::SetNextWindowPos(ImVec2(hint_x, hint_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(hint_width, hint_height),
                             ImGuiCond_Always);
    if (focus_netstats_hint_) {
      ImGui::SetNextWindowFocus();
      focus_netstats_hint_ = false;
    }
    ImGui::Begin("##NetStatsHint", nullptr, hint_flags);

    ImGui::TextDisabled("NET STATS");
    double video_delay_ms = -1;
    for (const NetStatsView& stats : stats_snapshot) {
      if (stats.input && stats.delay.estimated_receive_ms >= 0) {
        video_delay_ms =
            std::max(video_delay_ms, stats.delay.estimated_receive_ms);
      }
    }
    if (video_delay_ms >= 0) {
      ImGui::Text("Video delay: %.1f ms", video_delay_ms);
    } else {
      ImGui::TextDisabled("Video delay: --");
    }
    if (stats_snapshot.empty()) {
      ImGui::TextUnformatted("No stats yet");
      ImGui::End();
      return hint_y + hint_height;
    }

    if (ImGui::BeginTable("NetStatsHintTable", 9,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_ScrollX |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y))) {
      ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 150);
      ImGui::TableSetupColumn("Peer", ImGuiTableColumnFlags_WidthFixed, 65);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 45);
      ImGui::TableSetupColumn("Bitrate", ImGuiTableColumnFlags_WidthFixed, 85);
      ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, 40);
      ImGui::TableSetupColumn("Net ms", ImGuiTableColumnFlags_WidthFixed, 80);
      ImGui::TableSetupColumn("Buffer ms", ImGuiTableColumnFlags_WidthFixed, 85);
      ImGui::TableSetupColumn("Decode ms", ImGuiTableColumnFlags_WidthFixed, 85);
      ImGui::TableSetupColumn("Recv ms", ImGuiTableColumnFlags_WidthFixed, 85);
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableHeadersRow();

      for (const NetStatsView& s : stats_snapshot) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(s.source_id.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", s.session_id);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(s.input ? "in" : "out");
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%lu", s.bitrate_bps);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u", s.fps);
        const double delays[] = {s.delay.network_ms, s.delay.jitter_buffer_ms,
                                 s.delay.decode_ms, s.delay.estimated_receive_ms};
        for (int index = 0; index < 4; ++index) {
          ImGui::TableSetColumnIndex(5 + index);
          if (delays[index] >= 0) {
            ImGui::Text("%.1f", delays[index]);
          } else {
            ImGui::TextDisabled("--");
          }
        }
      }
      ImGui::EndTable();
    }

    ImGui::End();
    return hint_y + hint_height;
  }

  void DrawEventLogHint(const UiLayout& layout, float requested_minimum_y) {
    const ImGuiWindowFlags hint_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;
      const float hint_width = std::max(1.0f, layout.control_size.x);
      const float hint_height = std::max(1.0f,
        layout.viewport_pos.y + layout.viewport_size.y - kPanelMargin - requested_minimum_y);
      const float hint_x = layout.control_pos.x;
      const float preferred_y = requested_minimum_y;
    const float minimum_y = layout.viewport_pos.y + kPanelMargin;
    const float maximum_y = layout.viewport_pos.y + layout.viewport_size.y -
                            kPanelMargin - hint_height;
    const float hint_y =
        std::max(minimum_y, std::min(preferred_y, maximum_y));
    ImGui::SetNextWindowBgAlpha(0.96f);
    ImGui::SetNextWindowPos(ImVec2(hint_x, hint_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(hint_width, hint_height),
                             ImGuiCond_Always);
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
    if (options_.no_render) {
      std::cout << message << std::endl;
      return;
    }
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
    if ((viewed_session_ != 0 && !IsBoundSession(viewed_session_)) ||
        (vehicle_takeover_active_ &&
         !IsBoundSession(vehicle_control_targets_.selected_target()))) {
      viewed_session_ = 0;
      ResetVehicleControlOnUi(false);
    }
    if (pending_reset_session_state_.exchange(false)) {
      ResetSessionStateOnUi();
    }
    if (pending_reset_vehicle_control_.exchange(false)) {
      ResetVehicleControlOnUi(false);
    }
  }

  void RequestSessionStateReset() { pending_reset_session_state_.store(true); }

  void RequestVehicleControlReset() {
    pending_reset_vehicle_control_.store(true);
  }

  void ResetSessionStateOnUi() {
    local_yolo_.Clear();
    StopMediaFeed();
    remote_audio_player_.Clear();
    CloseFullscreenVideo();

    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      net_stats_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(video_mutex_);
      remote_video_frames_by_source_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(vision_detection_mutex_);
      vision_detections_by_source_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(edge_feedback_mutex_);
      edge_feedback_by_session_.clear();
    }

    remote_video_frames_.store(0);
    remote_audio_frames_.store(0);
    local_audio_frames_.store(0);
    remote_video_frame_seq_.store(0);
    ReleaseVideoTextures();
  }

  void ResetVehicleControlOnUi(bool clear_targets) {
    vehicle_takeover_active_ = false;
    if (clear_targets) {
      {
        std::lock_guard<std::mutex> lock(device_identity_mutex_);
        device_slots_.Clear();
      }
      viewed_session_ = 0;
      vehicle_control_targets_.Clear();
      video_view_state_.Clear();
    }
    vehicle_control_enabled_ = false;
    ClearVehicleInput();
    gamepad_dog_actions_.Reset();
    vehicle_event_acks_.Clear();
    has_last_vehicle_command_ = false;
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
    } else if (selected_video_source_ == 0) {
      StartVideoFeed();
    }
    EnsureAudioSource();
  }

  void EnsureAudioSource() {
    if (!audio_source_added_) {
      AddAudioSource();
    } else {
      ApplyAudioInputSelection();
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
    ApplyAudioInputSelection();
  }

  void ApplyAudioInputSelection() {
    audio_capture_.Stop();
    if (options_.no_render || !audio_source_added_ ||
        selected_audio_input_ < 0 ||
        selected_audio_input_ >=
            static_cast<int>(audio_input_choices_.size())) {
      return;
    }
    const AudioDeviceChoice& choice =
        audio_input_choices_[selected_audio_input_];
    if (!choice.enabled) {
      AppendLog("Audio input disabled");
      return;
    }

    rtc_audio::AudioCaptureOptions capture_options;
    if (choice.device_id != rtc_audio::kSystemDefaultDeviceId) {
      capture_options.device_name = choice.label;
    }
    capture_options.sample_rate = 48000;
    capture_options.channels = 1;
    capture_options.frame_duration_ms = 10;
    std::string error_message;
    const bool started = audio_capture_.Start(
        capture_options,
        [this](const rtc_audio::AudioFrameView& frame) {
          RtcPCMData pcm;
          pcm.bits_per_sample = frame.bits_per_sample;
          pcm.sample_rate = frame.sample_rate;
          pcm.number_of_channels = frame.number_of_channels;
          pcm.number_of_frames = frame.number_of_frames;
          pcm.buffer = const_cast<void*>(frame.data);
          pcm.sz_buffer = frame.data_size;
          if (RtcSendAudioFrame(kExternalAudioSource, &pcm) ==
              RtcErrorCode::OK) {
            local_audio_frames_.fetch_add(1, std::memory_order_relaxed);
          }
        },
        &error_message);
    if (!started) {
      AppendLog(error_message);
      return;
    }
    AppendLog(std::string("Audio input started: ") + choice.label);
  }

  void ApplyAudioOutputSelection() {
    remote_audio_player_.Stop();
    if (options_.no_render || selected_audio_output_ < 0 ||
        selected_audio_output_ >=
            static_cast<int>(audio_output_choices_.size())) {
      return;
    }
    const AudioDeviceChoice& choice =
        audio_output_choices_[selected_audio_output_];
    if (!choice.enabled) {
      AppendLog("Audio output disabled");
      return;
    }

    rtc_audio::AudioPlaybackOptions playback_options;
    if (choice.device_id != rtc_audio::kSystemDefaultDeviceId) {
      playback_options.device_name = choice.label;
    }
    std::string error_message;
    if (!remote_audio_player_.Start(playback_options, &error_message)) {
      AppendLog(error_message);
      return;
    }
    AppendLog(std::string("Audio output selected: ") + choice.label);
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
    audio_capture_.Stop();
    stop_media_feed_.store(true);
    if (video_thread_.joinable()) {
      video_thread_.join();
    }
  }

  void FreeMediaBuffers() {
    for (RtcYUV420pFrame& frame : yuv_frames_) {
      delete[] frame.buffer;
      frame.buffer = nullptr;
    }
    yuv_frames_.clear();
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

    if (state == RtcP2PState::P2PConnected) {
      instance_->vehicle_control_targets_.SetP2PConnected(sessionid, true);
      instance_->AppendLog(
          "Vehicle peer connected; select it before keyboard control");
    }
    if (state == RtcP2PState::P2PDisconnected || state == RtcP2PState::P2PClosed ||
        state == RtcP2PState::P2PFailed) {
      {
        std::lock_guard<std::mutex> lock(instance_->device_identity_mutex_);
        instance_->device_slots_.Disconnect(sessionid);
      }
      {
        std::lock_guard<std::mutex> lock(instance_->video_mutex_);
        for (auto frame = instance_->remote_video_frames_by_source_.begin();
             frame != instance_->remote_video_frames_by_source_.end();) {
          if (frame->second && frame->second->remote_sessionid == sessionid) {
            frame = instance_->remote_video_frames_by_source_.erase(frame);
          } else {
            ++frame;
          }
        }
      }
      {
        std::lock_guard<std::mutex> lock(instance_->edge_feedback_mutex_);
        instance_->edge_feedback_by_session_.erase(sessionid);
      }
      instance_->video_view_state_.Erase(sessionid);
      if (instance_->vehicle_control_targets_.SetP2PConnected(sessionid,
                                                               false)) {
        instance_->RequestVehicleControlReset();
      }
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

    if (label &&
        std::strcmp(label, vts_rtc::vehicle::kVehicleControlChannelLabel) == 0) {
      const bool open = state == RtcDataChannelState::DataChannelOpen;
      instance_->vehicle_control_targets_.SetControlChannelOpen(sessionid,
                                                                 open);
    }
  }

  static void OnServerConnectionState(RtcServerConnectionState state) {
    if (!instance_) {
      return;
    }
    instance_->server_state_.store(state);
    if (state != ServerLogined) {
      instance_->auto_room_opened_.store(false);
      {
        std::lock_guard<std::mutex> lock(instance_->device_identity_mutex_);
        instance_->device_slots_.Clear();
      }
      instance_->RequestVehicleControlReset();
      instance_->RequestSessionStateReset();
    }
    std::ostringstream oss;
    oss << "Server state: " << ServerStateText(state);
    instance_->AppendLog(oss.str());
  }

  static void OnRecvMessage(RtcSessionId remote_sessionid, RtcDataChannelLabel label,
                            const char* msg, size_t msg_size) {
    if (!instance_) {
      return;
    }
    if (label &&
        std::strcmp(label, rtc_runtime::kDeviceIdentityChannelLabel) == 0) {
      if (msg == nullptr || msg_size == 0 || msg_size > 64) {
        return;
      }
      const std::string device_id(msg, msg_size);
      if (!rtc_runtime::IsValidDeviceId(device_id)) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(instance_->device_identity_mutex_);
        instance_->device_slots_.Identify(remote_sessionid, device_id);
      }
      const auto selected = instance_->vehicle_control_targets_.selected_target();
      if (selected != 0 && !instance_->IsBoundSession(selected)) {
        instance_->RequestVehicleControlReset();
      }
      instance_->AppendLog("Device identity: " + device_id + " session=" +
                           std::to_string(remote_sessionid));
      return;
    }
    if (label &&
        std::strcmp(label, vts_rtc::vision::kVisionDetectionChannelLabel) == 0) {
      instance_->HandleVisionDetectionMessage(remote_sessionid, msg, msg_size);
      return;
    }
    if (label &&
        std::strcmp(label, vts_rtc::vehicle::kVehicleEventChannelLabel) == 0) {
      instance_->HandleVehicleEventMessage(remote_sessionid, msg, msg_size);
      return;
    }
    if (label &&
        std::strcmp(label, vts_rtc::vehicle::kVehicleStateChannelLabel) == 0) {
      instance_->HandleVehicleStateMessage(remote_sessionid, msg, msg_size);
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

  void HandleVehicleEventMessage(RtcSessionId remote_sessionid,
                                 const char* msg,
                                 size_t msg_size) {
    if (!msg || msg_size == 0) {
      AppendLog("Vehicle event decode failed: empty payload");
      return;
    }
    const vts_rtc::vehicle::DecodeResult decoded =
        vts_rtc::vehicle::DecodeEnvelope(
            reinterpret_cast<const uint8_t*>(msg), msg_size);
    if (!decoded ||
        decoded.envelope.type != vts_rtc::vehicle::MessageType::EventAck) {
      std::ostringstream oss;
      oss << "Vehicle event decode failed from " << remote_sessionid;
      if (!decoded) {
        oss << ": " << decoded.error_message;
      } else {
        oss << ": unexpected message type";
      }
      AppendLog(oss.str());
      return;
    }

    const vts_rtc::vehicle::EventAck& ack = decoded.envelope.event_ack;
    rtc_console::DogActionAckContext context;
    if (vehicle_event_acks_.ResolveDogAction(ack.request_id, &context)) {
      if (context.log_result || !ack.accepted) {
        std::ostringstream oss;
        oss << "Dog action result: action=" << DogActionText(context.action)
            << " request_id=" << ack.request_id
            << " accepted=" << (ack.accepted ? "true" : "false")
            << " error=" << VehicleErrorCodeText(ack.error_code);
        AppendLog(oss.str());
      }
      return;
    }

    std::ostringstream oss;
    oss << "Vehicle event ack from " << remote_sessionid
        << " request_id=" << ack.request_id
        << " accepted=" << (ack.accepted ? "true" : "false")
        << " error=" << VehicleErrorCodeText(ack.error_code);
    AppendLog(oss.str());
  }

  void HandleVehicleStateMessage(RtcSessionId remote_sessionid,
                                 const char* msg,
                                 size_t msg_size) {
    if (!msg || msg_size == 0) {
      std::ostringstream oss;
      oss << "Vehicle state decode failed from " << remote_sessionid
          << ": empty payload";
      AppendLog(oss.str());
      return;
    }

    const vts_rtc::vehicle::DecodeResult decoded =
        vts_rtc::vehicle::DecodeEnvelope(
            reinterpret_cast<const uint8_t*>(msg), msg_size);
    if (!decoded) {
      std::ostringstream oss;
      oss << "Vehicle state decode failed from " << remote_sessionid
          << " bytes=" << msg_size << ": " << decoded.error_message;
      AppendLog(oss.str());
      return;
    }
    if (decoded.envelope.type !=
        vts_rtc::vehicle::MessageType::VehicleState) {
      std::ostringstream oss;
      oss << "Vehicle state decode failed from " << remote_sessionid
          << " bytes=" << msg_size << ": unexpected message type="
          << static_cast<uint32_t>(decoded.envelope.type);
      AppendLog(oss.str());
      return;
    }

    const vts_rtc::vehicle::VehicleState& state =
        decoded.envelope.vehicle_state;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(edge_feedback_mutex_);
      EdgeFeedbackView& feedback =
          edge_feedback_by_session_[remote_sessionid];
      feedback.envelope_seq = decoded.envelope.seq;
      feedback.state = state;
      feedback.updated_at = now;
    }
    if (!vehicle_state_log_limiter_.ShouldLog(
            static_cast<uint64_t>(remote_sessionid),
            static_cast<uint32_t>(state.active_gear), state.watchdog_stopped,
            now)) {
      return;
    }

    std::ostringstream oss;
    oss << "Vehicle state from " << remote_sessionid << " ["
        << vts_rtc::vehicle::kVehicleStateChannelLabel << "] bytes="
        << msg_size << " seq=" << decoded.envelope.seq
        << " active_gear=" << VehicleGearText(state.active_gear)
        << " last_received_drive_seq=" << state.last_received_drive_seq
        << " watchdog_stopped="
        << (state.watchdog_stopped ? "true" : "false");
    AppendLog(oss.str());
  }

  void HandleVisionDetectionMessage(RtcSessionId remote_sessionid,
                                    const char* msg,
                                    size_t msg_size) {
    if (yolo_enabled_) return;
    if (!msg || msg_size == 0) {
      return;
    }

    const vts_rtc::vision::DecodeResult decoded =
        vts_rtc::vision::DecodeEnvelope(
            reinterpret_cast<const uint8_t*>(msg), msg_size);
    if (!decoded) {
      AppendLog(std::string("Vision detection decode failed: ") +
                decoded.error_message);
      return;
    }

    if (decoded.envelope.type == vts_rtc::vision::MessageType::ClassMap) {
      const vts_rtc::vision::ClassMap& class_map =
          decoded.envelope.class_map;
      std::map<uint32_t, std::string> labels_by_id;
      for (const vts_rtc::vision::ClassInfo& class_info :
           class_map.classes) {
        if (!class_info.label.empty()) {
          labels_by_id[class_info.class_id] = class_info.label;
        }
      }
      {
        std::lock_guard<std::mutex> lock(vision_detection_mutex_);
        vision_class_labels_by_id_ = labels_by_id;
      }
      std::ostringstream oss;
      oss << "Vision class map received: model=" << class_map.model_name
          << " version=" << class_map.model_version
          << " classes=" << labels_by_id.size();
      AppendLog(oss.str());
      return;
    }

    if (decoded.envelope.type != vts_rtc::vision::MessageType::FrameDetections) {
      return;
    }

    const vts_rtc::vision::FrameDetections& frame_detections =
        decoded.envelope.frame_detections;
    if (frame_detections.source_id.empty()) {
      return;
    }

    VisionDetectionOverlayView overlay;
    overlay.frame_detections = frame_detections;
    overlay.updated_at = std::chrono::steady_clock::now();

    const std::string stream_key =
        MakeVideoStreamKey(remote_sessionid, frame_detections.source_id);
    {
      std::lock_guard<std::mutex> lock(vision_detection_mutex_);
      vision_detections_by_source_[stream_key] = overlay;
    }
  }

  static void OnRecvAudioFrame(RtcSessionId,
                               RtcAudioSourceId,
                               RtcMediaSourceType,
                               size_t bits_per_sample,
                               size_t sample_rate,
                               size_t number_of_channels,
                               size_t number_of_frames,
                               const void* audio_data,
                               size_t sz_audio_data) {
    if (!instance_) {
      return;
    }
    instance_->remote_audio_frames_.fetch_add(1);
    if (!instance_->options_.no_render) {
      rtc_audio::AudioFrameView frame;
      frame.bits_per_sample = bits_per_sample;
      frame.sample_rate = sample_rate;
      frame.number_of_channels = number_of_channels;
      frame.number_of_frames = number_of_frames;
      frame.data = audio_data;
      frame.data_size = sz_audio_data;
      instance_->remote_audio_player_.PushFrame(frame);
    }
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
    view->stream_key = MakeVideoStreamKey(remote_sessionid, sourceid);
    view->source_id = sourceid;
    view->remote_sessionid = remote_sessionid;
    view->source_type = source_type;
    view->width = width;
    view->height = height;
    view->dimension = dimension;
    view->frame_seq = instance_->remote_video_frame_seq_.fetch_add(1) + 1;
    view->buffer.assign(buffer, buffer + sz_buffer);

    if (instance_->yolo_enabled_ && dimension == 4 && width <= 16384 && height <= 16384) {
      rtc_console::LocalYoloFrame frame;
      frame.key = view->stream_key;
      frame.source = view->source_id;
      frame.sequence = view->frame_seq;
      frame.width = static_cast<int>(width);
      frame.height = static_cast<int>(height);
      frame.pixels = view->buffer;
      frame.received = std::chrono::steady_clock::now();
      instance_->local_yolo_.Submit(std::move(frame));
    }
    if (instance_->options_.no_render) return;

    {
      std::lock_guard<std::mutex> lock(instance_->video_mutex_);
      instance_->remote_video_frames_by_source_[view->stream_key] = view;
    }
  }

  static void OnVideoDelayStats(RtcSessionId session_id, RtcVideoSourceId source,
                               const RtcVideoDelayStatsV1* delay) {
    if (!instance_ || !source || !delay ||
        delay->struct_size != sizeof(RtcVideoDelayStatsV1)) {
      return;
    }
    const std::string key = std::to_string(session_id) + "|" + source + "|in";
    std::lock_guard<std::mutex> lock(instance_->stats_mutex_);
    auto found = instance_->net_stats_.find(key);
    if (found != instance_->net_stats_.end()) {
      found->second.delay = *delay;
    }
  }

  static void OnChannelNetworkStats(RtcSessionId session_id, RtcNetStats stats) {
    if (!instance_) {
      return;
    }

    NetStatsView view;
    view.session_id = session_id;
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
          std::to_string(session_id) + "|" + view.source_id +
          (view.input ? "|in" : "|out");
      std::lock_guard<std::mutex> lock(instance_->stats_mutex_);
      instance_->net_stats_[source_key] = view;
    }
  }

 private:
  static RtcConsoleApp* instance_;

  rtc_console::AppOptions options_;
  bool auto_open_room_ = false;
  std::atomic<RtcServerConnectionState> server_state_{ServerDisconnected};
  std::atomic<bool> auto_room_opened_{false};
  std::chrono::steady_clock::time_point last_auto_open_attempt_{};
  std::atomic<uint64_t> remote_video_frames_{0};
  std::atomic<uint64_t> remote_audio_frames_{0};
  std::atomic<uint64_t> local_audio_frames_{0};
  std::atomic<uint64_t> remote_video_frame_seq_{0};
  std::atomic<bool> pending_reset_session_state_{false};
  std::atomic<bool> pending_reset_vehicle_control_{false};
  rtc_console::VehicleControlTargetRegistry vehicle_control_targets_;
  rtc_audio::AudioCapture audio_capture_;
  rtc_audio::AudioPlayback remote_audio_player_;
  SDLOpenGLWindow* ui_window_ = nullptr;

  std::mutex log_mutex_;
  std::vector<std::string> logs_;
  rtc_console::VehicleStateLogLimiter vehicle_state_log_limiter_;
  bool auto_scroll_ = true;
  int active_page_ = 0;
  bool focus_netstats_hint_ = false;
  bool focus_eventlog_hint_ = false;
  bool vehicle_control_enabled_ = false;
  bool vehicle_takeover_active_ = false;
  bool video_debug_enabled_ = false;
  RtcSessionId viewed_session_ = 0;
  std::map<std::string, std::vector<ImGuiKey>> key_bindings_;
  bool vehicle_keyboard_active_ = false;
  bool vehicle_gamepad_active_ = false;
  float vehicle_throttle_ = 0.5f;
  double steering_wheel_range_deg_ = 900.0;
  double steering_wheel_angle_deg_ = 0.0;
  rtc_console::VehicleControlInput vehicle_keyboard_input_;
  rtc_console::VehicleControlInput vehicle_gamepad_input_;
  rtc_console::VehicleControlInput vehicle_input_;
  SDL_Gamepad* gamepad_ = nullptr;
  uint64_t last_gamepad_throttle_change_ms_ = 0;
  rtc_console::GamepadDogActionState gamepad_dog_actions_;
  rtc_console::VehicleEventAckTracker vehicle_event_acks_;
  uint64_t vehicle_control_seq_ = 1;
  uint64_t vehicle_event_request_id_ = 1;
  uint64_t video_view_control_seq_ = 1;
  uint64_t last_vehicle_control_sent_ms_ = 0;
  uint64_t last_vehicle_send_error_log_ms_ = 0;
  bool has_last_vehicle_command_ = false;
  vts_rtc::vehicle::DriveCommand last_vehicle_command_;

  std::mutex stats_mutex_;
  std::map<std::string, NetStatsView> net_stats_;

  std::mutex video_mutex_;
  std::map<std::string, std::shared_ptr<VideoFrameView>> remote_video_frames_by_source_;
  std::map<std::string, VideoTextureView> video_textures_;
  std::map<std::string, std::string> video_render_log_states_;
  std::ofstream video_render_log_file_{"rtc_console_video_render.log",
                                       std::ios::app};
  bool video_render_log_error_reported_ = false;
  std::mutex vision_detection_mutex_;
  rtc_console::LocalYolo local_yolo_;
  bool yolo_enabled_ = false;
  std::map<std::string, VisionDetectionOverlayView> vision_detections_by_source_;
  std::map<uint32_t, std::string> vision_class_labels_by_id_;
  std::mutex edge_feedback_mutex_;
  std::map<uint32_t, EdgeFeedbackView> edge_feedback_by_session_;
  bool render_lr_pixel_interleave_ = false;
  std::map<RtcSessionId, uint64_t> video_view_frame_thresholds_;
  rtc_console::VideoViewState video_view_state_;
  std::string fullscreen_video_stream_key_;
  bool focus_fullscreen_video_ = false;
  bool fullscreen_video_window_forced_ = false;
  bool fullscreen_video_window_was_fullscreen_ = false;

  bool rtc_inited_ = false;
  bool video_source_added_ = false;
  bool audio_source_added_ = false;

  std::vector<std::string> video_sources_ = {"Local YUV420p"};
  int selected_video_source_ = 0;
  std::vector<AudioDeviceChoice> audio_input_choices_;
  std::vector<AudioDeviceChoice> audio_output_choices_;
  int selected_audio_input_ = 0;
  int selected_audio_output_ = 1;

  std::vector<std::string> rooms_;
  int selected_room_ = -1;

  char open_room_id_[128];

  std::string rtc_cfg_path_;
  std::string yuv_path_;

  std::atomic<bool> stop_media_feed_{false};
  std::thread video_thread_;

  std::vector<RtcYUV420pFrame> yuv_frames_;
};

RtcConsoleApp* RtcConsoleApp::instance_ = nullptr;

}  // namespace

int main(int argc, char** argv) {
  try {
    const rtc_console::AppOptions options =
        rtc_console::ParseOptions(argc, argv);
    if (options.show_help) {
      rtc_console::PrintUsage(argv[0]);
      return 0;
    }

    RtcConsoleApp app(options);
    if (!app.Init()) {
      return 1;
    }
    app.Run();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "rtc_console: " << ex.what() << std::endl;
    rtc_console::PrintUsage(argv[0]);
    return 1;
  }
}
