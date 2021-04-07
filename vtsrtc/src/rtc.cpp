#include "rtc.h"
#include "log_manager.h"
#include <fstream>
#include <nlohmann/json.hpp>

VTS_RTC_NAMESPACE_BEGIN

std::shared_ptr<RtcAgent> RtcAgent::Create(const std::string& rtc_config_filepath, 
	const RecvMessageHandler& recv_msg_handler,
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

	auto rtc_agent = std::shared_ptr<RtcAgent>(new RtcAgent(rtc_config, recv_msg_handler, recv_frame_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

std::shared_ptr<RtcAgent> RtcAgent::Create(const RtcConfig& rtc_config, 
	const RecvMessageHandler& recv_msg_handler,
	const RecvFrameHandler& recv_frame_handler) {
	LogInst->init();

	auto rtc_agent = std::shared_ptr<RtcAgent>(new RtcAgent(rtc_config, recv_msg_handler, recv_frame_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

RtcAgent::RtcAgent(const RtcConfig& rtc_config, 
	const RecvMessageHandler& recv_msg_handler,
	const RecvFrameHandler& recv_frame_handler)
	: rtc_device_manager_{ std::make_shared<RtcDeviceManager>() },
	rtc_conn_manager_{ std::make_unique<RtcConnectionManager>(
		rtc_config, rtc_device_manager_, recv_msg_handler, recv_frame_handler) } {
}

bool RtcAgent::Init() {
	return rtc_conn_manager_->Init();
}

VideoDevices RtcAgent::GetVideoDevices() const {
	return rtc_device_manager_->GetVideoDevices();
}

bool RtcAgent::AddVideoSource(size_t device_index, const VideoDeviceCapability& device_capability) const {
	return rtc_device_manager_->AddVideoCapturer(device_index, device_capability);
}

bool RtcAgent::AddVideoSource(const VideoSourceId& video_sourceid) const {
	return rtc_conn_manager_->AddVideoSource(video_sourceid);
}

RoomCode RtcAgent::QueryRoom(const RoomId& roomid, Room& room) const {
	return rtc_conn_manager_->QueryRoom(roomid, room);
}

RoomCode RtcAgent::QueryRooms(Rooms& rooms) const {
	return rtc_conn_manager_->QueryRooms(rooms);
}

RoomCode RtcAgent::OpenRoom(const RoomId& roomid, enum RoomType room_type) const {
	return rtc_conn_manager_->OpenRoom(roomid, room_type);
}

RoomCode RtcAgent::JoinRoom(const RoomId& roomid) const {
	return rtc_conn_manager_->JoinRoom(roomid);
}

RoomCode RtcAgent::LeaveRoom() const {
	return rtc_conn_manager_->LeaveRoom();
}

SessionIds RtcAgent::QueryRemoteAgents() const {
	return rtc_conn_manager_->QueryRemoteAgents();
}

bool RtcAgent::Send(const std::string& msg, SessionId remote_sessionid) const {
	return rtc_conn_manager_->Send(msg, remote_sessionid);
}

bool RtcAgent::Send(const std::string& msg, const SessionIds & remote_sessionids) const {
	return rtc_conn_manager_->Send(msg, remote_sessionids);
}

bool RtcAgent::Broadcast(const std::string& msg) const {
	return rtc_conn_manager_->Broadcast(msg);
}

void RtcAgent::SendFrame(const VideoSourceId& video_sourceid, const YUV420pFrame& video_frame) const {
	rtc_conn_manager_->OnFrame(video_sourceid, video_frame);
}

VTS_RTC_NAMESPACE_END
