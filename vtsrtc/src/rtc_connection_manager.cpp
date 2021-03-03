#include "rtc_connection_manager.h"
#include "http_status_code.hpp"

RtcConnectionManager::RtcConnectionManager(const vts_rtc::RtcConfig& rtc_config, 
	const vts_rtc::RecvMessageHandler& recv_msg_handler, const vts_rtc::RecvFrameHandler& recv_frame_handler)
	: recv_msg_handler_(recv_msg_handler), recv_frame_handler_(recv_frame_handler) {
	http_client_ = std::make_shared<HttpClient>(rtc_config.api_server_url);
	InitWebsocketCallbacks(rtc_config.signaling_server_url);

	for (const auto& ice_server : rtc_config.ice_servers) {
		webrtc::PeerConnectionInterface::IceServer webrtc_ice_server;
		webrtc_ice_server.urls = ice_server.urls;
		webrtc_ice_server.username = ice_server.username;
		webrtc_ice_server.password = ice_server.password;
		rtc_config_.servers.emplace_back(webrtc_ice_server);
	}

	network_thread_ = rtc::Thread::CreateWithSocketServer();
	network_thread_->SetName("network", nullptr);
	network_thread_->Start();

	worker_thread_ = rtc::Thread::Create();
	worker_thread_->SetName("worker", nullptr);
	worker_thread_->Start();

	signaling_thread_ = rtc::Thread::Create();
	signaling_thread_->SetName("signaling", nullptr);
	signaling_thread_->Start();

	peer_conn_factory_ = webrtc::CreatePeerConnectionFactory(
		network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
		nullptr,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		webrtc::CreateBuiltinVideoEncoderFactory(),
		webrtc::CreateBuiltinVideoDecoderFactory(),
		nullptr, nullptr);

	if (!peer_conn_factory_) {
		// critical error
		LOG_ERROR("[WEBRTC] Create peer connection factory failed");
		exit(EXIT_FAILURE);
	}
}

RtcConnectionManager::~RtcConnectionManager() {
	if (ws_conn_) {
		ws_conn_->send_close(1000, "Rtcagent closed");
	}

	if (ws_client_) {
		ws_client_->stop();
		if (ws_client_thread_.joinable()) {
			ws_client_thread_.join();
		}
	}
}

void RtcConnectionManager::SetDeviceManager(std::shared_ptr<RtcDeviceManager> device_manager) {
	rtc_device_manager_ = device_manager;
}

void RtcConnectionManager::AddVideoSource(const vts_rtc::VideoSourceId& video_sourceid) {
	external_feed_tracksources_[video_sourceid] = 
		new rtc::RefCountedObject<RtcExternalFeedTrackSource>(video_sourceid, std::make_unique<RtcVideoSource>());
}

vts_rtc::RoomCode RtcConnectionManager::QueryRoom(const vts_rtc::RoomId& roomid, vts_rtc::Room& room) const {
	try {
		LOG_INFO("Http client query room info by roomid: %s", roomid.c_str());
		auto response = http_client_->request("GET", "/room/" + roomid);
		json result_obj = json::parse(response->content.string(), nullptr, false);
		if (result_obj.is_discarded()) {
			LOG_ERROR("Http client query room info, parse content failed, not valid json");
			return vts_rtc::RoomCode::InternalError;
		}

		auto status = result_obj[HttpStatus::status_field].get<int>();
		auto message = result_obj[HttpStatus::message_field].get<std::string>();
		LOG_INFO("Http client query room info, error code: %d, error message: %s", status, message.c_str());

		if (status == HttpStatus::OK) {
			json data_obj = result_obj[HttpStatus::data_field];
			room = data_obj.get<vts_rtc::Room>();
			return vts_rtc::RoomCode::OK;
		}

		if (status == HttpStatus::RoomNotExisted) {
			return vts_rtc::RoomCode::RoomNotExisted;
		}

		return vts_rtc::RoomCode::InternalError;
	}
	catch (const SimpleWeb::system_error& e) {
		LOG_ERROR("Http client query room info occurs error: %s", e.what());
		return vts_rtc::RoomCode::InternalError;
	}
}

