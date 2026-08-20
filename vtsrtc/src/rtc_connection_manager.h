#pragma once

#include "rtc_types.h"
#include "rtc_connection.h"
#include "rtc_external_audio_device.h"
#include "rtc_device_manager.h"
#include "statistics/rtc_statistics.h"
#include <nlohmann/json.hpp>
#include <client_http.hpp>
#include <client_ws.hpp>
#include <api/task_queue/default_task_queue_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <rtc_base/thread.h>
#include "log/log_webrtc_hook.h"
#include <mutex>

using json = nlohmann::json;

VTS_RTC_NAMESPACE_BEGIN
inline void to_json(json& J, const Room& R) {
	J = {
		{ "roomid", R.roomid },
		{ "room_type", R.room_type },
		{ "sessionids", R.sessionids },
		{ "broadcaster_sessionid", R.broadcaster_sessionid }
	};
}

inline void from_json(const json& J, Room& R) {
	J.at("roomid").get_to(R.roomid);
	J.at("room_type").get_to(R.room_type);
	J.at("sessionids").get_to(R.sessionids);
	J.at("broadcaster_sessionid").get_to(R.broadcaster_sessionid);
}
VTS_RTC_NAMESPACE_END

class RtcExternalFeedTrackSource : public webrtc::VideoTrackSource {
public:
	explicit RtcExternalFeedTrackSource(const std::string& label,
		std::unique_ptr<RtcVideoSource> video_source,
		vts_rtc::PriorityType priority)
		: webrtc::VideoTrackSource(false),
		label_(label),
		video_source_(std::move(video_source)),
		priority_(priority) {}

private:
	rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
		return video_source_.get();
	}

public:
	std::string label_ = "external_feed";
	std::unique_ptr<RtcVideoSource> video_source_;
	vts_rtc::PriorityType priority_ = vts_rtc::PriorityType::Low;
};

class RtcConnectionManager : public std::enable_shared_from_this<RtcConnectionManager> {
	using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;
	using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;
	using WsConnection = std::shared_ptr<WsClient::Connection>;
	using SteadyTimer = std::shared_ptr<SimpleWeb::asio::steady_timer>;

public:
	explicit RtcConnectionManager(const vts_rtc::RtcConfig& rtc_config,
		std::shared_ptr<RtcDeviceManager> device_manager,
		const vts_rtc::RoomHandler& room_handler,
		const vts_rtc::UserHandler& user_handler,
		const vts_rtc::P2PStateHandler& P2P_state_handler,
		const vts_rtc::DataChannelStateHandler& datachannel_state_handler,
		const vts_rtc::ServerConnectionStateHandler& serverconnection_state_handler,
		const vts_rtc::SRSStateHandler& SRS_state_handler,
		const vts_rtc::SRSResponseHandler& SRS_response_handler,
		const vts_rtc::RecvMessageHandler& recv_msg_handler,
		const vts_rtc::RecvAudioFrameHandler& recv_audioframe_handler,
		const vts_rtc::RecvFrameHandler& recv_frame_handler,
		const vts_rtc::ChannelNetworkStatsHandler& channel_network_stats_handler);
	~RtcConnectionManager();
	bool Init();

	bool AddDataChannel(const std::string& label,
		vts_rtc::PriorityType priority, bool ordered, int max_retransmits);
	bool AddAudioSource(const vts_rtc::AudioSourceId& audio_sourceid,
		vts_rtc::PriorityType priority);
	bool AddVideoSource(const vts_rtc::VideoSourceId& video_sourceid,
		vts_rtc::PriorityType priority);

	vts_rtc::SessionIds QueryRemoteAgents() const;
	vts_rtc::ErrorCode QueryRoom(const vts_rtc::RoomId& roomid, vts_rtc::Room& room) const;
	vts_rtc::ErrorCode QueryRooms(vts_rtc::Rooms& rooms) const;
	vts_rtc::ErrorCode OpenRoom(const vts_rtc::RoomId& roomid,
		enum vts_rtc::RoomType room_type, bool force);
	vts_rtc::ErrorCode CloseRoom(const vts_rtc::RoomId& roomid);
	vts_rtc::ErrorCode JoinRoom(const vts_rtc::RoomId& roomid);
	vts_rtc::ErrorCode LeaveRoom();

	vts_rtc::ErrorCode PublishToSRS(const vts_rtc::SRSStreamurl& streamurl);
	vts_rtc::ErrorCode UnpublishRtc2SRS(const vts_rtc::SRSStreamurl& streamurl);
	vts_rtc::ErrorCode PlayFromSRS(const vts_rtc::SRSStreamurl& streamurl);
	vts_rtc::ErrorCode UnplayFromSRS(const vts_rtc::SRSStreamurl& streamurl);

