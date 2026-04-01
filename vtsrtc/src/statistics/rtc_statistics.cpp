#include "rtc_statistics.h"

#include "api/stats/rtcstats_objects.h"
#include "log/log_manager.h"
#include "video/call_stats.h"

RtcStatistics::RtcStatistics() {}

RtcStatistics::~RtcStatistics() { stats_report_callback_ = nullptr; }

void RtcStatistics::Reset() {
  sender_media_ssrc_vs_id_.clear();
  receiver_media_id_vs_ssrc_.clear();
  net_stats_.clear();
  prev_total_packet_send_delay_.clear();
}

void RtcStatistics::SetStatisticsReportCallback(
    const vts_rtc::ChannelNetworkStatsHandler& stats_callback) {
  stats_report_callback_ = stats_callback;
}

void RtcStatistics::AddSessionSendersMediaSsrcVsId(
    vts_rtc::SessionId remote_sessionid, uint32_t ssrc,
    vts_rtc::VideoSourceId sourceid) {
  session_sender_media_ssrc_vs_id_[remote_sessionid][ssrc] = sourceid;
}

void RtcStatistics::AddSessionReceiversMediaSsrcVsId(
    vts_rtc::SessionId remote_sessionid, vts_rtc::VideoSourceId sourceid,
    uint32_t ssrc) {
  session_receiver_media_id_vs_ssrc_[remote_sessionid][sourceid] = ssrc;
}

void RtcStatistics::RemoveSessionMediaSsrcVsId(
    vts_rtc::SessionId remote_sessionid) {
  for (auto it = session_sender_media_ssrc_vs_id_.begin();
       it != session_sender_media_ssrc_vs_id_.end();) {
    if (it->first == remote_sessionid) {
      for (auto iter = it->second.begin(); iter != it->second.end(); iter++) {
        ResetHistoryNetStats(iter->second);
      }
      session_sender_media_ssrc_vs_id_.erase(it++);
    } else
      it++;
  }

  for (auto it = session_receiver_media_id_vs_ssrc_.begin();
       it != session_receiver_media_id_vs_ssrc_.end();) {
    if (it->first == remote_sessionid) {
      for (auto iter = it->second.begin(); iter != it->second.end(); iter++) {
        ResetHistoryNetStats(iter->first);
      }
      session_receiver_media_id_vs_ssrc_.erase(it++);
    } else
      it++;
  }
}

void RtcStatistics::ResetHistoryNetStats(vts_rtc::VideoSourceId sourceid) {
  for (auto it = net_stats_.begin(); it != net_stats_.end();) {
    if (it->first == sourceid)
      net_stats_.erase(it++);
    else
      it++;
  }
}

