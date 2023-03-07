#include "rtc.h"
#include "log/log_manager.h"
#include <fstream>
#include <nlohmann/json.hpp>

VTS_RTC_NAMESPACE_BEGIN

std::shared_ptr<RtcAgent> RtcAgent::Create(
	const std::string& rtc_config_filepath,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const SRSResponseHandler& SRS_response_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler,
	const ChannelNetworkStatsHandler& channel_network_stats_handler) {

	// check if content of rtc_config_filepath is valid json format
	nlohmann::json rtc_cfg_obj;
	try {
		std::ifstream ifs(rtc_config_filepath);
		ifs >> rtc_cfg_obj;
	}
	catch (const nlohmann::json::parse_error& exp) {
		printf("Create rtc agent failed, config file (%s) is not valid json", rtc_config_filepath.c_str());
		return nullptr;
	}

	if (!rtc_cfg_obj.contains("log_path")) {
		LogInst->init("");
	}
	else
	{
		std::string log_path = rtc_cfg_obj["log_path"].get<std::string>();
		LogInst->init(log_path);
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

	if(rtc_cfg_obj.contains("resolution_limit") && rtc_cfg_obj["resolution_limit"].is_array()) {
		auto resolution_limit_array = rtc_cfg_obj["resolution_limit"].get<json::array_t>();
		for (const auto& resolution_limit_obj : resolution_limit_array) {
			for(const auto& item: resolution_limit_obj.items())
			{
				if(item.value().is_array())
				{
					std::vector<long> resolution;
					for(const auto& res: item.value())
						resolution.push_back(res);
					rtc_config.resolution_limit.insert(std::make_pair(item.key(), std::make_pair(resolution[0], resolution[1])));
				}
			}
		}
	}

	if(rtc_cfg_obj.contains("encode_params") && rtc_cfg_obj["encode_params"].is_array()) {
		auto encode_params_array = rtc_cfg_obj["encode_params"].get<json::array_t>();
		for (const auto& encode_params_obj : encode_params_array) {
			if(encode_params_obj.contains("use_codec_pool")) {
				rtc_config.encode_params.use_codec_pool = encode_params_obj["use_codec_pool"].get<bool>();
			}
			if(encode_params_obj.contains("codecs") && encode_params_obj["codecs"].is_array()) {
				for(const auto& codec_obj: encode_params_obj["codecs"].get<json::array_t>()) {
					auto val = codec_obj.get<unsigned int>();
					rtc_config.encode_params.codecs.push_back(val);
				}
			}
			if(encode_params_obj.contains("qp_range") && encode_params_obj["qp_range"].is_array()) {
				std::vector<unsigned int> qp_range;
				for(const auto& qp_range_obj: encode_params_obj["qp_range"].get<json::array_t>()) {
					auto val = qp_range_obj.get<unsigned int>();
					qp_range.push_back(val);
				}
				rtc_config.encode_params.qp_range = std::make_pair(qp_range[0], qp_range[1]);
			}
			if(encode_params_obj.contains("qp_threshold") && encode_params_obj["qp_threshold"].is_array()) {
				std::vector<unsigned int> qp_threshold;
				for(const auto& qp_threshold_obj: encode_params_obj["qp_threshold"].get<json::array_t>()) {
					auto val = qp_threshold_obj.get<unsigned int>();
					qp_threshold.push_back(val);
				}
				rtc_config.encode_params.qp_threshold = std::make_pair(qp_threshold[0], qp_threshold[1]);
			}
			if(encode_params_obj.contains("I_frame_interval")) {
				rtc_config.encode_params.I_frame_interval = encode_params_obj["I_frame_interval"].get<unsigned int>();
			}
			if(encode_params_obj.contains("bitrate_mode")) {
				rtc_config.encode_params.bitrate_mode = encode_params_obj["bitrate_mode"].get<std::string>();
			}
			if(encode_params_obj.contains("bitrate_maxmum")) {
				rtc_config.encode_params.bitrate_maxmum = encode_params_obj["bitrate_maxmum"].get<unsigned int>();
			}
		}
	}

	if(rtc_cfg_obj.contains("strategy") && rtc_cfg_obj["strategy"].is_array()) {
		auto strategy_array = rtc_cfg_obj["strategy"].get<json::array_t>();
		for (const auto& sub_strategy_obj : strategy_array) {
			if(sub_strategy_obj.contains("use_strategy")) {
				rtc_config.use_strategy = sub_strategy_obj["use_strategy"].get<bool>();
				if(rtc_config.use_strategy) LOG_WARN("Use resolution vs bitrates strategy");
			}
			else {
				for(const auto& item: sub_strategy_obj.items()) {
					if(item.value().is_array()) {
					std::vector<long> sub_strategy;
					for(const auto& bitrate: item.value())
						sub_strategy.push_back(bitrate);
					std::string res = item.key();
					long width = 0;
					long height = 0;
					sscanf(res.c_str(), "%ld*%ld", &width, &height);
					rtc_config.strategy.insert(std::make_pair(width * height, sub_strategy));
					}
				}
			}
		}
	}

	if(rtc_cfg_obj.contains("netstats_report")) {
		rtc_config.netstats_report = rtc_cfg_obj["netstats_report"].get<bool>();
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
	
	if (rtc_cfg_obj.contains("reconnect_interval")) {
		rtc_config.reconnect_interval = rtc_cfg_obj["reconnect_interval"].get<long>();
	}

	auto rtc_agent = std::shared_ptr<RtcAgent>(
		new RtcAgent(
			rtc_config,
			room_handler,
			user_handler,
			P2P_state_handler,
			datachannel_state_handler,
			serverconnection_state_handler,
			SRS_state_handler,
			SRS_response_handler,
			recv_msg_handler,
			recv_audioframe_handler,
			recv_frame_handler,
			channel_network_stats_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

std::shared_ptr<RtcAgent> RtcAgent::Create(
	const RtcConfig& rtc_config,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const SRSResponseHandler& SRS_response_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler,
	const ChannelNetworkStatsHandler& channel_network_stats_handler) {
	LogInst->init("");

	auto rtc_agent = std::shared_ptr<RtcAgent>(new RtcAgent(
		rtc_config,
		room_handler,
		user_handler,
		P2P_state_handler,
		datachannel_state_handler,
		serverconnection_state_handler,
		SRS_state_handler,
		SRS_response_handler,
		recv_msg_handler,
		recv_audioframe_handler,
		recv_frame_handler,
		channel_network_stats_handler));
	return rtc_agent->Init() ? rtc_agent : nullptr;
}

RtcAgent::RtcAgent(
	const RtcConfig& rtc_config,
	const RoomHandler& room_handler,
	const UserHandler& user_handler,
	const P2PStateHandler& P2P_state_handler,
	const DataChannelStateHandler& datachannel_state_handler,
	const ServerConnectionStateHandler& serverconnection_state_handler,
	const SRSStateHandler& SRS_state_handler,
	const SRSResponseHandler& SRS_response_handler,
	const RecvMessageHandler& recv_msg_handler,
	const RecvAudioFrameHandler& recv_audioframe_handler,
	const RecvFrameHandler& recv_frame_handler,
	const ChannelNetworkStatsHandler& channel_network_stats_handler) {
	logic_thread_ = rtc::Thread::Create();
	logic_thread_->SetName("logic-thread", nullptr);
	logic_thread_->Start();

	logic_thread_->Invoke<void>(RTC_FROM_HERE,
		[this,
		&rtc_config,
		&room_handler,
		&user_handler,
		&P2P_state_handler,
		&datachannel_state_handler,
		&serverconnection_state_handler,
		&SRS_state_handler,
		&SRS_response_handler,
		&recv_msg_handler,
		&recv_audioframe_handler,
		&recv_frame_handler,
		&channel_network_stats_handler]() {
			rtc_device_manager_ = std::make_shared<RtcDeviceManager>();
			rtc_conn_manager_ = std::make_shared<RtcConnectionManager>(
				rtc_config,
				rtc_device_manager_,
				room_handler,
				user_handler,
				P2P_state_handler,
				datachannel_state_handler,
				serverconnection_state_handler,
				SRS_state_handler,
				SRS_response_handler,
				recv_msg_handler,
				recv_audioframe_handler,
				recv_frame_handler,
				channel_network_stats_handler);
		});
}

RtcAgent::~RtcAgent() {
	logic_thread_->PostTask(RTC_FROM_HERE, [this]() {
		rtc_device_manager_ = nullptr;
		rtc_conn_manager_ = nullptr;
		});
	logic_thread_->Stop();
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
