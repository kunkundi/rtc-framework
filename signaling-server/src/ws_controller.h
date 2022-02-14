#pragma once

#include "log_manager.h"
#include <nlohmann/json.hpp>
#include <server_ws.hpp>

struct Room;
using RoomId = std::string;
using Rooms = std::map<RoomId, Room>;
using SessionId = unsigned int;
using SessionIds = std::vector<SessionId>;
using WsServer = SimpleWeb::SocketServer<SimpleWeb::WS>;
using json = nlohmann::json;

enum class RoomType {
	VideoBroadcasting = 0,  // one to many
	VideoConference = 1  // many to many
};

struct Room {
	RoomId roomid;
	RoomType room_type;
	SessionIds sessionids;
	SessionId broadcaster_sessionid = -1;
};

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


template <typename T>
class IdGenerator {
public:
	IdGenerator() : id_(0) {
	}

	~IdGenerator() = default;

	T Next() {
		std::lock_guard<std::mutex> lg(mutex_);
		return id_++;
	}

private:
	T id_;
	std::mutex mutex_;
};

class WsController {
	friend class HttpController;
	using WsConnection = std::shared_ptr<WsServer::Connection>;
	using SteadyTimer = std::shared_ptr<SimpleWeb::asio::steady_timer>;

public:
	explicit WsController(std::shared_ptr<SimpleWeb::io_context> io_context,
		long client_ping_timeout,
		std::shared_ptr<WsServer> ws_server);
	~WsController() = default;

private:
	void OnOpen(WsConnection conn);
	void OnMessage(WsConnection conn, std::shared_ptr<WsServer::InMessage> in_message);
	void OnError(WsConnection conn, const SimpleWeb::error_code& error_code);
	void OnClose(WsConnection conn, int status, const std::string& reason);

	void SetClientPingTimeout(const SimpleWeb::error_code& ec, SteadyTimer pingtimer);
	bool IsSessionidExisted(SessionId sessionid, RoomId& roomid) const;
	void OpenRoom(Room newroom);
	void LeaveRoom(SessionId sessionid);
	void CloseRoom(RoomId& oldroom);
	void CloseConnectionAndTimer(WsConnection conn, bool notify_client);
	
private:
	long client_ping_timeout_ = 3000;  // unit: milliseconds
	std::shared_ptr<SimpleWeb::io_context> io_context_;
	std::map<WsConnection, SteadyTimer> conn_pingtimer_map_;

	IdGenerator<SessionId> sessionid_generator_;
	Rooms rooms_;
	std::map<SessionId, WsConnection> sessionid_conn_map_;
};
