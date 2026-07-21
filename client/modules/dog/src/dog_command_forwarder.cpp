// 狗指令转发器实现。
//
// 实现 VehicleControlInterface，将车辆控制指令（DriveCommand）映射为狗速度指令，
// 通过 WebSocket / rosbridge 转发到机器狗。
//
// 架构概览：
//   VehicleControlModule::Tick 线程     io_context 线程
//   ──────────────────────────────     ─────────────────
//   SendDriveCommand(drive_cmd)
//     → DriveCommandToVelocity(cmd)    （油门/方向 → vx/wz）
//     → BuildRosbridgeMessage(vx,vy,wz)（生成 rosbridge JSON）
//     → ForwardVelocity(vx,vy,wz)
//       → BuildWsFrame(json)          （生成 WebSocket 帧）
//       → io_context->post(lambda)    （跨线程派发）
//                                        lambda 执行:
//                                          asio::write(socket, frame)
//                                          → TCP → 10.10.10.10:9090
//                                            → rosbridge → ROS 话题
//
// 为什么不用 SimpleWeb::SocketClient？
//   SimpleWeb 内置的 DNS resolver 在 Orin NX 上解析 IP 地址 "10.10.10.10"
//   时会返回 EAI_NONAME（DNS 配置问题）。因此改用原生 asio::ip::tcp::socket
//   + SimpleWeb::asio::ip::make_address() 直连，完全绕过 DNS。
//
// WebSocket 客户端帧格式 (RFC 6455 §5.6):
//   [FIN+opcode] [MASK+len] [mask(4 bytes)] [masked payload]
//   客户端→服务端必须带 mask。

#include "rtc_dog/dog_command_forwarder.h"
#include "rtc_logging/rtc_logging.h"

#include <client_ws.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <sstream>
#include <utility>

namespace rtc_dog {

namespace {

constexpr auto kMinSendInterval = std::chrono::milliseconds(50);

// ── DriveCommand → 狗速度映射 ──────────────────────────────────────────────

struct DogVelocity {
  float vx = 0.0f;
  float vy = 0.0f;
  float wz = 0.0f;
};

DogVelocity DriveCommandToVelocity(
    const vts_rtc::vehicle::DriveCommand& cmd,
    float max_forward_speed,
    float max_angular_speed) {
  DogVelocity vel;

  // 刹车优先。
  if (cmd.brake > 0.0f) {
    return vel;
  }

  // 油门 → vx。
  float sign = 0.0f;
  switch (cmd.drive_direction) {
    case vts_rtc::vehicle::DriveDirection::Forward:
      sign = 1.0f;
      break;
    case vts_rtc::vehicle::DriveDirection::Reverse:
      sign = -1.0f;
      break;
    case vts_rtc::vehicle::DriveDirection::Stop:
    default:
      sign = 0.0f;
      break;
  }
  vel.vx = sign * cmd.throttle * max_forward_speed;

  // 转向 → wz。
  switch (cmd.steering_direction) {
    case vts_rtc::vehicle::SteeringDirection::Left:
      vel.wz = max_angular_speed;
      break;
    case vts_rtc::vehicle::SteeringDirection::Right:
      vel.wz = -max_angular_speed;
      break;
    case vts_rtc::vehicle::SteeringDirection::Center:
    default:
      vel.wz = 0.0f;
      break;
  }

  return vel;
}

// ── Rosbridge JSON ─────────────────────────────────────────────────────────

std::string BuildRosbridgeVelocityMessage(float vx, float vy, float wz) {
  nlohmann::json msg = {
      {"op", "publish"},
      {"topic", "/alphadog_node/set_velocity"},
      {"msg",
       {
           {"vx", vx},
           {"vy", vy},
           {"wz", wz},
       }},
  };
  return msg.dump();
}

// ── WebSocket 帧构建 ───────────────────────────────────────────────────────

std::string BuildWsFrame(const std::string& payload) {
  std::string frame;
  frame.reserve(2 + 4 + payload.size());

  frame.push_back(static_cast<char>(0x81));

  size_t len = payload.size();
  if (len <= 125) {
    frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(len)));
  } else if (len <= 65535) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }

  uint8_t mask[4];
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<unsigned int> dist(0, 255);
  for (int i = 0; i < 4; ++i) {
    mask[i] = static_cast<uint8_t>(dist(gen));
    frame.push_back(static_cast<char>(mask[i]));
  }

  for (size_t i = 0; i < len; ++i) {
    frame.push_back(payload[i] ^ mask[i % 4]);
  }

  return frame;
}

