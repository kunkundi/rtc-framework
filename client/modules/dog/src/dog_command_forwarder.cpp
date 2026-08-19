#include "rtc_dog/dog_command_forwarder.h"
#include "rtc_logging/rtc_logging.h"

#include <client_ws.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rtc_dog {
namespace {

constexpr auto kMinSendInterval = std::chrono::milliseconds(50);
constexpr size_t kMaxIncomingFrameSize = 64 * 1024;
constexpr const char* kWebSocketMagic =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr const char* kDogBehaviorResultTopic =
    "/agent_skill/do_dog_behavior/execute/result";

struct DogVelocity {
  float vx = 0.0f;
  float vy = 0.0f;
  float wz = 0.0f;
};

struct ParsedWsUrl {
  std::string host;
  std::string port;
  std::string host_header;
  std::string path = "/";
};

DogVelocity DriveCommandToVelocity(
    const vts_rtc::vehicle::DriveCommand& command,
    float max_forward_speed,
    float max_angular_speed) {
  DogVelocity velocity;
  if (command.brake > 0.0f) {
    return velocity;
  }

  float drive_sign = 0.0f;
  switch (command.drive_direction) {
    case vts_rtc::vehicle::DriveDirection::Forward:
      drive_sign = 1.0f;
      break;
    case vts_rtc::vehicle::DriveDirection::Reverse:
      drive_sign = -1.0f;
      break;
    case vts_rtc::vehicle::DriveDirection::Stop:
    default:
      break;
  }
  velocity.vx = drive_sign * command.throttle * max_forward_speed;

  switch (command.steering_direction) {
    case vts_rtc::vehicle::SteeringDirection::Left:
      velocity.wz = max_angular_speed;
      break;
    case vts_rtc::vehicle::SteeringDirection::Right:
      velocity.wz = -max_angular_speed;
      break;
    case vts_rtc::vehicle::SteeringDirection::Center:
    default:
      break;
  }
  return velocity;
}

std::string BuildRosbridgeVelocityMessage(float vx, float vy, float wz) {
  nlohmann::json message = {
      {"op", "publish"},
      {"topic", "/alphadog_node/set_velocity"},
      {"msg", {{"vx", vx}, {"vy", vy}, {"wz", wz}}},
  };
  return message.dump();
}

// 将站立/趴下编码为 actionlib goal，发布到
// /agent_skill/do_dog_behavior/execute/goal。goal_id 与 args 由调用方给定，
// args 为内嵌的行为 JSON 字符串。stamp 用当前时间、header.seq 递增、
// invoker 用 App-591，对齐板子上实测可用的键盘脚本。
std::string BuildRosbridgeDogBehaviorGoalMessage(const char* goal_id,
                                                 const char* args,
                                                 uint64_t header_seq,
                                                 int64_t stamp_secs) {
  nlohmann::json message = {
      {"op", "publish"},
      {"topic", "/agent_skill/do_dog_behavior/execute/goal"},
      {"msg",
       {{"header",
         {{"seq", header_seq},
          {"stamp", {{"secs", stamp_secs}, {"nsecs", 0}}},
          {"frame_id", ""}}},
        {"goal_id",
         {{"stamp", {{"secs", stamp_secs}, {"nsecs", 0}}},
          {"id", goal_id ? goal_id : ""}}},
        {"goal",
         {{"invoker", "App-591"},
          {"invoke_priority", 15},
          {"hold_time", 5.0},
          {"args", args ? args : ""}}}}},
  };
  return message.dump();
}

// 构造开机 goal，发布到 /alphadog_node/do_action/goal，action_id 固定为 0。
// 与板子上用 rostopic pub 实测成功的极简格式一致，msg 只含 goal.action_id。
// 狗的开机流程约 1.5 秒，远端 RTC 连接建立的耗时可覆盖该缓冲，因此连接
// rosbridge 成功后立即发一次，只要该帧写入 WebSocket 即可，不等回执。
std::string BuildRosbridgeBootUpGoalMessage() {
  nlohmann::json message = {
      {"op", "publish"},
      {"topic", "/alphadog_node/do_action/goal"},
      {"msg", {{"goal", {{"action_id", 0}}}}},
  };
  return message.dump();
}

std::string BuildRosbridgeBehaviorResultSubscriptionMessage() {
  nlohmann::json message = {
      {"op", "subscribe"},
      {"id", "rtc_dog_behavior_result"},
      {"topic", kDogBehaviorResultTopic},
  };
  return message.dump();
}

std::mt19937& RandomGenerator() {
  thread_local std::mt19937 generator(std::random_device{}());
  return generator;
}

uint8_t RandomByte() {
  std::uniform_int_distribution<unsigned int> distribution(0, 255);
  return static_cast<uint8_t>(distribution(RandomGenerator()));
}

// 当前 Unix 时间秒，用于 ROS stamp 与 goal_id 唯一化。
int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 生成 [minimum, maximum] 区间随机数，用于 goal_id 唯一化。
uint32_t RandomRange(uint32_t minimum, uint32_t maximum) {
  std::uniform_int_distribution<uint32_t> distribution(minimum, maximum);
  return distribution(RandomGenerator());
}

std::string BuildWsFrame(const std::string& payload, uint8_t opcode = 0x1) {
  std::string frame;
  frame.reserve(14 + payload.size());
  frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));

  const uint64_t length = static_cast<uint64_t>(payload.size());
  if (length <= 125) {
    frame.push_back(static_cast<char>(0x80 | length));
  } else if (length <= 65535) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((length >> 8) & 0xFF));
    frame.push_back(static_cast<char>(length & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((length >> shift) & 0xFF));
    }
  }

  uint8_t mask[4];
  for (size_t i = 0; i < 4; ++i) {
    mask[i] = RandomByte();
    frame.push_back(static_cast<char>(mask[i]));
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(static_cast<char>(
        static_cast<uint8_t>(payload[i]) ^ mask[i % 4]));
  }
  return frame;
}

