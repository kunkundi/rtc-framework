#include "c_rtc.h"

#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
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
constexpr float kVideoViewMinSize = 220.0f;
constexpr float kVideoCardPadding = 14.0f;
constexpr float kVideoOverlayPadding = 8.0f;

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
};

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

ImGuiKey TranslateKeySym(KeySym ks) {
  switch (ks) {
    case XK_Tab:
      return ImGuiKey_Tab;
    case XK_Left:
      return ImGuiKey_LeftArrow;
    case XK_Right:
      return ImGuiKey_RightArrow;
    case XK_Up:
      return ImGuiKey_UpArrow;
    case XK_Down:
      return ImGuiKey_DownArrow;
    case XK_Prior:
      return ImGuiKey_PageUp;
    case XK_Next:
      return ImGuiKey_PageDown;
    case XK_Home:
      return ImGuiKey_Home;
    case XK_End:
      return ImGuiKey_End;
    case XK_Insert:
      return ImGuiKey_Insert;
    case XK_Delete:
      return ImGuiKey_Delete;
    case XK_BackSpace:
      return ImGuiKey_Backspace;
    case XK_space:
      return ImGuiKey_Space;
    case XK_Return:
    case XK_KP_Enter:
      return ImGuiKey_Enter;
    case XK_Escape:
      return ImGuiKey_Escape;
    case XK_apostrophe:
      return ImGuiKey_Apostrophe;
    case XK_comma:
      return ImGuiKey_Comma;
    case XK_minus:
      return ImGuiKey_Minus;
    case XK_period:
      return ImGuiKey_Period;
    case XK_slash:
      return ImGuiKey_Slash;
    case XK_semicolon:
      return ImGuiKey_Semicolon;
    case XK_equal:
      return ImGuiKey_Equal;
    case XK_bracketleft:
      return ImGuiKey_LeftBracket;
    case XK_backslash:
      return ImGuiKey_Backslash;
    case XK_bracketright:
      return ImGuiKey_RightBracket;
    case XK_grave:
      return ImGuiKey_GraveAccent;
    case XK_Caps_Lock:
      return ImGuiKey_CapsLock;
    case XK_Scroll_Lock:
      return ImGuiKey_ScrollLock;
    case XK_Num_Lock:
      return ImGuiKey_NumLock;
    case XK_Print:
      return ImGuiKey_PrintScreen;
    case XK_Pause:
      return ImGuiKey_Pause;
    case XK_KP_0:
      return ImGuiKey_Keypad0;
    case XK_KP_1:
      return ImGuiKey_Keypad1;
    case XK_KP_2:
      return ImGuiKey_Keypad2;
    case XK_KP_3:
      return ImGuiKey_Keypad3;
    case XK_KP_4:
      return ImGuiKey_Keypad4;
    case XK_KP_5:
      return ImGuiKey_Keypad5;
    case XK_KP_6:
      return ImGuiKey_Keypad6;
    case XK_KP_7:
      return ImGuiKey_Keypad7;
    case XK_KP_8:
      return ImGuiKey_Keypad8;
    case XK_KP_9:
      return ImGuiKey_Keypad9;
    case XK_KP_Decimal:
      return ImGuiKey_KeypadDecimal;
    case XK_KP_Divide:
      return ImGuiKey_KeypadDivide;
    case XK_KP_Multiply:
      return ImGuiKey_KeypadMultiply;
    case XK_KP_Subtract:
      return ImGuiKey_KeypadSubtract;
    case XK_KP_Add:
      return ImGuiKey_KeypadAdd;
    case XK_KP_Equal:
      return ImGuiKey_KeypadEqual;
    case XK_Shift_L:
      return ImGuiKey_LeftShift;
    case XK_Shift_R:
      return ImGuiKey_RightShift;
    case XK_Control_L:
      return ImGuiKey_LeftCtrl;
    case XK_Control_R:
      return ImGuiKey_RightCtrl;
    case XK_Alt_L:
      return ImGuiKey_LeftAlt;
    case XK_Alt_R:
      return ImGuiKey_RightAlt;
    case XK_Super_L:
      return ImGuiKey_LeftSuper;
    case XK_Super_R:
      return ImGuiKey_RightSuper;
    case XK_Menu:
      return ImGuiKey_Menu;
    case XK_0:
      return ImGuiKey_0;
    case XK_1:
      return ImGuiKey_1;
    case XK_2:
      return ImGuiKey_2;
    case XK_3:
      return ImGuiKey_3;
    case XK_4:
      return ImGuiKey_4;
    case XK_5:
      return ImGuiKey_5;
    case XK_6:
      return ImGuiKey_6;
    case XK_7:
      return ImGuiKey_7;
    case XK_8:
      return ImGuiKey_8;
    case XK_9:
      return ImGuiKey_9;
    case XK_a:
    case XK_A:
      return ImGuiKey_A;
    case XK_b:
    case XK_B:
      return ImGuiKey_B;
    case XK_c:
    case XK_C:
      return ImGuiKey_C;
    case XK_d:
    case XK_D:
      return ImGuiKey_D;
    case XK_e:
    case XK_E:
      return ImGuiKey_E;
    case XK_f:
    case XK_F:
      return ImGuiKey_F;
    case XK_g:
    case XK_G:
      return ImGuiKey_G;
    case XK_h:
    case XK_H:
      return ImGuiKey_H;
    case XK_i:
    case XK_I:
      return ImGuiKey_I;
    case XK_j:
    case XK_J:
      return ImGuiKey_J;
    case XK_k:
    case XK_K:
      return ImGuiKey_K;
    case XK_l:
    case XK_L:
      return ImGuiKey_L;
    case XK_m:
    case XK_M:
      return ImGuiKey_M;
    case XK_n:
    case XK_N:
      return ImGuiKey_N;
    case XK_o:
    case XK_O:
      return ImGuiKey_O;
    case XK_p:
    case XK_P:
      return ImGuiKey_P;
    case XK_q:
    case XK_Q:
      return ImGuiKey_Q;
    case XK_r:
    case XK_R:
      return ImGuiKey_R;
    case XK_s:
    case XK_S:
      return ImGuiKey_S;
    case XK_t:
    case XK_T:
      return ImGuiKey_T;
    case XK_u:
    case XK_U:
      return ImGuiKey_U;
    case XK_v:
    case XK_V:
      return ImGuiKey_V;
    case XK_w:
    case XK_W:
      return ImGuiKey_W;
    case XK_x:
    case XK_X:
      return ImGuiKey_X;
    case XK_y:
    case XK_Y:
      return ImGuiKey_Y;
    case XK_z:
    case XK_Z:
      return ImGuiKey_Z;
    case XK_F1:
      return ImGuiKey_F1;
    case XK_F2:
      return ImGuiKey_F2;
    case XK_F3:
      return ImGuiKey_F3;
    case XK_F4:
      return ImGuiKey_F4;
    case XK_F5:
      return ImGuiKey_F5;
    case XK_F6:
      return ImGuiKey_F6;
    case XK_F7:
      return ImGuiKey_F7;
    case XK_F8:
      return ImGuiKey_F8;
    case XK_F9:
      return ImGuiKey_F9;
    case XK_F10:
      return ImGuiKey_F10;
    case XK_F11:
      return ImGuiKey_F11;
    case XK_F12:
      return ImGuiKey_F12;
    default:
      return ImGuiKey_None;
  }
}

