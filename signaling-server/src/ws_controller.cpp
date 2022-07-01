#include "ws_controller.h"

WsController::WsController(std::shared_ptr<SimpleWeb::io_context> io_context,
	long client_ping_timeout,
	std::shared_ptr<WsServer> ws_server) :
	io_context_(io_context), client_ping_timeout_(client_ping_timeout) {
	using namespace std::placeholders;

	auto& endpnt = ws_server->endpoint["^/signaling/?$"];
	endpnt.on_open = std::bind(&WsController::OnOpen, this, _1);
	endpnt.on_message = std::bind(&WsController::OnMessage, this, _1, _2);
	endpnt.on_error = std::bind(&WsController::OnError, this, _1, _2);
	endpnt.on_close = std::bind(&WsController::OnClose, this, _1, _2, _3);
}

void WsController::OnOpen(WsConnection conn) {
	LOG_INFO("Websocket onopen, remote peer: [%s]:[%u]",
		conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port());

	auto new_sessionid = sessionid_generator_.Next();
	sessionid_conn_map_[new_sessionid] = conn;

	SteadyTimer pingtimer = std::make_shared<SimpleWeb::asio::steady_timer>(
		io_context_->get_executor(), std::chrono::milliseconds(client_ping_timeout_));
	pingtimer->async_wait(std::bind(&WsController::SetClientPingTimeout, this, std::placeholders::_1, conn));
	conn_pingtimer_map_[conn] = pingtimer;

	json info_obj = {
		{ "command", "take_info" },
		{ "type", "login_succeed" },
		{ "sessionid", new_sessionid }
	};
	conn->send(info_obj.dump());
}

void WsController::OnMessage(WsConnection conn, std::shared_ptr<WsServer::InMessage> in_message) {
	auto msg_json = json::parse(in_message->string(), nullptr, false);
	if (msg_json.is_discarded()) {
		LOG_ERROR("Parse message failed, not vaild json.");
		return;
	}

	if (!msg_json.contains("command")) {
		LOG_ERROR("Message donot contain command field");
		return;
	}

	auto command = msg_json["command"].get<std::string>();

	if (command == "take_heartbeat") {
		// reset expire time when receive ping message
		if (conn_pingtimer_map_.find(conn) != conn_pingtimer_map_.cend()) {
			auto pingtimer = conn_pingtimer_map_[conn];
			try {
				pingtimer->expires_after(std::chrono::milliseconds(client_ping_timeout_));
				pingtimer->async_wait(std::bind(&WsController::SetClientPingTimeout, this, std::placeholders::_1, conn));
			}
			catch (const boost::system::system_error& ec) {
				LOG_ERROR("Call expires_after method of pingtimer failed, reason: %s", ec.what());
			}
		}

 		msg_json["type"] = "pong";
 		conn->send(msg_json.dump());
	}
	else if (command == "take_configuration") {
		if (!msg_json.contains("type")) {
			LOG_ERROR("Message donot contain type field");
			return;
		}

		auto type = msg_json["type"].get<std::string>();

		LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive command [%s], type [%s], message size: %llu",
			conn->remote_endpoint().address().to_string().c_str(),
			conn->remote_endpoint().port(), command.c_str(), type.c_str(), in_message->size());

		if (type == "offer" || type == "answer") {
			auto to_sessionid = msg_json["to"].get<SessionId>();
			if (sessionid_conn_map_.find(to_sessionid) != sessionid_conn_map_.cend()) {
				const auto& to_conn = sessionid_conn_map_[to_sessionid];
				if (type == "offer") {
					msg_json["type"] = "forward_offer";
				}
				else {
					msg_json["type"] = "forward_answer";
				}
				to_conn->send(msg_json.dump());
			}
		}
	}
	else if (command == "take_candidate") {
		LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive command [%s], message size: %llu",
			conn->remote_endpoint().address().to_string().c_str(),
			conn->remote_endpoint().port(), command.c_str(), in_message->size());
		auto to_sessionid = msg_json["to"].get<SessionId>();
		if (sessionid_conn_map_.find(to_sessionid) != sessionid_conn_map_.cend()) {
			const auto& to_conn = sessionid_conn_map_[to_sessionid];
			to_conn->send(msg_json.dump());
		}
	}
	else if (command == "take_info") {
		LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive command [%s], message size: %llu",
			conn->remote_endpoint().address().to_string().c_str(),
			conn->remote_endpoint().port(), command.c_str(), in_message->size());
	}
	else if (command == "take_roominfo") {
		LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive command [%s], message size: %llu",
			conn->remote_endpoint().address().to_string().c_str(),
			conn->remote_endpoint().port(), command.c_str(), in_message->size());
	}
	else
	{
		if (in_message->size() > 300) {
			LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive message size: %llu",
				conn->remote_endpoint().address().to_string().c_str(),
				conn->remote_endpoint().port(), in_message->size());
		}
		else {
			LOG_INFO("Websocket onmessage, remote peer: [%s]:[%u], receive message size: %llu",
				conn->remote_endpoint().address().to_string().c_str(),
				conn->remote_endpoint().port(), in_message->string().c_str());
		}
	}
}

void WsController::OnError(WsConnection conn, const SimpleWeb::error_code& ec) {
	LOG_ERROR("Websocket onerror, remote peer: [%s]:[%u], error value: %d, error message: %s",
		conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(), 
		ec.value(), ec.message().c_str());

	// 10053: A established connection was aborted by the software in your host machine
	// 10054: Connection closed by peer
// 	if (ec.value() == 10053 || ec.value() == 10054) {
// 		this->CloseConnectionAndTimer(conn, false);
// 	}

	this->CloseConnectionAndTimer(conn);
}

