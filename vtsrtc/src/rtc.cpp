#include "rtc.h"
#include "log_manager.h"
#include <fstream>
#include <nlohmann/json.hpp>

VTS_RTC_NAMESPACE_BEGIN

std::shared_ptr<RtcAgent> RtcAgent::Create(
	const std::string& rtc_config_filepath,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler) {
	LogInst->init();

	// check if content of rtc_config_filepath is valid json format
	nlohmann::json rtc_cfg_obj;
	try {
		std::ifstream ifs(rtc_config_filepath);
		ifs >> rtc_cfg_obj;
	}
	catch (const nlohmann::json::parse_error& exp) {
		LOG_ERROR("Create rtc agent failed, config file (%s) is not valid json", rtc_config_filepath.c_str());
		return nullptr;
	}

	// convert json object to RtcConfig variable
	RtcConfig rtc_config;
	if (!rtc_cfg_obj.contains("api_server")) {
		LOG_ERROR("Create rtc agent failed, config file do not contain api_server field");
		return nullptr;
	}
	rtc_config.api_server_url = rtc_cfg_obj["api_server"].get<std::string>();

	if (!rtc_cfg_obj.contains("signaling_server")) {
		LOG_ERROR("Create rtc agent failed, config file do not contain signaling_server field");
		return nullptr;
	}
	rtc_config.signaling_server_url = rtc_cfg_obj["signaling_server"].get<std::string>();

	rtc_config.SRS_api_server_url = rtc_cfg_obj.contains("SRS_api_server") ?
		rtc_cfg_obj["SRS_api_server"].get<std::string>() : "";

	auto is_LAN = true;
	if (rtc_cfg_obj.contains("LAN")) {
		is_LAN = rtc_cfg_obj["LAN"].get<bool>();
	}

	if (!is_LAN) {
		if (rtc_cfg_obj.contains("ice_servers") && rtc_cfg_obj["ice_servers"].is_array()) {
			auto ice_servers_array = rtc_cfg_obj["ice_servers"].get<json::array_t>();
			for (const auto& ice_server_obj : ice_servers_array) {
				if (ice_server_obj.contains("urls") && ice_server_obj["urls"].is_array()) {
					RtcConfig::IceServer ice_server;
					ice_server.urls = ice_server_obj["urls"].get<std::vector<std::string>>();
					if (ice_server_obj.contains("username") && ice_server_obj.contains("credential")) {
						ice_server.username = ice_server_obj["username"].get<std::string>();
						ice_server.password = ice_server_obj["credential"].get<std::string>();
					}
					rtc_config.ice_servers.emplace_back(ice_server);
				}
			}
		}

		if (rtc_config.ice_servers.size() == 0) {
			RtcConfig::IceServer ice_server;
			ice_server.urls = { "stun:stun.l.google.com:19302" };
			rtc_config.ice_servers.emplace_back(ice_server);
		}
	}

	if (rtc_cfg_obj.contains("use_NVENC")) {
		rtc_config.use_NVENC = rtc_cfg_obj["use_NVENC"].get<bool>();
	}

	if (rtc_cfg_obj.contains("use_NVDEC")) {
		rtc_config.use_NVDEC = rtc_cfg_obj["use_NVDEC"].get<bool>();
	}

	if (rtc_cfg_obj.contains("ping_timeout")) {
		rtc_config.ping_timeout = rtc_cfg_obj["ping_timeout"].get<long>();
	}

	if (rtc_cfg_obj.contains("pong_timeout")) {
		rtc_config.pong_timeout = rtc_cfg_obj["pong_timeout"].get<long>();
	}

	auto rtc_agent = std::shared_ptr<RtcAgent>(
		new RtcAgent(
			rtc_config,
			room_handler,
			user_handler,
			P2P_state_handler,
			SRS_state_handler,
			datachannel_state_handler,
			serverconnection_state_handler,
			recv_msg_handler,
			recv_audioframe_handler,
			recv_frame_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

std::shared_ptr<RtcAgent> RtcAgent::Create(
	const RtcConfig& rtc_config,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler) {
	LogInst->init();

	auto rtc_agent = std::shared_ptr<RtcAgent>(new RtcAgent(
		rtc_config,
		room_handler,
		user_handler,
		P2P_state_handler,
		SRS_state_handler,
		datachannel_state_handler,
		serverconnection_state_handler,
		recv_msg_handler,
		recv_audioframe_handler,
		recv_frame_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

RtcAgent::RtcAgent(
	const RtcConfig& rtc_config,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler) {
	logic_thread_ = rtc::Thread::Create();
	logic_thread_->SetName("logic-thread", nullptr);
	logic_thread_->Start();

	logic_thread_->Invoke<void>(RTC_FROM_HERE,
		[this,
		&rtc_config,
		&room_handler,
		&user_handler,
		&P2P_state_handler,
		&SRS_state_handler,
		&datachannel_state_handler,
		&serverconnection_state_handler,
		&recv_msg_handler,
		&recv_audioframe_handler,
		&recv_frame_handler]() {
			rtc_device_manager_ = std::make_shared<RtcDeviceManager>();
			rtc_conn_manager_ = std::make_unique<RtcConnectionManager>(
				rtc_config,
				rtc_device_manager_,
				room_handler,
				user_handler,
				P2P_state_handler,
				SRS_state_handler,
				datachannel_state_handler,
				serverconnection_state_handler,
				recv_msg_handler,
				recv_audioframe_handler, recv_frame_handler);
		});
}

RtcAgent::~RtcAgent() {
	logic_thread_->Invoke<void>(RTC_FROM_HERE,
		[this]() {
			rtc_device_manager_ = nullptr;
			rtc_conn_manager_ = nullptr;
		});
}

bool RtcAgent::Init() {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this]() {
			return rtc_conn_manager_->Init();
		});
}

VideoDevices RtcAgent::GetVideoDevices() const {
	return logic_thread_->Invoke<VideoDevices>(RTC_FROM_HERE,
		[this]() {
			return rtc_device_manager_->GetVideoDevices();
		});
}

bool RtcAgent::AddDataChannel(const std::string& label,
	PriorityType priority,
	bool ordered,
	int max_retransmits) {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, &label, priority, ordered, max_retransmits]() {
			return rtc_conn_manager_->AddDataChannel(label, priority, ordered, max_retransmits);
		});
}

bool RtcAgent::AddAudioSource(const AudioSourceId& audio_sourceid,
	PriorityType priority) const {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, &audio_sourceid, priority]() {
			return rtc_conn_manager_->AddAudioSource(audio_sourceid, priority);
		});
}

