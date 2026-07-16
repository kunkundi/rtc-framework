#include "rtc_logging/rtc_logging.h"
#include "rtc_runtime/process_runtime.h"
#include "rtc_runtime/rtc_session.h"

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rtc_runtime;

namespace {

struct ReceiverOptions {
  SessionOptions session;
  std::string output_dir;
  int frame_limit = 0;
  int idle_timeout_sec = 0;
  int dump_every = 0;
  int dump_max_frames = 0;
};

bool ParsePositiveInt(const std::string& text, int* value) {
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || parsed < 0 ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

void PrintReceiverUsage(const char* program) {
  std::cout
      << "Usage: " << program << " [options]\n"
      << "\n"
      << "This binary is a headless RTC receiver client. It auto logs in, opens\n"
      << "the target room, receives remote media, and can optionally dump frames\n"
      << "to PPM snapshots for offline inspection.\n"
      << "\n"
      << "Options:\n"
      << "  --room zhejianglab         Room to auto open after RTC login\n"
      << "  --config rtc.cfg           RTC config path, defaults to nearby rtc.cfg\n"
      << "  --join-retry-ms 3000       Retry interval for auto open\n"
      << "  --status-interval-sec 5    Periodic status log interval, 0 disables logs\n"
      << "  --frame-limit 0            Exit after receiving N video frames, 0 means run until quit\n"
      << "  --idle-timeout-sec 0       Exit if no video arrives for N seconds after room open, 0 disables\n"
      << "  --dump-every 0             Dump every Nth received frame as PPM, 0 disables dumping\n"
      << "  --dump-max-frames 0        Stop dumping after N frames, 0 means no limit\n"
      << "  --output-dir recv_frames   Directory used for optional PPM dumps\n"
      << "  --help                     Show this message\n"
      << "\n"
      << "Examples:\n"
      << "  " << program << "\n"
      << "  " << program << " --room zhejianglab --frame-limit 300\n"
      << "  " << program
      << " --dump-every 60 --dump-max-frames 10 --output-dir ./recv_frames\n"
      << std::endl;
}

ReceiverOptions ParseReceiverArgs(int argc, char** argv) {
  ReceiverOptions options;

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
      PrintReceiverUsage(argv[0]);
      std::exit(0);
    } else if (arg == "--room") {
      options.session.room_id = require_value("--room");
    } else if (arg == "--config") {
      options.session.config_path = require_value("--config");
    } else if (arg == "--join-retry-ms") {
      if (!ParsePositiveInt(require_value("--join-retry-ms"),
                            &options.session.join_retry_ms)) {
        throw std::runtime_error("invalid --join-retry-ms value");
      }
    } else if (arg == "--status-interval-sec") {
      if (!ParsePositiveInt(require_value("--status-interval-sec"),
                            &options.session.status_interval_sec)) {
        throw std::runtime_error("invalid --status-interval-sec value");
      }
    } else if (arg == "--frame-limit") {
      if (!ParsePositiveInt(require_value("--frame-limit"), &options.frame_limit)) {
        throw std::runtime_error("invalid --frame-limit value");
      }
    } else if (arg == "--idle-timeout-sec") {
      if (!ParsePositiveInt(require_value("--idle-timeout-sec"),
                            &options.idle_timeout_sec)) {
        throw std::runtime_error("invalid --idle-timeout-sec value");
      }
    } else if (arg == "--dump-every") {
      if (!ParsePositiveInt(require_value("--dump-every"), &options.dump_every)) {
        throw std::runtime_error("invalid --dump-every value");
      }
    } else if (arg == "--dump-max-frames") {
      if (!ParsePositiveInt(require_value("--dump-max-frames"),
                            &options.dump_max_frames)) {
        throw std::runtime_error("invalid --dump-max-frames value");
      }
    } else if (arg == "--output-dir") {
      options.output_dir = require_value("--output-dir");
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (options.dump_max_frames > 0 && options.dump_every == 0) {
    throw std::runtime_error("--dump-max-frames requires --dump-every");
  }
  if (options.dump_every > 0 && options.output_dir.empty()) {
    options.output_dir = "recv_frames";
  }

  return options;
}

int64_t SteadyClockNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string SanitizeToken(const char* text) {
  std::string token = text ? text : "";
  if (token.empty()) {
    return "unknown";
  }

  for (char& ch : token) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (!std::isalnum(value) && ch != '_' && ch != '-') {
      ch = '_';
    }
  }
  return token;
}

void EnsureDirectoryExists(const std::string& path) {
  if (path.empty() || path == ".") {
    return;
  }

  std::string current;
  if (!path.empty() && path[0] == '/') {
    current = "/";
  }

  size_t pos = 0;
  while (pos < path.size()) {
    const size_t next = path.find('/', pos);
    const size_t length =
        next == std::string::npos ? path.size() - pos : next - pos;
    const std::string component = path.substr(pos, length);
    pos = next == std::string::npos ? path.size() : next + 1;

    if (component.empty() || component == ".") {
      continue;
    }

    if (!current.empty() && current.back() != '/') {
      current += "/";
    }
    current += component;

    if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
      throw std::runtime_error("failed to create directory: " + current +
                               ", errno=" + std::to_string(errno));
    }
  }
}

