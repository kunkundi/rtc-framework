#pragma once

#include "rtc_types.h"
#include "api/stats/rtc_stats_report.h"

class RtcStatistics {
public:
    RtcStatistics();
    ~RtcStatistics();

    void SetStatisticsReportCallback(const vts_rtc::ChannelNetworkStatsHandler& stats_callback);
    void SetSendersMediaSsrcVsId(std::map<uint32_t, vts_rtc::VideoSourceId>& sender_media_ssrc_vs_id);
	void SetReceiversMediaSsrcVsId(std::map<vts_rtc::VideoSourceId, uint32_t>& receiver_media_id_vs_ssrc);
    void OnStatisticsReport(const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

private:
    vts_rtc::ChannelNetworkStatsHandler stats_report_callback_ = nullptr;
	std::map<uint32_t, vts_rtc::VideoSourceId> sender_media_ssrc_vs_id_;
	std::map<vts_rtc::VideoSourceId, uint32_t> receiver_media_id_vs_ssrc_;

    std::map<vts_rtc::VideoSourceId, vts_rtc::NetStats> net_stats_;
};