std::string GenerateWebSocketKey() {
  std::string nonce(16, '\0');
  for (char& byte : nonce) {
    byte = static_cast<char>(RandomByte());
  }
  return SimpleWeb::Crypto::Base64::encode(nonce);
}

std::string Trim(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool HeaderContainsToken(const std::string& value, const char* expected) {
  std::istringstream tokens(value);
  std::string token;
  while (std::getline(tokens, token, ',')) {
    if (ToLower(Trim(token)) == expected) {
      return true;
    }
  }
  return false;
}

bool ParseWsUrl(const std::string& url,
                ParsedWsUrl* parsed,
                std::string* error_message) {
  if (!parsed || url.compare(0, 5, "ws://") != 0) {
    if (error_message) {
      *error_message = "rosbridge_url must start with ws://";
    }
    return false;
  }

  const std::string remainder = url.substr(5);
  const size_t path_pos = remainder.find('/');
  const std::string authority = remainder.substr(0, path_pos);
  if (authority.empty()) {
    if (error_message) {
      *error_message = "rosbridge_url must contain a host";
    }
    return false;
  }

  parsed->path = path_pos == std::string::npos
                     ? "/"
                     : remainder.substr(path_pos);
  parsed->host_header = authority;
  parsed->port = "80";

  if (authority.front() == '[') {
    const size_t close_bracket = authority.find(']');
    if (close_bracket == std::string::npos) {
      if (error_message) {
        *error_message = "rosbridge_url contains an invalid IPv6 host";
      }
      return false;
    }
    parsed->host = authority.substr(1, close_bracket - 1);
    if (close_bracket + 1 < authority.size()) {
      if (authority[close_bracket + 1] != ':' ||
          close_bracket + 2 >= authority.size()) {
        if (error_message) {
          *error_message = "rosbridge_url contains an invalid port";
        }
        return false;
      }
      parsed->port = authority.substr(close_bracket + 2);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      if (authority.find(':') != colon || colon == 0 ||
          colon + 1 >= authority.size()) {
        if (error_message) {
          *error_message = "IPv6 addresses in rosbridge_url must use brackets";
        }
        return false;
      }
      parsed->host = authority.substr(0, colon);
      parsed->port = authority.substr(colon + 1);
    } else {
      parsed->host = authority;
    }
  }

  try {
    const unsigned long port = std::stoul(parsed->port);
    if (port == 0 || port > 65535) {
      throw std::out_of_range("port");
    }
  } catch (const std::exception&) {
    if (error_message) {
      *error_message = "rosbridge_url contains an invalid port";
    }
    return false;
  }
  return true;
}

bool ValidateHandshakeResponse(const std::string& response,
                               const std::string& key,
                               std::string* error_message) {
  std::istringstream stream(response);
  std::string status_line;
  if (!std::getline(stream, status_line)) {
    if (error_message) {
      *error_message = "empty WebSocket handshake response";
    }
    return false;
  }

  std::istringstream status(Trim(status_line));
  std::string http_version;
  std::string status_code;
  status >> http_version >> status_code;
  if (http_version.compare(0, 5, "HTTP/") != 0 || status_code != "101") {
    if (error_message) {
      *error_message = "WebSocket handshake did not return HTTP 101";
    }
    return false;
  }

  std::map<std::string, std::string> headers;
  std::string line;
  while (std::getline(stream, line)) {
    line = Trim(line);
    if (line.empty()) {
      break;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string name = ToLower(Trim(line.substr(0, colon)));
    const std::string value = Trim(line.substr(colon + 1));
    auto it = headers.find(name);
    if (it == headers.end()) {
      headers.emplace(name, value);
    } else {
      it->second += "," + value;
    }
  }

  const auto upgrade = headers.find("upgrade");
  const auto connection = headers.find("connection");
  const auto accept = headers.find("sec-websocket-accept");
  const std::string expected_accept = SimpleWeb::Crypto::Base64::encode(
      SimpleWeb::Crypto::sha1(key + kWebSocketMagic));
  if (upgrade == headers.end() || ToLower(upgrade->second) != "websocket" ||
      connection == headers.end() ||
      !HeaderContainsToken(connection->second, "upgrade") ||
      accept == headers.end() || Trim(accept->second) != expected_accept) {
    if (error_message) {
      *error_message = "WebSocket handshake headers are invalid";
    }
    return false;
  }
  return true;
}

bool IsFinitePositive(float value) {
  return std::isfinite(value) != 0 && value > 0.0f;
}

}  // namespace

struct DogCommandForwarder::Impl {
  using Socket = SimpleWeb::asio::ip::tcp::socket;
  using ReadCallback = std::function<void(
      const SimpleWeb::error_code&, std::vector<uint8_t>)>;
  using DogActionCompletionCallback =
      rtc_vehicle::VehicleControlInterface::DogActionCompletionCallback;
  using VehicleCommandResult = rtc_vehicle::VehicleCommandResult;

  struct PendingBehaviorGoal {
    uint64_t request_id = 0;
    std::shared_ptr<SimpleWeb::asio::steady_timer> timer;
  };

  Config config;
  ParsedWsUrl endpoint;
  std::shared_ptr<SimpleWeb::asio::io_context> io_context;
  std::unique_ptr<SimpleWeb::asio::io_context::work> work_keepalive;
  std::unique_ptr<std::thread> io_thread;
  std::shared_ptr<Socket> socket;
  std::shared_ptr<SimpleWeb::asio::steady_timer> operation_timer;
  std::shared_ptr<SimpleWeb::asio::steady_timer> reconnect_timer;
  std::shared_ptr<SimpleWeb::asio::steady_timer> shutdown_timer;
  SimpleWeb::asio::streambuf incoming_buffer;

  std::deque<std::string> pending_writes;
  std::shared_ptr<std::string> active_write;
  std::string handshake_request;
  std::string handshake_key;

  std::atomic<bool> connected{false};
  std::atomic<bool> running{false};
  bool stopping = false;
  bool shutdown_finished = false;
  bool opened = false;
  uint64_t connection_generation = 0;

  std::mutex initial_mutex;
  std::condition_variable initial_cv;
  bool initial_finished = false;
  bool initial_connected = false;

  float last_vx = 0.0f;
  float last_vy = 0.0f;
  float last_wz = 0.0f;
  std::chrono::steady_clock::time_point last_sent_at{};

  // 行为指令 header.seq 的递增序号，对齐参考脚本的自增计数。
  uint64_t behavior_seq = 0;
  std::map<std::string, PendingBehaviorGoal> pending_behavior_goals;
  std::mutex completion_mutex;
  DogActionCompletionCallback dog_action_completion;

  explicit Impl(const Config& source_config)
      : config(source_config),
        incoming_buffer(kMaxIncomingFrameSize + 8192) {}

  bool Start(std::string* error_message) {
    if (running.load(std::memory_order_acquire)) {
      return connected.load(std::memory_order_acquire);
    }
    if (!IsFinitePositive(config.max_forward_speed) ||
        !IsFinitePositive(config.max_angular_speed)) {
      if (error_message) {
        *error_message = "dog speed limits must be finite and positive";
      }
      return false;
    }
    if (config.reconnect_interval_ms <= 0 || config.connect_timeout_ms <= 0 ||
        config.shutdown_timeout_ms <= 0 ||
        config.behavior_result_timeout_ms <= 0) {
      if (error_message) {
        *error_message = "dog network timeouts must be positive";
      }
      return false;
    }
    if (!ParseWsUrl(config.rosbridge_url, &endpoint, error_message)) {
      return false;
    }

    SimpleWeb::error_code address_error;
    SimpleWeb::asio::ip::make_address(endpoint.host, address_error);
    if (address_error) {
      if (error_message) {
        *error_message = "rosbridge_url must contain a numeric IP address";
      }
      return false;
    }

    io_context = std::make_shared<SimpleWeb::asio::io_context>();
    work_keepalive =
        std::make_unique<SimpleWeb::asio::io_context::work>(*io_context);
    initial_finished = false;
    initial_connected = false;
    stopping = false;
    shutdown_finished = false;
    running.store(true, std::memory_order_release);

    try {
      io_thread = std::make_unique<std::thread>([this]() {
        try {
          io_context->run();
        } catch (const std::exception& exception) {
          connected.store(false, std::memory_order_release);
          running.store(false, std::memory_order_release);
          rtc_logging::LogError(
              std::string("dog command forwarder: io exception: ") +
              exception.what());
          NotifyInitialConnection(false);
        }
      });
    } catch (const std::exception& exception) {
      running.store(false, std::memory_order_release);
      work_keepalive.reset();
      io_context.reset();
      if (error_message) {
        *error_message = std::string("failed to start dog io thread: ") +
                         exception.what();
      }
      return false;
    }

    io_context->post([this]() { DoConnect(); });
    const auto wait_time =
        std::chrono::milliseconds(config.connect_timeout_ms + 500);
    {
      std::unique_lock<std::mutex> lock(initial_mutex);
      initial_cv.wait_for(lock, wait_time,
                          [this]() { return initial_finished; });
      if (initial_finished && initial_connected) {
        if (error_message) {
          error_message->clear();
        }
        return true;
      }
    }

    if (error_message && error_message->empty()) {
      *error_message = "failed to connect to rosbridge";
    }
    Stop();
    return false;
  }

  void Stop() {
    const bool was_running =
        running.exchange(false, std::memory_order_acq_rel);
    if (was_running && io_context) {
      io_context->post([this]() { BeginShutdown(); });
    } else if (io_context) {
      work_keepalive.reset();
      io_context->stop();
    }
    if (io_thread && io_thread->joinable()) {
      io_thread->join();
    }
    io_thread.reset();
    CloseSocket();
    operation_timer.reset();
    reconnect_timer.reset();
    shutdown_timer.reset();
    work_keepalive.reset();
    io_context.reset();
    connected.store(false, std::memory_order_release);
  }

  void NotifyInitialConnection(bool success) {
    std::lock_guard<std::mutex> lock(initial_mutex);
    if (initial_finished) {
      return;
    }
    initial_finished = true;
    initial_connected = success;
    initial_cv.notify_all();
  }

  void DoConnect() {
    if (!running.load(std::memory_order_acquire) || stopping || !io_context) {
      return;
    }
    if (reconnect_timer) {
      SimpleWeb::error_code ignored;
      reconnect_timer->cancel(ignored);
      reconnect_timer.reset();
    }

    CloseSocket();
    incoming_buffer.consume(incoming_buffer.size());
    pending_writes.clear();
    active_write.reset();
    connected.store(false, std::memory_order_release);
    const uint64_t generation = ++connection_generation;

    SimpleWeb::error_code address_error;
    const auto address =
        SimpleWeb::asio::ip::make_address(endpoint.host, address_error);
    if (address_error) {
      HandleConnectionFailure(generation, "invalid rosbridge IP address");
      return;
    }
    const unsigned short port =
        static_cast<unsigned short>(std::stoul(endpoint.port));
    const SimpleWeb::asio::ip::tcp::endpoint remote_endpoint(address, port);
    socket = std::make_shared<Socket>(*io_context);
    const std::shared_ptr<Socket> current_socket = socket;

    ArmOperationTimeout(generation);
    current_socket->async_connect(
        remote_endpoint,
        [this, generation, current_socket](const SimpleWeb::error_code& error) {
          if (generation != connection_generation) {
            return;
          }
          if (error) {
            HandleConnectionFailure(
                generation, std::string("connect failed: ") + error.message());
            return;
          }

          SimpleWeb::error_code option_error;
          current_socket->set_option(
              SimpleWeb::asio::ip::tcp::no_delay(true), option_error);
          if (option_error) {
            rtc_logging::LogError(
                std::string("dog command forwarder: tcp_nodelay failed: ") +
                option_error.message());
          }
          StartHandshake(generation, current_socket);
        });
  }

  void StartHandshake(uint64_t generation,
                      const std::shared_ptr<Socket>& current_socket) {
    handshake_key = GenerateWebSocketKey();
    std::ostringstream request;
    request << "GET " << endpoint.path << " HTTP/1.1\r\n"
            << "Host: " << endpoint.host_header << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << handshake_key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";
    handshake_request = request.str();

    SimpleWeb::asio::async_write(
        *current_socket, SimpleWeb::asio::buffer(handshake_request),
        [this, generation, current_socket](const SimpleWeb::error_code& error,
                                           size_t) {
          if (generation != connection_generation) {
            return;
          }
          if (error) {
            HandleConnectionFailure(
                generation,
                std::string("handshake write failed: ") + error.message());
            return;
          }
          SimpleWeb::asio::async_read_until(
              *current_socket, incoming_buffer, "\r\n\r\n",
              [this, generation, current_socket](
                  const SimpleWeb::error_code& read_error,
                  size_t header_size) {
                if (generation != connection_generation) {
                  return;
                }
                if (read_error) {
                  HandleConnectionFailure(
                      generation,
                      std::string("handshake read failed: ") +
                          read_error.message());
                  return;
                }

                const auto begin = SimpleWeb::asio::buffers_begin(
                    incoming_buffer.data());
                const std::string response(begin, begin + header_size);
                incoming_buffer.consume(header_size);
                std::string validation_error;
                if (!ValidateHandshakeResponse(response, handshake_key,
                                               &validation_error)) {
                  HandleConnectionFailure(generation, validation_error);
                  return;
                }

                SendConnectionStop(generation, current_socket);
              });
        });
  }

  void SendConnectionStop(uint64_t generation,
                          const std::shared_ptr<Socket>& current_socket) {
    // 每次建立连接都先写入零速度，避免沿用断线前的运动状态。
    const std::shared_ptr<std::string> stop_frame =
        std::make_shared<std::string>(BuildWsFrame(
            BuildRosbridgeVelocityMessage(0.0f, 0.0f, 0.0f)));
    SimpleWeb::asio::async_write(
        *current_socket, SimpleWeb::asio::buffer(*stop_frame),
        [this, generation, current_socket, stop_frame](
            const SimpleWeb::error_code& error, size_t) {
          if (generation != connection_generation) {
            return;
          }
          if (error) {
            HandleConnectionFailure(
                generation,
                std::string("connection stop write failed: ") +
                    error.message());
            return;
          }

          CancelOperationTimeout();
          connected.store(true, std::memory_order_release);
          rtc_logging::LogInfo(
              "dog command forwarder: connected to rosbridge at " +
              endpoint.host_header + endpoint.path);
          SendBehaviorResultSubscription();
          SendBootUpGoal();
          NotifyInitialConnection(true);
          BeginReadFrame(generation);
        });
  }

  // 连接成功后发一次开机 goal，触发狗的 boot up 流程。开机约 1.5 秒，
  // 远端 RTC 连接建立的耗时可覆盖该缓冲；此处只需保证该帧入队发出，
  // 不等回执、不阻塞后续指令。
  void SendBootUpGoal() {
    const std::string frame =
        BuildWsFrame(BuildRosbridgeBootUpGoalMessage());
    // 不冲掉刚发的零速度帧，走队尾顺序发送。
    QueueFrameOnIo(frame, false, false, false);
    rtc_logging::LogInfo(
        "dog command forwarder: queued boot up goal");
  }

  void SendBehaviorResultSubscription() {
    QueueFrameOnIo(
        BuildWsFrame(BuildRosbridgeBehaviorResultSubscriptionMessage()),
        false, false, false);
  }

  void ArmOperationTimeout(uint64_t generation) {
    if (!operation_timer) {
      operation_timer =
          std::make_shared<SimpleWeb::asio::steady_timer>(*io_context);
    }
    operation_timer->expires_after(
        std::chrono::milliseconds(config.connect_timeout_ms));
    operation_timer->async_wait(
        [this, generation](const SimpleWeb::error_code& error) {
          if (!error && generation == connection_generation) {
            HandleConnectionFailure(generation,
                                    "connect or handshake timed out");
          }
        });
  }

  void CancelOperationTimeout() {
    if (operation_timer) {
      SimpleWeb::error_code ignored;
      operation_timer->cancel(ignored);
    }
  }

  void HandleConnectionFailure(uint64_t generation,
                               const std::string& reason) {
    if (generation != connection_generation) {
      return;
    }
    ++connection_generation;
    CancelOperationTimeout();
    connected.store(false, std::memory_order_release);
    FailPendingBehaviorGoals(
        vts_rtc::vehicle::VehicleErrorCode::InvalidState,
        "rosbridge connection lost before dog action completed");
    CloseSocket();
    pending_writes.clear();
    active_write.reset();
    NotifyInitialConnection(false);

    rtc_logging::LogError(
        std::string("dog command forwarder: ") + reason);
    if (!running.load(std::memory_order_acquire) || stopping) {
      FinishShutdown();
      return;
    }
    ScheduleReconnect();
  }

  void ScheduleReconnect() {
    if (!running.load(std::memory_order_acquire) || stopping || !io_context) {
      return;
    }
    if (!reconnect_timer) {
      reconnect_timer =
          std::make_shared<SimpleWeb::asio::steady_timer>(*io_context);
    }
    reconnect_timer->expires_after(
        std::chrono::milliseconds(config.reconnect_interval_ms));
    reconnect_timer->async_wait(
        [this](const SimpleWeb::error_code& error) {
          if (!error && running.load(std::memory_order_acquire) && !stopping) {
            DoConnect();
          }
        });
  }

  void AsyncReadBytes(uint64_t generation,
                      size_t byte_count,
                      const ReadCallback& callback) {
    if (generation != connection_generation || !socket) {
      callback(SimpleWeb::asio::error::operation_aborted, {});
      return;
    }

    const auto consume = [this, byte_count, callback]() {
      std::vector<uint8_t> bytes(byte_count);
      const auto begin =
          SimpleWeb::asio::buffers_begin(incoming_buffer.data());
      for (size_t i = 0; i < byte_count; ++i) {
        bytes[i] = static_cast<uint8_t>(*(begin + i));
      }
      incoming_buffer.consume(byte_count);
      callback(SimpleWeb::error_code(), std::move(bytes));
    };

    if (incoming_buffer.size() >= byte_count) {
      consume();
      return;
    }

    const size_t missing = byte_count - incoming_buffer.size();
    const std::shared_ptr<Socket> current_socket = socket;
    SimpleWeb::asio::async_read(
        *current_socket, incoming_buffer,
        SimpleWeb::asio::transfer_exactly(missing),
        [this, generation, consume, callback](
            const SimpleWeb::error_code& error, size_t) {
          if (generation != connection_generation) {
            return;
          }
          if (error) {
            callback(error, {});
            return;
          }
          consume();
        });
  }

  void BeginReadFrame(uint64_t generation) {
    if (generation != connection_generation ||
        !connected.load(std::memory_order_acquire) || stopping) {
      return;
    }
    AsyncReadBytes(
        generation, 2,
        [this, generation](const SimpleWeb::error_code& error,
                           std::vector<uint8_t> header) {
          if (error) {
            HandleConnectionFailure(
                generation,
                std::string("WebSocket read failed: ") + error.message());
            return;
          }
          const uint8_t opcode = header[0] & 0x0F;
          const bool masked = (header[1] & 0x80) != 0;
          const uint8_t length_code = header[1] & 0x7F;
          if (masked) {
            HandleConnectionFailure(generation,
                                    "server sent a masked WebSocket frame");
            return;
          }
          if (length_code <= 125) {
            ReadFramePayload(generation, opcode, length_code);
          } else {
            const size_t extended_size = length_code == 126 ? 2 : 8;
            AsyncReadBytes(
                generation, extended_size,
                [this, generation, opcode](
                    const SimpleWeb::error_code& length_error,
                    std::vector<uint8_t> encoded_length) {
                  if (length_error) {
                    HandleConnectionFailure(
                        generation,
                        std::string("WebSocket length read failed: ") +
                            length_error.message());
                    return;
                  }
                  uint64_t payload_size = 0;
                  for (uint8_t byte : encoded_length) {
                    payload_size = (payload_size << 8) | byte;
                  }
                  if (payload_size > kMaxIncomingFrameSize) {
                    HandleConnectionFailure(
                        generation, "WebSocket frame exceeds 64 KiB");
                    return;
                  }
                  ReadFramePayload(generation, opcode,
                                   static_cast<size_t>(payload_size));
                });
          }
        });
  }

  void ReadFramePayload(uint64_t generation,
                        uint8_t opcode,
                        size_t payload_size) {
    if (payload_size > kMaxIncomingFrameSize) {
      HandleConnectionFailure(generation, "WebSocket frame exceeds 64 KiB");
      return;
    }
    AsyncReadBytes(
        generation, payload_size,
        [this, generation, opcode](const SimpleWeb::error_code& error,
                                   std::vector<uint8_t> payload) {
          if (error) {
            HandleConnectionFailure(
                generation,
                std::string("WebSocket payload read failed: ") +
                    error.message());
            return;
          }
          if (opcode == 0x8) {
            HandleConnectionFailure(generation,
                                    "rosbridge closed the WebSocket");
            return;
          }
          if (opcode == 0x9) {
            const std::string ping_payload(payload.begin(), payload.end());
            QueueFrameOnIo(BuildWsFrame(ping_payload, 0xA), false, false,
                           true);
          } else if (opcode == 0x1) {
            HandleIncomingText(
                std::string(payload.begin(), payload.end()));
          }
          BeginReadFrame(generation);
        });
  }

  bool EnqueueVelocityFrame(std::string frame, bool is_stop) {
    if (!running.load(std::memory_order_acquire) ||
        !connected.load(std::memory_order_acquire) || !io_context) {
      return false;
    }
    io_context->post(
        [this, frame = std::move(frame), is_stop]() mutable {
          if (!running.load(std::memory_order_acquire) ||
              !connected.load(std::memory_order_acquire) || stopping) {
            return;
          }
          QueueFrameOnIo(std::move(frame), !is_stop, is_stop, false);
        });
    return true;
  }

  bool EnqueueBehaviorFrame(std::string frame,
                            std::string goal_id,
                            uint64_t request_id) {
    if (!running.load(std::memory_order_acquire) ||
        !connected.load(std::memory_order_acquire) || !io_context) {
      return false;
    }
    io_context->post([this, frame = std::move(frame),
                      goal_id = std::move(goal_id), request_id]() mutable {
      if (!running.load(std::memory_order_acquire) ||
          !connected.load(std::memory_order_acquire) || stopping || !socket ||
          !socket->is_open()) {
        VehicleCommandResult failed;
        failed.accepted = false;
        failed.error_code =
            vts_rtc::vehicle::VehicleErrorCode::InvalidState;
        failed.detail = "rosbridge WebSocket is not connected";
        NotifyDogActionCompletion(request_id, failed);
        return;
      }
      PendingBehaviorGoal pending;
      pending.request_id = request_id;
      pending.timer =
          std::make_shared<SimpleWeb::asio::steady_timer>(*io_context);
      pending.timer->expires_after(
          std::chrono::milliseconds(config.behavior_result_timeout_ms));
      pending.timer->async_wait(
          [this, goal_id](const SimpleWeb::error_code& error) {
            if (error) {
              return;
            }
            VehicleCommandResult timed_out;
            timed_out.accepted = false;
            timed_out.error_code =
                vts_rtc::vehicle::VehicleErrorCode::Internal;
            timed_out.detail = "dog behavior result timed out";
            CompleteBehaviorGoal(goal_id, timed_out);
          });
      pending_behavior_goals[goal_id] = std::move(pending);
      // 行为 goal 是可靠事务，必须按发送顺序完整保留，禁止与速度帧合并。
      QueueFrameOnIo(std::move(frame), false, false, false);
    });
    return true;
  }

  void QueueFrameOnIo(std::string frame,
                      bool replace_latest,
                      bool discard_pending,
                      bool high_priority) {
    if (!socket || !socket->is_open()) {
      return;
    }
    if (discard_pending) {
      pending_writes.clear();
      pending_writes.push_back(std::move(frame));
    } else if (high_priority) {
      pending_writes.push_front(std::move(frame));
    } else if (replace_latest && !pending_writes.empty()) {
      pending_writes.back() = std::move(frame);
    } else {
      pending_writes.push_back(std::move(frame));
    }
    StartWrite();
  }

  void StartWrite() {
    if (active_write || pending_writes.empty() || !socket ||
        !socket->is_open()) {
      return;
    }
    const uint64_t generation = connection_generation;
    const std::shared_ptr<Socket> current_socket = socket;
    active_write =
        std::make_shared<std::string>(std::move(pending_writes.front()));
    pending_writes.pop_front();
    const std::shared_ptr<std::string> current_frame = active_write;
    SimpleWeb::asio::async_write(
        *current_socket, SimpleWeb::asio::buffer(*current_frame),
        [this, generation, current_socket, current_frame](
            const SimpleWeb::error_code& error, size_t) {
          if (generation != connection_generation) {
            return;
          }
          if (error) {
            HandleConnectionFailure(
                generation,
                std::string("WebSocket write failed: ") + error.message());
            return;
          }
          active_write.reset();
          if (!pending_writes.empty()) {
            StartWrite();
          } else if (stopping) {
            FinishShutdown();
          }
        });
  }

  void BeginShutdown() {
    if (shutdown_finished) {
      return;
    }
    stopping = true;
    CancelOperationTimeout();
    if (reconnect_timer) {
      SimpleWeb::error_code ignored;
      reconnect_timer->cancel(ignored);
    }
    if (!connected.load(std::memory_order_acquire) || !socket ||
        !socket->is_open()) {
      FinishShutdown();
      return;
    }

    if (!shutdown_timer) {
      shutdown_timer =
          std::make_shared<SimpleWeb::asio::steady_timer>(*io_context);
    }
    shutdown_timer->expires_after(
        std::chrono::milliseconds(config.shutdown_timeout_ms));
    shutdown_timer->async_wait(
        [this](const SimpleWeb::error_code& error) {
          if (!error) {
            FinishShutdown();
          }
        });

    pending_writes.clear();
    pending_writes.push_back(BuildWsFrame(
        BuildRosbridgeVelocityMessage(0.0f, 0.0f, 0.0f)));
    StartWrite();
  }

  void FinishShutdown() {
    if (shutdown_finished) {
      return;
    }
    shutdown_finished = true;
    ++connection_generation;
    if (shutdown_timer) {
      SimpleWeb::error_code ignored;
      shutdown_timer->cancel(ignored);
    }
    connected.store(false, std::memory_order_release);
    FailPendingBehaviorGoals(
        vts_rtc::vehicle::VehicleErrorCode::InvalidState,
        "dog command forwarder stopped before action completed");
    CloseSocket();
    pending_writes.clear();
    active_write.reset();
    work_keepalive.reset();
    if (io_context) {
      io_context->stop();
    }
  }

  void CloseSocket() {
    if (!socket) {
      return;
    }
    SimpleWeb::error_code ignored;
    socket->cancel(ignored);
    socket->shutdown(Socket::shutdown_both, ignored);
    socket->close(ignored);
    socket.reset();
  }

  void SetDogActionCompletionCallback(
      const DogActionCompletionCallback& callback) {
    std::lock_guard<std::mutex> lock(completion_mutex);
    dog_action_completion = callback;
  }

  void NotifyDogActionCompletion(uint64_t request_id,
                                 const VehicleCommandResult& result) {
    DogActionCompletionCallback callback;
    {
      std::lock_guard<std::mutex> lock(completion_mutex);
      callback = dog_action_completion;
    }
    if (callback) {
      callback(request_id, result);
    }
  }

  void CompleteBehaviorGoal(const std::string& goal_id,
                            const VehicleCommandResult& result) {
    const auto pending = pending_behavior_goals.find(goal_id);
    if (pending == pending_behavior_goals.end()) {
      return;
    }
    const uint64_t request_id = pending->second.request_id;
    if (pending->second.timer) {
      SimpleWeb::error_code ignored;
      pending->second.timer->cancel(ignored);
    }
    pending_behavior_goals.erase(pending);
    NotifyDogActionCompletion(request_id, result);
  }

  void FailPendingBehaviorGoals(
      vts_rtc::vehicle::VehicleErrorCode error_code,
      const std::string& detail) {
    std::vector<uint64_t> request_ids;
    request_ids.reserve(pending_behavior_goals.size());
    for (const auto& entry : pending_behavior_goals) {
      request_ids.push_back(entry.second.request_id);
      if (entry.second.timer) {
        SimpleWeb::error_code ignored;
        entry.second.timer->cancel(ignored);
      }
    }
    pending_behavior_goals.clear();
    for (uint64_t request_id : request_ids) {
      VehicleCommandResult failed;
      failed.accepted = false;
      failed.error_code = error_code;
      failed.detail = detail;
      NotifyDogActionCompletion(request_id, failed);
    }
  }

  void HandleIncomingText(const std::string& payload) {
    try {
      const nlohmann::json message = nlohmann::json::parse(payload);
      if (message.value("op", "") != "publish" ||
          message.value("topic", "") != kDogBehaviorResultTopic ||
          !message.contains("msg")) {
        return;
      }
      const nlohmann::json& status = message.at("msg").at("status");
      const std::string goal_id =
          status.at("goal_id").at("id").get<std::string>();
      const int status_code = status.at("status").get<int>();
      VehicleCommandResult result;
      result.accepted = status_code == 3;
      result.error_code =
          result.accepted
              ? vts_rtc::vehicle::VehicleErrorCode::None
              : vts_rtc::vehicle::VehicleErrorCode::Internal;
      if (!result.accepted) {
        result.detail =
            "dog behavior action failed with status " +
            std::to_string(status_code);
      }
      CompleteBehaviorGoal(goal_id, result);
    } catch (const std::exception& exception) {
      rtc_logging::LogError(
          std::string("dog command forwarder: invalid behavior result: ") +
          exception.what());
    }
  }
};

DogCommandForwarder::DogCommandForwarder(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

DogCommandForwarder::~DogCommandForwarder() {
  Close();
}

bool DogCommandForwarder::Open(std::string* error_message) {
  if (impl_->opened) {
    return true;
  }
  if (!Start(error_message)) {
    return false;
  }
  impl_->opened = true;
  rtc_logging::LogInfo("dog command forwarder: opened");
  return true;
}

void DogCommandForwarder::Close() {
  if (!impl_->opened &&
      !impl_->running.load(std::memory_order_acquire)) {
    return;
  }
  impl_->opened = false;
  StopImpl();
  rtc_logging::LogInfo("dog command forwarder: closed");
}

rtc_vehicle::VehicleCommandResult
DogCommandForwarder::SendDriveCommand(
    const vts_rtc::vehicle::DriveCommand& command) {
  rtc_vehicle::VehicleCommandResult result;
  DogVelocity velocity;
  if (command.brake <= 0.0f) {
    velocity = DriveCommandToVelocity(
        command, impl_->config.max_forward_speed,
        impl_->config.max_angular_speed);
  }

  if (!std::isfinite(velocity.vx) || !std::isfinite(velocity.vy) ||
      !std::isfinite(velocity.wz)) {
    result.accepted = false;
    result.error_code =
        vts_rtc::vehicle::VehicleErrorCode::InvalidArgument;
    result.detail = "velocity contains NaN or infinity";
    return result;
  }
  if (!ForwardVelocity(velocity.vx, velocity.vy, velocity.wz)) {
    result.accepted = false;
    result.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidState;
    result.detail = "rosbridge WebSocket is not connected";
    return result;
  }

  result.accepted = true;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::None;
  return result;
}

rtc_vehicle::VehicleCommandResult
DogCommandForwarder::SendGearCommand(
    vts_rtc::vehicle::VehicleGear gear) {
  rtc_vehicle::VehicleCommandResult result;
  result.accepted = true;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::None;
  rtc_logging::LogInfo(
      std::string("dog command forwarder: gear set to ") +
      (gear == vts_rtc::vehicle::VehicleGear::Forward    ? "Forward"
       : gear == vts_rtc::vehicle::VehicleGear::Reverse ? "Reverse"
                                                         : "Neutral"));
  return result;
}

rtc_vehicle::VehicleCommandResult
DogCommandForwarder::SendDogAction(
    const vts_rtc::vehicle::DogAction& action) {
  rtc_vehicle::VehicleCommandResult result;
  result.accepted = false;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidArgument;

  const float lateral_speed = std::max(0.0f, action.speed) *
                              impl_->config.max_lateral_speed;
  bool forwarded = false;
  switch (action.action) {
    case vts_rtc::vehicle::DogActionType::LateralLeft:
      forwarded = ForwardVelocity(0.0f, lateral_speed, 0.0f);
      break;
    case vts_rtc::vehicle::DogActionType::LateralRight:
      forwarded = ForwardVelocity(0.0f, -lateral_speed, 0.0f);
      break;
    case vts_rtc::vehicle::DogActionType::LateralStop:
      forwarded = ForwardVelocity(0.0f, 0.0f, 0.0f);
      break;
    case vts_rtc::vehicle::DogActionType::Stand:
      forwarded = ForwardDogBehaviorGoal(
          "cli_stand", "[{\"behavior\":\"force_recovery_balance_stand\"}]",
          action.request_id);
      break;
    case vts_rtc::vehicle::DogActionType::LieDown:
      // 趴下是动作链：先强制平衡站立，再执行休息。
      forwarded = ForwardDogBehaviorGoal(
          "cli_rest",
          "[{\"behavior\":\"force_recovery_balance_stand\"},{\"behavior\":\"rest\"}]",
          action.request_id);
      break;
    case vts_rtc::vehicle::DogActionType::Unknown:
    default:
      result.detail = "unknown dog action";
      return result;
  }

  if (!forwarded) {
    result.error_code = vts_rtc::vehicle::VehicleErrorCode::InvalidState;
    result.detail = "rosbridge WebSocket is not connected";
    return result;
  }

  result.accepted = true;
  result.completion_pending =
      action.action == vts_rtc::vehicle::DogActionType::Stand ||
      action.action == vts_rtc::vehicle::DogActionType::LieDown;
  result.error_code = vts_rtc::vehicle::VehicleErrorCode::None;
  return result;
}

void DogCommandForwarder::SendStop() {
  ForwardVelocity(0.0f, 0.0f, 0.0f);
}

bool DogCommandForwarder::ForwardVelocity(float vx, float vy, float wz) {
  if (!impl_->running.load(std::memory_order_acquire) ||
      !impl_->connected.load(std::memory_order_acquire)) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool changed =
      vx != impl_->last_vx || vy != impl_->last_vy || wz != impl_->last_wz;
  const bool is_stop = vx == 0.0f && vy == 0.0f && wz == 0.0f;
  if (!is_stop && !changed &&
      impl_->last_sent_at.time_since_epoch().count() != 0 &&
      now - impl_->last_sent_at < kMinSendInterval) {
    return true;
  }

  const std::string frame =
      BuildWsFrame(BuildRosbridgeVelocityMessage(vx, vy, wz));
  if (!impl_->EnqueueVelocityFrame(frame, is_stop)) {
    return false;
  }
  impl_->last_vx = vx;
  impl_->last_vy = vy;
  impl_->last_wz = wz;
  impl_->last_sent_at = now;
  if (changed && !is_stop) {
    rtc_logging::LogInfo(
        "dog command forwarder: queued velocity command");
  }
  return true;
}

bool DogCommandForwarder::ForwardDogBehaviorGoal(const char* goal_id,
                                                 const char* args,
                                                 uint64_t request_id) {
  if (!impl_->running.load(std::memory_order_acquire) ||
      !impl_->connected.load(std::memory_order_acquire)) {
    return false;
  }

  // 狗的动作服务器按 goal_id.id 去重，相邻同 id 的任务会被忽略，
  // 因此在基名后追加时间戳+随机数，保证每次发送的 id 都不同。
  const int64_t now_secs = NowUnixSeconds();
  const uint64_t header_seq = ++impl_->behavior_seq;
  const std::string unique_goal_id =
      std::string(goal_id) + "_" + std::to_string(now_secs) + "_" +
      std::to_string(RandomRange(1000, 9999));

  const std::string frame = BuildWsFrame(
      BuildRosbridgeDogBehaviorGoalMessage(unique_goal_id.c_str(), args,
                                           header_seq, now_secs));
  if (!impl_->EnqueueBehaviorFrame(frame, unique_goal_id, request_id)) {
    return false;
  }
  rtc_logging::LogInfo(
      std::string("dog command forwarder: queued behavior goal ") +
      unique_goal_id);
  return true;
}

void DogCommandForwarder::SetDogActionCompletionCallback(
    const DogActionCompletionCallback& callback) {
  impl_->SetDogActionCompletionCallback(callback);
}

bool DogCommandForwarder::IsConnected() const {
  return impl_->connected.load(std::memory_order_acquire);
}

bool DogCommandForwarder::Start(std::string* error_message) {
  return impl_->Start(error_message);
}

void DogCommandForwarder::StopImpl() {
  impl_->Stop();
}

}  // namespace rtc_dog
