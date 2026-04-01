#pragma once

#include "c_rtc.h"
#include "rtc_camera_common.h"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <string>

namespace rtc_camera_headless {

class RtcHeadlessSession {
 public:
  explicit RtcHeadlessSession(const CaptureOptions& options);
  ~RtcHeadlessSession();

  bool Init();
  void Shutdown();
  void Tick();

  void NoteCapturedFrame();
  bool IsRoomJoined() const;
  uint64_t sent_frames() const;

  bool SendI420Frame(const uint8_t* i420_data,
                     size_t i420_size,
                     size_t width,
                     size_t height,
                     size_t stride_y,
                     size_t stride_u,
                     size_t stride_v);

 private:
  void MaybeJoinRoom(std::chrono::steady_clock::time_point now);
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

  static RtcHeadlessSession* instance_;

  CaptureOptions options_;
  std::string rtc_cfg_path_;
  std::atomic<bool> rtc_inited_{false};
  std::atomic<bool> room_joined_{false};
  std::atomic<RtcServerConnectionState> server_state_{ServerDisconnected};
  std::atomic<uint64_t> captured_frames_{0};
  std::atomic<uint64_t> sent_frames_{0};
  std::atomic<uint64_t> remote_video_frames_{0};
  std::chrono::steady_clock::time_point last_join_attempt_{};
  std::chrono::steady_clock::time_point last_status_{};
};

}  // namespace rtc_camera_headless