vts_rtc::RoomCode RtcConnectionManager::QueryRooms(vts_rtc::Rooms& rooms) const {
	try {
		LOG_INFO("Http client query rooms");
		auto response = http_client_->request("GET", "/rooms");
		json result_obj = json::parse(response->content.string(), nullptr, false);
		if (result_obj.is_discarded()) {
			LOG_ERROR("Http client query rooms, parse content failed, not valid json");
			return vts_rtc::RoomCode::InternalError;
		}

		auto status = result_obj[HttpStatus::status_field].get<int>();
		auto message = result_obj[HttpStatus::message_field].get<std::string>();
		LOG_INFO("Http client query rooms, error code: %d, error message: %s", status, message.c_str());

		if (status == HttpStatus::OK) {
			json data_obj = result_obj[HttpStatus::data_field];
			rooms = data_obj.get<vts_rtc::Rooms>();
			return vts_rtc::RoomCode::OK;
		}

		return vts_rtc::RoomCode::InternalError;
	}
	catch (const SimpleWeb::system_error& e) {
		LOG_ERROR("Http client query rooms occurs error: %s", e.what());
		return vts_rtc::RoomCode::InternalError;
	}
}

vts_rtc::RoomCode RtcConnectionManager::OpenRoom(const vts_rtc::RoomId& roomid, enum vts_rtc::RoomType room_type) {
	if (!current_sessionid_) {
		LOG_ERROR("Http client cannot open room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		std::unordered_map<vts_rtc::RoomType, const char*> roomtype_map = {
			{ vts_rtc::RoomType::VideoBroadcasting, "VideoBroadcasting" },
			{ vts_rtc::RoomType::VideoConference, "VideoConference" }
		};
		LOG_INFO("Http client open room by sessionid: %u, roomid: %s, room type: %s", 
			*current_sessionid_, roomid.c_str(), roomtype_map[room_type]);

		json room_obj = {
			{ "sessionid", *current_sessionid_ },
			{ "roomid", roomid },
			{ "room_type", room_type }
		};
		auto response = http_client_->request("POST", "/room/open", room_obj.dump());
		json result_obj = json::parse(response->content.string(), nullptr, false);
		if (result_obj.is_discarded()) {
			LOG_ERROR("Http client open room, parse content failed, not valid json");
			return vts_rtc::RoomCode::InternalError;
		}

		auto status = result_obj[HttpStatus::status_field].get<int>();
		auto message = result_obj[HttpStatus::message_field].get<std::string>();
		LOG_INFO("Http client open room, error code: %d, error message: %s", status, message.c_str());

		if (status == HttpStatus::OK) {
			current_roomid_ = std::make_shared<vts_rtc::RoomId>(roomid);
			return vts_rtc::RoomCode::OK;
		}

		if (status == HttpStatus::RoomAlreadyExisted) {
			return vts_rtc::RoomCode::RoomAlreadyExisted;
		}

		if (status == HttpStatus::SessionidAlreadyInRoom) {
			return vts_rtc::RoomCode::AgentAlreadyInRoom;
		}

		return vts_rtc::RoomCode::InternalError;
	}
	catch (const SimpleWeb::system_error& e) {
		LOG_ERROR("Http client open room occurs error: %s", e.what());
		return vts_rtc::RoomCode::InternalError;
	}
}

