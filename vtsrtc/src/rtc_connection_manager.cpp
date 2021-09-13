#include <cuda.h>

#include "rtc_connection_manager.h"
#include "http_status_code.hpp"
#include "nvh264_encoder_factory.h"
#include "nvh264_decoder_factory.h"

RtcConnectionManager::RtcConnectionManager(const vts_rtc::RtcConfig& rtc_config,
	std::shared_ptr<RtcDeviceManager> device_manager,
	const vts_rtc::RecvMessageHandler& recv_msg_handler,
	const vts_rtc::RecvFrameHandler& recv_frame_handler,
	const vts_rtc::NetworkDisconnectedHandler& network_disconnected_handler)
	: logic_thread_(rtc::Thread::Current()),
	rtc_config_(rtc_config),
	rtc_device_manager_(device_manager),
	recv_msg_handler_(recv_msg_handler),
	recv_frame_handler_(recv_frame_handler),
	network_disconnected_handler_(network_disconnected_handler) {
	http_client_ = std::make_shared<HttpClient>(rtc_config_.api_server_url);
	// 通过优化语句顺序，可以做到不加锁
	ws_io_context_ = std::make_shared<SimpleWeb::io_context>();
}

RtcConnectionManager::~RtcConnectionManager() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	this->LeaveRoom();

	if (worker_thread_) {
		worker_thread_->Invoke<void>(RTC_FROM_HERE,
			[this]() {
				adm_taskqueue_ = nullptr;
				audio_device_moudle_ = nullptr;
			});
	}

	// @attention: io_context stop method is thread-safe
	// @attention: io_context must be destoryed in other thread
	// 不能在ws_client_thread_线程中停止，因为ws_io_context_->run()已经阻塞住ws_client_thread_线程
	if (ws_io_context_) {
		ws_io_context_->stop();
	}

	if (ws_client_thread_) {
		ws_client_thread_->Invoke<void>(RTC_FROM_HERE,
			[this]() {
				ping_timer_ = nullptr;
				pong_timer_ = nullptr;
				reconnect_timer_ = nullptr;
				ws_client_ = nullptr;
			});
	}

	// TO DO
	// @attention: must be called here, otherwise crash (why!!!)
	rtpsender_priority_map_.clear();
}

bool RtcConnectionManager::Init() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (logic_thread_ && InitPeerConnectionFactory()) {
		ws_client_thread_ = rtc::Thread::Create();
		ws_client_thread_->SetName("websocket-client", nullptr);
		ws_client_thread_->Start();

		ws_client_thread_->PostTask(RTC_FROM_HERE,
			[this]() {
				InitWebsocket();
			});

		return true;
	}

	return false;
}

bool RtcConnectionManager::InitPeerConnectionFactory() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	network_thread_ = rtc::Thread::CreateWithSocketServer();
	network_thread_->SetName("network", nullptr);
	network_thread_->Start();

	worker_thread_ = rtc::Thread::Create();
	worker_thread_->SetName("worker", nullptr);
	worker_thread_->Start();

	signaling_thread_ = rtc::Thread::Create();
	signaling_thread_->SetName("signaling", nullptr);
	signaling_thread_->Start();

	// To be improved
	// create dummy AudioDeviceModule for fixing initialization crash of VTS apollo docker
	// @attention: invoke method will block the current thread until execution is complete
	audio_device_moudle_ = worker_thread_->Invoke<rtc::scoped_refptr<webrtc::AudioDeviceModule>>(
		RTC_FROM_HERE,
		[this]() {
			adm_taskqueue_ = webrtc::CreateDefaultTaskQueueFactory();
			return webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::AudioLayer::kDummyAudio, adm_taskqueue_.get());
		});

	bool cuda_device_available = false;
	if (cuInit(0) == CUresult::CUDA_SUCCESS) {
		int num_of_GPUs = 0;
		if (cuDeviceGetCount(&num_of_GPUs) == CUresult::CUDA_SUCCESS &&
			num_of_GPUs > 0) {
			cuda_device_available = true;
		}
	}

	std::unique_ptr<webrtc::VideoEncoderFactory> video_encoder_factory = nullptr;
	std::unique_ptr<webrtc::VideoDecoderFactory> video_decoder_factory = nullptr;
	if (cuda_device_available) {
		LOG_INFO("Cuda device available, use Nvidia H264 video codec");
		video_encoder_factory = std::make_unique<webrtc::NvH264EncoderFactory>();
		video_decoder_factory = std::make_unique<webrtc::NvH264DecoderFactory>();
	}
	else {
		LOG_INFO("Cuda device not available, use builtin video codec");
		video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
		video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
	}

	peer_conn_factory_ = webrtc::CreatePeerConnectionFactory(
		network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
		audio_device_moudle_,
		webrtc::CreateBuiltinAudioEncoderFactory(),
		webrtc::CreateBuiltinAudioDecoderFactory(),
		std::move(video_encoder_factory),
		std::move(video_decoder_factory),
		nullptr, nullptr);

	if (!peer_conn_factory_) {
		// critical error
		LOG_ERROR("[WEBRTC] Create peer connection factory failed");
		return false;
	}

	return true;
}

