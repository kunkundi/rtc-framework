#include <SDL3/SDL.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop_requested(false);

void OnSignal(int) {
  g_stop_requested.store(true);
}

int Xioctl(int fd, unsigned long request, void* arg) {
  int result = 0;
  do {
    result = ioctl(fd, request, arg);
  } while (result == -1 && errno == EINTR);
  return result;
}

std::string FourccToString(uint32_t fourcc) {
  char text[5] = {
      static_cast<char>(fourcc & 0xff),
      static_cast<char>((fourcc >> 8) & 0xff),
      static_cast<char>((fourcc >> 16) & 0xff),
      static_cast<char>((fourcc >> 24) & 0xff),
      '\0'};
  for (int i = 0; i < 4; ++i) {
    if (text[i] == '\0' || text[i] < 32 || text[i] > 126) {
      text[i] = '.';
    }
  }
  return std::string(text);
}

int ClampToByte(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return value;
}

uint32_t MakeFourcc(const char* text) {
  return v4l2_fourcc(text[0], text[1], text[2], text[3]);
}

struct FormatRequest {
  std::string name;
  std::vector<uint32_t> candidates;
};

struct CaptureOptions {
  std::string device = "/dev/video0";
  int width = 0;
  int height = 0;
  int frame_limit = 0;
  int cycles = 1;
  int buffer_count = 4;
  int timeout_ms = 2000;
  int warmup_frames = 0;
  int warmup_delay_ms = 0;
  int reopen_delay_ms = 0;
  bool list_formats = false;
  bool headless = false;
  bool reopen_per_cycle = false;
  std::string save_frame_path;
  FormatRequest format;
  bool trigger_enabled = false;
  std::string trigger_chip = "/dev/gpiochip0";
  int trigger_line = -1;
  int trigger_low_us = 16000;
  int trigger_high_us = 33000;
};

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return value;
}

bool ParsePositiveInt(const std::string& text, int* value) {
  char* end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

FormatRequest ParseFormatRequest(const std::string& text) {
  const std::string key = ToLower(text);
  FormatRequest request;
  request.name = text;

  if (key == "yuyv") {
    request.candidates = {V4L2_PIX_FMT_YUYV};
    return request;
  }
  if (key == "uyvy") {
    request.candidates = {V4L2_PIX_FMT_UYVY};
    return request;
  }
  if (key == "grey" || key == "gray") {
    request.candidates = {V4L2_PIX_FMT_GREY};
    return request;
  }
  if (key == "raw10_rggb") {
    request.candidates = {V4L2_PIX_FMT_SRGGB10P, V4L2_PIX_FMT_SRGGB10};
    return request;
  }
  if (key == "raw10_bggr") {
    request.candidates = {V4L2_PIX_FMT_SBGGR10P, V4L2_PIX_FMT_SBGGR10};
    return request;
  }
  if (key == "raw10_grbg") {
    request.candidates = {V4L2_PIX_FMT_SGRBG10P, V4L2_PIX_FMT_SGRBG10};
    return request;
  }
  if (key == "raw10_gbrg") {
    request.candidates = {V4L2_PIX_FMT_SGBRG10P, V4L2_PIX_FMT_SGBRG10};
    return request;
  }
  if (key == "raw12_rggb") {
    request.candidates = {V4L2_PIX_FMT_SRGGB12P, V4L2_PIX_FMT_SRGGB12};
    return request;
  }
  if (key == "raw12_bggr") {
    request.candidates = {V4L2_PIX_FMT_SBGGR12P, V4L2_PIX_FMT_SBGGR12};
    return request;
  }
  if (key == "raw12_grbg") {
    request.candidates = {V4L2_PIX_FMT_SGRBG12P, V4L2_PIX_FMT_SGRBG12};
    return request;
  }
  if (key == "raw12_gbrg") {
    request.candidates = {V4L2_PIX_FMT_SGBRG12P, V4L2_PIX_FMT_SGBRG12};
    return request;
  }
  if (text.size() == 4) {
    request.candidates = {MakeFourcc(text.c_str())};
    return request;
  }

  throw std::runtime_error("unsupported format alias: " + text);
}

void PrintUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "\n"
      << "Options:\n"
      << "  --device /dev/video0       Video node to open\n"
      << "  --width 1280               Requested capture width\n"
      << "  --height 720               Requested capture height\n"
      << "  --format uyvy              Requested pixel format, default is uyvy\n"
      << "  --frame-limit 300          Exit after N frames, 0 means run until quit\n"
      << "  --cycles 1                 Number of capture cycles in one process\n"
      << "  --buffer-count 4           Number of mmap capture buffers\n"
      << "  --timeout-ms 2000          Poll timeout while waiting for frames\n"
      << "  --warmup-frames 0          Discard N frames after each open\n"
      << "  --warmup-delay-ms 0        Sleep before warmup after each open\n"
      << "  --reopen-per-cycle         Close and reopen device between cycles\n"
      << "  --reopen-delay-ms 0        Delay between cycles, useful with reopen-per-cycle\n"
      << "  --list-formats             Print supported formats and exit\n"
      << "  --headless                 Capture without opening an SDL window\n"
      << "  --save-frame frame.ppm     Save the first decoded frame as PPM\n"
      << "  --trigger-chip /dev/gpiochip0  GPIO chip for optional FSYNC generation\n"
      << "  --trigger-line 145         GPIO line for optional FSYNC generation\n"
      << "  --trigger-low-us 16000     Low pulse width for optional FSYNC generation\n"
      << "  --trigger-high-us 33000    High pulse width for optional FSYNC generation\n"
      << "  --imx390-trigger           Enable built-in FSYNC defaults for z_imx390_5200_9295\n"
      << "  --help                     Show this message\n"
      << "\n"
      << "Examples:\n"
      << "  " << program << " --device /dev/video0\n"
      << "  " << program
      << " --device /dev/video0 --format raw10_rggb --width 1280 --height 800\n"
      << "  " << program
      << " --device /dev/video1 --format raw12_grbg --width 3840 --height 2160\n"
      << "  " << program
      << " --device /dev/video0 --imx390-trigger\n"
      << "  " << program
      << " --device /dev/video0 --cycles 3 --frame-limit 60 --warmup-frames 4\n"
      << std::endl;
}