bool IsFinite(float value) {
  return std::isfinite(value) != 0;
}

const char* ValidateVelocity(float vx, float vy, float wz) {
  // 仅检查非数值（NaN/Inf），不限制幅值范围——幅值由 DriveCommand 协议层
  // 的 throttle ∈ [0,1] 和用户配置的 max_forward_speed / max_angular_speed
  // 共同决定，这些已在 SendDriveCommand 中映射后得到保证。
  if (!IsFinite(vx) || !IsFinite(vy) || !IsFinite(wz)) {
    return "velocity contains NaN or infinity";
  }
  return nullptr;
}

}  // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct DogCommandForwarder::Impl {
  DogCommandForwarder::Config config;

  std::shared_ptr<SimpleWeb::asio::io_context> io_context;
  std::unique_ptr<std::thread> io_thread;
  std::unique_ptr<SimpleWeb::asio::io_context::work> work_keepalive;

  std::shared_ptr<SimpleWeb::asio::ip::tcp::socket> socket;
  std::shared_ptr<SimpleWeb::asio::steady_timer> reconnect_timer;

  std::mutex socket_mutex;
  std::atomic<bool> connected{false};
  std::atomic<bool> running{false};

  // 节流状态。
  float last_vx = 0.0f;
  float last_vy = 0.0f;
  float last_wz = 0.0f;
  std::chrono::steady_clock::time_point last_sent_at{};

  bool opened = false;

  explicit Impl(const DogCommandForwarder::Config& c) : config(c) {}
};

// ─── DogCommandForwarder ─────────────────────────────────────────────────────

DogCommandForwarder::DogCommandForwarder(const Config& config)
    : impl_(std::make_unique<Impl>(config)) {}

DogCommandForwarder::~DogCommandForwarder() {
  Close();
}

// ── VehicleControlInterface 实现 ────────────────────────────────────────────

bool DogCommandForwarder::Open(std::string* error_message) {
  if (impl_->opened) {
    return true;
  }
  if (!Start()) {
    if (error_message) {
      *error_message = "failed to start dog command forwarder";
    }
    return false;
  }
  impl_->opened = true;
  rtc_logging::LogInfo("dog command forwarder: opened");
  return true;
}

void DogCommandForwarder::Close() {
  impl_->opened = false;

  // 在停止 io_context 之前，通过 socket 同步发送零速度帧，
  // 确保狗在关闭过程中收到停车指令，避免 io_context::stop()
  // 丢弃已入队的异步回调。
  {
    std::lock_guard<std::mutex> lock(impl_->socket_mutex);
    if (impl_->socket && impl_->socket->is_open()) {
      const std::string json =
          BuildRosbridgeVelocityMessage(0.0f, 0.0f, 0.0f);
      const std::string frame = BuildWsFrame(json);
      SimpleWeb::error_code ec;
      SimpleWeb::asio::write(*impl_->socket,
                             SimpleWeb::asio::buffer(frame), ec);
      if (ec) {
        rtc_logging::LogError(
            std::string("dog command forwarder: shutdown stop error ") +
            std::to_string(ec.value()) + ": " + ec.message());
      } else {
        rtc_logging::LogInfo(
            "dog command forwarder: shutdown stop sent");
      }
    }
  }

  StopImpl();
}