void WsController::OnClose(WsConnection conn, int status, const std::string& reason) {
	LOG_INFO("Websocket onclose, remote peer: [%s]:[%u], status value: %d, reason: %s",
		conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port(),
		status, reason.c_str());

	this->CloseConnectionAndTimer(conn);
}

void WsController::SetClientPingTimeout(const SimpleWeb::error_code& ec, WsConnection conn) {
	if (!ec) {
		// exclude SimpleWeb::asio::error::operation_aborted
		LOG_WARN("remote peer: [%s]:[%u] ping timeout, client not available",
			conn->remote_endpoint().address().to_string().c_str(), conn->remote_endpoint().port());

		conn->send_close(1000, "closed by signaling server for ping timeout");
	}
}

bool WsController::IsSessionidExisted(SessionId sessionid, RoomId& roomid) const {
	for (const auto& roomid_room : rooms_) {
		const auto& room_sessionids = roomid_room.second.sessionids;
		auto iter = std::find(room_sessionids.cbegin(), room_sessionids.cend(), sessionid);
		if (iter != room_sessionids.cend()) {
			roomid = roomid_room.first;
			return true;
		}
	}
	return false;
}

void WsController::OpenRoom(Room newroom) {
	rooms_[newroom.roomid] = newroom;
	LOG_WARN("Room [%s] is opened", newroom.roomid.c_str());

	// notify rtc agent
	for (const auto& sessionid_conn : sessionid_conn_map_) {
		json roominfo_obj = {
			{ "command", "take_roominfo" },
			{ "type", "new" },
			{ "roomid", newroom.roomid }
		};
		sessionid_conn.second->send(roominfo_obj.dump());
	}
}

void WsController::CloseRoom(RoomId& roomid) {
	rooms_.erase(roomid);
	LOG_WARN("Room [%s] is closed", roomid.c_str());

	// notify rtc agent
	for (const auto& sessionid_conn : sessionid_conn_map_) {
		json roominfo_obj = {
			{ "command", "take_roominfo" },
			{ "type", "delete" },
			{ "roomid", roomid }
		};
		sessionid_conn.second->send(roominfo_obj.dump());
	}
}

void WsController::LeaveRoom(SessionId sessionid) { 
	RoomId existed_roomid;
	while (this->IsSessionidExisted(sessionid, existed_roomid)) {
		if (rooms_.find(existed_roomid) == rooms_.cend()) {
			LOG_ERROR("Rooms do not contain existed_roomid: %s, it cannot be.", existed_roomid.c_str());
			continue;
		}

		auto& existed_room = rooms_[existed_roomid];
		auto& m_sessionids = existed_room.sessionids;
		auto cnt = std::count(m_sessionids.cbegin(), m_sessionids.cend(), sessionid);
		if (cnt <= 0) {
			LOG_ERROR("Room [%s] must contain sessionid: %d, it cannot be.",
				existed_roomid.c_str(), sessionid);
			continue;
		}

		if (cnt > 1) {
			// just print log
			LOG_ERROR("Room [%s] contains more than one same sessionid: %d, it cannot be.",
				existed_roomid.c_str(), sessionid);
		}

		if ((existed_room.room_type == RoomType::VideoBroadcasting &&
			existed_room.broadcaster_sessionid == sessionid) ||
			m_sessionids.size() - cnt == 0) {
			LOG_WARN("Room [%s] is closed, because%s%s", existed_roomid.c_str(),
				(existed_room.room_type == RoomType::VideoBroadcasting &&
					existed_room.broadcaster_sessionid == sessionid) ? " room type is VideoBroadcasting and this session is the broadcaster" : "",
				(m_sessionids.size() - cnt == 0) ? ", no session in this room" : "");
			rooms_.erase(existed_roomid);

			// notify rtc agent
			for (const auto& sessionid_conn : sessionid_conn_map_) {
				json roominfo_obj = {
					{ "command", "take_roominfo" },
					{ "type", "delete" },
					{ "roomid", existed_roomid }
				};
				sessionid_conn.second->send(roominfo_obj.dump());
			}
		}

		else {
			m_sessionids.erase(std::remove(
				m_sessionids.begin(), m_sessionids.end(), sessionid), m_sessionids.end());
			LOG_WARN("Session [%d] is removed from room [%s], remanent session number [%d]", sessionid, existed_roomid.c_str(), m_sessionids.size());
		}
	}
}

void WsController::CloseConnectionAndTimer(WsConnection conn) {
	// close connection
	for (auto iter = sessionid_conn_map_.begin(); iter != sessionid_conn_map_.end();) {
		if (iter->second == conn) {
			this->LeaveRoom(iter->first);
			LOG_WARN("Sessionid [%d] is removed, due to CloseConnectionAndTimer", iter->first);
			iter = sessionid_conn_map_.erase(iter);
		}
		else {
			++iter;
		}
	}

	// async close pingtimer
	if (conn_pingtimer_map_.find(conn) != conn_pingtimer_map_.cend()) {
		auto pingtimer = conn_pingtimer_map_[conn];
		pingtimer->cancel();
		SimpleWeb::asio::post(pingtimer->get_executor(), [this, pingtimer, conn]() {
			conn_pingtimer_map_.erase(conn);
			});
	}
}