CaptureOptions ParseArgs(int argc, char** argv) {
  CaptureOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string("missing value for ") + name);
      }
      ++i;
      return argv[i];
    };

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      exit(0);
    } else if (arg == "--device") {
      options.device = require_value("--device");
    } else if (arg == "--width") {
      if (!ParsePositiveInt(require_value("--width"), &options.width)) {
        throw std::runtime_error("invalid --width value");
      }
    } else if (arg == "--height") {
      if (!ParsePositiveInt(require_value("--height"), &options.height)) {
        throw std::runtime_error("invalid --height value");
      }
    } else if (arg == "--frame-limit") {
      if (!ParsePositiveInt(require_value("--frame-limit"), &options.frame_limit)) {
        throw std::runtime_error("invalid --frame-limit value");
      }
    } else if (arg == "--cycles") {
      if (!ParsePositiveInt(require_value("--cycles"), &options.cycles) ||
          options.cycles < 1) {
        throw std::runtime_error("invalid --cycles value");
      }
    } else if (arg == "--buffer-count") {
      if (!ParsePositiveInt(require_value("--buffer-count"), &options.buffer_count) ||
          options.buffer_count < 2) {
        throw std::runtime_error("invalid --buffer-count value");
      }
    } else if (arg == "--timeout-ms") {
      if (!ParsePositiveInt(require_value("--timeout-ms"), &options.timeout_ms)) {
        throw std::runtime_error("invalid --timeout-ms value");
      }
    } else if (arg == "--warmup-frames") {
      if (!ParsePositiveInt(require_value("--warmup-frames"), &options.warmup_frames)) {
        throw std::runtime_error("invalid --warmup-frames value");
      }
    } else if (arg == "--warmup-delay-ms") {
      if (!ParsePositiveInt(require_value("--warmup-delay-ms"),
                            &options.warmup_delay_ms)) {
        throw std::runtime_error("invalid --warmup-delay-ms value");
      }
    } else if (arg == "--reopen-delay-ms") {
      if (!ParsePositiveInt(require_value("--reopen-delay-ms"),
                            &options.reopen_delay_ms)) {
        throw std::runtime_error("invalid --reopen-delay-ms value");
      }
    } else if (arg == "--reopen-per-cycle") {
      options.reopen_per_cycle = true;
    } else if (arg == "--format") {
      options.format = ParseFormatRequest(require_value("--format"));
    } else if (arg == "--save-frame") {
      options.save_frame_path = require_value("--save-frame");
    } else if (arg == "--trigger-chip") {
      options.trigger_chip = require_value("--trigger-chip");
    } else if (arg == "--trigger-line") {
      if (!ParsePositiveInt(require_value("--trigger-line"), &options.trigger_line)) {
        throw std::runtime_error("invalid --trigger-line value");
      }
      options.trigger_enabled = true;
    } else if (arg == "--trigger-low-us") {
      if (!ParsePositiveInt(require_value("--trigger-low-us"), &options.trigger_low_us)) {
        throw std::runtime_error("invalid --trigger-low-us value");
      }
      options.trigger_enabled = true;
    } else if (arg == "--trigger-high-us") {
      if (!ParsePositiveInt(require_value("--trigger-high-us"), &options.trigger_high_us)) {
        throw std::runtime_error("invalid --trigger-high-us value");
      }
      options.trigger_enabled = true;
    } else if (arg == "--imx390-trigger") {
      options.trigger_enabled = true;
      options.trigger_chip = "/dev/gpiochip0";
      options.trigger_line = 145;
      options.trigger_low_us = 16000;
      options.trigger_high_us = 33000;
    } else if (arg == "--list-formats") {
      options.list_formats = true;
    } else if (arg == "--headless") {
      options.headless = true;
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  if (options.format.candidates.empty()) {
    options.format = ParseFormatRequest("uyvy");
  }
  if (options.trigger_enabled && options.trigger_line < 0) {
    throw std::runtime_error(
        "trigger requested but no GPIO line was provided; use --trigger-line or --imx390-trigger");
  }
  return options;
}

class GpioTriggerGenerator {
 public:
  ~GpioTriggerGenerator() { Stop(); }

