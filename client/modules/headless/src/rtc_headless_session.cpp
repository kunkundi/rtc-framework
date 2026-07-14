#include "rtc_headless/rtc_headless_session.h"

#include <cstring>
#include <sstream>

namespace rtc_camera_headless {
namespace {

constexpr const char* kDataChannelLabel = "datachannel";
constexpr const char* kVisionDetectionChannelLabel = "vision.detect.v1";
constexpr const char* kExternalVideoSource = "merged_image";

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

bool IsP2PConnectedState(RtcP2PState state) {
  return state == P2PConnected;
}

bool IsP2PDisconnectedState(RtcP2PState state) {
  return state == P2PDisconnected || state == P2PFailed || state == P2PClosed;
}

}  // namespace

RtcHeadlessSession* RtcHeadlessSession::instance_ = nullptr;

RtcHeadlessSession::RtcHeadlessSession(const CaptureOptions& options)
    : RtcHeadlessSession(options, Features(), Callbacks()) {}

RtcHeadlessSession::RtcHeadlessSession(const CaptureOptions& options,
                                       const Features& features)
    : RtcHeadlessSession(options, features, Callbacks()) {}

RtcHeadlessSession::RtcHeadlessSession(const CaptureOptions& options,
                                       const Features& features,
                                       const Callbacks& callbacks)
    : options_(options), features_(features), callbacks_(callbacks) {
  instance_ = this;
}

RtcHeadlessSession::~RtcHeadlessSession() {
  Shutdown();
  instance_ = nullptr;
}

bool RtcHeadlessSession::Init() {
  rtc_cfg_path_ = ResolveConfigPath(options_.config_path);
  LogInfo(std::string("rtc.cfg: ") + rtc_cfg_path_);

  RtcInitParams params;
  std::memset(&params, 0, sizeof(params));
  params.config_filepath = rtc_cfg_path_.c_str();
  params.room_handler = &RtcHeadlessSession::OnRoom;
  params.P2P_state_handler = &RtcHeadlessSession::OnP2PState;
  params.datachannel_state_handler = &RtcHeadlessSession::OnDataChannelState;
  params.serverconnection_state_handler = &RtcHeadlessSession::OnServerConnectionState;
  params.recv_msg_handler = &RtcHeadlessSession::OnRecvMessage;
  params.recv_audioframe_handler = &RtcHeadlessSession::OnRecvAudioFrame;
  params.recv_frame_handler = &RtcHeadlessSession::OnRecvFrame;
  params.channel_network_stats_handler = &RtcHeadlessSession::OnChannelNetworkStats;

  const RtcErrorCode init_code = RtcInitAgentV2(params);
  if (init_code != RtcErrorCode::OK) {
    LogRtcCall("RtcInitAgentV2", init_code);
    return false;
  }
  rtc_inited_.store(true);
  last_status_ = std::chrono::steady_clock::now();
  LogInfo("RtcInitAgentV2 success");

  if (features_.enable_data_channel) {
    const RtcErrorCode dc_code =
        RtcAddDataChannel(kDataChannelLabel, RtcPriorityType::High, true, -1);
    LogRtcCall("RtcAddDataChannel", dc_code);

    const RtcErrorCode vision_dc_code = RtcAddDataChannel(
        kVisionDetectionChannelLabel, RtcPriorityType::Medium, false, 0);
    LogRtcCall("RtcAddDataChannel(vision.detect.v1)", vision_dc_code);
  }

  for (const DataChannelConfig& channel :
       features_.additional_data_channels) {
    const RtcErrorCode channel_code = RtcAddDataChannel(
        channel.label.c_str(), channel.priority, channel.ordered,
        channel.max_retransmits);
    const std::string action =
        std::string("RtcAddDataChannel(") + channel.label + ")";
    LogRtcCall(action.c_str(), channel_code);
    if (channel_code != RtcErrorCode::OK) {
      return false;
    }
  }

  if (features_.enable_external_video_source) {
    const RtcErrorCode video_code =
        RtcAddExternalVideoSource(kExternalVideoSource, RtcPriorityType::High);
    LogRtcCall("RtcAddExternalVideoSource", video_code);
    if (video_code != RtcErrorCode::OK) {
      return false;
    }
  }

  return true;
}

void RtcHeadlessSession::Shutdown() {
  if (!rtc_inited_.exchange(false)) {
    return;
  }

  const RtcErrorCode leave_code = RtcLeaveRoom();
  if (leave_code != RtcErrorCode::OK &&
      leave_code != RtcErrorCode::AgentNotLogined) {
    LogRtcCall("RtcLeaveRoom", leave_code);
  }
  RtcDestoryAgent();
  room_joined_.store(false);
  room_retry_requested_.store(false);
  connected_peer_count_.store(0);
  {
    std::lock_guard<std::mutex> lock(connected_peers_mutex_);
    connected_peers_.clear();
  }
}

void RtcHeadlessSession::Tick() {
  if (!rtc_inited_.load()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  MaybeEnterRoom(now);
  if (options_.status_interval_sec > 0 &&
      now - last_status_ >=
          std::chrono::seconds(options_.status_interval_sec)) {
    PrintStatus();
    last_status_ = now;
  }
}

void RtcHeadlessSession::NoteCapturedFrame() {
  captured_frames_.fetch_add(1, std::memory_order_relaxed);
}

bool RtcHeadlessSession::IsRoomJoined() const {
  return room_joined_.load();
}

bool RtcHeadlessSession::IsReadyToSend() const {
  return room_joined_.load() && connected_peer_count_.load() > 0;
}

uint64_t RtcHeadlessSession::captured_frames() const {
  return captured_frames_.load();
}

uint64_t RtcHeadlessSession::sent_frames() const {
  return sent_frames_.load();
}

uint64_t RtcHeadlessSession::remote_video_frames() const {
  return remote_video_frames_.load();
}

uint64_t RtcHeadlessSession::remote_audio_frames() const {
  return remote_audio_frames_.load();
}

uint64_t RtcHeadlessSession::received_messages() const {
  return received_messages_.load();
}

bool RtcHeadlessSession::SendI420Frame(const uint8_t* i420_data,
                                       size_t i420_size,
                                       size_t width,
                                       size_t height,
                                       size_t stride_y,
                                       size_t stride_u,
                                       size_t stride_v) {
  if (!i420_data || i420_size == 0) {
    return false;
  }

  RtcYUV420pFrame frame;
  frame.width = width;
  frame.height = height;
  frame.stride_Y = stride_y;
  frame.stride_U = stride_u;
  frame.stride_V = stride_v;
  frame.buffer = const_cast<unsigned char*>(i420_data);
  frame.sz_buffer = i420_size;

  const RtcErrorCode send_code = RtcSendFrame(kExternalVideoSource, &frame);
  if (send_code != RtcErrorCode::OK) {
    LogRtcCall("RtcSendFrame", send_code);
    return false;
  }

  sent_frames_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void RtcHeadlessSession::MaybeEnterRoom(
    std::chrono::steady_clock::time_point now) {
  if (server_state_.load() != ServerLogined || room_joined_.load()) {
    return;
  }
  const bool retry_requested =
      room_retry_requested_.exchange(false, std::memory_order_relaxed);
  if (!retry_requested &&
      last_join_attempt_.time_since_epoch().count() != 0 &&
      now - last_join_attempt_ <
          std::chrono::milliseconds(options_.join_retry_ms)) {
    return;
  }
  last_join_attempt_ = now;

  RtcErrorCode code = RtcErrorCode::Failed;
  const char* action = "";
  if (features_.room_action == RoomAction::Open) {
    action = "RtcOpenRoom";
    code = RtcOpenRoom(const_cast<char*>(options_.room_id.c_str()),
                       features_.open_room_type, features_.open_room_force);
  } else {
    action = "RtcJoinRoom";
    code = RtcJoinRoom(const_cast<char*>(options_.room_id.c_str()));
  }

  LogRtcCall(action, code);
  if (code == RtcErrorCode::OK || code == RtcErrorCode::AgentAlreadyInRoom) {
    room_joined_.store(true);
    room_retry_requested_.store(false, std::memory_order_relaxed);
  }
}

void RtcHeadlessSession::PrintStatus() const {
  const char* room_state_label =
      features_.room_action == RoomAction::Open ? "opened" : "joined";
  std::ostringstream oss;
  oss << "status: server=" << ServerStateText(server_state_.load())
      << " room=" << options_.room_id
      << " " << room_state_label << "="
      << (room_joined_.load() ? "yes" : "no")
      << " p2p_connected=" << connected_peer_count_.load()
      << " ready=" << (IsReadyToSend() ? "yes" : "no")
      << " captured=" << captured_frames_.load()
      << " sent=" << sent_frames_.load()
      << " recv_msg=" << received_messages_.load()
      << " recv_audio=" << remote_audio_frames_.load()
      << " recv_video=" << remote_video_frames_.load();
  LogInfo(oss.str());
}

void RtcHeadlessSession::LogRtcCall(const char* action, RtcErrorCode code) const {
  std::ostringstream oss;
  oss << action << ": " << RtcErrorMessage(code) << " ("
      << static_cast<int>(code) << ")";
  if (code == RtcErrorCode::OK || code == RtcErrorCode::AgentAlreadyInRoom) {
    LogInfo(oss.str());
  } else {
    LogError(oss.str());
  }
}

void RtcHeadlessSession::OnRoom(RtcRoomOperation op, RtcRoomId roomid) {
  if (!instance_) {
    return;
  }
  const std::string target_room =
      roomid != nullptr ? std::string(roomid) : std::string();
  if (target_room == instance_->options_.room_id) {
    if (op == RoomDelete) {
      instance_->room_joined_.store(false);
      instance_->room_retry_requested_.store(false,
                                             std::memory_order_relaxed);
      instance_->connected_peer_count_.store(0, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(instance_->connected_peers_mutex_);
        instance_->connected_peers_.clear();
      }
    } else if (op == RoomNew &&
               instance_->features_.room_action == RoomAction::Join &&
               !instance_->room_joined_.load()) {
      instance_->room_retry_requested_.store(true,
                                             std::memory_order_relaxed);
    }
  }
  std::ostringstream oss;
  oss << "room event: op=" << static_cast<int>(op)
      << " room=" << (roomid ? roomid : "");
  LogInfo(oss.str());
}

void RtcHeadlessSession::OnP2PState(RtcSessionId sessionid, RtcP2PState state) {
  if (!instance_) {
    return;
  }
  if (IsP2PConnectedState(state) || IsP2PDisconnectedState(state)) {
    std::lock_guard<std::mutex> lock(instance_->connected_peers_mutex_);
    if (IsP2PConnectedState(state)) {
      instance_->connected_peers_.insert(sessionid);
    } else {
      instance_->connected_peers_.erase(sessionid);
    }
    instance_->connected_peer_count_.store(
        static_cast<uint32_t>(instance_->connected_peers_.size()),
        std::memory_order_relaxed);
  }
  std::ostringstream oss;
  oss << "p2p: session=" << sessionid << " state=" << P2PStateText(state);
  LogInfo(oss.str());
  if (instance_->callbacks_.p2p_state) {
    instance_->callbacks_.p2p_state(sessionid, state);
  }
}

void RtcHeadlessSession::OnDataChannelState(RtcSessionId sessionid,
                                            RtcDataChannelLabel label,
                                            RtcDataChannelState state) {
  if (!instance_) {
    return;
  }
  std::ostringstream oss;
  oss << "datachannel: session=" << sessionid
      << " label=" << (label ? label : "")
      << " state=" << static_cast<int>(state);
  LogInfo(oss.str());
  if (instance_->callbacks_.datachannel_state) {
    instance_->callbacks_.datachannel_state(sessionid, label, state);
  }
}

void RtcHeadlessSession::OnServerConnectionState(RtcServerConnectionState state) {
  if (!instance_) {
    return;
  }
  instance_->server_state_.store(state);
  if (state != ServerLogined) {
    instance_->room_joined_.store(false);
    instance_->room_retry_requested_.store(false, std::memory_order_relaxed);
    instance_->connected_peer_count_.store(0, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(instance_->connected_peers_mutex_);
      instance_->connected_peers_.clear();
    }
  }

  std::ostringstream oss;
  oss << "server state: " << ServerStateText(state);
  LogInfo(oss.str());
  if (instance_->callbacks_.server_connection_state) {
    instance_->callbacks_.server_connection_state(state);
  }
}

void RtcHeadlessSession::OnRecvMessage(RtcSessionId remote_sessionid,
                                       RtcDataChannelLabel label,
                                       const char* msg,
                                       size_t msg_size) {
  if (!instance_) {
    return;
  }
  instance_->received_messages_.fetch_add(1, std::memory_order_relaxed);
  if (instance_->callbacks_.recv_message) {
    instance_->callbacks_.recv_message(remote_sessionid, label, msg, msg_size);
  }
  std::ostringstream oss;
  oss << "recv msg from " << remote_sessionid << " ["
      << (label ? label : "") << "] bytes=" << msg_size;
  LogInfo(oss.str());
}

void RtcHeadlessSession::OnRecvAudioFrame(RtcSessionId remote_sessionid,
                                          RtcAudioSourceId sourceid,
                                          RtcMediaSourceType source_type,
                                          size_t bits_per_sample,
                                          size_t sample_rate,
                                          size_t number_of_channels,
                                          size_t number_of_frames,
                                          const void* audio_data,
                                          size_t sz_audio_data) {
  if (!instance_) {
    return;
  }
  instance_->remote_audio_frames_.fetch_add(1, std::memory_order_relaxed);
  if (instance_->callbacks_.recv_audio_frame) {
    instance_->callbacks_.recv_audio_frame(
        remote_sessionid, sourceid, source_type, bits_per_sample, sample_rate,
        number_of_channels, number_of_frames, audio_data, sz_audio_data);
  }
}

void RtcHeadlessSession::OnRecvFrame(RtcSessionId remote_sessionid,
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

  const uint64_t received = instance_->remote_video_frames_.fetch_add(1) + 1;
  if (instance_->callbacks_.recv_frame) {
    instance_->callbacks_.recv_frame(remote_sessionid, sourceid, source_type,
                                     width, height, dimension, buffer,
                                     sz_buffer);
  }
  if ((received % 120) == 1) {
    std::ostringstream oss;
    oss << "recv frame #" << received << " from session=" << remote_sessionid
        << " source=" << (sourceid ? sourceid : "")
        << " type=" << static_cast<int>(source_type)
        << " size=" << width << "x" << height;
    LogInfo(oss.str());
  }
}

void RtcHeadlessSession::OnChannelNetworkStats(RtcSessionId, RtcNetStats) {}

}  // namespace rtc_camera_headless
