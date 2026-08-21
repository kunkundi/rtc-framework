#pragma once

#include <map>
#include <utility>

#include "api/stats/rtc_stats_report.h"
#include "rtc_types.h"

class RtcStatistics {
 public:
  RtcStatistics();
  ~RtcStatistics();

  void Reset();
  void SetStatisticsReportCallback(
      const vts_rtc::ChannelNetworkStatsHandler& stats_callback);

  void AddSessionSendersMediaSsrcVsId(vts_rtc::SessionId remote_sessionid,
                                      uint32_t ssrc,
                                      vts_rtc::VideoSourceId sourceid);
  void AddSessionReceiversMediaSsrcVsId(vts_rtc::SessionId remote_sessionid,
                                        vts_rtc::VideoSourceId sourceid,
                                        uint32_t ssrc);

  void RemoveSessionMediaSsrcVsId(vts_rtc::SessionId remote_sessionid);

  void OnStatisticsReport(
      const vts_rtc::SessionId session_id,
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

 private:
  void ResetHistoryNetStats(vts_rtc::VideoSourceId sourceid);
  void LogNetworkDiagnostics(
      vts_rtc::SessionId session_id,
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

  struct OutboundVideoHistory {
    bool initialized = false;
    uint64_t bytes_sent = 0;
    uint64_t retransmitted_packets_sent = 0;
    uint32_t nack_count = 0;
    uint32_t pli_count = 0;
    int32_t remote_packets_lost = 0;
    int64_t timestamp_us = 0;
  };

 private:
  vts_rtc::ChannelNetworkStatsHandler stats_report_callback_ = nullptr;
  std::map<uint32_t, vts_rtc::VideoSourceId> sender_media_ssrc_vs_id_;
  std::map<vts_rtc::VideoSourceId, uint32_t> receiver_media_id_vs_ssrc_;

  std::map<vts_rtc::SessionId, std::map<uint32_t, vts_rtc::VideoSourceId>>
      session_sender_media_ssrc_vs_id_;
  std::map<vts_rtc::SessionId, std::map<vts_rtc::VideoSourceId, uint32_t>>
      session_receiver_media_id_vs_ssrc_;

  std::map<vts_rtc::VideoSourceId, vts_rtc::NetStats> net_stats_;
  // 存储上一次的 total_packet_send_delay 累积值（秒），用于计算增量
  std::map<vts_rtc::VideoSourceId, double> prev_total_packet_send_delay_;
  // 存储上一次的 jitter_buffer_delay 累积值（秒），用于计算增量平均
  std::map<vts_rtc::VideoSourceId, double> prev_jitter_buffer_delay_;
  // 存储上一次的 jitter_buffer_emitted_count 累积值，用于计算增量平均
  std::map<vts_rtc::VideoSourceId, uint64_t>
      prev_jitter_buffer_emitted_count_;
  std::map<std::pair<vts_rtc::SessionId, uint32_t>, OutboundVideoHistory>
      outbound_video_history_;
  std::map<std::pair<vts_rtc::SessionId, uint32_t>, int64_t>
      last_pacer_diagnosis_us_;
};