  void Start(const CaptureOptions& options) {
    if (!options.trigger_enabled) {
      return;
    }

    chip_fd_ = open(options.trigger_chip.c_str(), O_RDONLY);
    if (chip_fd_ < 0) {
      throw std::runtime_error("failed to open " + options.trigger_chip + ": " +
                               strerror(errno));
    }

    gpiohandle_request request;
    memset(&request, 0, sizeof(request));
    request.lines = 1;
    request.lineoffsets[0] = static_cast<unsigned int>(options.trigger_line);
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = 0;
    snprintf(request.consumer_label, sizeof(request.consumer_label),
             "v4l2_camera_test");

    if (ioctl(chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
      const std::string error = strerror(errno);
      close(chip_fd_);
      chip_fd_ = -1;
      throw std::runtime_error("failed to request GPIO line " +
                               std::to_string(options.trigger_line) + " on " +
                               options.trigger_chip + ": " + error);
    }

    line_fd_ = request.fd;
    low_us_ = options.trigger_low_us;
    high_us_ = options.trigger_high_us;
    stop_.store(false);
    worker_ = std::thread([this]() { RunLoop(); });

    std::cout << "Started GPIO trigger on " << options.trigger_chip
              << " line " << options.trigger_line << " low=" << low_us_
              << "us high=" << high_us_ << "us" << std::endl;
  }

  void Stop() {
    stop_.store(true);
    if (worker_.joinable()) {
      worker_.join();
    }

    if (line_fd_ >= 0) {
      close(line_fd_);
      line_fd_ = -1;
    }
    if (chip_fd_ >= 0) {
      close(chip_fd_);
      chip_fd_ = -1;
    }
  }

 private:
  void SetValue(uint8_t value) {
    gpiohandle_data data;
    memset(&data, 0, sizeof(data));
    data.values[0] = value;
    if (ioctl(line_fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
      throw std::runtime_error("failed to set GPIO value: " +
                               std::string(strerror(errno)));
    }
  }

  void RunLoop() {
    try {
      while (!stop_.load()) {
        SetValue(0);
        std::this_thread::sleep_for(std::chrono::microseconds(low_us_));
        if (stop_.load()) {
          break;
        }
        SetValue(1);
        std::this_thread::sleep_for(std::chrono::microseconds(high_us_));
      }
      SetValue(0);
    } catch (const std::exception& ex) {
      std::cerr << "GPIO trigger thread stopped: " << ex.what() << std::endl;
      stop_.store(true);
    }
  }

  std::atomic<bool> stop_{false};
  int chip_fd_ = -1;
  int line_fd_ = -1;
  int low_us_ = 16000;
  int high_us_ = 33000;
  std::thread worker_;
};

enum class BayerPattern {
  kRGGB,
  kBGGR,
  kGRBG,
  kGBRG,
};

enum class PixelClass {
  kYuyvFamily,
  kGrey,
  kBayer,
  kUnsupported,
};

struct PixelFormatInfo {
  PixelClass pixel_class = PixelClass::kUnsupported;
  BayerPattern bayer_pattern = BayerPattern::kRGGB;
  int bits_per_sample = 8;
  bool packed = false;
  bool uyvy_order = false;
};

PixelFormatInfo DescribePixelFormat(uint32_t format) {
  if (format == V4L2_PIX_FMT_YUYV) {
    PixelFormatInfo info;
    info.pixel_class = PixelClass::kYuyvFamily;
    info.uyvy_order = false;
    return info;
  }
  if (format == V4L2_PIX_FMT_UYVY) {
    PixelFormatInfo info;
    info.pixel_class = PixelClass::kYuyvFamily;
    info.uyvy_order = true;
    return info;
  }
  if (format == V4L2_PIX_FMT_GREY) {
    PixelFormatInfo info;
    info.pixel_class = PixelClass::kGrey;
    return info;
  }

  const struct BayerEntry {
    uint32_t fourcc;
    BayerPattern pattern;
    int bits;
    bool packed;
  } entries[] = {
      {V4L2_PIX_FMT_SRGGB8, BayerPattern::kRGGB, 8, false},
      {V4L2_PIX_FMT_SBGGR8, BayerPattern::kBGGR, 8, false},
      {V4L2_PIX_FMT_SGRBG8, BayerPattern::kGRBG, 8, false},
      {V4L2_PIX_FMT_SGBRG8, BayerPattern::kGBRG, 8, false},
      {V4L2_PIX_FMT_SRGGB10, BayerPattern::kRGGB, 10, false},
      {V4L2_PIX_FMT_SBGGR10, BayerPattern::kBGGR, 10, false},
      {V4L2_PIX_FMT_SGRBG10, BayerPattern::kGRBG, 10, false},
      {V4L2_PIX_FMT_SGBRG10, BayerPattern::kGBRG, 10, false},
      {V4L2_PIX_FMT_SRGGB10P, BayerPattern::kRGGB, 10, true},
      {V4L2_PIX_FMT_SBGGR10P, BayerPattern::kBGGR, 10, true},
      {V4L2_PIX_FMT_SGRBG10P, BayerPattern::kGRBG, 10, true},
      {V4L2_PIX_FMT_SGBRG10P, BayerPattern::kGBRG, 10, true},
      {V4L2_PIX_FMT_SRGGB12, BayerPattern::kRGGB, 12, false},
      {V4L2_PIX_FMT_SBGGR12, BayerPattern::kBGGR, 12, false},
      {V4L2_PIX_FMT_SGRBG12, BayerPattern::kGRBG, 12, false},
      {V4L2_PIX_FMT_SGBRG12, BayerPattern::kGBRG, 12, false},
      {V4L2_PIX_FMT_SRGGB12P, BayerPattern::kRGGB, 12, true},
      {V4L2_PIX_FMT_SBGGR12P, BayerPattern::kBGGR, 12, true},
      {V4L2_PIX_FMT_SGRBG12P, BayerPattern::kGRBG, 12, true},
      {V4L2_PIX_FMT_SGBRG12P, BayerPattern::kGBRG, 12, true},
      {V4L2_PIX_FMT_SRGGB16, BayerPattern::kRGGB, 16, false},
      {V4L2_PIX_FMT_SBGGR16, BayerPattern::kBGGR, 16, false},
      {V4L2_PIX_FMT_SGRBG16, BayerPattern::kGRBG, 16, false},
      {V4L2_PIX_FMT_SGBRG16, BayerPattern::kGBRG, 16, false},
  };

  for (const auto& entry : entries) {
    if (entry.fourcc == format) {
      PixelFormatInfo info;
      info.pixel_class = PixelClass::kBayer;
      info.bayer_pattern = entry.pattern;
      info.bits_per_sample = entry.bits;
      info.packed = entry.packed;
      return info;
    }
  }

  return PixelFormatInfo();
}

void PrintCurrentFormat(const v4l2_format& format) {
  const auto& pix = format.fmt.pix;
  std::cout << "Selected format: " << FourccToString(pix.pixelformat) << " ("
            << pix.width << "x" << pix.height << "), bytesperline="
            << pix.bytesperline << ", sizeimage=" << pix.sizeimage << std::endl;
}

bool CurrentFormatMatchesRequest(const v4l2_format& current,
                                 const CaptureOptions& options) {
  if (options.width > 0 && current.fmt.pix.width !=
                               static_cast<uint32_t>(options.width)) {
    return false;
  }
  if (options.height > 0 && current.fmt.pix.height !=
                                static_cast<uint32_t>(options.height)) {
    return false;
  }
  return std::find(options.format.candidates.begin(), options.format.candidates.end(),
                   current.fmt.pix.pixelformat) != options.format.candidates.end();
}

void EnumerateFrameSizes(int fd, uint32_t pixel_format) {
  v4l2_frmsizeenum frame_size;
  memset(&frame_size, 0, sizeof(frame_size));
  frame_size.pixel_format = pixel_format;
  for (frame_size.index = 0;
       Xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frame_size) == 0;
       ++frame_size.index) {
    std::cout << "      size[" << frame_size.index << "]: ";
    if (frame_size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      std::cout << frame_size.discrete.width << "x"
                << frame_size.discrete.height;
    } else if (frame_size.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
      std::cout << frame_size.stepwise.min_width << "x"
                << frame_size.stepwise.min_height << " .. "
                << frame_size.stepwise.max_width << "x"
                << frame_size.stepwise.max_height << " step "
                << frame_size.stepwise.step_width << "x"
                << frame_size.stepwise.step_height;
    } else {
      std::cout << "continuous";
    }
    std::cout << std::endl;
  }
}

void PrintSupportedFormats(int fd) {
  v4l2_fmtdesc desc;
  memset(&desc, 0, sizeof(desc));
  desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  std::cout << "Supported V4L2 capture formats:" << std::endl;
  for (desc.index = 0; Xioctl(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
    std::cout << "  [" << desc.index << "] "
              << FourccToString(desc.pixelformat)
              << "  description=" << reinterpret_cast<const char*>(desc.description)
              << std::endl;
    EnumerateFrameSizes(fd, desc.pixelformat);
  }
}

struct MappedBuffer {
  void* start = nullptr;
  size_t length = 0;
};

class V4l2CaptureDevice {
 public:
  ~V4l2CaptureDevice() { Close(); }

  void Open(const CaptureOptions& options) {
    fd_ = open(options.device.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (fd_ < 0) {
      throw std::runtime_error("failed to open " + options.device + ": " +
                               strerror(errno));
    }
    device_path_ = options.device;

    v4l2_capability caps;
    memset(&caps, 0, sizeof(caps));
    if (Xioctl(fd_, VIDIOC_QUERYCAP, &caps) < 0) {
      throw std::runtime_error("VIDIOC_QUERYCAP failed: " +
                               std::string(strerror(errno)));
    }
    if ((caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
      throw std::runtime_error("device is not a V4L2 capture node");
    }
    if ((caps.capabilities & V4L2_CAP_STREAMING) == 0) {
      throw std::runtime_error("device does not support streaming I/O");
    }

    std::cout << "Opened " << device_path_ << " driver=" << caps.driver
              << " card=" << caps.card << std::endl;

    if (options.list_formats) {
      PrintSupportedFormats(fd_);
      return;
    }

    ConfigureFormat(options);
    InitMmap(options.buffer_count);
    QueueAllBuffers();
    StartStreaming();
  }

  void Close() {
    StopStreaming();
    for (auto& buffer : buffers_) {
      if (buffer.start && buffer.length > 0) {
        munmap(buffer.start, buffer.length);
      }
    }
    buffers_.clear();

    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

  bool DequeueFrame(std::vector<uint8_t>* rgb_frame) {
    if (fd_ < 0) {
      throw std::runtime_error("capture device is not open");
    }

    pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int poll_ret = poll(&pfd, 1, timeout_ms_);
    if (poll_ret == 0) {
      std::cerr << "poll timeout after " << timeout_ms_ << " ms" << std::endl;
      return false;
    }
    if (poll_ret < 0) {
      if (errno == EINTR) {
        return false;
      }
      throw std::runtime_error("poll failed: " + std::string(strerror(errno)));
    }

    v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
      if (errno == EAGAIN) {
        return false;
      }
      throw std::runtime_error("VIDIOC_DQBUF failed: " +
                               std::string(strerror(errno)));
    }

    if (buffer.index >= buffers_.size()) {
      throw std::runtime_error("driver returned invalid buffer index");
    }

    const uint8_t* data =
        static_cast<const uint8_t*>(buffers_[buffer.index].start);
    DecodeFrame(data, buffer.bytesused, rgb_frame);

    if (Xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QBUF failed: " +
                               std::string(strerror(errno)));
    }
    return true;
  }

  bool streaming_started() const { return streaming_started_; }
  bool list_only() const { return list_only_; }
  uint32_t pixel_format() const { return format_.fmt.pix.pixelformat; }
  uint32_t width() const { return format_.fmt.pix.width; }
  uint32_t height() const { return format_.fmt.pix.height; }
  const std::string& device_path() const { return device_path_; }

 private:
  void ConfigureFormat(const CaptureOptions& options) {
    timeout_ms_ = options.timeout_ms;
    list_only_ = false;

    v4l2_format current;
    memset(&current, 0, sizeof(current));
    current.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (Xioctl(fd_, VIDIOC_G_FMT, &current) < 0) {
      throw std::runtime_error("VIDIOC_G_FMT failed: " +
                               std::string(strerror(errno)));
    }

    if (CurrentFormatMatchesRequest(current, options)) {
      format_ = current;
      format_info_ = DescribePixelFormat(format_.fmt.pix.pixelformat);
      if (format_info_.pixel_class == PixelClass::kUnsupported) {
        throw std::runtime_error("selected pixel format " +
                                 FourccToString(format_.fmt.pix.pixelformat) +
                                 " is not supported by this test tool");
      }
      PrintCurrentFormat(format_);
      return;
    }

    std::vector<uint32_t> candidates = options.format.candidates;

    bool configured = false;
    for (const uint32_t candidate : candidates) {
      v4l2_format desired = current;
      desired.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (options.width > 0) {
        desired.fmt.pix.width = static_cast<uint32_t>(options.width);
      }
      if (options.height > 0) {
        desired.fmt.pix.height = static_cast<uint32_t>(options.height);
      }
      desired.fmt.pix.pixelformat = candidate;
      desired.fmt.pix.field = V4L2_FIELD_ANY;

      if (Xioctl(fd_, VIDIOC_S_FMT, &desired) < 0) {
        continue;
      }

      const bool matches_requested =
          std::find(candidates.begin(), candidates.end(),
                    desired.fmt.pix.pixelformat) != candidates.end();
      if (matches_requested) {
        format_ = desired;
        format_info_ = DescribePixelFormat(format_.fmt.pix.pixelformat);
        configured = true;
        break;
      }
    }

    if (!configured) {
      throw std::runtime_error(
          "unable to apply the requested width/height/pixel-format");
    }

    if (format_info_.pixel_class == PixelClass::kUnsupported) {
      throw std::runtime_error("selected pixel format " +
                               FourccToString(format_.fmt.pix.pixelformat) +
                               " is not supported by this test tool");
    }
    PrintCurrentFormat(format_);
  }

  void InitMmap(int requested_count) {
    v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = static_cast<uint32_t>(requested_count);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
      throw std::runtime_error("VIDIOC_REQBUFS failed: " +
                               std::string(strerror(errno)));
    }
    if (req.count < 2) {
      throw std::runtime_error("not enough mmap buffers returned by driver");
    }

    buffers_.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buffer;
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (Xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
        throw std::runtime_error("VIDIOC_QUERYBUF failed: " +
                                 std::string(strerror(errno)));
      }

      buffers_[i].length = buffer.length;
      buffers_[i].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd_, buffer.m.offset);
      if (buffers_[i].start == MAP_FAILED) {
        buffers_[i].start = nullptr;
        throw std::runtime_error("mmap failed: " + std::string(strerror(errno)));
      }
    }
  }