void RtcConnectionManager::InitWebsocket() {
	RTC_DCHECK_RUN_ON(ws_client_thread_.get());

	ws_client_ = std::make_shared<WsClient>(rtc_config_.signaling_server_url);
	ws_client_->io_service = ws_io_context_;

	ws_client_->on_open = [this](WsConnection conn) {
		LOG_INFO("Websocket onopen, remote peer: %s:%u",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port());
		
		network_disconnected_notified_ = false;

		{
			std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
			ws_conn_ = conn;
		}

		if (reconnect_timer_) {
			reconnect_timer_->cancel();
		}
		lock_reconnect_ = false;

		if (!ping_timer_) {
			ping_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
				ws_io_context_->get_executor(), std::chrono::milliseconds(rtc_config_.ping_timeout));
		}
		else {
			ping_timer_->expires_after(std::chrono::milliseconds(rtc_config_.ping_timeout));
		}
		ping_timer_->async_wait(std::bind(&RtcConnectionManager::SetPingTimeout, this, std::placeholders::_1));
	};

	ws_client_->on_message = [this](WsConnection conn, std::shared_ptr<WsClient::InMessage> in_message) {
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

		// filter heartbeat log info
		if (command != "take_heartbeat") {
			LOG_INFO("Websocket onmessage, remote peer: %s:%u, receive message size: %llu",
				conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(), in_message->size());
		}

		if (command == "take_heartbeat") {
			if (pong_timer_) {
				pong_timer_->cancel();
			}
			ping_timer_->expires_after(std::chrono::milliseconds(rtc_config_.ping_timeout));
			ping_timer_->async_wait(std::bind(&RtcConnectionManager::SetPingTimeout, this, std::placeholders::_1));
		}
		else if (command == "take_info") {
			if (msg_json.contains("type")) {
				auto type = msg_json["type"].get<std::string>();
				if (type == "login_succeed") {
					if (msg_json.contains("sessionid")) {
						auto sessionid = msg_json["sessionid"].get<vts_rtc::SessionId>();
						std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
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

			auto roomid = msg_json["roomid"].get<vts_rtc::RoomId>();
			if (type == "forward_offer") {
				logic_thread_->PostTask(RTC_FROM_HERE,
					[this, from_sessionid, sdp]() {
						this->InteractRemotePeer(from_sessionid, false, sdp);
					});
			}
			else if (type == "forward_answer") {
				logic_thread_->PostTask(RTC_FROM_HERE,
					[this, from_sessionid, sdp]() {
						this->AckRemotePeerSdp(from_sessionid, sdp);
					});
			}
		}
		else if (command == "take_candidate") {
			logic_thread_->PostTask(RTC_FROM_HERE,
				[this, msg_json]() {
					auto from_sessionid = msg_json["from"].get<vts_rtc::SessionId>();
					auto to_sessionid = msg_json["to"].get<vts_rtc::SessionId>();

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
				});
		}
	};
	
	ws_client_->on_error = [this](WsConnection conn, const SimpleWeb::error_code& ec) {
		LOG_ERROR("Websocket onerror, remote peer: %s:%u, error value: %d, error message: %s",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(),
			ec.value(), ec.message().c_str());

		{
			std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
			current_sessionid_ = nullptr;
			ws_conn_ = nullptr;
		}

		ReconnectWebsocket();
	};

	ws_client_->on_close = [this](WsConnection conn, int status, const std::string& reason) {
		LOG_INFO("Websocket onclose, remote peer: %s:%u, status value: %d, reason: %s",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(),
			status, reason.c_str());

		{
			std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
			current_sessionid_ = nullptr;
			ws_conn_ = nullptr;
		}

		ReconnectWebsocket();
	};
	
	ws_client_->start([]() { LOG_INFO("Websocket client is connecting..."); });
	ws_io_context_->run();
}

void RtcConnectionManager::SetPingTimeout(const SimpleWeb::error_code& ec) {
	RTC_DCHECK_RUN_ON(ws_client_thread_.get());

	if (!ec) {
		// exclude SimpleWeb::asio::error::operation_aborted
		json heartbeat_json = {
			{ "command", "take_heartbeat" },
			{ "type", "ping" }
		};
		{
			std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
			ws_conn_->send(heartbeat_json.dump());
		}
		
		
		if (!pong_timer_) {
			pong_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
				ws_io_context_->get_executor(), std::chrono::milliseconds(rtc_config_.pong_timeout));
		}
		else {
			pong_timer_->expires_after(std::chrono::milliseconds(rtc_config_.pong_timeout));
		}
		pong_timer_->async_wait([this](const SimpleWeb::error_code& ec) {
			if (!ec) {
				// this means client is disconnected from websocket server
				LOG_WARN("pong timeout, server not available");
				
				{
					std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
					ws_conn_->send_close(1000, "Rtcagent closed");
					current_sessionid_ = nullptr;
					ws_conn_ = nullptr;
				}

				ReconnectWebsocket();
			}
			});
	}
}

void RtcConnectionManager::ReconnectWebsocket() {
	RTC_DCHECK_RUN_ON(ws_client_thread_.get());

	if (!network_disconnected_notified_) {
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this]() {
				// remove p2p connections when WebSocket disconnected
				remotesessionid_rtcconn_map_.clear();

				if (network_disconnected_handler_) {
					network_disconnected_handler_();
				}
			});

		network_disconnected_notified_ = true;
	}

	if (lock_reconnect_) {
		return;
	}

	if (ping_timer_) {
		ping_timer_->cancel();
	}
	if (pong_timer_) {
		pong_timer_->cancel();
	}

	// stop first
	ws_client_->stop();
	ws_client_->start([]() { LOG_INFO("Websocket client is reconnecting..."); });

	//InitWebsocket();
	lock_reconnect_ = true;
	
	if (!reconnect_timer_) {
		reconnect_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
			ws_io_context_->get_executor(), std::chrono::milliseconds(rtc_config_.reconnect_timeout));
	}
	else {
		reconnect_timer_->expires_after(std::chrono::milliseconds(rtc_config_.reconnect_timeout));
	}
	reconnect_timer_->async_wait([this](const SimpleWeb::error_code& ec) {
		if (!ec) {
			lock_reconnect_ = false;
			ReconnectWebsocket();
		}
		});
}