	bool SendData(vts_rtc::SessionId sessionid,
		const std::string& channel_label, const std::string& msg) const;
	bool BroadcastData(const std::string& channel_label,
		const std::string& msg) const;
	bool SendAudioFrame(const vts_rtc::AudioSourceId& audio_sourceid,
		const vts_rtc::PCMData& pcmdata);
	void SendFrame(const vts_rtc::VideoSourceId& video_sourceid,
		const vts_rtc::YUV420pFrame& frame);

private:
	bool InitPeerConnectionFactory();
	void DestroyAllPeerConnection();
	void DestroyPeerConnection(vts_rtc::SessionId remote_sessionid);
	void InitWebsocket();
	void SetPingTimeout(const SimpleWeb::error_code& ec);
	void ReconnectWebsocket();
	// @attention: must be called after CreateAnswer on ANSWER side or SetRemoteDescription on OFFER side
	void SetRtpSendersPriority();
	void AddAudioTrack2PeerConnection(
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn);
	void AddVideoTrack2PeerConnection(
		rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn);
	void InitStatsReport();
	void StatsReport(SteadyTimer steady_timer);
	void InteractRemotePeer(vts_rtc::SessionId remote_sessionid, bool offer_peer, const std::string& remote_sdp);
	void AckRemotePeerSdp(vts_rtc::SessionId remote_sessionid, const std::string& remote_sdp);
    // limit input frame size
	webrtc::VideoFrame BuildAndLimitFrameSize(const vts_rtc::VideoSourceId& video_sourceid, const vts_rtc::YUV420pFrame& frame);

private:
	// logic_thread_ is created in RtcAgent Constructor method
	// @attention: call some method in logic_thread_ to avoid data synchronization, mainly
	// for InteractRemotePeer method and AckRemotePeerSdp method
	rtc::Thread* logic_thread_ = nullptr;

	const vts_rtc::RtcConfig rtc_config_;

	vts_rtc::RoomHandler room_handler_ = nullptr;
	vts_rtc::UserHandler user_handler_ = nullptr;
	vts_rtc::P2PStateHandler P2P_state_handler_ = nullptr;
	vts_rtc::DataChannelStateHandler datachannel_state_handler_ = nullptr;
	vts_rtc::ServerConnectionStateHandler serverconnection_state_handler_ = nullptr;
	vts_rtc::SRSStateHandler SRS_state_handler_ = nullptr;
	vts_rtc::SRSResponseHandler SRS_publish_state_handler_ = nullptr;
	vts_rtc::RecvMessageHandler recv_msg_handler_ = nullptr;
	vts_rtc::RecvAudioFrameHandler recv_audioframe_handler_ = nullptr;
	vts_rtc::RecvFrameHandler recv_frame_handler_ = nullptr;
	vts_rtc::ChannelNetworkStatsHandler channel_network_stats_handler_ = nullptr;

	std::shared_ptr<HttpClient> http_client_ = nullptr;
	std::unique_ptr<HttpClient> SRS_http_client_ = nullptr;
	// @attention: io_context run, stop, get_executor method is thread-safe
	std::shared_ptr<SimpleWeb::io_context> ws_io_context_ = nullptr;
	bool network_disconnected_notified_ = false;
	SteadyTimer ping_timer_ = nullptr, pong_timer_ = nullptr;
	bool lock_reconnect_ = false;
	SteadyTimer reconnect_timer_ = nullptr;
	// @attention: WsClient start() and stop() method is thread-safe
	std::shared_ptr<WsClient> ws_client_ = nullptr;
	std::unique_ptr<rtc::Thread> ws_client_thread_ = nullptr;
	// shared mutex for current_sessionid_ and ws_conn_
	std::mutex cursessionid_wsconn_mtx_;
	std::shared_ptr<vts_rtc::SessionId> current_sessionid_ = nullptr;
	// @attention: WsConnection send() and send_close() is thread-safe
	WsConnection ws_conn_ = nullptr;

	std::shared_ptr<RtcDeviceManager> rtc_device_manager_ = nullptr;
	std::map<vts_rtc::VideoSourceId,
		rtc::scoped_refptr<RtcExternalFeedTrackSource>>
		external_feed_tracksources_;

	std::shared_ptr<RtcStatistics> statistics_collector_ = nullptr;
	std::map<vts_rtc::VideoSourceId, unsigned short> external_feed_tracksources_with_numid_;
	std::map<uint32_t, vts_rtc::VideoSourceId> external_feed_tracksources_ssrc_vs_id_;
	std::map<vts_rtc::VideoSourceId, uint32_t> receiver_tracksources_id_vs_ssrc_;
	rtc::scoped_refptr<RtcChannelStatsObserver> rtc_channel_stats_observer_;
	std::unique_ptr<rtc::Thread> stats_report_thread_ = nullptr;
	SteadyTimer stats_report_timer_ = nullptr;
	std::shared_ptr<SimpleWeb::io_context> stats_report_io_context_ = nullptr;
	bool stats_report_inited_ = false;

	std::map<vts_rtc::AudioSourceId,
		rtc::scoped_refptr<webrtc::AudioSourceInterface>>
		external_audiosources_;
	std::map<rtc::scoped_refptr<webrtc::RtpSenderInterface>,
		vts_rtc::PriorityType> rtpsender_priority_map_;

	std::map<std::string, webrtc::DataChannelInit> label_datachannelinit_map_;

	rtc::scoped_refptr<RtcExternalAudioDeviceModule>
		external_audio_device_module_;
	rtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_moudle_;
	std::unique_ptr<rtc::Thread> signaling_thread_, worker_thread_, network_thread_;
	rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_conn_factory_;
	std::map<vts_rtc::SessionId, std::shared_ptr<RtcConnection>>
		remotesessionid_rtcconn_map_;
	std::vector<std::shared_ptr<Rtc2SRSConnection>>
		SRS_publish_conns_, SRS_play_conns_;
	std::mutex mtx_;

	FileLog* webrtc_log_hook_ = nullptr;
};
