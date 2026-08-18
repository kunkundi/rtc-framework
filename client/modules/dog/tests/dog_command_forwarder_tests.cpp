#include "rtc_dog/dog_command_forwarder.h"

#include <client_ws.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

std::string ExtractHeader(const std::string& request,
                          const std::string& expected_name) {
  std::istringstream stream(request);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    if (line.substr(0, colon) == expected_name) {
      const size_t value_start = line.find_first_not_of(" \t", colon + 1);
      return value_start == std::string::npos
                 ? ""
                 : line.substr(value_start);
    }
  }
  return "";
}

std::string RequestLine(const std::string& request) {
  const size_t end = request.find("\r\n");
  return request.substr(0, end);
}

bool ReadExact(SimpleWeb::asio::ip::tcp::socket* socket,
               void* data,
               size_t size) {
  SimpleWeb::error_code error;
  SimpleWeb::asio::read(*socket, SimpleWeb::asio::buffer(data, size), error);
  return !error;
}

bool ReadClientFrame(SimpleWeb::asio::ip::tcp::socket* socket,
                     std::string* payload) {
  uint8_t header[2] = {0, 0};
  if (!ReadExact(socket, header, sizeof(header)) ||
      (header[1] & 0x80) == 0) {
    return false;
  }

  uint64_t payload_size = header[1] & 0x7F;
  if (payload_size == 126) {
    uint8_t extended[2] = {0, 0};
    if (!ReadExact(socket, extended, sizeof(extended))) {
      return false;
    }
    payload_size = (static_cast<uint64_t>(extended[0]) << 8) | extended[1];
  } else if (payload_size == 127) {
    uint8_t extended[8] = {0};
    if (!ReadExact(socket, extended, sizeof(extended))) {
      return false;
    }
    payload_size = 0;
    for (uint8_t byte : extended) {
      payload_size = (payload_size << 8) | byte;
    }
  }
  if (payload_size > 64 * 1024) {
    return false;
  }

  uint8_t mask[4] = {0};
  if (!ReadExact(socket, mask, sizeof(mask))) {
    return false;
  }
  std::vector<uint8_t> encoded(static_cast<size_t>(payload_size));
  if (!encoded.empty() &&
      !ReadExact(socket, encoded.data(), encoded.size())) {
    return false;
  }

  payload->resize(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    (*payload)[i] = static_cast<char>(encoded[i] ^ mask[i % 4]);
  }
  return true;
}

class ReconnectingWebSocketServer {
 public:
  ReconnectingWebSocketServer()
      : acceptor_(io_context_,
                  SimpleWeb::asio::ip::tcp::endpoint(
                      SimpleWeb::asio::ip::address_v4::loopback(), 0)) {
    worker_ = std::thread([this]() { Run(); });
  }

  ~ReconnectingWebSocketServer() {
    Stop();
  }

  unsigned short port() const {
    return acceptor_.local_endpoint().port();
  }

