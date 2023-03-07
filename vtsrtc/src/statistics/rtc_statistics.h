#pragma once

#include "rtc_types.h"
#include "api/stats/rtc_stats_report.h"

class RtcStatistics {
public:
    RtcStatistics();
    ~RtcStatistics();

    void Reset();
    void SetStatisticsReportCallback(const vts_rtc::ChannelNetworkStatsHandler& stats_callback);

	void AddSessionSendersMediaSsrcVsId(vts_rtc::SessionId remote_sessionid, uint32_t ssrc, vts_rtc::VideoSourceId sourceid);
	void AddSessionReceiversMediaSsrcVsId(vts_rtc::SessionId remote_sessionid, vts_rtc::VideoSourceId sourceid, uint32_t ssrc);

	void RemoveSessionMediaSsrcVsId(vts_rtc::SessionId remote_sessionid);

    void OnStatisticsReport(const vts_rtc::SessionId session_id, const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report);

private:
    void ResetHistoryNetStats(vts_rtc::VideoSourceId sourceid);

private:
    vts_rtc::ChannelNetworkStatsHandler stats_report_callback_ = nullptr;
	std::map<uint32_t, vts_rtc::VideoSourceId> sender_media_ssrc_vs_id_;
	std::map<vts_rtc::VideoSourceId, uint32_t> receiver_media_id_vs_ssrc_;

    std::map<vts_rtc::SessionId, std::map<uint32_t, vts_rtc::VideoSourceId>> session_sender_media_ssrc_vs_id_;
    std::map<vts_rtc::SessionId, std::map<vts_rtc::VideoSourceId, uint32_t>> session_receiver_media_id_vs_ssrc_;

    std::map<vts_rtc::VideoSourceId, vts_rtc::NetStats> net_stats_;
};