bool RtcConnectionManager::AddDataChannel(const std::string& label, vts_rtc::PriorityType priority,
	bool ordered, int max_retransmits) {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (label_datachannelinit_map_.find(label) != label_datachannelinit_map_.cend()) {
		return false;
	}

	webrtc::DataChannelInit config;
	config.ordered = ordered;
	// if max_retransmits < 0, it means reliable
	if (max_retransmits >= 0) {
		config.maxRetransmits = max_retransmits;
	}
	config.priority = static_cast<webrtc::Priority>(priority);

	label_datachannelinit_map_[label] = config;

	return true;
}

bool RtcConnectionManager::AddVideoSource(const vts_rtc::VideoSourceId& video_sourceid, vts_rtc::PriorityType priority) {
	RTC_DCHECK_RUN_ON(logic_thread_);
	
	if (external_feed_tracksources_.find(video_sourceid) != external_feed_tracksources_.cend()) {
		return false;
	}

	external_feed_tracksources_[video_sourceid] = 
		new rtc::RefCountedObject<RtcExternalFeedTrackSource>(video_sourceid, std::make_unique<RtcVideoSource>(), priority);
	return true;
}

vts_rtc::SessionIds RtcConnectionManager::QueryRemoteAgents() const {
	RTC_DCHECK_RUN_ON(logic_thread_);

	vts_rtc::SessionIds remote_sessionids;
	remote_sessionids.reserve(remotesessionid_rtcconn_map_.size());
	for (const auto& sessionid_rtcconn : remotesessionid_rtcconn_map_) {
		remote_sessionids.emplace_back(sessionid_rtcconn.first);
	}
	return remote_sessionids;
}