  void QueueAllBuffers() {
    for (uint32_t i = 0; i < buffers_.size(); ++i) {
      v4l2_buffer buffer;
      memset(&buffer, 0, sizeof(buffer));
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = i;
      if (Xioctl(fd_, VIDIOC_QBUF, &buffer) < 0) {
        throw std::runtime_error("VIDIOC_QBUF failed while priming buffers: " +
                                 std::string(strerror(errno)));
      }
    }
  }

  void StartStreaming() {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (Xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      throw std::runtime_error("VIDIOC_STREAMON failed: " +
                               std::string(strerror(errno)));
    }
    streaming_started_ = true;
  }

  void StopStreaming() {
    if (fd_ >= 0 && streaming_started_) {
      v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      Xioctl(fd_, VIDIOC_STREAMOFF, &type);
      streaming_started_ = false;
    }
  }

  static int NormalizedToByte(uint16_t value, int bits_per_sample) {
    const int max_value = (1 << bits_per_sample) - 1;
    return (static_cast<int>(value) * 255 + max_value / 2) / max_value;
  }

  void DecodeFrame(const uint8_t* data, size_t bytes_used,
                   std::vector<uint8_t>* rgb_frame) const {
    if (format_info_.pixel_class == PixelClass::kYuyvFamily) {
      DecodeYuyvFamily(data, bytes_used, rgb_frame);
      return;
    }
    if (format_info_.pixel_class == PixelClass::kGrey) {
      DecodeGrey(data, bytes_used, rgb_frame);
      return;
    }
    if (format_info_.pixel_class == PixelClass::kBayer) {
      DecodeBayer(data, bytes_used, rgb_frame);
      return;
    }
    throw std::runtime_error("unsupported pixel format for decoding");
  }