bool RtcAgent::AddVideoSource(size_t device_index, const VideoDeviceCapability& device_capability,
	PriorityType priority) const {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, device_index, &device_capability, priority]() {
			return rtc_device_manager_->AddVideoCapturer(device_index, device_capability, priority);
		});
}

bool RtcAgent::AddVideoSource(const VideoSourceId& video_sourceid, PriorityType priority) const {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, &video_sourceid, priority]() {
			return rtc_conn_manager_->AddVideoSource(video_sourceid, priority);
		});
}

ErrorCode RtcAgent::QueryRoom(const RoomId& roomid, Room& room) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &roomid, &room]() {
			return rtc_conn_manager_->QueryRoom(roomid, room);
		});
}

ErrorCode RtcAgent::QueryRooms(Rooms& rooms) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &rooms]() {
			return rtc_conn_manager_->QueryRooms(rooms);
		});
}

ErrorCode RtcAgent::OpenRoom(const RoomId& roomid,
	enum RoomType room_type, bool force) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &roomid, room_type, force]() {
			return rtc_conn_manager_->OpenRoom(roomid, room_type, force);
		});
}

ErrorCode RtcAgent::CloseRoom(const RoomId& roomid) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &roomid]() {
			return rtc_conn_manager_->CloseRoom(roomid);
		});
}

ErrorCode RtcAgent::JoinRoom(const RoomId& roomid) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &roomid]() {
			return rtc_conn_manager_->JoinRoom(roomid);
		});
}

ErrorCode RtcAgent::LeaveRoom() const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this]() {
			return rtc_conn_manager_->LeaveRoom();
		});
}

SessionIds RtcAgent::QueryRemoteAgents() const {
	return logic_thread_->Invoke<SessionIds>(RTC_FROM_HERE,
		[this]() {
			return rtc_conn_manager_->QueryRemoteAgents();
		});
}

ErrorCode RtcAgent::PublishToSRS(const SRSStreamurl& streamurl) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &streamurl]() {
			return rtc_conn_manager_->PublishToSRS(streamurl);
		});
}

ErrorCode RtcAgent::UnpublishToSRS(const vts_rtc::SRSStreamurl& streamurl) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &streamurl]() {
			return rtc_conn_manager_->UnpublishRtc2SRS(streamurl);
		});
}

ErrorCode RtcAgent::PlayFromSRS(const SRSStreamurl& streamurl) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &streamurl]() {
			return rtc_conn_manager_->PlayFromSRS(streamurl);
		});
}

ErrorCode RtcAgent::UnplayFromSRS(const vts_rtc::SRSStreamurl& streamurl) const {
	return logic_thread_->Invoke<ErrorCode>(RTC_FROM_HERE,
		[this, &streamurl] {
			return rtc_conn_manager_->UnplayFromSRS(streamurl);
		});
}

bool RtcAgent::SendData(SessionId sessionid, const std::string& channel_label,
	const std::string& msg) const {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, sessionid, &channel_label, &msg]() {
			return rtc_conn_manager_->SendData(sessionid, channel_label, msg);
		});
}

bool RtcAgent::BroadcastData(const std::string& channel_label,
	const std::string& msg) const {
	return logic_thread_->Invoke<bool>(RTC_FROM_HERE,
		[this, &channel_label, &msg]() {
			return rtc_conn_manager_->BroadcastData(channel_label, msg);
		});
}

void RtcAgent::SendAudioFrame(const AudioSourceId& audio_sourceid,
	const PCMData& pcmdata) const {
	return logic_thread_->Invoke<void>(RTC_FROM_HERE,
		[this, &audio_sourceid, &pcmdata]() {
			rtc_conn_manager_->SendAudioFrame(audio_sourceid, pcmdata);
		});
}

void RtcAgent::SendFrame(const VideoSourceId& video_sourceid, const YUV420pFrame& video_frame) const {
	return logic_thread_->Invoke<void>(RTC_FROM_HERE,
		[this, &video_sourceid, &video_frame]() {
			rtc_conn_manager_->SendFrame(video_sourceid, video_frame);
		});
}

VTS_RTC_NAMESPACE_END