void WritePpm(const std::string& path,
              size_t width,
              size_t height,
              size_t dimension,
              const unsigned char* buffer,
              size_t sz_buffer) {
  if (!buffer) {
    throw std::runtime_error("received empty frame buffer");
  }
  if (dimension != 4) {
    throw std::runtime_error("PPM dump expects BGRA frames with dimension=4");
  }

  const size_t pixel_count = width * height;
  const size_t expected_size = pixel_count * dimension;
  if (sz_buffer < expected_size) {
    throw std::runtime_error("received frame buffer is smaller than expected");
  }

  std::vector<unsigned char> rgb(pixel_count * 3);
  for (size_t i = 0, j = 0; i < expected_size; i += 4, j += 3) {
    rgb[j + 0] = buffer[i + 2];
    rgb[j + 1] = buffer[i + 1];
    rgb[j + 2] = buffer[i + 0];
  }

  std::ofstream file(path.c_str(), std::ios::binary);
  if (!file.is_open()) {
    throw std::runtime_error("failed to open output file: " + path);
  }

  file << "P6\n" << width << " " << height << "\n255\n";
  file.write(reinterpret_cast<const char*>(rgb.data()),
             static_cast<std::streamsize>(rgb.size()));
  if (!file) {
    throw std::runtime_error("failed to write output file: " + path);
  }
}

class ReceiverHeadlessClient {
 public:
  explicit ReceiverHeadlessClient(const ReceiverOptions& options)
      : options_(options),
        session_(options.session, MakeFeatures(), MakeCallbacks()) {}

  bool Init() {
    if (options_.dump_every > 0) {
      EnsureDirectoryExists(options_.output_dir);
      rtc_logging::LogInfo(std::string("frame dump dir: ") + options_.output_dir);
    }
    return session_.Init();
  }

