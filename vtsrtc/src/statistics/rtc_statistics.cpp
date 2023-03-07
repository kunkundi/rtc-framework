#include "rtc_statistics.h"
#include "log/log_manager.h"
#include "api/stats/rtcstats_objects.h"
#include "video/call_stats.h"

RtcStatistics::RtcStatistics() {

}

RtcStatistics::~RtcStatistics() {
    stats_report_callback_ = nullptr;
}

void RtcStatistics::Reset() {
    sender_media_ssrc_vs_id_.clear();
	receiver_media_id_vs_ssrc_.clear();
	net_stats_.clear();
}

void RtcStatistics::SetStatisticsReportCallback(const vts_rtc::ChannelNetworkStatsHandler& stats_callback) {
    stats_report_callback_ = stats_callback;
}

void RtcStatistics::AddSessionSendersMediaSsrcVsId(vts_rtc::SessionId remote_sessionid, uint32_t ssrc, vts_rtc::VideoSourceId sourceid) {
	session_sender_media_ssrc_vs_id_[remote_sessionid][ssrc] = sourceid;
}

void RtcStatistics::AddSessionReceiversMediaSsrcVsId(vts_rtc::SessionId remote_sessionid, vts_rtc::VideoSourceId sourceid, uint32_t ssrc) {
	session_receiver_media_id_vs_ssrc_[remote_sessionid][sourceid] = ssrc;
}