  void DecodeGrey(const uint8_t* data, size_t bytes_used,
                  std::vector<uint8_t>* rgb_frame) const {
    const size_t stride =
        format_.fmt.pix.bytesperline ? format_.fmt.pix.bytesperline : width();
    if (bytes_used < stride * height()) {
      throw std::runtime_error("GREY frame is smaller than expected");
    }

    rgb_frame->assign(static_cast<size_t>(width()) * height() * 3, 0);
    for (uint32_t y = 0; y < height(); ++y) {
      const uint8_t* src = data + static_cast<size_t>(y) * stride;
      uint8_t* dst = rgb_frame->data() + static_cast<size_t>(y) * width() * 3;
      for (uint32_t x = 0; x < width(); ++x) {
        dst[x * 3 + 0] = src[x];
        dst[x * 3 + 1] = src[x];
        dst[x * 3 + 2] = src[x];
      }
    }
  }

  static void YuvToRgb(int y, int u, int v, uint8_t* rgb) {
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    const int r = (298 * c + 409 * e + 128) >> 8;
    const int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    const int b = (298 * c + 516 * d + 128) >> 8;
    rgb[0] = static_cast<uint8_t>(ClampToByte(r));
    rgb[1] = static_cast<uint8_t>(ClampToByte(g));
    rgb[2] = static_cast<uint8_t>(ClampToByte(b));
  }

