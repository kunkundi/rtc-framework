#include "rtc_statistics.h"
#include "log_manager.h"
#include "api/stats/rtcstats_objects.h"

RtcStatistics::RtcStatistics() {

}

RtcStatistics::~RtcStatistics() {
    stats_report_callback_ = nullptr;
}

void RtcStatistics::SetStatisticsReportCallback(const vts_rtc::ChannelNetworkStatsHandler& stats_callback) {
    stats_report_callback_ = stats_callback;
}

void RtcStatistics::SetMediaSsrcVsId(
        std::map<uint32_t, vts_rtc::VideoSourceId>& media_ssrc_vs_id,
        std::map<vts_rtc::VideoSourceId, uint32_t>& media_id_vs_ssrc) {
    media_ssrc_vs_id_.insert(media_ssrc_vs_id.begin(), media_ssrc_vs_id.end());
    media_id_vs_ssrc_.insert(media_id_vs_ssrc.begin(), media_id_vs_ssrc.end());
}

void RtcStatistics::OnStatisticsReport(const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    for(auto ssrc_vs_id: media_ssrc_vs_id_) {
		std::string inbound_str = "RTCOutboundRTPVideoStream_" + std::to_string(ssrc_vs_id.first);
		const webrtc::RTCOutboundRTPStreamStats* media_stats = (const webrtc::RTCOutboundRTPStreamStats*)(report->Get(inbound_str));
		if (media_stats != NULL) {
            net_stats_[ssrc_vs_id.second].input = false;

            if(*media_stats->media_type == "audio") {
                net_stats_[ssrc_vs_id.second].audio_stats.sourceid = ssrc_vs_id.second;
                net_stats_[ssrc_vs_id.second].audio_stats.bitrate_bps = *media_stats->bytes_sent;
            }

            if(*media_stats->media_type == "video") {
                net_stats_[ssrc_vs_id.second].video_stats.sourceid = ssrc_vs_id.second;
                net_stats_[ssrc_vs_id.second].video_stats.width = *media_stats->frame_width;
                net_stats_[ssrc_vs_id.second].video_stats.height = *media_stats->frame_height;
                net_stats_[ssrc_vs_id.second].video_stats.bitrate_bps = *media_stats->bytes_sent;
                net_stats_[ssrc_vs_id.second].video_stats.fps = *media_stats->frames_per_second;
                net_stats_[ssrc_vs_id.second].video_stats.loss_rate = 0;
                net_stats_[ssrc_vs_id.second].video_stats.delay_ms = *media_stats->total_packet_send_delay - net_stats_[ssrc_vs_id.second].video_stats.delay_ms;
                net_stats_[ssrc_vs_id.second].video_stats.key_frame_count = *media_stats->key_frames_encoded;
                net_stats_[ssrc_vs_id.second].video_stats.fir_count = *media_stats->fir_count;
                net_stats_[ssrc_vs_id.second].video_stats.pli_count = *media_stats->pli_count;
                net_stats_[ssrc_vs_id.second].video_stats.nack_count = *media_stats->nack_count;
                net_stats_[ssrc_vs_id.second].video_stats.codec_name = *media_stats->encoder_implementation;
            }
			if(stats_report_callback_) { stats_report_callback_(net_stats_[ssrc_vs_id.second]); }
		}
	}
}