void RtcStatistics::RemoveSessionMediaSsrcVsId(vts_rtc::SessionId remote_sessionid) {
	for (auto it = session_sender_media_ssrc_vs_id_.begin(); it != session_sender_media_ssrc_vs_id_.end();) {
		if (it->first == remote_sessionid) {
			for (auto iter = it->second.begin(); iter != it->second.end(); iter++) {
				ResetHistoryNetStats(iter->second);
			}
			session_sender_media_ssrc_vs_id_.erase(it++);
		}
		else
			it++;
	}

	for (auto it = session_receiver_media_id_vs_ssrc_.begin(); it != session_receiver_media_id_vs_ssrc_.end();) {
		if (it->first == remote_sessionid) {
			for (auto iter = it->second.begin(); iter != it->second.end(); iter++) {
				ResetHistoryNetStats(iter->first);
			}
			session_receiver_media_id_vs_ssrc_.erase(it++);
		}
		else
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

void RtcStatistics::OnStatisticsReport(const vts_rtc::SessionId session_id, const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
	for(auto& it: session_sender_media_ssrc_vs_id_)
		for (auto& ssrc_vs_id : it.second) {
			vts_rtc::NetStats net_stats_out;
			net_stats_out.input = false;
			if (net_stats_.find(ssrc_vs_id.second) == net_stats_.end())
			{
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

			std::string outbound_str = "RTCOutboundRTPVideoStream_" + std::to_string(ssrc_vs_id.first);
			const webrtc::RTCOutboundRTPStreamStats* media_stats = (const webrtc::RTCOutboundRTPStreamStats*)(report->Get(outbound_str));
			if (media_stats != NULL) {
				if (*media_stats->media_type == "audio") {
					net_stats_out.audio_stats.sourceid = ssrc_vs_id.second;
					net_stats_out.audio_stats.bitrate_bps = (&media_stats->bytes_sent)->is_defined() ? *media_stats->bytes_sent * 8 : 0;
				}

				if (*media_stats->media_type == "video") {
					net_stats_out.video_stats.sourceid = ssrc_vs_id.second;
					net_stats_out.video_stats.width = (&media_stats->frame_width)->is_defined() ? *media_stats->frame_width : 0;
					net_stats_out.video_stats.height = (&media_stats->frame_height)->is_defined() ? *media_stats->frame_height : 0;
					net_stats_out.video_stats.bitrate_bps = (&media_stats->bytes_sent)->is_defined() ? (*media_stats->bytes_sent - net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps) * 8 : 0;
					net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps = (&media_stats->bytes_sent)->is_defined() ? *media_stats->bytes_sent : 0;
					net_stats_out.video_stats.fps = (&media_stats->frames_per_second)->is_defined() ? *media_stats->frames_per_second : 0;

					auto packets_sent = (&media_stats->packets_sent)->is_defined() ? *media_stats->packets_sent - net_stats_[ssrc_vs_id.second].video_stats.packets_sent : 0;
					auto packets_resent = (&media_stats->retransmitted_packets_sent)->is_defined() ? *media_stats->retransmitted_packets_sent - net_stats_[ssrc_vs_id.second].video_stats.packets_resent : 0;
					net_stats_[ssrc_vs_id.second].video_stats.packets_sent = (&media_stats->packets_sent)->is_defined() ? *media_stats->packets_sent : 0;
					net_stats_[ssrc_vs_id.second].video_stats.packets_resent = (&media_stats->retransmitted_packets_sent)->is_defined() ? *media_stats->retransmitted_packets_sent : 0;
					net_stats_out.video_stats.loss_rate = packets_sent ? ((packets_resent * 100 / packets_sent) > 100 ? 100 : packets_resent * 100 / packets_sent) : 0;

					net_stats_out.video_stats.key_frame_count = (&media_stats->key_frames_encoded)->is_defined() ? *media_stats->key_frames_encoded : 0;
					net_stats_out.video_stats.fir_count = (&media_stats->fir_count)->is_defined() ? *media_stats->fir_count : 0;
					net_stats_out.video_stats.pli_count = (&media_stats->pli_count)->is_defined() ? *media_stats->pli_count : 0;
					net_stats_out.video_stats.nack_count = (&media_stats->nack_count)->is_defined() ? *media_stats->nack_count : 0;
					net_stats_out.video_stats.codec_name = (&media_stats->encoder_implementation)->is_defined() ? *media_stats->encoder_implementation : 0;
				}

				auto transport_id = (&media_stats->transport_id)->is_defined() ? *media_stats->transport_id : 0;
				const webrtc::RTCTransportStats* transport_stats = (const webrtc::RTCTransportStats*)(report->Get(transport_id));
				if (transport_stats != NULL) {
					auto selected_candidate_pair_id = (&transport_stats->selected_candidate_pair_id)->is_defined() ? *transport_stats->selected_candidate_pair_id : 0;
					const webrtc::RTCIceCandidatePairStats* candidate_stats = (const webrtc::RTCIceCandidatePairStats*)(report->Get(selected_candidate_pair_id));
					if (candidate_stats != NULL) {
						net_stats_out.video_stats.delay_ms = (&candidate_stats->current_round_trip_time)->is_defined() ? *candidate_stats->current_round_trip_time / 2 * 1000 : 0;
					}
				}

				if (stats_report_callback_) { stats_report_callback_(session_id, net_stats_out); }
			}
		}

	for(auto& it: session_receiver_media_id_vs_ssrc_)
		for (auto& id_vs_ssrc : it.second) {
			vts_rtc::NetStats net_stats_out;
			net_stats_out.input = true;
			if (net_stats_.find(id_vs_ssrc.first) == net_stats_.end())
			{
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

			std::string inbound_str = "RTCInboundRTPVideoStream_" + std::to_string(id_vs_ssrc.second);
			const webrtc::RTCInboundRTPStreamStats* media_stats = (const webrtc::RTCInboundRTPStreamStats*)(report->Get(inbound_str));
			if (media_stats != NULL) {
				if (*media_stats->kind == "audio") {
					net_stats_out.audio_stats.sourceid = id_vs_ssrc.first;
					net_stats_out.audio_stats.bitrate_bps = (&media_stats->bytes_received)->is_defined() ? *media_stats->bytes_received : 0;
					net_stats_[id_vs_ssrc.first].audio_stats.bitrate_bps = (&media_stats->bytes_received)->is_defined() ? *media_stats->bytes_received * 8 : 0;
				}

				if (*media_stats->kind == "video") {
					net_stats_out.video_stats.sourceid = id_vs_ssrc.first;

					auto bytes_received = (&media_stats->bytes_received)->is_defined() ? *media_stats->bytes_received : 0;

					if (bytes_received == 0)
						continue;

					if (bytes_received > net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps) {
						net_stats_out.video_stats.bitrate_bps = (bytes_received - net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps) * 8;
						net_stats_[id_vs_ssrc.first].video_stats.bitrate_bps = bytes_received;
					}

					auto frames_decoded = (&media_stats->frames_decoded)->is_defined() ? *media_stats->frames_decoded : 0;

					if (frames_decoded == 0)
						continue;

					if (frames_decoded > net_stats_[id_vs_ssrc.first].video_stats.fps) {
						net_stats_out.video_stats.fps = frames_decoded - net_stats_[id_vs_ssrc.first].video_stats.fps;
						net_stats_[id_vs_ssrc.first].video_stats.fps = frames_decoded;
					}

					auto total_packets_received = (&media_stats->packets_received)->is_defined() ? *media_stats->packets_received : 0;
					auto total_packets_lost = (&media_stats->packets_lost)->is_defined() ? *media_stats->packets_lost : 0;

					if (total_packets_received <= 0 || total_packets_lost <= 0)
						continue;
					
					if(total_packets_received > net_stats_[id_vs_ssrc.first].video_stats.packets_received &&
						total_packets_lost > net_stats_[id_vs_ssrc.first].video_stats.packets_lost) {
						auto packets_received = total_packets_received - net_stats_[id_vs_ssrc.first].video_stats.packets_received;
						auto packets_lost = total_packets_lost - net_stats_[id_vs_ssrc.first].video_stats.packets_lost;
						net_stats_[id_vs_ssrc.first].video_stats.packets_received = (&media_stats->packets_received)->is_defined() ? *media_stats->packets_received : 0;
						net_stats_[id_vs_ssrc.first].video_stats.packets_lost = (&media_stats->packets_lost)->is_defined() ? *media_stats->packets_lost : 0;
						net_stats_out.video_stats.loss_rate = packets_received ? ((packets_lost * 100 / packets_received) > 100 ? 100 : packets_lost * 100 / packets_received) : 0;
						net_stats_[id_vs_ssrc.first].video_stats.loss_rate = (&media_stats->packets_lost)->is_defined() ? *media_stats->packets_lost : 0;
					}

					net_stats_out.video_stats.key_frame_count = (&media_stats->key_frames_decoded)->is_defined() ? *media_stats->key_frames_decoded : 0;
					net_stats_out.video_stats.fir_count = (&media_stats->fir_count)->is_defined() ? *media_stats->fir_count : 0;
					net_stats_out.video_stats.pli_count = (&media_stats->pli_count)->is_defined() ? *media_stats->pli_count : 0;
					net_stats_out.video_stats.nack_count = (&media_stats->nack_count)->is_defined() ? *media_stats->nack_count : 0;
					net_stats_out.video_stats.codec_name = (&media_stats->decoder_implementation)->is_defined() ? *media_stats->decoder_implementation : 0;
				}

				auto transport_id = (&media_stats->transport_id)->is_defined() ? *media_stats->transport_id : 0;
				const webrtc::RTCTransportStats* transport_stats = (const webrtc::RTCTransportStats*)(report->Get(transport_id));
				if (transport_stats != NULL) {
					auto selected_candidate_pair_id = (&transport_stats->selected_candidate_pair_id)->is_defined() ? *transport_stats->selected_candidate_pair_id : 0;
					const webrtc::RTCIceCandidatePairStats* candidate_stats = (const webrtc::RTCIceCandidatePairStats*)(report->Get(selected_candidate_pair_id));
					if (candidate_stats != NULL) {
						net_stats_out.video_stats.delay_ms = (&candidate_stats->current_round_trip_time)->is_defined() ? *candidate_stats->current_round_trip_time / 2 * 1000 : 0;
					}
				}

				auto media_stream_id = "RTCMediaStream_" + id_vs_ssrc.first;
				const webrtc::RTCMediaStreamStats* media_stream_stats = (const webrtc::RTCMediaStreamStats*)(report->Get(media_stream_id));
				if (media_stream_stats != NULL) {
					if (media_stream_stats->track_ids->size()) {
						auto media_stream_trackid = (*media_stream_stats->track_ids)[0];
						const webrtc::RTCMediaStreamTrackStats* media_stream_track_stats = (const webrtc::RTCMediaStreamTrackStats*)(report->Get(media_stream_trackid));
						if (media_stream_track_stats != NULL)
						{
							if (*media_stream_track_stats->kind == "audio") {
								net_stats_out.audio_stats.sourceid = id_vs_ssrc.first;
							}

							if (*media_stream_track_stats->kind == "video") {
								net_stats_out.video_stats.sourceid = id_vs_ssrc.first;
								net_stats_out.video_stats.width = (&media_stream_track_stats->frame_width)->is_defined() ? *media_stream_track_stats->frame_width : 0;
								net_stats_out.video_stats.height = (&media_stream_track_stats->frame_height)->is_defined() ? *media_stream_track_stats->frame_height : 0;
							}
						}
					}
				}

				if (stats_report_callback_) { stats_report_callback_(session_id, net_stats_out); }
			}
		}
}