rtc_vehicle::VehicleCommandResult
DogCommandForwarder::SendDriveCommand(
    const vts_rtc::vehicle::DriveCommand& command) {
  rtc_vehicle::VehicleCommandResult result;

  if (command.brake > 0.0f) {
    ForwardVelocity(0.0f, 0.0f, 0.0f);
    result.accepted = true;
    result.error_code = vts_rtc::vehicle::VehicleErrorCode::None;
    return result;
  }

  const DogVelocity vel = DriveCommandToVelocity(
      command, impl_->config.max_forward_speed,
      impl_->config.max_angular_speed);

  const char* validation_error =
      ValidateVelocity(vel.vx, vel.vy, vel.wz);
  if (validation_error) {
    result.accepted = false;
    result.error_code =
        vts_rtc::vehicle::VehicleErrorCode::InvalidArgument;
    result.detail = validation_error;
    return result;
  }

  ForwardVelocity(vel.vx, vel.vy, vel.wz);
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
       : gear == vts_rtc::vehicle::VehicleGear::Reverse   ? "Reverse"
                                                          : "Neutral"));
  return result;
}

void DogCommandForwarder::SendStop() {
  ForwardVelocity(0.0f, 0.0f, 0.0f);
}

// ── 内部转发 ────────────────────────────────────────────────────────────────

void DogCommandForwarder::ForwardVelocity(float vx, float vy, float wz) {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  const bool changed =
      vx != impl_->last_vx || vy != impl_->last_vy || wz != impl_->last_wz;
  const bool is_stop = (vx == 0.0f && vy == 0.0f && wz == 0.0f);

  if (!is_stop && !changed &&
      impl_->last_sent_at.time_since_epoch().count() != 0 &&
      now - impl_->last_sent_at < kMinSendInterval) {
    return;
  }

  impl_->last_vx = vx;
  impl_->last_vy = vy;
  impl_->last_wz = wz;
  impl_->last_sent_at = now;

  if (!impl_->io_context) {
    return;
  }

  const std::string json = BuildRosbridgeVelocityMessage(vx, vy, wz);
  if (is_stop) {
    rtc_logging::LogInfo("dog command forwarder: forwarding stop");
  } else {
    rtc_logging::LogInfo(
        std::string("dog command forwarder: forwarding velocity ") + json);
  }
  const std::string frame = BuildWsFrame(json);

  impl_->io_context->post([this, frame = std::move(frame)]() {
    std::lock_guard<std::mutex> lock(impl_->socket_mutex);
    if (!impl_->socket) {
      rtc_logging::LogError(
          "dog command forwarder: socket is null, dropping frame");
      return;
    }
    if (!impl_->socket->is_open()) {
      rtc_logging::LogError(
          "dog command forwarder: socket is closed, dropping frame");
      return;
    }

    SimpleWeb::error_code ec;
    size_t written = SimpleWeb::asio::write(
        *impl_->socket, SimpleWeb::asio::buffer(frame), ec);
    if (ec) {
      rtc_logging::LogError(
          std::string("dog command forwarder: send error ") +
          std::to_string(ec.value()) + ": " + ec.message());
    } else {
      rtc_logging::LogInfo(
          std::string("dog command forwarder: sent ") +
          std::to_string(written) + " bytes");
    }
  });
}

bool DogCommandForwarder::IsConnected() const {
  return impl_->connected.load(std::memory_order_acquire);
}

// ── 生命周期 ────────────────────────────────────────────────────────────────