  int Run() {
    bool room_joined_seen = false;
    std::chrono::steady_clock::time_point room_joined_at{};

    while (!StopRequested()) {
      session_.Tick();

      const auto now = std::chrono::steady_clock::now();
      const bool joined = session_.IsRoomJoined();
      if (joined && !room_joined_seen) {
        room_joined_seen = true;
        room_joined_at = now;
        rtc_logging::LogInfo(std::string("opened room: ") + options_.session.room_id);
      } else if (!joined) {
        room_joined_seen = false;
      }

      if (options_.frame_limit > 0 &&
          received_video_frames_.load(std::memory_order_relaxed) >=
              static_cast<uint64_t>(options_.frame_limit)) {
        rtc_logging::LogInfo("receive frame limit reached");
        RequestStop();
        break;
      }

      if (room_joined_seen && options_.idle_timeout_sec > 0) {
        const int64_t last_frame_ns =
            last_video_frame_ns_.load(std::memory_order_relaxed);
        const auto reference_time =
            last_frame_ns == 0
                ? room_joined_at
                : std::chrono::steady_clock::time_point(
                      std::chrono::nanoseconds(last_frame_ns));
        if (now - reference_time >=
            std::chrono::seconds(options_.idle_timeout_sec)) {
          std::ostringstream oss;
          oss << "no remote video for " << options_.idle_timeout_sec
              << " seconds, exiting";
          rtc_logging::LogInfo(oss.str());
          RequestStop();
          break;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    session_.Shutdown();
    PrintSummary();
    return 0;
  }

 private:
  static RtcSession::Features MakeFeatures() {
    RtcSession::Features features;
    features.enable_data_channel = false;
    features.enable_external_video_source = false;
    features.room_action = RtcSession::RoomAction::Open;
    features.open_room_type = RtcRoomType::VideoBroadcasting;
    features.open_room_force = false;
    return features;
  }

  RtcSession::Callbacks MakeCallbacks() {
    RtcSession::Callbacks callbacks;
    callbacks.recv_message =
        [this](RtcSessionId remote_sessionid, RtcDataChannelLabel label,
               const char* msg, size_t msg_size) {
          OnRecvMessage(remote_sessionid, label, msg, msg_size);
        };
    callbacks.recv_audio_frame =
        [this](RtcSessionId remote_sessionid, RtcAudioSourceId sourceid,
               RtcMediaSourceType source_type, size_t bits_per_sample,
               size_t sample_rate, size_t number_of_channels,
               size_t number_of_frames, const void* audio_data,
               size_t sz_audio_data) {
          OnRecvAudioFrame(remote_sessionid, sourceid, source_type,
                           bits_per_sample, sample_rate, number_of_channels,
                           number_of_frames, audio_data, sz_audio_data);
        };
    callbacks.recv_frame =
        [this](RtcSessionId remote_sessionid, RtcVideoSourceId sourceid,
               RtcMediaSourceType source_type, size_t width, size_t height,
               size_t dimension, const unsigned char* buffer,
               size_t sz_buffer) {
          OnRecvFrame(remote_sessionid, sourceid, source_type, width, height,
                      dimension, buffer, sz_buffer);
        };
    return callbacks;
  }

  void OnRecvMessage(RtcSessionId,
                     RtcDataChannelLabel,
                     const char*,
                     size_t msg_size) {
    received_message_bytes_.fetch_add(msg_size, std::memory_order_relaxed);
  }

  void OnRecvAudioFrame(RtcSessionId,
                        RtcAudioSourceId,
                        RtcMediaSourceType,
                        size_t,
                        size_t,
                        size_t,
                        size_t,
                        const void*,
                        size_t sz_audio_data) {
    received_audio_bytes_.fetch_add(sz_audio_data, std::memory_order_relaxed);
  }

  void OnRecvFrame(RtcSessionId remote_sessionid,
                   RtcVideoSourceId sourceid,
                   RtcMediaSourceType,
                   size_t width,
                   size_t height,
                   size_t dimension,
                   const unsigned char* buffer,
                   size_t sz_buffer) {
    const uint64_t frame_index =
        received_video_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    received_video_bytes_.fetch_add(sz_buffer, std::memory_order_relaxed);
    last_video_frame_ns_.store(SteadyClockNowNs(), std::memory_order_relaxed);

    if ((frame_index % 120) == 1) {
      std::ostringstream oss;
      oss << "receiver frame #" << frame_index
          << " session=" << remote_sessionid
          << " source=" << (sourceid ? sourceid : "") << " bgra=" << width
          << "x" << height << " bytes=" << sz_buffer;
      rtc_logging::LogInfo(oss.str());
    }

    if (!ShouldDumpFrame(frame_index)) {
      return;
    }

    std::ostringstream filename;
    filename << "frame_" << std::setw(6) << std::setfill('0') << frame_index
             << "_session_" << remote_sessionid << "_source_"
             << SanitizeToken(sourceid) << "_" << width << "x" << height
             << ".ppm";
    const std::string path = JoinPath(options_.output_dir, filename.str());

    try {
      WritePpm(path, width, height, dimension, buffer, sz_buffer);
      const uint64_t dump_index =
          dumped_video_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
      std::ostringstream oss;
      oss << "dumped frame #" << frame_index << " to " << path
          << " (dump count=" << dump_index << ")";
      rtc_logging::LogInfo(oss.str());
    } catch (const std::exception& ex) {
      rtc_logging::LogError(std::string("failed to dump frame: ") + ex.what());
    }
  }

  bool ShouldDumpFrame(uint64_t frame_index) const {
    if (options_.dump_every <= 0) {
      return false;
    }
    if ((frame_index % static_cast<uint64_t>(options_.dump_every)) != 0) {
      return false;
    }
    if (options_.dump_max_frames <= 0) {
      return true;
    }
    return dumped_video_frames_.load(std::memory_order_relaxed) <
           static_cast<uint64_t>(options_.dump_max_frames);
  }

  void PrintSummary() const {
    std::ostringstream oss;
    oss << "summary: recv_video=" << received_video_frames_.load()
        << " recv_audio=" << session_.remote_audio_frames()
        << " recv_msg=" << session_.received_messages()
        << " dumped=" << dumped_video_frames_.load()
        << " video_bytes=" << received_video_bytes_.load()
        << " audio_bytes=" << received_audio_bytes_.load()
        << " msg_bytes=" << received_message_bytes_.load();
    rtc_logging::LogInfo(oss.str());
  }

  ReceiverOptions options_;
  RtcSession session_;
  std::atomic<uint64_t> received_video_frames_{0};
  std::atomic<uint64_t> dumped_video_frames_{0};
  std::atomic<uint64_t> received_video_bytes_{0};
  std::atomic<uint64_t> received_audio_bytes_{0};
  std::atomic<uint64_t> received_message_bytes_{0};
  std::atomic<int64_t> last_video_frame_ns_{0};
};

}  // namespace

int main(int argc, char** argv) {
  InstallSignalHandlers();

  try {
    const ReceiverOptions options = ParseReceiverArgs(argc, argv);

    ReceiverHeadlessClient client(options);
    if (!client.Init()) {
      return 1;
    }
    return client.Run();
  } catch (const std::exception& ex) {
    rtc_logging::LogError(std::string("rtc_receiver_headless failed: ") + ex.what());
    return 1;
  }
}