  void DecodeYuyvFamily(const uint8_t* data, size_t bytes_used,
                        std::vector<uint8_t>* rgb_frame) const {
    const size_t stride = format_.fmt.pix.bytesperline
                              ? format_.fmt.pix.bytesperline
                              : static_cast<size_t>(width()) * 2;
    if (bytes_used < stride * height()) {
      throw std::runtime_error("YUYV/UYVY frame is smaller than expected");
    }

    rgb_frame->assign(static_cast<size_t>(width()) * height() * 3, 0);
    for (uint32_t y = 0; y < height(); ++y) {
      const uint8_t* src = data + static_cast<size_t>(y) * stride;
      uint8_t* dst = rgb_frame->data() + static_cast<size_t>(y) * width() * 3;
      for (uint32_t x = 0; x + 1 < width(); x += 2) {
        int y0 = 0;
        int y1 = 0;
        int u = 0;
        int v = 0;
        if (format_info_.uyvy_order) {
          u = src[0];
          y0 = src[1];
          v = src[2];
          y1 = src[3];
        } else {
          y0 = src[0];
          u = src[1];
          y1 = src[2];
          v = src[3];
        }
        YuvToRgb(y0, u, v, dst + x * 3);
        YuvToRgb(y1, u, v, dst + (x + 1) * 3);
        src += 4;
      }
    }
  }

  std::vector<uint16_t> UnpackBayer(const uint8_t* data, size_t bytes_used) const {
    const uint32_t w = width();
    const uint32_t h = height();
    const size_t stride = format_.fmt.pix.bytesperline
                              ? format_.fmt.pix.bytesperline
                              : (format_info_.packed
                                     ? static_cast<size_t>((w * format_info_.bits_per_sample + 7) / 8)
                                     : static_cast<size_t>(w) * 2);
    if (bytes_used < stride * h) {
      throw std::runtime_error("raw frame is smaller than expected");
    }

    std::vector<uint16_t> raw(static_cast<size_t>(w) * h, 0);
    for (uint32_t row = 0; row < h; ++row) {
      const uint8_t* src = data + static_cast<size_t>(row) * stride;
      uint16_t* dst = raw.data() + static_cast<size_t>(row) * w;
      if (format_info_.bits_per_sample == 8) {
        for (uint32_t x = 0; x < w; ++x) {
          dst[x] = src[x];
        }
        continue;
      }

      if (!format_info_.packed) {
        for (uint32_t x = 0; x < w; ++x) {
          const uint16_t value =
              static_cast<uint16_t>(src[x * 2] | (src[x * 2 + 1] << 8));
          const uint16_t mask =
              static_cast<uint16_t>((1u << format_info_.bits_per_sample) - 1u);
          dst[x] = value & mask;
        }
        continue;
      }

      if (format_info_.bits_per_sample == 10) {
        uint32_t x = 0;
        size_t offset = 0;
        while (x + 3 < w) {
          dst[x + 0] =
              static_cast<uint16_t>((src[offset + 0] << 2) | (src[offset + 4] & 0x03));
          dst[x + 1] = static_cast<uint16_t>(
              (src[offset + 1] << 2) | ((src[offset + 4] >> 2) & 0x03));
          dst[x + 2] = static_cast<uint16_t>(
              (src[offset + 2] << 2) | ((src[offset + 4] >> 4) & 0x03));
          dst[x + 3] = static_cast<uint16_t>(
              (src[offset + 3] << 2) | ((src[offset + 4] >> 6) & 0x03));
          x += 4;
          offset += 5;
        }
        for (; x < w; ++x) {
          const size_t base = static_cast<size_t>(x) * 10 / 8;
          const size_t shift = static_cast<size_t>(x % 4) * 2;
          const uint16_t value = static_cast<uint16_t>(
              (src[base] << 2) | ((src[base + 4 - (x % 4)] >> shift) & 0x03));
          dst[x] = value;
        }
      } else if (format_info_.bits_per_sample == 12) {
        uint32_t x = 0;
        size_t offset = 0;
        while (x + 1 < w) {
          dst[x + 0] =
              static_cast<uint16_t>((src[offset + 0] << 4) | (src[offset + 2] & 0x0f));
          dst[x + 1] = static_cast<uint16_t>(
              (src[offset + 1] << 4) | ((src[offset + 2] >> 4) & 0x0f));
          x += 2;
          offset += 3;
        }
      } else {
        throw std::runtime_error("packed Bayer depth is not supported");
      }
    }
    return raw;
  }

  static bool IsRed(BayerPattern pattern, int x, int y) {
    const bool even_row = (y & 1) == 0;
    const bool even_col = (x & 1) == 0;
    switch (pattern) {
      case BayerPattern::kRGGB:
        return even_row && even_col;
      case BayerPattern::kBGGR:
        return !even_row && !even_col;
      case BayerPattern::kGRBG:
        return even_row && !even_col;
      case BayerPattern::kGBRG:
        return !even_row && even_col;
    }
    return false;
  }