  bool WaitForConnections(size_t count,
                          std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return requests_.size() >= count; });
  }

  bool WaitForPayloads(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return payloads_.size() >= count; });
  }

  std::string request(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requests_.at(index);
  }

  std::string payload(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return payloads_.at(index);
  }

 private:
  void Stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    SimpleWeb::error_code ignored;
    acceptor_.close(ignored);
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      if (active_socket_) {
        active_socket_->cancel(ignored);
        active_socket_->close(ignored);
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Run() {
    for (size_t connection_index = 0;
         connection_index < 2 && !stopped_.load(); ++connection_index) {
      auto socket = std::make_shared<SimpleWeb::asio::ip::tcp::socket>(
          io_context_);
      {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        active_socket_ = socket;
      }

      SimpleWeb::error_code error;
      acceptor_.accept(*socket, error);
      if (error || stopped_.load()) {
        break;
      }

      SimpleWeb::asio::streambuf request_buffer;
      SimpleWeb::asio::read_until(*socket, request_buffer, "\r\n\r\n", error);
      if (error) {
        break;
      }
      const std::string request(
          SimpleWeb::asio::buffers_begin(request_buffer.data()),
          SimpleWeb::asio::buffers_end(request_buffer.data()));
      const std::string key = ExtractHeader(request, "Sec-WebSocket-Key");
      const std::string accept = SimpleWeb::Crypto::Base64::encode(
          SimpleWeb::Crypto::sha1(
              key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
      const std::string response =
          "HTTP/1.1 101 Switching Protocols\r\n"
          "Upgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Accept: " +
          accept + "\r\n\r\n";
      SimpleWeb::asio::write(*socket, SimpleWeb::asio::buffer(response), error);
      if (error) {
        break;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(RequestLine(request));
      }
      cv_.notify_all();

      const size_t frames_to_read = connection_index == 0 ? 2 : 3;
      for (size_t frame_index = 0; frame_index < frames_to_read;
           ++frame_index) {
        std::string payload;
        if (!ReadClientFrame(socket.get(), &payload)) {
          break;
        }
        {
          std::lock_guard<std::mutex> lock(mutex_);
          payloads_.push_back(std::move(payload));
        }
        cv_.notify_all();
      }

      socket->shutdown(
          SimpleWeb::asio::ip::tcp::socket::shutdown_both, error);
      socket->close(error);
      {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        active_socket_.reset();
      }
    }
  }

  SimpleWeb::asio::io_context io_context_;
  SimpleWeb::asio::ip::tcp::acceptor acceptor_;
  std::thread worker_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> requests_;
  std::vector<std::string> payloads_;
  std::mutex socket_mutex_;
  std::shared_ptr<SimpleWeb::asio::ip::tcp::socket> active_socket_;
};

// 单连接 WebSocket 服务端：完成握手后在同一连接上读取若干帧，用于
// 校验站立/趴下 actionlib goal 的转发内容，避免依赖重连时序。
class BehaviorGoalServer {
 public:
  BehaviorGoalServer()
      : acceptor_(io_context_,
                  SimpleWeb::asio::ip::tcp::endpoint(
                      SimpleWeb::asio::ip::address_v4::loopback(), 0)) {
    worker_ = std::thread([this]() { Run(); });
  }

  ~BehaviorGoalServer() {
    Stop();
  }

  unsigned short port() const {
    return acceptor_.local_endpoint().port();
  }

  bool WaitForPayloads(size_t count,
                       std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count]() { return payloads_.size() >= count; });
  }

  std::string payload(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return payloads_.at(index);
  }

 private:
  void Stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    SimpleWeb::error_code ignored;
    acceptor_.close(ignored);
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      if (active_socket_) {
        active_socket_->cancel(ignored);
        active_socket_->close(ignored);
      }
    }
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  void Run() {
    auto socket = std::make_shared<SimpleWeb::asio::ip::tcp::socket>(
        io_context_);
    {
      std::lock_guard<std::mutex> lock(socket_mutex_);
      active_socket_ = socket;
    }

    SimpleWeb::error_code error;
    acceptor_.accept(*socket, error);
    if (error || stopped_.load()) {
      return;
    }

    SimpleWeb::asio::streambuf request_buffer;
    SimpleWeb::asio::read_until(*socket, request_buffer, "\r\n\r\n", error);
    if (error) {
      return;
    }
    const std::string request(
        SimpleWeb::asio::buffers_begin(request_buffer.data()),
        SimpleWeb::asio::buffers_end(request_buffer.data()));
    const std::string key = ExtractHeader(request, "Sec-WebSocket-Key");
    const std::string accept = SimpleWeb::Crypto::Base64::encode(
        SimpleWeb::Crypto::sha1(
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    SimpleWeb::asio::write(*socket, SimpleWeb::asio::buffer(response), error);
    if (error) {
      return;
    }

    // 首帧为建立连接时写入的零速度基线，后续为待校验的动作帧。
    for (size_t frame_index = 0; frame_index < 4 && !stopped_.load();
         ++frame_index) {
      std::string payload;
      if (!ReadClientFrame(socket.get(), &payload)) {
        break;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        payloads_.push_back(std::move(payload));
      }
      cv_.notify_all();
    }

    socket->shutdown(
        SimpleWeb::asio::ip::tcp::socket::shutdown_both, error);
    socket->close(error);
  }

  SimpleWeb::asio::io_context io_context_;
  SimpleWeb::asio::ip::tcp::acceptor acceptor_;
  std::thread worker_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> payloads_;
  std::mutex socket_mutex_;
  std::shared_ptr<SimpleWeb::asio::ip::tcp::socket> active_socket_;
};

class StalledHandshakeServer {
 public:
  StalledHandshakeServer()
      : acceptor_(io_context_,
                  SimpleWeb::asio::ip::tcp::endpoint(
                      SimpleWeb::asio::ip::address_v4::loopback(), 0)) {
    worker_ = std::thread([this]() {
      auto socket = std::make_shared<SimpleWeb::asio::ip::tcp::socket>(
          io_context_);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        socket_ = socket;
      }
      SimpleWeb::error_code error;
      acceptor_.accept(*socket, error);
      if (!error) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return stopped_; });
      }
    });
  }

  ~StalledHandshakeServer() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
      SimpleWeb::error_code ignored;
      acceptor_.close(ignored);
      if (socket_) {
        socket_->cancel(ignored);
        socket_->close(ignored);
      }
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  unsigned short port() const {
    return acceptor_.local_endpoint().port();
  }

 private:
  SimpleWeb::asio::io_context io_context_;
  SimpleWeb::asio::ip::tcp::acceptor acceptor_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopped_ = false;
  std::shared_ptr<SimpleWeb::asio::ip::tcp::socket> socket_;
};

