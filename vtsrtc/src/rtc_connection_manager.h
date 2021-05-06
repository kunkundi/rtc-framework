#pragma once

#include "rtc_types.h"
#include "rtc_connection.h"
#include "rtc_device_manager.h"
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
//#include <rtc_base/ssl_adapter.h>

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

class RtcConnectionManager {
	using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;
	using WsClient = SimpleWeb::SocketClient<SimpleWeb::WS>;
	using WsConnection = std::shared_ptr<WsClient::Connection>;
	using SteadyTimer = std::shared_ptr<SimpleWeb::asio::steady_timer>;

public:
	explicit RtcConnectionManager(const vts_rtc::RtcConfig& rtc_config,
		std::shared_ptr<RtcDeviceManager> device_manager,
		const vts_rtc::RecvMessageHandler& recv_msg_handler,
		const vts_rtc::RecvFrameHandler& recv_frame_handler,
		const vts_rtc::NetworkDisconnectedHandler& network_disconnected_handler);
	~RtcConnectionManager();
	bool Init();

	bool AddDataChannel(const std::string& label, vts_rtc::PriorityType priority,
		bool ordered, int max_retransmits);
	bool AddVideoSource(const vts_rtc::VideoSourceId& video_sourceid, vts_rtc::PriorityType priority);

	vts_rtc::SessionIds QueryRemoteAgents() const;
	vts_rtc::RoomCode QueryRoom(const vts_rtc::RoomId& roomid, vts_rtc::Room& room) const;
	vts_rtc::RoomCode QueryRooms(vts_rtc::Rooms& rooms) const;
	vts_rtc::RoomCode OpenRoom(const vts_rtc::RoomId& roomid, enum vts_rtc::RoomType room_type);
	vts_rtc::RoomCode JoinRoom(const vts_rtc::RoomId& roomid);
	vts_rtc::RoomCode LeaveRoom();

	bool SendData(const std::string& channel_label, const std::string& msg) const;
	void SendFrame(const vts_rtc::VideoSourceId& video_sourceid, const vts_rtc::YUV420pFrame& frame);

private:
	bool InitPeerConnectionFactory();
	void InitWebsocket();
	void SetPingTimeout(const SimpleWeb::error_code& ec);
	void ReconnectWebsocket();
	// @attention: must be called after CreateAnswer on ANSWER side or SetRemoteDescription on OFFER side
	void SetRtpSendersPriority();
	void InteractRemotePeer(vts_rtc::SessionId remote_sessionid, bool offer_peer, const std::string& remote_sdp);
	void AckRemotePeerSdp(vts_rtc::SessionId remote_sessionid, const std::string& remote_sdp);

private:
	// logic_thread_ is created in RtcAgent Constructor method
	// @attention: call some method in logic_thread_ to avoid data synchronization, mainly
	// for InteractRemotePeer method and AckRemotePeerSdp method
	rtc::Thread* logic_thread_ = nullptr;

	const vts_rtc::RtcConfig rtc_config_;
	vts_rtc::RecvMessageHandler recv_msg_handler_ = nullptr;
	vts_rtc::RecvFrameHandler recv_frame_handler_ = nullptr;
	vts_rtc::NetworkDisconnectedHandler network_disconnected_handler_ = nullptr;

	std::shared_ptr<HttpClient> http_client_ = nullptr;
	// @attention: io_context run, stop, get_executor method is thread-safe
	std::shared_ptr<SimpleWeb::io_context> ws_io_context_ = nullptr;
	bool network_disconnected_notified_ = false;
	bool lock_reconnect_ = false;
	SteadyTimer ping_timer_ = nullptr, pong_timer_ = nullptr, reconnect_timer_ = nullptr;
	// @attention: WsClient start() and stop() method is thread-safe
	std::shared_ptr<WsClient> ws_client_ = nullptr;
	std::unique_ptr<rtc::Thread> ws_client_thread_ = nullptr;
	// shared mutex for current_sessionid_ and ws_conn_
	std::mutex cursessionid_wsconn_mtx_;
	std::shared_ptr<vts_rtc::SessionId> current_sessionid_ = nullptr;
	// @attention: WsConnection send() and send_close() is thread-safe
	WsConnection ws_conn_ = nullptr;

	std::shared_ptr<RtcDeviceManager> rtc_device_manager_ = nullptr;
	std::map<vts_rtc::VideoSourceId, rtc::scoped_refptr<RtcExternalFeedTrackSource>> external_feed_tracksources_;
	std::map<rtc::scoped_refptr<webrtc::RtpSenderInterface>, vts_rtc::PriorityType> rtpsender_priority_map_;

	std::map<std::string, webrtc::DataChannelInit> label_datachannelinit_map_;

	std::unique_ptr<webrtc::TaskQueueFactory> adm_taskqueue_;
	rtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_moudle_;
	std::unique_ptr<rtc::Thread> signaling_thread_, worker_thread_, network_thread_;
	rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_conn_factory_;
	std::map<vts_rtc::SessionId, std::shared_ptr<RtcConnection>> remotesessionid_rtcconn_map_;
};