  static bool IsBlue(BayerPattern pattern, int x, int y) {
    const bool even_row = (y & 1) == 0;
    const bool even_col = (x & 1) == 0;
    switch (pattern) {
      case BayerPattern::kRGGB:
        return !even_row && !even_col;
      case BayerPattern::kBGGR:
        return even_row && even_col;
      case BayerPattern::kGRBG:
        return !even_row && even_col;
      case BayerPattern::kGBRG:
        return even_row && !even_col;
    }
    return false;
  }

  static uint16_t SampleRaw(const std::vector<uint16_t>& raw, int width, int height,
                            int x, int y) {
    x = std::max(0, std::min(width - 1, x));
    y = std::max(0, std::min(height - 1, y));
    return raw[static_cast<size_t>(y) * width + x];
  }

  void DecodeBayer(const uint8_t* data, size_t bytes_used,
                   std::vector<uint8_t>* rgb_frame) const {
    const int w = static_cast<int>(width());
    const int h = static_cast<int>(height());
    const std::vector<uint16_t> raw = UnpackBayer(data, bytes_used);
    rgb_frame->assign(static_cast<size_t>(w) * h * 3, 0);

    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        uint16_t r = 0;
        uint16_t g = 0;
        uint16_t b = 0;
        const uint16_t center = SampleRaw(raw, w, h, x, y);

        if (IsRed(format_info_.bayer_pattern, x, y)) {
          r = center;
          g = static_cast<uint16_t>(
              (SampleRaw(raw, w, h, x - 1, y) + SampleRaw(raw, w, h, x + 1, y) +
               SampleRaw(raw, w, h, x, y - 1) + SampleRaw(raw, w, h, x, y + 1)) /
              4);
          b = static_cast<uint16_t>(
              (SampleRaw(raw, w, h, x - 1, y - 1) +
               SampleRaw(raw, w, h, x + 1, y - 1) +
               SampleRaw(raw, w, h, x - 1, y + 1) +
               SampleRaw(raw, w, h, x + 1, y + 1)) /
              4);
        } else if (IsBlue(format_info_.bayer_pattern, x, y)) {
          b = center;
          g = static_cast<uint16_t>(
              (SampleRaw(raw, w, h, x - 1, y) + SampleRaw(raw, w, h, x + 1, y) +
               SampleRaw(raw, w, h, x, y - 1) + SampleRaw(raw, w, h, x, y + 1)) /
              4);
          r = static_cast<uint16_t>(
              (SampleRaw(raw, w, h, x - 1, y - 1) +
               SampleRaw(raw, w, h, x + 1, y - 1) +
               SampleRaw(raw, w, h, x - 1, y + 1) +
               SampleRaw(raw, w, h, x + 1, y + 1)) /
              4);
        } else {
          g = center;
          const bool horizontal_is_red =
              IsRed(format_info_.bayer_pattern, x - 1, y) ||
              IsRed(format_info_.bayer_pattern, x + 1, y);
          if (horizontal_is_red) {
            r = static_cast<uint16_t>(
                (SampleRaw(raw, w, h, x - 1, y) + SampleRaw(raw, w, h, x + 1, y)) /
                2);
            b = static_cast<uint16_t>(
                (SampleRaw(raw, w, h, x, y - 1) + SampleRaw(raw, w, h, x, y + 1)) /
                2);
          } else {
            r = static_cast<uint16_t>(
                (SampleRaw(raw, w, h, x, y - 1) + SampleRaw(raw, w, h, x, y + 1)) /
                2);
            b = static_cast<uint16_t>(
                (SampleRaw(raw, w, h, x - 1, y) + SampleRaw(raw, w, h, x + 1, y)) /
                2);
          }
        }

        uint8_t* dst =
            rgb_frame->data() + (static_cast<size_t>(y) * w + x) * 3;
        dst[0] = static_cast<uint8_t>(NormalizedToByte(r, format_info_.bits_per_sample));
        dst[1] = static_cast<uint8_t>(NormalizedToByte(g, format_info_.bits_per_sample));
        dst[2] = static_cast<uint8_t>(NormalizedToByte(b, format_info_.bits_per_sample));
      }
    }
  }

  int fd_ = -1;
  int timeout_ms_ = 2000;
  bool streaming_started_ = false;
  bool list_only_ = false;
  std::string device_path_;
  v4l2_format format_{};
  PixelFormatInfo format_info_;
  std::vector<MappedBuffer> buffers_;
};

bool SavePpm(const std::string& path, uint32_t width, uint32_t height,
             const std::vector<uint8_t>& rgb_frame) {
  std::ofstream out(path.c_str(), std::ios::binary);
  if (!out.is_open()) {
    std::cerr << "failed to open " << path << " for writing" << std::endl;
    return false;
  }
  out << "P6\n" << width << " " << height << "\n255\n";
  out.write(reinterpret_cast<const char*>(rgb_frame.data()),
            static_cast<std::streamsize>(rgb_frame.size()));
  return out.good();
}

class SdlViewer {
 public:
  ~SdlViewer() { Shutdown(); }

  void Init(const std::string& title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
    }

    window_ =
        SDL_CreateWindow(title.c_str(), width, height, SDL_WINDOW_RESIZABLE);
    if (!window_) {
      throw std::runtime_error("SDL_CreateWindow failed: " +
                               std::string(SDL_GetError()));
    }

    renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
    if (!renderer_) {
      throw std::runtime_error("SDL_CreateRenderer failed: " +
                               std::string(SDL_GetError()));
    }