bool WaitForConnectionState(rtc_dog::DogCommandForwarder* forwarder,
                            bool connected,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (forwarder->IsConnected() == connected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return forwarder->IsConnected() == connected;
}

void TestSendAndReconnect() {
  ReconnectingWebSocketServer server;
  rtc_dog::DogCommandForwarder::Config config;
  config.rosbridge_url = "ws://127.0.0.1:" +
                         std::to_string(server.port()) + "/bridge";
  config.reconnect_interval_ms = 500;
  config.connect_timeout_ms = 1000;
  config.shutdown_timeout_ms = 100;
  config.max_forward_speed = 1.5f;
  rtc_dog::DogCommandForwarder forwarder(config);

  std::string error;
  Check(forwarder.Open(&error), "connect to local WebSocket server");
  Check(server.WaitForConnections(1, std::chrono::seconds(1)),
        "server observes initial connection");
  Check(server.request(0) == "GET /bridge HTTP/1.1",
        "use configured WebSocket path");
  Check(server.WaitForPayloads(1, std::chrono::seconds(1)),
        "initial connection sends a stop frame");
  const nlohmann::json initial_stop =
      nlohmann::json::parse(server.payload(0));
  Check(initial_stop.at("msg").at("vx").get<float>() == 0.0f &&
            initial_stop.at("msg").at("wz").get<float>() == 0.0f,
        "initial connection establishes a zero-velocity baseline");

  vts_rtc::vehicle::DriveCommand drive;
  drive.drive_direction = vts_rtc::vehicle::DriveDirection::Forward;
  drive.throttle = 0.5f;
  Check(forwarder.SendDriveCommand(drive).accepted,
        "accept command while rosbridge is connected");
  Check(server.WaitForPayloads(2, std::chrono::seconds(1)),
        "server receives first velocity frame");
  const nlohmann::json first = nlohmann::json::parse(server.payload(1));
  Check(first.at("topic") == "/alphadog_node/set_velocity",
        "publish to dog velocity topic");
  Check(first.at("msg").at("vx").get<float>() == 0.75f,
        "map throttle to configured forward speed");

  Check(WaitForConnectionState(&forwarder, false,
                               std::chrono::seconds(1)),
        "detect server disconnect");
  const rtc_vehicle::VehicleCommandResult disconnected =
      forwarder.SendDriveCommand(drive);
  Check(!disconnected.accepted &&
            disconnected.error_code ==
                vts_rtc::vehicle::VehicleErrorCode::InvalidState,
        "reject commands while rosbridge is disconnected");

  Check(server.WaitForConnections(2, std::chrono::seconds(2)),
        "automatically reconnect after disconnect");
  Check(WaitForConnectionState(&forwarder, true,
                               std::chrono::seconds(1)),
        "report reconnected state");
  Check(server.WaitForPayloads(3, std::chrono::seconds(1)),
        "reconnect sends a stop frame before a fresh command");
  const nlohmann::json reconnect_stop =
      nlohmann::json::parse(server.payload(2));
  Check(reconnect_stop.at("msg").at("vx").get<float>() == 0.0f &&
            reconnect_stop.at("msg").at("wz").get<float>() == 0.0f,
        "reconnect restores a zero-velocity baseline");
  drive.steering_direction =
      vts_rtc::vehicle::SteeringDirection::Left;
  Check(forwarder.SendDriveCommand(drive).accepted,
        "accept a fresh command after reconnect");
  Check(server.WaitForPayloads(4, std::chrono::seconds(1)),
        "server receives command after reconnect");
  forwarder.Close();
  Check(server.WaitForPayloads(5, std::chrono::seconds(1)),
        "server receives shutdown stop frame");
  const nlohmann::json stopped = nlohmann::json::parse(server.payload(4));
  Check(stopped.at("msg").at("vx").get<float>() == 0.0f &&
            stopped.at("msg").at("wz").get<float>() == 0.0f,
        "shutdown sends zero velocity");
}

void TestDogBehaviorGoalForwarding() {
  BehaviorGoalServer server;
  rtc_dog::DogCommandForwarder::Config config;
  config.rosbridge_url =
      "ws://127.0.0.1:" + std::to_string(server.port()) + "/bridge";
  config.connect_timeout_ms = 1000;
  config.shutdown_timeout_ms = 100;
  rtc_dog::DogCommandForwarder forwarder(config);

  std::string error;
  Check(forwarder.Open(&error), "connect for behavior goal test");
  Check(server.WaitForPayloads(1, std::chrono::seconds(1)),
        "initial connection establishes a zero-velocity baseline");

  vts_rtc::vehicle::DogAction stand;
  stand.request_id = 100;
  stand.action = vts_rtc::vehicle::DogActionType::Stand;
  Check(forwarder.SendDogAction(stand).accepted,
        "accept stand while rosbridge is connected");
  Check(server.WaitForPayloads(2, std::chrono::seconds(1)),
        "server receives stand goal frame");
  const nlohmann::json stand_goal =
      nlohmann::json::parse(server.payload(1));
  Check(stand_goal.at("topic") ==
            "/agent_skill/do_dog_behavior/execute/goal",
        "publish stand to dog behavior goal topic");
  const std::string stand_id =
      stand_goal.at("msg").at("goal_id").at("id");
  Check(stand_id.compare(0, 10, "cli_stand_") == 0,
        "stand goal id uses cli_stand_ prefix");
  Check(stand_goal.at("msg").at("goal").at("invoker") == "App-591",
        "stand goal uses App-591 invoker");
  const std::string stand_args =
      stand_goal.at("msg").at("goal").at("args");
  Check(stand_args == "[{\"behavior\":\"force_recovery_balance_stand\"}]",
        "stand args match expected behavior json");
  Check(stand_goal.at("msg").at("goal_id").at("stamp").at("secs").get<int64_t>() >
            0,
        "goal stamp secs uses current time");
  const uint64_t stand_seq =
      stand_goal.at("msg").at("header").at("seq").get<uint64_t>();
  Check(stand_seq > 0, "header seq is positive");

  vts_rtc::vehicle::DogAction rest;
  rest.request_id = 101;
  rest.action = vts_rtc::vehicle::DogActionType::LieDown;
  Check(forwarder.SendDogAction(rest).accepted,
        "accept lie_down while rosbridge is connected");
  Check(server.WaitForPayloads(3, std::chrono::seconds(1)),
        "server receives lie_down goal frame");
  const nlohmann::json rest_goal =
      nlohmann::json::parse(server.payload(2));
  const std::string rest_id =
      rest_goal.at("msg").at("goal_id").at("id");
  Check(rest_id.compare(0, 9, "cli_rest_") == 0,
        "lie_down goal id uses cli_rest_ prefix");
  Check(stand_id != rest_id,
        "adjacent dog actions use distinct goal ids");
  const std::string rest_args =
      rest_goal.at("msg").at("goal").at("args");
  Check(rest_args ==
            "[{\"behavior\":\"force_recovery_balance_stand\"},{\"behavior\":\"rest\"}]",
        "lie_down args match expected behavior json");
  const uint64_t rest_seq =
      rest_goal.at("msg").at("header").at("seq").get<uint64_t>();
  Check(rest_seq > stand_seq, "header seq is monotonically increasing");

  forwarder.Close();
}

void TestHandshakeTimeoutDoesNotHang() {
  StalledHandshakeServer server;
  rtc_dog::DogCommandForwarder::Config config;
  config.rosbridge_url =
      "ws://127.0.0.1:" + std::to_string(server.port());
  config.connect_timeout_ms = 100;
  config.shutdown_timeout_ms = 50;
  rtc_dog::DogCommandForwarder forwarder(config);

  const auto started_at = std::chrono::steady_clock::now();
  std::string error;
  Check(!forwarder.Open(&error), "reject a stalled WebSocket handshake");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
  Check(elapsed < std::chrono::seconds(1),
        "handshake timeout returns without hanging shutdown");
}

void TestInvalidSpeedLimitIsRejected() {
  rtc_dog::DogCommandForwarder::Config config;
  config.max_forward_speed = -1.0f;
  rtc_dog::DogCommandForwarder forwarder(config);
  std::string error;
  Check(!forwarder.Open(&error), "reject negative direct speed config");
  Check(error.find("finite and positive") != std::string::npos,
        "report invalid dog speed config");
}

}  // namespace

int main() {
  TestSendAndReconnect();
  TestDogBehaviorGoalForwarding();
  TestHandshakeTimeoutDoesNotHang();
  TestInvalidSpeedLimitIsRejected();
  std::cout << "rtc_dog_command_forwarder_tests passed" << std::endl;
  return 0;
}