vts_rtc::RoomCode RtcConnectionManager::JoinRoom(const vts_rtc::RoomId& roomid) {
	if (!current_sessionid_) {
		LOG_ERROR("Http client cannot join room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		LOG_INFO("Http client join room by sessionid: %u, roomid: %s", *current_sessionid_, roomid.c_str());

		json room_obj = {
			{ "sessionid", *current_sessionid_ },
			{ "roomid", roomid }
		};
		auto response = http_client_->request("POST", "/room/join", room_obj.dump());
		json result_obj = json::parse(response->content.string(), nullptr, false);
		if (result_obj.is_discarded()) {
			LOG_ERROR("Http client join room, parse content failed, not valid json");
			return vts_rtc::RoomCode::InternalError;
		}

		auto status = result_obj[HttpStatus::status_field].get<int>();
		auto message = result_obj[HttpStatus::message_field].get<std::string>();
		LOG_INFO("Http client join room, error code: %d, error message: %s", status, message.c_str());

		if (status == HttpStatus::OK) {
			json data_obj = result_obj[HttpStatus::data_field];
			current_roomid_ = std::make_shared<vts_rtc::RoomId>(roomid);
			auto room = data_obj.get<vts_rtc::Room>();
			switch (room.room_type)
			{
			case vts_rtc::RoomType::VideoBroadcasting:
				this->InteractRemotePeer(room.broadcaster_sessionid, true, "");
				break;
			case vts_rtc::RoomType::VideoConference:
				for (auto sessionid : room.sessionids) {
					if (*current_sessionid_ != sessionid) {
						this->InteractRemotePeer(sessionid, true, "");
					}
				}
				break;
			default:
				break;
			}
			return vts_rtc::RoomCode::OK;
		}

		if (status == HttpStatus::RoomNotExisted) {
			return vts_rtc::RoomCode::RoomNotExisted;
		}

		if (status == HttpStatus::SessionidAlreadyInRoom) {
			return vts_rtc::RoomCode::AgentAlreadyInRoom;
		}

		return vts_rtc::RoomCode::InternalError;
	}
	catch (const SimpleWeb::system_error& e) {
		LOG_ERROR("Http client join room occurs error: %s", e.what());
		return vts_rtc::RoomCode::InternalError;
	}
}

vts_rtc::RoomCode RtcConnectionManager::LeaveRoom() {
	if (!current_sessionid_) {
		LOG_ERROR("Http client cannot leave room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		LOG_INFO("Http client leave room by sessionid: %u", *current_sessionid_);

		json room_obj = {
			{ "sessionid", *current_sessionid_ }
		};
		auto response = http_client_->request("POST", "/room/leave", room_obj.dump());
		json result_obj = json::parse(response->content.string(), nullptr, false);
		if (result_obj.is_discarded()) {
			LOG_ERROR("Http client leave room, parse content failed, not valid json");
			return vts_rtc::RoomCode::InternalError;
		}

		auto status = result_obj[HttpStatus::status_field].get<int>();
		auto message = result_obj[HttpStatus::message_field].get<std::string>();
		LOG_INFO("Http client leave room, error code: %d, error message: %s", status, message.c_str());

		if (status == HttpStatus::OK) {
			current_roomid_ = nullptr;
			remotesessionid_rtcconn_map_.clear();

			return vts_rtc::RoomCode::OK;
		}

		return vts_rtc::RoomCode::InternalError;
	}
	catch (const SimpleWeb::system_error& e) {
		LOG_ERROR("Http client leave room occurs error: %s", e.what());
		return vts_rtc::RoomCode::InternalError;
	}
}

vts_rtc::SessionIds RtcConnectionManager::QueryRemoteAgents() const {
	vts_rtc::SessionIds remote_sessionids;
	remote_sessionids.reserve(remotesessionid_rtcconn_map_.size());
	for (const auto& sessionid_rtcconn : remotesessionid_rtcconn_map_) {
		remote_sessionids.emplace_back(sessionid_rtcconn.first);
	}
	return remote_sessionids;
}

bool RtcConnectionManager::Send(const std::string& msg, vts_rtc::SessionId remote_sessionid) const {
	// TO DO
	// It's important to use buffered_amount() and OnBufferedAmountChange to
	// ensure the data channel is used efficiently but without filling this buffer.
	if (remotesessionid_rtcconn_map_.find(remote_sessionid) == remotesessionid_rtcconn_map_.cend()) {
		LOG_ERROR("Send message to remote sessionid: %u, but sessionid not existed", remote_sessionid);
		return false;
	}

	const auto& rtc_conn = remotesessionid_rtcconn_map_.at(remote_sessionid);
	if (rtc_conn->GetDataChannelState() != RtcConnection::DataChannelState::kOpen) {
		LOG_ERROR("Send message to remote sessionid: %u, but data channel not open", remote_sessionid);
		return false;
	}

	rtc_conn->data_channel_->Send(webrtc::DataBuffer(msg));
	return true;
}

bool RtcConnectionManager::Send(const std::string& msg, const vts_rtc::SessionIds& remote_sessionids) const {
	bool succeed = false;
	for (auto remote_sessionid : remote_sessionids) {
		succeed |= this->Send(msg, remote_sessionid);
	}
	return succeed;
}

bool RtcConnectionManager::Broadcast(const std::string& msg) const {
	vts_rtc::SessionIds remote_sessionids;
	for (const auto& kv : remotesessionid_rtcconn_map_) {
		remote_sessionids.emplace_back(kv.first);
	}
	return this->Send(msg, remote_sessionids);
}

void RtcConnectionManager::OnFrame(const vts_rtc::VideoSourceId& video_sourceid, const vts_rtc::YUV420pFrame& frame) {
	if (external_feed_tracksources_.find(video_sourceid) != external_feed_tracksources_.cend()) {
		auto I420buffer = webrtc::I420Buffer::Copy(frame.width, frame.height,
			frame.buffer.data(), frame.stride_Y,
			frame.buffer.data() + frame.stride_Y * frame.height, frame.stride_U,
			frame.buffer.data() + frame.stride_Y * frame.height + frame.stride_U * ((frame.height + 1) / 2), frame.stride_V);
		
		auto duration = std::chrono::system_clock::now().time_since_epoch();
		auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(duration);

		auto inner_frame_builder = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(I420buffer)
			.set_rotation(webrtc::kVideoRotation_0)
			.set_timestamp_us(timestamp_us.count());
		external_feed_tracksources_[video_sourceid]->video_source_->OnFrame(inner_frame_builder.build());
	}
}

void RtcConnectionManager::InitWebsocketCallbacks(const std::string& signaling_server_url) {
	ws_client_ = std::make_shared<WsClient>(signaling_server_url);

	ws_client_->on_open = [this](WsConnection conn) {
		LOG_INFO("Websocket onopen, remote peer: %s:%u", 
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port());

		ws_conn_ = conn;
	};

	ws_client_->on_error = [this](WsConnection conn, const SimpleWeb::error_code& ec) {
		LOG_ERROR("Websocket onerror, remote peer: %s:%u, error value: %d, error message: %s",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(),
			ec.value(), ec.message().c_str());

		// 10053: A established connection was aborted by the software in your host machine
		// 10054: Connection closed by peer
		if (ec.value() == 10053 || ec.value() == 10054) {
			this->HandleWebsocketDisconnected();
		}
	};

	ws_client_->on_close = [this](WsConnection conn, int status, const std::string& reason) {
		LOG_INFO("Websocket onclose, remote peer: %s:%u, status value: %d, reason: %s",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(),
			status, reason.c_str());

		this->HandleWebsocketDisconnected();
	};

	ws_client_->on_message = [this](WsConnection conn, std::shared_ptr<WsClient::InMessage> in_message) {
		LOG_INFO("Websocket onmessage, remote peer: %s:%u, receive message size: %llu",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(), in_message->size());
		
		auto msg_json = json::parse(in_message->string(), nullptr, false);
		if (msg_json.is_discarded()) {
			LOG_ERROR("Websocket onmessage, parse message failed, not vaild json");
			return;
		}

		if (!msg_json.contains("command")) {
			LOG_ERROR("Websocket onmessage, message do not contain command field");
			return;
		}

		auto command = msg_json["command"].get<std::string>();
		if (command == "take_info") {
			if (msg_json.contains("type")) {
				auto type = msg_json["type"].get<std::string>();
				if (type == "login_succeed") {
					if (msg_json.contains("sessionid")) {
						auto sessionid = msg_json["sessionid"].get<vts_rtc::SessionId>();
						current_sessionid_ = std::make_shared<vts_rtc::SessionId>(sessionid);
					}
				}
			}
		}
		else if (command == "take_configuration") {
			if (!msg_json.contains("type")) {
				LOG_ERROR("Websocket onmessage, take configuration message do not contain type field");
				return;
			}

			auto type = msg_json["type"].get<std::string>();
			auto sdp = msg_json["sdp"].get<std::string>();
			auto from_sessionid = msg_json["from"].get<vts_rtc::SessionId>();
			auto to_sessionid = msg_json["to"].get<vts_rtc::SessionId>();
			// just check
			if (!current_sessionid_ || (current_sessionid_ && to_sessionid != *current_sessionid_)) {
				LOG_ERROR("Websocket onmessage, current_sessionid_ is nullptr or to_sessionid != *current_sessionid_");
			}
			
			auto roomid = msg_json["roomid"].get<vts_rtc::RoomId>();
			if (type == "forward_offer") {
				this->InteractRemotePeer(from_sessionid, false, sdp);
			}
			else if (type == "forward_answer") {
				this->AckRemotePeerSdp(from_sessionid, sdp);
			}
		}
		else if (command == "take_candidate") {
			auto from_sessionid = msg_json["from"].get<vts_rtc::SessionId>();
			auto to_sessionid = msg_json["to"].get<vts_rtc::SessionId>();
			// just check
			if (!current_sessionid_ || (current_sessionid_ && to_sessionid != *current_sessionid_)) {
				LOG_ERROR("Websocket onmessage, current_sessionid_ is nullptr or to_sessionid != *current_sessionid_");
			}

			if (remotesessionid_rtcconn_map_.find(from_sessionid) == remotesessionid_rtcconn_map_.cend()) {
				LOG_ERROR("Websocket onmessage, rtc connection of remote_sessionid: %u do not exist when take candidate", from_sessionid);
				return;
			}

			auto rtc_conn = remotesessionid_rtcconn_map_[from_sessionid];
			auto candidate = msg_json["candidate"].get<std::string>();
			auto sdp_mid = msg_json["sdp_mid"].get<std::string>();
			int sdp_mline_index = msg_json["sdp_mline_index"].get<int>();
			webrtc::SdpParseError error;
			auto candidate_object = webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate, &error);
			bool flag = rtc_conn->peer_conn_->AddIceCandidate(candidate_object);
			if (!flag) {
				LOG_ERROR("Websocket onmessage, rtc connection add ice candidate failed");
			}
		}
	};

	ws_client_thread_ = std::thread([this]() {
		ws_client_->start([]() { LOG_INFO("Websocket client is connecting..."); });
		});
}

void RtcConnectionManager::HandleWebsocketDisconnected() {
	// To be improved
	// support websocket reconnection
	this->LeaveRoom();
	current_sessionid_ = nullptr;
}

void RtcConnectionManager::InteractRemotePeer(vts_rtc::SessionId remote_sessionid, bool offer_peer, const std::string& remote_sdp) {
	auto rtc_conn = std::make_shared<RtcConnection>(*current_sessionid_, remote_sessionid);

	rtc_conn->on_connect_peer_failed = [this](vts_rtc::SessionId remote_sessionid) {
		// TO DO
		// Fix bug: cannot re-create PeerConnection when uncomment the following code
		//if (remotesessionid_rtcconn_map_.find(remote_sessionid) != remotesessionid_rtcconn_map_.cend()) {
		//	remotesessionid_rtcconn_map_.erase(remote_sessionid);
		//}
		LOG_INFO("Reconnect remote peer failed, remote sessionid: %u", remote_sessionid);
	};

	rtc_conn->on_create_sdp_succeed_ = [this, offer_peer](vts_rtc::SessionId remote_sessionid, const std::string& sdp) {
		json msg_obj = {
			{ "command", "take_configuration" },
			{ "type", offer_peer ? "offer" : "answer" },
			{ "sdp", sdp },
			{ "from", *current_sessionid_ },
			{ "to", remote_sessionid },
			{ "roomid", *current_roomid_}
		};

		ws_conn_->send(msg_obj.dump());
	};

	rtc_conn->on_ice_candidate_received_ = [this](vts_rtc::SessionId remote_sessionid, const std::tuple<std::string, std::string, int>& ice_candidate) {
		json msg_obj = {
			{ "command", "take_candidate" },
			{ "candidate", std::get<0>(ice_candidate) },
			{ "sdp_mid", std::get<1>(ice_candidate) },
			{ "sdp_mline_index", std::get<2>(ice_candidate) },
			{ "from", *current_sessionid_ },
			{ "to", remote_sessionid }
		};

		ws_conn_->send(msg_obj.dump());
	};

	rtc_conn->on_dc_message_received_ = recv_msg_handler_;

	rtc_conn->on_frame_received_ = recv_frame_handler_;

	rtc_conn->peer_conn_ = peer_conn_factory_->CreatePeerConnection(rtc_config_, nullptr, nullptr, &rtc_conn->peer_conn_observer_);
	if (!rtc_conn->peer_conn_) {
		LOG_ERROR("Interact remote peer, create peer connection failed");
		return;
	}

	// Add camera capturer video tracks
	if (rtc_device_manager_) {
		auto video_track_sources = rtc_device_manager_->GetVideoTrackSources();
		for (const auto& track_source : video_track_sources) {
			auto video_track = peer_conn_factory_->CreateVideoTrack(track_source->GetLabel(), track_source.get());
			rtc_conn->peer_conn_->AddTrack(video_track, { "vts_rtc_stream" });
		}
	}

	// Add external feed video tracks
	for (const auto& id_tracksource : external_feed_tracksources_) {
		auto video_track = peer_conn_factory_->CreateVideoTrack(id_tracksource.second->label_, id_tracksource.second.get());
		rtc_conn->peer_conn_->AddTrack(video_track, { "vts_rtc_stream" });
	}

	if (offer_peer) {
		webrtc::DataChannelInit data_channel_config;
		data_channel_config.ordered = true;
		rtc_conn->data_channel_ = rtc_conn->peer_conn_->CreateDataChannel("rtc_datachannel", &data_channel_config);
		if (!rtc_conn->data_channel_) {
			LOG_ERROR("Interact remote peer, create data channel failed");
			return;
		}
		rtc_conn->data_channel_->RegisterObserver(&rtc_conn->data_channel_observer_);
	}
	else {
		webrtc::SdpParseError error;
		auto remote_session_description = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, remote_sdp, &error);
		if (!remote_session_description) {
			LOG_ERROR("Interact remote peer, create SDP failed, line: %s, description: %s", error.line.c_str(), error.description.c_str());
			return;
		}
		rtc_conn->peer_conn_->SetRemoteDescription(std::move(remote_session_description), rtc_conn->set_remote_sdp_observer_);
	}

	webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
	if (offer_peer) {
		options.offer_to_receive_audio = 1;
		options.offer_to_receive_video = 1;
		rtc_conn->peer_conn_->CreateOffer(rtc_conn->create_sdp_observer_.get(), options);
	}
	else {
		options.offer_to_receive_audio = 1;
		options.offer_to_receive_video = 1;
		rtc_conn->peer_conn_->CreateAnswer(rtc_conn->create_sdp_observer_.get(), options);
	}

	remotesessionid_rtcconn_map_[remote_sessionid] = rtc_conn;
}

void RtcConnectionManager::AckRemotePeerSdp(vts_rtc::SessionId remote_sessionid, const std::string& remote_sdp) {
	if (remotesessionid_rtcconn_map_.find(remote_sessionid) == remotesessionid_rtcconn_map_.cend()) {
		LOG_ERROR("Ack remote peer SDP, rtc connection of remote_sessionid: %u do not exist", remote_sessionid);
		return;
	}

	auto rtc_conn = remotesessionid_rtcconn_map_[remote_sessionid];
	webrtc::SdpParseError error;
	auto remote_session_description = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, remote_sdp, &error);
	if (!remote_session_description) {
		LOG_ERROR("Ack remote peer SDP, create SDP failed, line: %s, description: %s", error.line.c_str(), error.description.c_str());
		remotesessionid_rtcconn_map_.erase(remote_sessionid);
		return;
	}
	rtc_conn->peer_conn_->SetRemoteDescription(std::move(remote_session_description), rtc_conn->set_remote_sdp_observer_);
}