bool DogCommandForwarder::Start() {
  if (impl_->running.load(std::memory_order_acquire)) {
    return true;
  }

  impl_->io_context = std::make_shared<SimpleWeb::asio::io_context>();
  if (!impl_->io_context) {
    rtc_logging::LogError(
        "dog command forwarder: failed to create io_context");
    return false;
  }

  impl_->work_keepalive =
      std::make_unique<SimpleWeb::asio::io_context::work>(
          *impl_->io_context);

  try {
    impl_->io_thread = std::make_unique<std::thread>([this]() {
      rtc_logging::LogInfo("dog command forwarder: io thread running");
      try {
        impl_->io_context->run();
      } catch (const std::exception& ex) {
        rtc_logging::LogError(
            std::string("dog command forwarder: io thread exception: ") +
            ex.what());
      }
    });
  } catch (const std::exception& ex) {
    rtc_logging::LogError(
        std::string("dog command forwarder: failed to spawn io thread: ") +
        ex.what());
    impl_->work_keepalive.reset();
    impl_->io_context.reset();
    return false;
  }

  impl_->running.store(true, std::memory_order_release);
  rtc_logging::LogInfo(
      "dog command forwarder: starting, target " +
      impl_->config.rosbridge_url);

  impl_->io_context->post([this]() { DoConnect(); });
  return true;
}

void DogCommandForwarder::StopImpl() {
  if (!impl_->running.load(std::memory_order_acquire)) {
    return;
  }
  impl_->running.store(false, std::memory_order_release);

  if (impl_->io_context) {
    impl_->io_context->post([this]() {
      if (impl_->reconnect_timer) {
        impl_->reconnect_timer->cancel();
        impl_->reconnect_timer.reset();
      }
      {
        std::lock_guard<std::mutex> lock(impl_->socket_mutex);
        if (impl_->socket && impl_->socket->is_open()) {
          SimpleWeb::error_code ec;
          impl_->socket->close(ec);
        }
        impl_->socket.reset();
      }
    });
    impl_->work_keepalive.reset();
    impl_->io_context->stop();
  }

  if (impl_->io_thread && impl_->io_thread->joinable()) {
    impl_->io_thread->join();
    impl_->io_thread.reset();
  }

  impl_->io_context.reset();
  impl_->connected.store(false, std::memory_order_release);
  rtc_logging::LogInfo("dog command forwarder: stopped");
}

// ── 连接管理（io_context 线程）──────────────────────────────────────────────

void DogCommandForwarder::ScheduleReconnect() {
  if (!impl_->running.load(std::memory_order_acquire) ||
      !impl_->io_context) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->socket_mutex);
    if (impl_->socket && impl_->socket->is_open()) {
      SimpleWeb::error_code ec;
      impl_->socket->close(ec);
    }
    impl_->socket.reset();
  }
  impl_->connected.store(false, std::memory_order_release);

  const auto delay = std::chrono::milliseconds(
      impl_->config.reconnect_interval_ms);
  if (!impl_->reconnect_timer) {
    impl_->reconnect_timer =
        std::make_shared<SimpleWeb::asio::steady_timer>(
            *impl_->io_context, delay);
  } else {
    impl_->reconnect_timer->expires_after(delay);
  }

  rtc_logging::LogInfo(
      "dog command forwarder: reconnecting in " +
      std::to_string(impl_->config.reconnect_interval_ms) + " ms");

  impl_->reconnect_timer->async_wait(
      [this](const SimpleWeb::error_code& ec) {
        if (!ec && impl_->running.load(std::memory_order_acquire)) {
          DoConnect();
        }
      });
}