vts_rtc::RoomCode RtcConnectionManager::QueryRoom(const vts_rtc::RoomId& roomid, vts_rtc::Room& room) const {
	RTC_DCHECK_RUN_ON(logic_thread_);
	
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
	RTC_DCHECK_RUN_ON(logic_thread_);
	
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
	RTC_DCHECK_RUN_ON(logic_thread_);

	auto copy_sessionid = -1;
	{
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		if (current_sessionid_) {
			copy_sessionid = *current_sessionid_;
		}
	}

	if (copy_sessionid == -1) {
		LOG_ERROR("Http client cannot open room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		std::map<vts_rtc::RoomType, const char*> roomtype_map = {
			{ vts_rtc::RoomType::VideoBroadcasting, "VideoBroadcasting" },
			{ vts_rtc::RoomType::VideoConference, "VideoConference" }
		};
		LOG_INFO("Http client open room by sessionid: %d, roomid: %s, room type: %s", 
			copy_sessionid, roomid.c_str(), roomtype_map[room_type]);

		json room_obj = {
			{ "sessionid", copy_sessionid },
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
	RTC_DCHECK_RUN_ON(logic_thread_);

	auto copy_sessionid = -1;
	{
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		if (current_sessionid_) {
			copy_sessionid = *current_sessionid_;
		}
	}

	if (copy_sessionid == -1) {
		LOG_ERROR("Http client cannot join room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		LOG_INFO("Http client join room by sessionid: %d, roomid: %s",
			copy_sessionid, roomid.c_str());

		json room_obj = {
			{ "sessionid", copy_sessionid },
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
			auto room = data_obj.get<vts_rtc::Room>();
			switch (room.room_type)
			{
			case vts_rtc::RoomType::VideoBroadcasting:
				this->InteractRemotePeer(room.broadcaster_sessionid, true, "");
				break;
			case vts_rtc::RoomType::VideoConference:
				for (auto sessionid : room.sessionids) {
					if (copy_sessionid != sessionid) {
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
	RTC_DCHECK_RUN_ON(logic_thread_);

	auto copy_sessionid = -1;
	{
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		if (current_sessionid_) {
			copy_sessionid = *current_sessionid_;
		}
	}

	if (copy_sessionid == -1) {
		LOG_ERROR("Http client cannot leave room, agent not logined");
		return vts_rtc::RoomCode::AgentNotLogined;
	}

	try {
		LOG_INFO("Http client leave room by sessionid: %d", copy_sessionid);

		json room_obj = {
			{ "sessionid", copy_sessionid }
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

bool RtcConnectionManager::SendData(const std::string& channel_label, const std::string& msg) const {
	RTC_DCHECK_RUN_ON(logic_thread_);
	
	bool succeed = false;
	for (const auto& sessionid_rtcconn : remotesessionid_rtcconn_map_) {
		succeed |= sessionid_rtcconn.second->SendData(channel_label, msg);
	}

	return succeed;
}

void RtcConnectionManager::SendFrame(const vts_rtc::VideoSourceId& video_sourceid, const vts_rtc::YUV420pFrame& frame) {
	RTC_DCHECK_RUN_ON(logic_thread_);
	
	if (external_feed_tracksources_.find(video_sourceid) != external_feed_tracksources_.cend()) {
		auto I420buffer = webrtc::I420Buffer::Copy(frame.width, frame.height,
			frame.buffer, frame.stride_Y,
			frame.buffer + frame.stride_Y * frame.height, frame.stride_U,
			frame.buffer + frame.stride_Y * frame.height + frame.stride_U * ((frame.height + 1) / 2), 
			frame.stride_V);

		auto inner_frame_builder = webrtc::VideoFrame::Builder()
			.set_video_frame_buffer(I420buffer)
			.set_rotation(webrtc::kVideoRotation_0)
			.set_timestamp_us(rtc::TimeMicros());
		external_feed_tracksources_[video_sourceid]->video_source_->OnFrame(inner_frame_builder.build());
	}
}

void RtcConnectionManager::SetRtpSendersPriority() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	for (const auto& rtpsender_priority : rtpsender_priority_map_) {
		auto& rtpsender = rtpsender_priority.first;
		auto rtpparams = rtpsender->GetParameters();
		if (rtpparams.encodings.size() > 0) {
			std::map<vts_rtc::PriorityType, double> bitrate_priority_map = {
				{ vts_rtc::PriorityType::VeryLow, 0.5 },
				{ vts_rtc::PriorityType::Low, 1.0 },
				{ vts_rtc::PriorityType::Medium, 2.0 },
				{ vts_rtc::PriorityType::High, 4.0 }
			};

			rtpparams.encodings[0].bitrate_priority = bitrate_priority_map[rtpsender_priority.second];
			rtpparams.encodings[0].network_priority = static_cast<webrtc::Priority>(rtpsender_priority.second);
			auto error = rtpsender->SetParameters(rtpparams);
			if (!error.ok()) {
				LOG_WARN("Set priority of rtpsender (%s) failed, reason: %s", rtpsender->id().c_str(), error.message());
			}
		}
		else {
			LOG_WARN("Set priority of rtpsender (%s) failed, reason: encodings empty", rtpsender->id().c_str());
		}
	}
}

void RtcConnectionManager::InteractRemotePeer(vts_rtc::SessionId remote_sessionid, bool offer_peer, const std::string& remote_sdp) {
	RTC_DCHECK_RUN_ON(logic_thread_);

	std::shared_ptr<RtcConnection> rtc_conn = nullptr;
	{
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		rtc_conn = std::make_shared<RtcConnection>(*current_sessionid_, remote_sessionid);
	}

	rtc_conn->on_iceconnect_failed = [this](vts_rtc::SessionId remote_sessionid) {
		LOG_INFO("Reconnect remote peer failed, remote sessionid: %u", remote_sessionid);

		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, remote_sessionid]() {
				// @attention: must run in logic thread, otherwise cannot re-create PeerConnection
				if (remotesessionid_rtcconn_map_.find(remote_sessionid) != remotesessionid_rtcconn_map_.cend()) {
					remotesessionid_rtcconn_map_.erase(remote_sessionid);
				}
			});
	};

	rtc_conn->on_create_sdp_succeed_ = [this, offer_peer](vts_rtc::SessionId remote_sessionid, const std::string& sdp) {
		// @attention: in signaling thread
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		if (current_sessionid_ && ws_conn_) {
			json msg_obj = {
				{ "command", "take_configuration" },
				{ "type", offer_peer ? "offer" : "answer" },
				{ "sdp", sdp },
				{ "from", *current_sessionid_ },
				{ "to", remote_sessionid },
				{ "roomid", "not_needed"}
			};

			ws_conn_->send(msg_obj.dump());
		}
	};

	rtc_conn->on_ice_candidate_received_ = [this](vts_rtc::SessionId remote_sessionid, const std::tuple<std::string, std::string, int>& ice_candidate) {
		// @attention: in signaling thread
		std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
		if (current_sessionid_ && ws_conn_) {
			json msg_obj = {
				{ "command", "take_candidate" },
				{ "candidate", std::get<0>(ice_candidate) },
				{ "sdp_mid", std::get<1>(ice_candidate) },
				{ "sdp_mline_index", std::get<2>(ice_candidate) },
				{ "from", *current_sessionid_ },
				{ "to", remote_sessionid }
			};

			ws_conn_->send(msg_obj.dump());
		}
	};

	rtc_conn->on_dc_message_received_ = recv_msg_handler_;

	rtc_conn->on_frame_received_ = recv_frame_handler_;

	webrtc::PeerConnectionInterface::RTCConfiguration peer_conn_config;
	for (const auto& ice_server : rtc_config_.ice_servers) {
		webrtc::PeerConnectionInterface::IceServer webrtc_ice_server;
		webrtc_ice_server.urls = ice_server.urls;
		webrtc_ice_server.username = ice_server.username;
		webrtc_ice_server.password = ice_server.password;
		peer_conn_config.servers.emplace_back(webrtc_ice_server);
	}
	// TO DO
	//peer_conn_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	webrtc::PeerConnectionDependencies depends(&rtc_conn->peer_conn_observer_);
	rtc_conn->peer_conn_ = peer_conn_factory_->CreatePeerConnection(peer_conn_config, std::move(depends));
	if (!rtc_conn->peer_conn_) {
		LOG_ERROR("Interact remote peer, create peer connection failed");
		return;
	}

	// Add camera capturer video tracks
	if (rtc_device_manager_) {
		auto video_track_sources = rtc_device_manager_->GetVideoTrackSources();
		for (const auto& track_source : video_track_sources) {
			auto video_track = peer_conn_factory_->CreateVideoTrack("track_" + track_source->GetLabel(), track_source.get());
			auto rtpsender_error = rtc_conn->peer_conn_->AddTrack(video_track, { "stream_" + track_source->GetLabel() });			
			if (rtpsender_error.ok()) {
				auto rtpsender = rtpsender_error.value();
				if (rtpsender) {
					rtpsender_priority_map_[rtpsender] = track_source->GetPriority();
				}
			}
			else {
				LOG_ERROR("[WEBRTC] Add track (%s) failed, reason: %s", track_source->GetLabel().c_str(), rtpsender_error.error().message());
			}
		}
	}

	// Add external feed video tracks
	for (const auto& id_tracksource : external_feed_tracksources_) {
		auto track_source = id_tracksource.second;
		auto video_track = peer_conn_factory_->CreateVideoTrack("track_" + track_source->label_, track_source.get());
		auto rtpsender_error = rtc_conn->peer_conn_->AddTrack(video_track, { "stream_" + track_source->label_ });
		if (rtpsender_error.ok()) {
			auto rtpsender = rtpsender_error.value();
			if (rtpsender) {
				rtpsender_priority_map_[rtpsender] = track_source->priority_;
			}
		}
		else {
			LOG_ERROR("[WEBRTC] Add track (%s) failed, reason: %s", track_source->label_.c_str(), rtpsender_error.error().message());
		}
	}

	if (offer_peer) {
		// Add data channels (just for offer side for now)
		for (const auto& label_dcinit : label_datachannelinit_map_) {
			bool succeed = rtc_conn->AddDataChannel(label_dcinit.first, label_dcinit.second);
			if (!succeed) {
				LOG_ERROR("Add data channel (%s) failed", label_dcinit.first.c_str());
			}
		}

		// create offer
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		options.offer_to_receive_audio = 0;
		options.offer_to_receive_video = 1;
		rtc_conn->peer_conn_->CreateOffer(rtc_conn->create_sdp_observer_.get(), options);
	}
	else {
		// set remote offer SDP
		webrtc::SdpParseError error;
		auto remote_session_description = webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, remote_sdp, &error);
		if (!remote_session_description) {
			LOG_ERROR("Interact remote peer, create SDP failed, line: %s, description: %s", error.line.c_str(), error.description.c_str());
			return;
		}
		rtc_conn->peer_conn_->SetRemoteDescription(std::move(remote_session_description), rtc_conn->set_remote_sdp_observer_);

		// create answer
		webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
		options.offer_to_receive_audio = 0;
		options.offer_to_receive_video = 1;
		rtc_conn->peer_conn_->CreateAnswer(rtc_conn->create_sdp_observer_.get(), options);

		this->SetRtpSendersPriority();
	}

	remotesessionid_rtcconn_map_[remote_sessionid] = rtc_conn;
}

void RtcConnectionManager::AckRemotePeerSdp(vts_rtc::SessionId remote_sessionid, const std::string& remote_sdp) {
	RTC_DCHECK_RUN_ON(logic_thread_);
	
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

	this->SetRtpSendersPriority();
}