void RtcStatistics::OnStatisticsReport(
    const vts_rtc::SessionId session_id,
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  for (auto& it : session_sender_media_ssrc_vs_id_)
    for (auto& ssrc_vs_id : it.second) {
      vts_rtc::NetStats net_stats_out;
      net_stats_out.input = false;
      if (net_stats_.find(ssrc_vs_id.second) == net_stats_.end()) {
        net_stats_[ssrc_vs_id.second].input = false;
        net_stats_[ssrc_vs_id.second].audio_stats.sourceid = "";
        net_stats_[ssrc_vs_id.second].audio_stats.bitrate_bps = 0;
        net_stats_[ssrc_vs_id.second].video_stats.sourceid = "";
        net_stats_[ssrc_vs_id.second].video_stats.width = 0;
        net_stats_[ssrc_vs_id.second].video_stats.height = 0;
        net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps = 0;
        net_stats_[ssrc_vs_id.second].video_stats.fps = 0;
        net_stats_[ssrc_vs_id.second].video_stats.loss_rate = 0;
        net_stats_[ssrc_vs_id.second].video_stats.delay_ms = 0;
        net_stats_[ssrc_vs_id.second].video_stats.key_frame_count = 0;
        net_stats_[ssrc_vs_id.second].video_stats.fir_count = 0;
        net_stats_[ssrc_vs_id.second].video_stats.pli_count = 0;
        net_stats_[ssrc_vs_id.second].video_stats.nack_count = 0;
        net_stats_[ssrc_vs_id.second].video_stats.packets_sent = 0;
        net_stats_[ssrc_vs_id.second].video_stats.packets_resent = 0;
        net_stats_[ssrc_vs_id.second].video_stats.packets_received = 0;
        net_stats_[ssrc_vs_id.second].video_stats.packets_lost = 0;
        net_stats_[ssrc_vs_id.second].video_stats.codec_name = "";
      }

      std::string outbound_str =
          "RTCOutboundRTPVideoStream_" + std::to_string(ssrc_vs_id.first);
      const webrtc::RTCOutboundRTPStreamStats* media_stats =
          (const webrtc::RTCOutboundRTPStreamStats*)(report->Get(outbound_str));
      if (media_stats != NULL) {
        // 				if (*media_stats->media_type == "audio")
        // { net_stats_out.audio_stats.sourceid = ssrc_vs_id.second;
        // 					net_stats_out.audio_stats.bitrate_bps
        // =
        // (&media_stats->bytes_sent)->is_defined() ? *media_stats->bytes_sent *
        // 8 : 0;
        // 				}

        if ((&media_stats->media_type)->is_defined() &&
            *media_stats->media_type == "video") {
          net_stats_out.video_stats.sourceid = ssrc_vs_id.second;
          net_stats_out.video_stats.width =
              (&media_stats->frame_width)->is_defined()
                  ? *media_stats->frame_width
                  : 0;
          net_stats_out.video_stats.height =
              (&media_stats->frame_height)->is_defined()
                  ? *media_stats->frame_height
                  : 0;
          net_stats_out.video_stats.bitrate_bps =
              (&media_stats->bytes_sent)->is_defined()
                  ? (*media_stats->bytes_sent -
                     net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps) *
                        8
                  : 0;
          net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps =
              (&media_stats->bytes_sent)->is_defined()
                  ? *media_stats->bytes_sent
                  : 0;
          net_stats_out.video_stats.fps =
              (&media_stats->frames_per_second)->is_defined()
                  ? *media_stats->frames_per_second
                  : 0;

          auto packets_sent =
              (&media_stats->packets_sent)->is_defined()
                  ? *media_stats->packets_sent -
                        net_stats_[ssrc_vs_id.second].video_stats.packets_sent
                  : 0;
          auto packets_resent =
              (&media_stats->retransmitted_packets_sent)->is_defined()
                  ? *media_stats->retransmitted_packets_sent -
                        net_stats_[ssrc_vs_id.second].video_stats.packets_resent
                  : 0;
          net_stats_[ssrc_vs_id.second].video_stats.packets_sent =
              (&media_stats->packets_sent)->is_defined()
                  ? *media_stats->packets_sent
                  : 0;
          net_stats_[ssrc_vs_id.second].video_stats.packets_resent =
              (&media_stats->retransmitted_packets_sent)->is_defined()
                  ? *media_stats->retransmitted_packets_sent
                  : 0;
          net_stats_out.video_stats.loss_rate =
              packets_sent ? ((packets_resent * 100 / packets_sent) > 100
                                  ? 100
                                  : packets_resent * 100 / packets_sent)
                           : 0;

          net_stats_out.video_stats.key_frame_count =
              (&media_stats->key_frames_encoded)->is_defined()
                  ? *media_stats->key_frames_encoded
                  : 0;
          net_stats_out.video_stats.fir_count =
              (&media_stats->fir_count)->is_defined() ? *media_stats->fir_count
                                                      : 0;
          net_stats_out.video_stats.pli_count =
              (&media_stats->pli_count)->is_defined() ? *media_stats->pli_count
                                                      : 0;
          net_stats_out.video_stats.nack_count =
              (&media_stats->nack_count)->is_defined()
                  ? *media_stats->nack_count
                  : 0;
          net_stats_out.video_stats.codec_name =
              (&media_stats->encoder_implementation)->is_defined()
                  ? *media_stats->encoder_implementation
                  : 0;

          // 计算发送端延时统计
          unsigned int network_delay_ms = 0;
          unsigned int encode_time_ms = 0;
          unsigned int packet_send_delay_ms = 0;

          // 获取网络延时（RTT/2）
          bool transport_id_defined =
              (&media_stats->transport_id)->is_defined();
          std::string transport_id_str =
              transport_id_defined ? *media_stats->transport_id : "";
          // 记录候选对 ID，便于后续日志输出诊断信息
          std::string candidate_pair_id;

          if (transport_id_defined && !transport_id_str.empty()) {
            const webrtc::RTCTransportStats* transport_stats =
                (const webrtc::RTCTransportStats*)(report->Get(
                    transport_id_str));
            if (transport_stats != NULL) {
              bool candidate_pair_defined =
                  (&transport_stats->selected_candidate_pair_id)->is_defined();
              candidate_pair_id =
                  candidate_pair_defined
                      ? *transport_stats->selected_candidate_pair_id
                      : "";

              if (candidate_pair_defined && !candidate_pair_id.empty()) {
                const webrtc::RTCIceCandidatePairStats* candidate_stats =
                    (const webrtc::RTCIceCandidatePairStats*)(report->Get(
                        candidate_pair_id));
                if (candidate_stats != NULL) {
                  bool rtt_defined =
                      (&candidate_stats->current_round_trip_time)->is_defined();
                  double rtt_value =
                      rtt_defined ? *candidate_stats->current_round_trip_time
                                  : 0.0;
                  if (rtt_defined && rtt_value > 0) {
                    network_delay_ms =
                        static_cast<unsigned int>(rtt_value / 2 * 1000);
                  }
                }
              }
            }
          }

          // 获取总编码时间（秒转毫秒）
          // 注意：total_encode_time是累积值，需要除以帧数得到平均编码时间
          bool encode_time_defined =
              (&media_stats->total_encode_time)->is_defined();
          double encode_time_value =
              encode_time_defined ? *media_stats->total_encode_time : 0.0;
          uint32_t frames_encoded = (&media_stats->frames_encoded)->is_defined()
                                        ? *media_stats->frames_encoded
                                        : 0;
          if (encode_time_defined && encode_time_value > 0 &&
              frames_encoded > 0) {
            // 计算平均编码时间（每帧）
            encode_time_ms = static_cast<unsigned int>(
                (encode_time_value / frames_encoded) * 1000);
          } else if (encode_time_defined && encode_time_value > 0) {
            // 如果没有帧数信息，直接使用总时间（不准确，但作为参考）
            encode_time_ms =
                static_cast<unsigned int>(encode_time_value * 1000);
          }

          // 获取总包发送延时（秒转毫秒）
          // 注意：total_packet_send_delay是累积值，需要计算增量来得到当前统计周期的延时
          bool packet_delay_defined =
              (&media_stats->total_packet_send_delay)->is_defined();
          double total_packet_delay_value =
              packet_delay_defined ? *media_stats->total_packet_send_delay
                                   : 0.0;
          uint64_t total_packets_sent =
              (&media_stats->packets_sent)->is_defined()
                  ? *media_stats->packets_sent
                  : 0;

          // 获取上一次的累积值
          double prev_total_packet_delay =
              prev_total_packet_send_delay_[ssrc_vs_id.second];
          double packet_delay_increment =
              total_packet_delay_value - prev_total_packet_delay;

          // 使用增量值计算当前统计周期的平均包发送延时
          if (packet_delay_defined && packet_delay_increment > 0 &&
              packets_sent > 0) {
            // 计算本次统计周期的平均包发送延时（每包）
            double avg_packet_delay_seconds =
                packet_delay_increment / packets_sent;
            packet_send_delay_ms =
                static_cast<unsigned int>(avg_packet_delay_seconds * 1000);
            // 保存当前的累积值，用于下次计算增量
            prev_total_packet_send_delay_[ssrc_vs_id.second] =
                total_packet_delay_value;
          } else if (packet_delay_defined && total_packet_delay_value > 0 &&
                     total_packets_sent > 0) {
            // 如果无法计算增量（首次统计或增量无效），使用累积值作为参考
            // 注意：这会导致延时被历史数据稀释，但总比没有好
            double avg_packet_delay_seconds =
                total_packet_delay_value / total_packets_sent;
            packet_send_delay_ms =
                static_cast<unsigned int>(avg_packet_delay_seconds * 1000);
            // 保存当前的累积值，用于下次计算增量
            prev_total_packet_send_delay_[ssrc_vs_id.second] =
                total_packet_delay_value;
          } else if (packet_delay_defined && total_packet_delay_value > 0 &&
                     frames_encoded > 0) {
            // 如果没有包数信息，回退到使用帧数（不准确，但作为参考）
            packet_send_delay_ms = static_cast<unsigned int>(
                (total_packet_delay_value / frames_encoded) * 1000);
            prev_total_packet_send_delay_[ssrc_vs_id.second] =
                total_packet_delay_value;
          } else if (packet_delay_defined && total_packet_delay_value > 0) {
            packet_send_delay_ms =
                static_cast<unsigned int>(total_packet_delay_value * 1000);
            prev_total_packet_send_delay_[ssrc_vs_id.second] =
                total_packet_delay_value;
          }

          // 收集诊断信息用于分析 PacketSendDelay 大的原因
          uint64_t bytes_sent = (&media_stats->bytes_sent)->is_defined()
                                    ? *media_stats->bytes_sent
                                    : 0;
          double target_bitrate = (&media_stats->target_bitrate)->is_defined()
                                      ? *media_stats->target_bitrate
                                      : 0.0;
          double fps = (&media_stats->frames_per_second)->is_defined()
                           ? *media_stats->frames_per_second
                           : 0.0;
          uint32_t frames_sent = (&media_stats->frames_sent)->is_defined()
                                     ? *media_stats->frames_sent
                                     : 0;

          // 计算实际码率（基于已发送字节数）
          unsigned long actual_bitrate_bps =
              net_stats_out.video_stats.bitrate_bps;

          // 计算包发送速率（包/秒）
          double packet_rate = 0.0;
          if (packets_sent > 0) {
            // packets_sent
            // 是增量值（本次统计周期内发送的包数），假设统计周期是1秒
            packet_rate = static_cast<double>(packets_sent);
          }

          // 计算平均包大小（字节）
          double avg_packet_size = 0.0;
          if (total_packets_sent > 0 && bytes_sent > 0) {
            avg_packet_size =
                static_cast<double>(bytes_sent) / total_packets_sent;
          }

          net_stats_out.video_stats.delay_ms =
              network_delay_ms + encode_time_ms + packet_send_delay_ms;

          // 仅对真正有视频流量的源做统计和日志，过滤掉没有发送任何视频帧的“空”统计
          const bool has_video_traffic =
              (net_stats_out.video_stats.bitrate_bps > 0) ||
              (net_stats_out.video_stats.fps > 0) || (frames_sent > 0);
          if (!has_video_traffic) {
            // 没有有效视频数据，跳过该源，避免打印无用的非媒体流统计
            continue;
          }

          LOG_WARN(
              "[VideoStats-Send] SourceID: %s, SessionID: %u, NetworkDelay: %u "
              "ms, EncodeTime: %u ms, PacketSendDelay: %u ms, ReportedDelay: "
              "%u ms",
              net_stats_out.video_stats.sourceid.c_str(), session_id,
              network_delay_ms, encode_time_ms, packet_send_delay_ms,
              net_stats_out.video_stats.delay_ms);

          // 如果 PacketSendDelay 过大，输出详细诊断信息
          if (packet_send_delay_ms > 50) {
            LOG_WARN(
                "[PacerDiagnosis] ========== PacketSendDelay Analysis "
                "==========");
            LOG_WARN(
                "[PacerDiagnosis] SourceID: %s, PacketSendDelay: %u ms "
                "(threshold: 50ms)",
                net_stats_out.video_stats.sourceid.c_str(),
                packet_send_delay_ms);

            // 1. 码率分析
            double bitrate_ratio = 0.0;
            if (target_bitrate > 0 && actual_bitrate_bps > 0) {
              bitrate_ratio = actual_bitrate_bps * 100.0 / target_bitrate;
            }
            LOG_WARN("[PacerDiagnosis] [1] Bitrate Analysis:");
            LOG_WARN("[PacerDiagnosis]   - TargetBitrate: %.0f bps (%.2f Mbps)",
                     target_bitrate, target_bitrate / 1000000.0);
            LOG_WARN("[PacerDiagnosis]   - ActualBitrate: %lu bps (%.2f Mbps)",
                     actual_bitrate_bps, actual_bitrate_bps / 1000000.0);
            LOG_WARN(
                "[PacerDiagnosis]   - BitrateRatio: %.2f%% (100%% = ideal)",
                bitrate_ratio);

            // 2. 包发送分析
            double packets_per_frame = 0.0;
            if (frames_encoded > 0 && total_packets_sent > 0) {
              packets_per_frame =
                  static_cast<double>(total_packets_sent) / frames_encoded;
            }
            LOG_WARN("[PacerDiagnosis] [2] Packet Sending Analysis:");
            LOG_WARN("[PacerDiagnosis]   - TotalPacketsSent: %llu (cumulative)",
                     total_packets_sent);
            LOG_WARN("[PacerDiagnosis]   - PacketsInPeriod: %llu (this period)",
                     packets_sent);
            LOG_WARN(
                "[PacerDiagnosis]   - PacketRate: %.1f pps (packets per "
                "second)",
                packet_rate);
            LOG_WARN("[PacerDiagnosis]   - AvgPacketSize: %.1f bytes",
                     avg_packet_size);
            LOG_WARN("[PacerDiagnosis]   - PacketsPerFrame: %.2f (avg)",
                     packets_per_frame);

            // 3. 帧编码分析
            LOG_WARN("[PacerDiagnosis] [3] Frame Encoding Analysis:");
            LOG_WARN("[PacerDiagnosis]   - FramesEncoded: %u (cumulative)",
                     frames_encoded);
            LOG_WARN("[PacerDiagnosis]   - FramesSent: %u (cumulative)",
                     frames_sent);
            LOG_WARN("[PacerDiagnosis]   - FPS: %.2f", fps);
            LOG_WARN("[PacerDiagnosis]   - EncodeTime: %u ms (avg per frame)",
                     encode_time_ms);

            // 4. 延时组成分析
            LOG_WARN("[PacerDiagnosis] [4] Delay Breakdown:");
            LOG_WARN("[PacerDiagnosis]   - NetworkDelay: %u ms (RTT/2)",
                     network_delay_ms);
            LOG_WARN("[PacerDiagnosis]   - EncodeTime: %u ms", encode_time_ms);
            LOG_WARN(
                "[PacerDiagnosis]   - PacketSendDelay: %u ms (pacer buffer)",
                packet_send_delay_ms);
            LOG_WARN("[PacerDiagnosis]   - TotalDelay: %u ms",
                     net_stats_out.video_stats.delay_ms);

            // 5. 根本原因分析
            LOG_WARN("[PacerDiagnosis] [5] Root Cause Analysis:");
            bool found_cause = false;

            if (target_bitrate > 0 && actual_bitrate_bps > 0) {
              if (bitrate_ratio > 120) {
                LOG_WARN(
                    "[PacerDiagnosis]   - [CAUSE] Actual bitrate (%.0f%%) "
                    "exceeds target bitrate!",
                    bitrate_ratio);
                LOG_WARN(
                    "[PacerDiagnosis]     -> Solution: Reduce encoder bitrate "
                    "or increase target bitrate");
                found_cause = true;
              } else if (bitrate_ratio < 50) {
                LOG_WARN(
                    "[PacerDiagnosis]   - [CAUSE] Actual bitrate (%.0f%%) is "
                    "much lower than target!",
                    bitrate_ratio);
                LOG_WARN(
                    "[PacerDiagnosis]     -> Solution: Increase encoder "
                    "bitrate or check network bandwidth");
                found_cause = true;
              }
            }

            // 检查包发送速率
            if (packet_rate > 0 && avg_packet_size > 0) {
              double estimated_bitrate = packet_rate * avg_packet_size * 8;
              if (target_bitrate > 0 &&
                  estimated_bitrate > target_bitrate * 1.2) {
                LOG_WARN(
                    "[PacerDiagnosis]   - [CAUSE] Packet rate too high: %.1f "
                    "pps, estimated bitrate %.0f bps exceeds target %.0f bps",
                    packet_rate, estimated_bitrate, target_bitrate);
                LOG_WARN(
                    "[PacerDiagnosis]     -> Solution: Reduce packet size or "
                    "frame rate");
                found_cause = true;
              }
            }

            // 检查帧间隔与延时的关系
            if (fps > 0) {
              double frame_interval_ms = 1000.0 / fps;
              if (packet_send_delay_ms > frame_interval_ms * 2) {
                LOG_WARN(
                    "[PacerDiagnosis]   - [CAUSE] PacketSendDelay (%u ms) >> "
                    "frame interval (%.1f ms), pacer queue accumulating!",
                    packet_send_delay_ms, frame_interval_ms);
                LOG_WARN(
                    "[PacerDiagnosis]     -> Solution: Reduce frame rate or "
                    "increase pacer send rate");
                found_cause = true;
              }
            }

            // 检查包大小
            if (avg_packet_size > 1200) {  // MTU 通常是 1500，1200 已经很大
              LOG_WARN(
                  "[PacerDiagnosis]   - [CAUSE] Average packet size (%.1f "
                  "bytes) is very large, may cause fragmentation delay",
                  avg_packet_size);
              LOG_WARN(
                  "[PacerDiagnosis]     -> Solution: Reduce packet size or "
                  "adjust encoder settings");
              found_cause = true;
            }

            // 检查编码时间
            if (encode_time_ms > 20) {  // 编码时间过长
              LOG_WARN(
                  "[PacerDiagnosis]   - [CAUSE] Encode time (%u ms) is high, "
                  "may cause frame delay",
                  encode_time_ms);
              LOG_WARN(
                  "[PacerDiagnosis]     -> Solution: Optimize encoder or "
                  "reduce resolution/quality");
              found_cause = true;
            }

            // 检查每帧包数
            if (packets_per_frame > 10) {
              LOG_WARN(
                  "[PacerDiagnosis]   - [CAUSE] Packets per frame (%.2f) is "
                  "high, large frames may cause pacer buffering",
                  packets_per_frame);
              LOG_WARN(
                  "[PacerDiagnosis]     -> Solution: Reduce frame size or "
                  "increase packet size");
              found_cause = true;
            }

            if (!found_cause) {
              LOG_WARN(
                  "[PacerDiagnosis]   - [UNKNOWN] No obvious cause found, may "
                  "be network congestion or pacer configuration issue");
              LOG_WARN(
                  "[PacerDiagnosis]     -> Solution: Check network conditions, "
                  "adjust pacer settings, or increase initial bitrate");
            }

            LOG_WARN(
                "[PacerDiagnosis] ==========================================");
          }
        }

        if (stats_report_callback_) {
          stats_report_callback_(session_id, net_stats_out);
        }
      }
    }

  for (auto& it : session_receiver_media_id_vs_ssrc_)
    for (auto& id_vs_ssrc : it.second) {
      vts_rtc::NetStats net_stats_out;
      net_stats_out.input = true;
      if (net_stats_.find(id_vs_ssrc.first) == net_stats_.end()) {
        net_stats_[id_vs_ssrc.first].input = true;
        net_stats_[id_vs_ssrc.first].audio_stats.sourceid = "";
        net_stats_[id_vs_ssrc.first].audio_stats.bitrate_bps = 0;
        net_stats_[id_vs_ssrc.first].video_stats.sourceid = "";
        net_stats_[id_vs_ssrc.first].video_stats.width = 0;
        net_stats_[id_vs_ssrc.first].video_stats.height = 0;
        net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps = 0;
        net_stats_[id_vs_ssrc.first].video_stats.fps = 0;
        net_stats_[id_vs_ssrc.first].video_stats.loss_rate = 0;
        net_stats_[id_vs_ssrc.first].video_stats.delay_ms = 0;
        net_stats_[id_vs_ssrc.first].video_stats.key_frame_count = 0;
        net_stats_[id_vs_ssrc.first].video_stats.fir_count = 0;
        net_stats_[id_vs_ssrc.first].video_stats.pli_count = 0;
        net_stats_[id_vs_ssrc.first].video_stats.nack_count = 0;
        net_stats_[id_vs_ssrc.first].video_stats.packets_sent = 0;
        net_stats_[id_vs_ssrc.first].video_stats.packets_resent = 0;
        net_stats_[id_vs_ssrc.first].video_stats.packets_received = 0;
        net_stats_[id_vs_ssrc.first].video_stats.packets_lost = 0;
        net_stats_[id_vs_ssrc.first].video_stats.codec_name = "";
      }

      std::string inbound_str =
          "RTCInboundRTPVideoStream_" + std::to_string(id_vs_ssrc.second);
      const webrtc::RTCInboundRTPStreamStats* media_stats =
          (const webrtc::RTCInboundRTPStreamStats*)(report->Get(inbound_str));
      if (media_stats != NULL) {
        // 				if (*media_stats->kind == "audio") {
        // 					net_stats_out.audio_stats.sourceid
        // = id_vs_ssrc.first;
        // net_stats_out.audio_stats.bitrate_bps =
        // (&media_stats->bytes_received)->is_defined() ?
        // *media_stats->bytes_received : 0;
        // 					net_stats_[id_vs_ssrc.first].audio_stats.bitrate_bps
        // = (&media_stats->bytes_received)->is_defined() ?
        // *media_stats->bytes_received * 8 : 0;
        // 				}

        if ((&media_stats->kind)->is_defined() &&
            *media_stats->kind == "video") {
          net_stats_out.video_stats.sourceid = id_vs_ssrc.first;

          auto bytes_received = (&media_stats->bytes_received)->is_defined()
                                    ? *media_stats->bytes_received
                                    : 0;

          if (bytes_received >
              net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps) {
            net_stats_out.video_stats.bitrate_bps =
                (bytes_received -
                 net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps) *
                8;
            net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps =
                bytes_received;
          }

          auto frames_decoded = (&media_stats->frames_decoded)->is_defined()
                                    ? *media_stats->frames_decoded
                                    : 0;

          if (frames_decoded > net_stats_[id_vs_ssrc.first].video_stats.fps) {
            net_stats_out.video_stats.fps =
                frames_decoded - net_stats_[id_vs_ssrc.first].video_stats.fps;
            net_stats_[id_vs_ssrc.first].video_stats.fps = frames_decoded;
          }

          auto total_packets_received =
              (&media_stats->packets_received)->is_defined()
                  ? *media_stats->packets_received
                  : 0;
          auto total_packets_lost = (&media_stats->packets_lost)->is_defined()
                                        ? *media_stats->packets_lost
                                        : 0;

          if (total_packets_received >
                  net_stats_[id_vs_ssrc.first].video_stats.packets_received &&
              total_packets_lost >
                  net_stats_[id_vs_ssrc.first].video_stats.packets_lost) {
            auto packets_received =
                total_packets_received -
                net_stats_[id_vs_ssrc.first].video_stats.packets_received;
            auto packets_lost =
                total_packets_lost -
                net_stats_[id_vs_ssrc.first].video_stats.packets_lost;
            net_stats_[id_vs_ssrc.first].video_stats.packets_received =
                (&media_stats->packets_received)->is_defined()
                    ? *media_stats->packets_received
                    : 0;
            net_stats_[id_vs_ssrc.first].video_stats.packets_lost =
                (&media_stats->packets_lost)->is_defined()
                    ? *media_stats->packets_lost
                    : 0;
            net_stats_out.video_stats.loss_rate =
                packets_received
                    ? ((packets_lost * 100 / packets_received) > 100
                           ? 100
                           : packets_lost * 100 / packets_received)
                    : 0;
            net_stats_[id_vs_ssrc.first].video_stats.loss_rate =
                (&media_stats->packets_lost)->is_defined()
                    ? *media_stats->packets_lost
                    : 0;
          }

          net_stats_out.video_stats.key_frame_count =
              (&media_stats->key_frames_decoded)->is_defined()
                  ? *media_stats->key_frames_decoded
                  : 0;
          net_stats_out.video_stats.fir_count =
              (&media_stats->fir_count)->is_defined() ? *media_stats->fir_count
                                                      : 0;
          net_stats_out.video_stats.pli_count =
              (&media_stats->pli_count)->is_defined() ? *media_stats->pli_count
                                                      : 0;
          net_stats_out.video_stats.nack_count =
              (&media_stats->nack_count)->is_defined()
                  ? *media_stats->nack_count
                  : 0;
          net_stats_out.video_stats.codec_name =
              (&media_stats->decoder_implementation)->is_defined()
                  ? *media_stats->decoder_implementation
                  : 0;

          // 计算端到端延时统计
          unsigned int network_delay_ms = 0;
          unsigned int jitter_buffer_delay_ms = 0;
          unsigned int total_decode_time_ms = 0;
          unsigned int e2e_delay_ms = 0;

          // 获取网络延时（RTT/2）
          bool transport_id_defined =
              (&media_stats->transport_id)->is_defined();
          std::string transport_id_str =
              transport_id_defined ? *media_stats->transport_id : "";

          if (transport_id_defined && !transport_id_str.empty()) {
            const webrtc::RTCTransportStats* transport_stats =
                (const webrtc::RTCTransportStats*)(report->Get(
                    transport_id_str));
            if (transport_stats != NULL) {
              bool candidate_pair_defined =
                  (&transport_stats->selected_candidate_pair_id)->is_defined();
              std::string candidate_pair_id =
                  candidate_pair_defined
                      ? *transport_stats->selected_candidate_pair_id
                      : "";

              if (candidate_pair_defined && !candidate_pair_id.empty()) {
                const webrtc::RTCIceCandidatePairStats* candidate_stats =
                    (const webrtc::RTCIceCandidatePairStats*)(report->Get(
                        candidate_pair_id));
                if (candidate_stats != NULL) {
                  bool rtt_defined =
                      (&candidate_stats->current_round_trip_time)->is_defined();
                  double rtt_value =
                      rtt_defined ? *candidate_stats->current_round_trip_time
                                  : 0.0;
                  if (rtt_defined && rtt_value > 0) {
                    network_delay_ms =
                        static_cast<unsigned int>(rtt_value / 2 * 1000);
                  }
                }
              }
            }
          }

          // 尝试从RTCMediaStreamTrackStats获取jitter_buffer_delay
          // 根据WebRTC标准，jitter_buffer_delay在RTCMediaStreamTrackStats中
          if ((&media_stats->track_id)->is_defined()) {
            std::string track_id = *media_stats->track_id;
            const webrtc::RTCMediaStreamTrackStats* track_stats =
                (const webrtc::RTCMediaStreamTrackStats*)(report->Get(
                    track_id));
            if (track_stats != NULL) {
              bool jitter_delay_defined =
                  (&track_stats->jitter_buffer_delay)->is_defined();
              double jitter_delay_value =
                  jitter_delay_defined ? *track_stats->jitter_buffer_delay
                                       : 0.0;
              if (jitter_delay_defined && jitter_delay_value > 0) {
                jitter_buffer_delay_ms =
                    static_cast<unsigned int>(jitter_delay_value * 1000);
              }
            }
          }

          // 获取总解码时间（秒转毫秒）
          bool decode_time_defined =
              (&media_stats->total_decode_time)->is_defined();
          double decode_time_value =
              decode_time_defined ? *media_stats->total_decode_time : 0.0;
          if (decode_time_defined && decode_time_value > 0) {
            // total_decode_time是累积值，需要除以帧数得到平均解码时间
            uint32_t frames_decoded =
                (&media_stats->frames_decoded)->is_defined()
                    ? *media_stats->frames_decoded
                    : 0;
            if (frames_decoded > 0) {
              // 使用平均解码时间
              total_decode_time_ms = static_cast<unsigned int>(
                  (decode_time_value / frames_decoded) * 1000);
            } else {
              total_decode_time_ms =
                  static_cast<unsigned int>(decode_time_value * 1000);
            }
          }

          // 计算端到端延时估算
          // 包括：网络延时 + jitter buffer延时 + 解码时间
          // 注意：这仍然不包括采集时间和渲染时间，因为需要video-timing
          // extension
          e2e_delay_ms =
              network_delay_ms + jitter_buffer_delay_ms + total_decode_time_ms;

          // 设置总延时：优先使用jitter
          // buffer延时（最准确），否则使用网络延时+解码时间
          if (jitter_buffer_delay_ms > 0) {
            // jitter buffer延时已经包含了网络传输和缓冲的延时
            net_stats_out.video_stats.delay_ms =
                jitter_buffer_delay_ms + total_decode_time_ms;
          } else if (network_delay_ms > 0) {
            net_stats_out.video_stats.delay_ms =
                network_delay_ms + total_decode_time_ms;
          } else if (total_decode_time_ms > 0) {
            net_stats_out.video_stats.delay_ms = total_decode_time_ms;
          } else {
            net_stats_out.video_stats.delay_ms = 0;
          }

          // 打印接收端延时统计日志
          // 注意：EstimatedE2EDelay 包括网络延时 + jitter buffer + 解码时间
          // 但实际端到端延时还应该包括：发送端采集时间 + 发送端编码时间 +
          // 发送端包发送延时 + 接收端渲染时间
          LOG_INFO(
              "[VideoStats-Recv] SourceID: %s, SessionID: %u, NetworkDelay: %u "
              "ms, JitterBufferDelay: %u ms, DecodeTime: %u ms, "
              "EstimatedE2EDelay: %u ms, ReportedDelay: %u ms",
              net_stats_out.video_stats.sourceid.c_str(), session_id,
              network_delay_ms, jitter_buffer_delay_ms, total_decode_time_ms,
              e2e_delay_ms, net_stats_out.video_stats.delay_ms);
        }

        auto media_stream_id = "RTCMediaStream_" + id_vs_ssrc.first;
        const webrtc::RTCMediaStreamStats* media_stream_stats =
            (const webrtc::RTCMediaStreamStats*)(report->Get(media_stream_id));
        if (media_stream_stats != NULL) {
          if (media_stream_stats->track_ids->size()) {
            auto media_stream_trackid = (*media_stream_stats->track_ids)[0];
            const webrtc::RTCMediaStreamTrackStats* media_stream_track_stats =
                (const webrtc::RTCMediaStreamTrackStats*)(report->Get(
                    media_stream_trackid));
            if (media_stream_track_stats != NULL) {
              // 							if
              // (*media_stream_track_stats->kind == "audio") {
              // 								net_stats_out.audio_stats.sourceid
              // = id_vs_ssrc.first;
              // 							}

              if (*media_stream_track_stats->kind == "video") {
                net_stats_out.video_stats.sourceid = id_vs_ssrc.first;
                net_stats_out.video_stats.width =
                    (&media_stream_track_stats->frame_width)->is_defined()
                        ? *media_stream_track_stats->frame_width
                        : 0;
                net_stats_out.video_stats.height =
                    (&media_stream_track_stats->frame_height)->is_defined()
                        ? *media_stream_track_stats->frame_height
                        : 0;
              }
            }
          }
        }

        if (stats_report_callback_) {
          stats_report_callback_(session_id, net_stats_out);
        }
      }
    }
}