void DogCommandForwarder::DoConnect() {
  if (!impl_->running.load(std::memory_order_acquire)) return;

  if (impl_->reconnect_timer) {
    impl_->reconnect_timer->cancel();
    impl_->reconnect_timer.reset();
  }

  {
    std::lock_guard<std::mutex> lock(impl_->socket_mutex);
    if (impl_->socket && impl_->socket->is_open()) {
      SimpleWeb::error_code ec;
      impl_->socket->close(ec);
    }
    impl_->socket.reset();
  }
  impl_->connected.store(false, std::memory_order_release);

  const std::string& url = impl_->config.rosbridge_url;
  std::string host = "10.10.10.10";
  std::string port = "9090";

  const char* kPrefix = "ws://";
  if (url.compare(0, 5, kPrefix) == 0) {
    std::string rest = url.substr(5);
    size_t colon = rest.find(':');
    size_t slash = rest.find('/');
    if (colon != std::string::npos) {
      host = rest.substr(0, colon);
      size_t port_end =
          (slash != std::string::npos) ? slash : rest.size();
      port = rest.substr(colon + 1, port_end - colon - 1);
    } else {
      size_t host_end =
          (slash != std::string::npos) ? slash : rest.size();
      host = rest.substr(0, host_end);
    }
  }

  rtc_logging::LogInfo(
      "dog command forwarder: connecting to " + host + ":" + port);

  SimpleWeb::error_code ec;
  auto addr = SimpleWeb::asio::ip::make_address(host, ec);
  if (ec) {
    rtc_logging::LogError(
        std::string("dog command forwarder: invalid IP ") + host +
        ": " + ec.message());
    ScheduleReconnect();
    return;
  }

  // 安全解析端口号，避免 std::stoi 对非数字输入抛出异常。
  unsigned short port_num = 9090;
  try {
    int parsed = std::stoi(port);
    if (parsed < 0 || parsed > 65535) {
      rtc_logging::LogError(
          "dog command forwarder: port " + port +
          " out of range, using 9090");
    } else {
      port_num = static_cast<unsigned short>(parsed);
    }
  } catch (const std::exception&) {
    rtc_logging::LogError(
        "dog command forwarder: invalid port '" + port +
        "', using 9090");
  }

  auto sock = std::make_shared<SimpleWeb::asio::ip::tcp::socket>(
      *impl_->io_context);
  SimpleWeb::asio::ip::tcp::endpoint endpoint(addr, port_num);

  sock->connect(endpoint, ec);
  if (ec) {
    rtc_logging::LogError(
        std::string("dog command forwarder: connect failed: ") +
        ec.message());
    ScheduleReconnect();
    return;
  }

  {
    SimpleWeb::asio::ip::tcp::no_delay option(true);
    sock->set_option(option, ec);
    if (ec) {
      rtc_logging::LogError(
          std::string("dog command forwarder: tcp_nodelay failed: ") +
          ec.message());
    }
  }

  // WebSocket 升级握手。
  std::ostringstream req;
  req << "GET / HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      << "Sec-WebSocket-Version: 13\r\n"
      << "\r\n";
  std::string req_str = req.str();

  SimpleWeb::asio::write(*sock, SimpleWeb::asio::buffer(req_str), ec);
  if (ec) {
    rtc_logging::LogError(
        std::string("dog command forwarder: handshake write failed: ") +
        ec.message());
    ScheduleReconnect();
    return;
  }

  SimpleWeb::asio::streambuf response;
  SimpleWeb::asio::read_until(*sock, response, "\r\n\r\n", ec);
  if (ec) {
    rtc_logging::LogError(
        std::string("dog command forwarder: handshake read failed: ") +
        ec.message());
    ScheduleReconnect();
    return;
  }

  std::string response_str(
      SimpleWeb::asio::buffers_begin(response.data()),
      SimpleWeb::asio::buffers_begin(response.data()) +
          response.size());

  if (response_str.find("101") == std::string::npos) {
    rtc_logging::LogError(
        "dog command forwarder: handshake rejected: " +
        response_str);
    ScheduleReconnect();
    return;
  }

  rtc_logging::LogInfo(
      "dog command forwarder: handshake ok, response: " +
      response_str.substr(0, response_str.find("\r\n")));

  {
    std::lock_guard<std::mutex> lock(impl_->socket_mutex);
    impl_->socket = std::move(sock);
  }
  impl_->connected.store(true, std::memory_order_release);
  rtc_logging::LogInfo(
      "dog command forwarder: connected to rosbridge at " + host +
      ":" + port);
}

}  // namespace rtc_dog