    if (!SDL_SetRenderLogicalPresentation(renderer_, width, height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
      throw std::runtime_error("SDL_SetRenderLogicalPresentation failed: " +
                               std::string(SDL_GetError()));
    }

    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!texture_) {
      throw std::runtime_error("SDL_CreateTexture failed: " +
                               std::string(SDL_GetError()));
    }
  }

  bool PumpEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        return false;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        return false;
      }
      if (event.type == SDL_EVENT_KEY_DOWN &&
          (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q)) {
        return false;
      }
    }
    return true;
  }

  void Present(const std::vector<uint8_t>& rgb_frame, int width, int height) {
    if (!SDL_UpdateTexture(texture_, nullptr, rgb_frame.data(), width * 3)) {
      throw std::runtime_error("SDL_UpdateTexture failed: " +
                               std::string(SDL_GetError()));
    }
    if (!SDL_RenderClear(renderer_)) {
      throw std::runtime_error("SDL_RenderClear failed: " +
                               std::string(SDL_GetError()));
    }
    if (!SDL_RenderTexture(renderer_, texture_, nullptr, nullptr)) {
      throw std::runtime_error("SDL_RenderTexture failed: " +
                               std::string(SDL_GetError()));
    }
    if (!SDL_RenderPresent(renderer_)) {
      throw std::runtime_error("SDL_RenderPresent failed: " +
                               std::string(SDL_GetError()));
    }
  }

  void SetTitle(const std::string& title) {
    if (window_) {
      SDL_SetWindowTitle(window_, title.c_str());
    }
  }

  void Shutdown() {
    if (texture_) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    if (renderer_) {
      SDL_DestroyRenderer(renderer_);
      renderer_ = nullptr;
    }
    if (window_) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    SDL_Quit();
  }

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
};

std::string BuildWindowTitle(const V4l2CaptureDevice& device, double fps) {
  std::ostringstream oss;
  oss << "v4l2_camera_test | " << device.device_path() << " | "
      << device.width() << "x" << device.height() << " | "
      << FourccToString(device.pixel_format()) << " | "
      << std::fixed << std::setprecision(1) << fps << " fps";
  return oss.str();
}

void RunWarmup(V4l2CaptureDevice& device, const CaptureOptions& options) {
  if (options.warmup_delay_ms > 0) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(options.warmup_delay_ms));
  }
  if (options.warmup_frames <= 0) {
    return;
  }

  std::vector<uint8_t> discard_frame;
  int discarded = 0;
  while (!g_stop_requested.load() && discarded < options.warmup_frames) {
    if (!device.DequeueFrame(&discard_frame)) {
      continue;
    }
    ++discarded;
  }
  std::cout << "Warmup discarded " << discarded << " frame(s)." << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  try {
    const CaptureOptions options = ParseArgs(argc, argv);
    GpioTriggerGenerator trigger_generator;
    if (options.trigger_enabled) {
      trigger_generator.Start(options);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    SdlViewer viewer;
    std::vector<uint8_t> rgb_frame;
    bool saved_frame = false;
    bool viewer_initialized = false;
    int total_captured_frames = 0;

    V4l2CaptureDevice device;
    auto open_cycle_device = [&]() {
      device.Close();
      device.Open(options);
      if (options.list_formats) {
        return;
      }
      RunWarmup(device, options);
      if (!options.headless && !viewer_initialized) {
        viewer.Init("v4l2_camera_test", static_cast<int>(device.width()),
                    static_cast<int>(device.height()));
        viewer.SetTitle(BuildWindowTitle(device, 0.0));
        viewer_initialized = true;
      }
    };

    open_cycle_device();
    if (options.list_formats) {
      return 0;
    }

    for (int cycle = 0; cycle < options.cycles && !g_stop_requested.load();
         ++cycle) {
      if (cycle > 0) {
        if (options.reopen_delay_ms > 0) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(options.reopen_delay_ms));
        }
        if (options.reopen_per_cycle) {
          open_cycle_device();
        } else {
          RunWarmup(device, options);
        }
      }

      int cycle_captured_frames = 0;
      auto fps_window_begin = std::chrono::steady_clock::now();
      int fps_window_frames = 0;
      std::cout << "Starting cycle " << (cycle + 1) << "/" << options.cycles
                << std::endl;

      while (!g_stop_requested.load()) {
        if (!options.headless && !viewer.PumpEvents()) {
          g_stop_requested.store(true);
          break;
        }

        if (!device.DequeueFrame(&rgb_frame)) {
          continue;
        }

        ++cycle_captured_frames;
        ++total_captured_frames;
        ++fps_window_frames;

        if (!saved_frame && !options.save_frame_path.empty()) {
          saved_frame = SavePpm(options.save_frame_path, device.width(),
                                device.height(), rgb_frame);
          if (saved_frame) {
            std::cout << "Saved first frame to " << options.save_frame_path
                      << std::endl;
          }
        }

        if (!options.headless) {
          viewer.Present(rgb_frame, static_cast<int>(device.width()),
                         static_cast<int>(device.height()));
        }

        const auto now = std::chrono::steady_clock::now();
        const double elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - fps_window_begin)
                .count();
        if (elapsed_ms >= 1000.0) {
          const double fps = fps_window_frames * 1000.0 / elapsed_ms;
          std::cout << "cycle=" << (cycle + 1) << " fps=" << std::fixed
                    << std::setprecision(2) << fps << std::endl;
          if (!options.headless) {
            viewer.SetTitle(BuildWindowTitle(device, fps));
          }
          fps_window_begin = now;
          fps_window_frames = 0;
        }

        if (options.frame_limit > 0 &&
            cycle_captured_frames >= options.frame_limit) {
          break;
        }
      }

      std::cout << "Cycle " << (cycle + 1) << " captured "
                << cycle_captured_frames << " frame(s)." << std::endl;
    }

    std::cout << "Captured " << total_captured_frames << " frame(s) total."
              << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "v4l2_camera_test error: " << ex.what() << std::endl;
    return 1;
  }
}