void UpdateModifierKeys(ImGuiIO& io, unsigned int state) {
  io.AddKeyEvent(ImGuiMod_Ctrl, (state & ControlMask) != 0);
  io.AddKeyEvent(ImGuiMod_Shift, (state & ShiftMask) != 0);
  io.AddKeyEvent(ImGuiMod_Alt, (state & Mod1Mask) != 0);
  io.AddKeyEvent(ImGuiMod_Super, (state & Mod4Mask) != 0);
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

class X11OpenGLWindow {
 public:
  bool Init(const char* title, int width, int height) {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      return false;
    }

    const int screen = DefaultScreen(display_);
    static int visual_attribs[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_DEPTH_SIZE,
        24,
        GLX_STENCIL_SIZE,
        8,
        None,
    };

    XVisualInfo* visual = glXChooseVisual(display_, screen, visual_attribs);
    if (!visual) {
      XCloseDisplay(display_);
      display_ = nullptr;
      return false;
    }

    Colormap colormap = XCreateColormap(display_, RootWindow(display_, visual->screen),
                                        visual->visual, AllocNone);

    XSetWindowAttributes swa;
    swa.colormap = colormap;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | FocusChangeMask;

    window_ = XCreateWindow(display_, RootWindow(display_, visual->screen), 0, 0,
                            width, height, 0, visual->depth, InputOutput,
                            visual->visual, CWColormap | CWEventMask, &swa);

    XStoreName(display_, window_, title);

    wm_delete_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, window_, &wm_delete_, 1);

    glx_context_ = glXCreateContext(display_, visual, nullptr, True);
    XFree(visual);

    if (!glx_context_) {
      XDestroyWindow(display_, window_);
      XCloseDisplay(display_);
      window_ = 0;
      display_ = nullptr;
      return false;
    }

    glXMakeCurrent(display_, window_, glx_context_);
    XMapWindow(display_, window_);
    width_ = width;
    height_ = height;
    return true;
  }

  void Shutdown() {
    if (display_) {
      glXMakeCurrent(display_, None, nullptr);
      if (glx_context_) {
        glXDestroyContext(display_, glx_context_);
        glx_context_ = nullptr;
      }
      if (window_) {
        XDestroyWindow(display_, window_);
        window_ = 0;
      }
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  }

  void PumpEvents(ImGuiIO& io, bool* should_close) {
    while (XPending(display_) > 0) {
      XEvent event;
      XNextEvent(display_, &event);

      switch (event.type) {
        case ClientMessage:
          if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_) {
            *should_close = true;
          }
          break;

        case ConfigureNotify:
          width_ = event.xconfigure.width;
          height_ = event.xconfigure.height;
          break;

        case MotionNotify:
          io.AddMousePosEvent(static_cast<float>(event.xmotion.x),
                              static_cast<float>(event.xmotion.y));
          break;

        case ButtonPress:
          UpdateModifierKeys(io, event.xbutton.state);
          if (event.xbutton.button == Button1) {
            io.AddMouseButtonEvent(0, true);
          } else if (event.xbutton.button == Button2) {
            io.AddMouseButtonEvent(2, true);
          } else if (event.xbutton.button == Button3) {
            io.AddMouseButtonEvent(1, true);
          } else if (event.xbutton.button == Button4) {
            io.AddMouseWheelEvent(0.0f, 1.0f);
          } else if (event.xbutton.button == Button5) {
            io.AddMouseWheelEvent(0.0f, -1.0f);
          }
          break;

        case ButtonRelease:
          UpdateModifierKeys(io, event.xbutton.state);
          if (event.xbutton.button == Button1) {
            io.AddMouseButtonEvent(0, false);
          } else if (event.xbutton.button == Button2) {
            io.AddMouseButtonEvent(2, false);
          } else if (event.xbutton.button == Button3) {
            io.AddMouseButtonEvent(1, false);
          }
          break;

        case KeyPress: {
          UpdateModifierKeys(io, event.xkey.state);

          KeySym keysym = NoSymbol;
          char buffer[64] = {0};
          const int len = XLookupString(&event.xkey, buffer, sizeof(buffer) - 1,
                                        &keysym, nullptr);

          const ImGuiKey key = TranslateKeySym(keysym);
          if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, true);
          }

          if (len > 0) {
            buffer[len] = '\0';
            io.AddInputCharactersUTF8(buffer);
          }
          break;
        }

        case KeyRelease: {
          if (XEventsQueued(display_, QueuedAfterReading)) {
            XEvent next_event;
            XPeekEvent(display_, &next_event);
            if (next_event.type == KeyPress &&
                next_event.xkey.time == event.xkey.time &&
                next_event.xkey.keycode == event.xkey.keycode) {
              break;
            }
          }

          UpdateModifierKeys(io, event.xkey.state);
          KeySym keysym = XkbKeycodeToKeysym(display_, event.xkey.keycode, 0, 0);
          const ImGuiKey key = TranslateKeySym(keysym);
          if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, false);
          }
          break;
        }

        case FocusOut:
          io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
          io.AddMouseButtonEvent(0, false);
          io.AddMouseButtonEvent(1, false);
          io.AddMouseButtonEvent(2, false);
          break;

        default:
          break;
      }
    }
  }

  void SwapBuffers() { glXSwapBuffers(display_, window_); }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  Display* display_ = nullptr;
  Window window_ = 0;
  GLXContext glx_context_ = nullptr;
  Atom wm_delete_ = 0;
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
    X11OpenGLWindow window;
    if (!window.Init("rtc-solutions | p2p_imgui", kMainWindowWidth,
                     kMainWindowHeight)) {
      AppendLog("X11OpenGLWindow init failed");
      return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplOpenGL3_Init("#version 130");

    bool should_close = false;
    auto last_tick = std::chrono::steady_clock::now();

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
      const float video_view_size = std::max(
          kVideoViewMinSize, std::min(panel_avail.x - kVideoCardPadding * 2.0f,
                                      panel_avail.y - kVideoCardPadding * 2.0f));
      const float y_offset = std::max(0.0f, (panel_avail.y - video_view_size) * 0.5f);
      ImGui::SetCursorPosY(start_pos.y + y_offset);
      DrawSingleVideoFrame(*frame_snapshot.front(), video_view_size, panel_avail.x);
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
    const float max_by_width = cell_width - style.CellPadding.x * 2.0f - 12.0f;
    const float max_by_height =
        (panel_avail.y - style.ItemSpacing.y * static_cast<float>(rows - 1)) /
            static_cast<float>(rows) -
        style.CellPadding.y * 2.0f - 12.0f;
    const float video_view_size =
        std::max(160.0f, std::min(max_by_width, max_by_height));
    const float row_height = video_view_size + style.CellPadding.y * 2.0f + 12.0f;
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
        DrawSingleVideoFrame(*frame_snapshot[i], video_view_size, cell_width);
      }
      ImGui::EndTable();
    }

    ImGui::End();
  }

  void DrawSingleVideoFrame(const VideoFrameView& frame,
                            float video_view_size,
                            float content_width) {
    UpdateTextureFromFrame(frame);

    const float start_x = ImGui::GetCursorPosX();
    const float x_offset = std::max(0.0f, (content_width - video_view_size) * 0.5f);
    if (x_offset > 0.0f) {
      ImGui::SetCursorPosX(start_x + x_offset);
    }

    const auto tex_it = video_textures_.find(frame.stream_key);
    if (tex_it == video_textures_.end() || tex_it->second.texture == 0) {
      ImGui::Dummy(ImVec2(video_view_size, video_view_size));
      return;
    }

    if (x_offset > 0.0f) {
      ImGui::SetCursorPosX(start_x + x_offset);
    }
    const ImVec2 box_size(video_view_size, video_view_size);
    const ImVec2 box_min = ImGui::GetCursorScreenPos();
    const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);

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

    const float offset_x = (box_size.x - draw_width) * 0.5f;
    const float offset_y = (box_size.y - draw_height) * 0.5f;
    const ImVec2 image_min(box_min.x + offset_x, box_min.y + offset_y);
    const ImVec2 image_max(image_min.x + draw_width, image_min.y + draw_height);
    draw_list->AddImage((ImTextureID)(intptr_t)tex_it->second.texture, image_min,
                        image_max, ImVec2(0, 0), ImVec2(1, 1));
    draw_list->AddRect(box_min, box_max, IM_COL32(80, 80, 80, 255));

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

    ImGui::Dummy(box_size);
  }

  void UpdateTextureFromFrame(const VideoFrameView& frame) {
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
    }

    if (texture_view.uploaded_frame_seq == frame.frame_seq) {
      return;
    }

    glBindTexture(GL_TEXTURE_2D, texture_view.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int width = static_cast<int>(frame.width);
    const int height = static_cast<int>(frame.height);
    if (texture_view.width != width || texture_view.height != height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA,
                   GL_UNSIGNED_INT_8_8_8_8, frame.buffer.data());
      texture_view.width = width;
      texture_view.height = height;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA,
                      GL_UNSIGNED_INT_8_8_8_8, frame.buffer.data());
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    texture_view.uploaded_frame_seq = frame.frame_seq;
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
