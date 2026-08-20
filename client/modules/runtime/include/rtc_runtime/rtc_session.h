#pragma once

#include "c_rtc.h"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace rtc_runtime {

struct SessionOptions {
  std::string room_id = "zhejianglab";
  std::string config_path;
  int join_retry_ms = 3000;
  int status_interval_sec = 5;
};

class RtcSession {
 public:
  enum class RoomAction {
    Join,
    Open,
  };

  struct DataChannelConfig {
    std::string label;
    RtcPriorityType priority = RtcPriorityType::Medium;
    bool ordered = true;
    int max_retransmits = -1;
  };

  struct ExternalVideoSourceConfig {
    std::string source_id;
    RtcPriorityType priority = RtcPriorityType::High;
  };

  struct Features {
    bool enable_data_channel = false;
    bool enable_external_audio_source = false;
    std::string external_audio_source_id;
    bool enable_external_video_source = false;
    std::string external_video_source_id;
    RoomAction room_action = RoomAction::Join;
    RtcRoomType open_room_type = RtcRoomType::VideoBroadcasting;
    bool open_room_force = false;
    std::vector<DataChannelConfig> additional_data_channels;
    std::vector<ExternalVideoSourceConfig> additional_external_video_sources;
  };

  using RecvMessageCallback =
      std::function<void(RtcSessionId, RtcDataChannelLabel, const char*, size_t)>;
  using P2PStateCallback = std::function<void(RtcSessionId, RtcP2PState)>;
  using DataChannelStateCallback = std::function<void(
      RtcSessionId, RtcDataChannelLabel, RtcDataChannelState)>;
  using ServerConnectionStateCallback =
      std::function<void(RtcServerConnectionState)>;
  using RecvAudioFrameCallback = std::function<void(
      RtcSessionId,
      RtcAudioSourceId,
      RtcMediaSourceType,
      size_t,
      size_t,
      size_t,
      size_t,
      const void*,
      size_t)>;
  using RecvFrameCallback = std::function<void(RtcSessionId,
                                               RtcVideoSourceId,
                                               RtcMediaSourceType,
                                               size_t,
                                               size_t,
                                               size_t,
                                               const unsigned char*,
                                               size_t)>;

  struct Callbacks {
    RecvMessageCallback recv_message;
    P2PStateCallback p2p_state;
    DataChannelStateCallback datachannel_state;
    ServerConnectionStateCallback server_connection_state;
    RecvAudioFrameCallback recv_audio_frame;
    RecvFrameCallback recv_frame;
  };

  explicit RtcSession(const SessionOptions& options);
  RtcSession(const SessionOptions& options, const Features& features);
  RtcSession(const SessionOptions& options,
             const Features& features,
             const Callbacks& callbacks);
  ~RtcSession();

  bool Init();
  void Shutdown();
  void Tick();

  void NoteCapturedFrame();
  bool IsRoomJoined() const;
  bool IsReadyToSend() const;
  uint64_t captured_frames() const;
  uint64_t sent_frames() const;
  uint64_t sent_audio_frames() const;
  uint64_t remote_video_frames() const;
  uint64_t remote_audio_frames() const;
  uint64_t received_messages() const;

  bool SendI420Frame(const uint8_t* i420_data,
                     size_t i420_size,
                     size_t width,
                     size_t height,
                     size_t stride_y,
                     size_t stride_u,
                     size_t stride_v);
  bool SendI420Frame(const char* video_source_id,
                     const uint8_t* i420_data,
                     size_t i420_size,
                     size_t width,
                     size_t height,
                     size_t stride_y,
                     size_t stride_u,
                     size_t stride_v);
  bool SendAudioFrame(const void* audio_data,
                      size_t audio_data_size,
                      size_t bits_per_sample,
                      size_t sample_rate,
                      size_t number_of_channels,
                      size_t number_of_frames);

 private:
  void MaybeEnterRoom(std::chrono::steady_clock::time_point now);
  void PrintStatus() const;
  void LogRtcCall(const char* action, RtcErrorCode code) const;

  static void OnRoom(RtcRoomOperation op, RtcRoomId roomid);
  static void OnP2PState(RtcSessionId sessionid, RtcP2PState state);
  static void OnDataChannelState(RtcSessionId sessionid,
                                 RtcDataChannelLabel label,
                                 RtcDataChannelState state);
  static void OnServerConnectionState(RtcServerConnectionState state);
  static void OnRecvMessage(RtcSessionId remote_sessionid,
                            RtcDataChannelLabel label,
                            const char* msg,
                            size_t msg_size);
  static void OnRecvAudioFrame(RtcSessionId,
                               RtcAudioSourceId,
                               RtcMediaSourceType,
                               size_t,
                               size_t,
                               size_t,
                               size_t,
                               const void*,
                               size_t);
  static void OnRecvFrame(RtcSessionId remote_sessionid,
                          RtcVideoSourceId sourceid,
                          RtcMediaSourceType source_type,
                          size_t width,
                          size_t height,
                          size_t dimension,
                          const unsigned char* buffer,
                          size_t sz_buffer);
  static void OnChannelNetworkStats(RtcSessionId, RtcNetStats);

  static RtcSession* instance_;

  SessionOptions options_;
  Features features_;
  Callbacks callbacks_;
  std::string rtc_cfg_path_;
  std::atomic<bool> rtc_inited_{false};
  std::atomic<bool> room_joined_{false};
  std::atomic<RtcServerConnectionState> server_state_{ServerDisconnected};
  std::atomic<uint64_t> captured_frames_{0};
  std::atomic<uint64_t> sent_frames_{0};
  std::atomic<uint64_t> sent_audio_frames_{0};
  std::atomic<uint64_t> received_messages_{0};
  std::atomic<uint64_t> remote_audio_frames_{0};
  std::atomic<uint64_t> remote_video_frames_{0};
  std::atomic<bool> room_retry_requested_{false};
  std::atomic<uint32_t> connected_peer_count_{0};
  std::unordered_set<std::string> external_video_source_ids_;
  std::unordered_set<std::string> external_audio_source_ids_;
  std::chrono::steady_clock::time_point last_join_attempt_{};
  std::chrono::steady_clock::time_point last_status_{};
  mutable std::mutex connected_peers_mutex_;
  std::unordered_set<RtcSessionId> connected_peers_;
};

}  // 命名空间 rtc_runtime
