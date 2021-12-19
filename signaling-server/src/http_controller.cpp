#include "http_controller.h"
#include <boost/filesystem.hpp>

#define LOG_REQUEST_INFO(X) LOG_INFO("Http %s, remote peer: %s:%u, method: %s, path: %s, query string: %s, http version: %s", \
	X, request->remote_endpoint().address().to_string().c_str(), request->remote_endpoint().port(), \
	request->method.c_str(), request->path.c_str(), request->query_string.c_str(), request->http_version.c_str()); \

HttpController::HttpController(std::shared_ptr<HttpServer> http_server, std::shared_ptr<WsController> ws_ctrl)
	: ws_ctrl_(ws_ctrl) {
	using namespace std::placeholders;

	http_server->default_resource["GET"] = std::bind(&HttpController::QueryDefaultResource, this, _1, _2);
	
	http_server->resource["/room/(.*)"]["GET"] = std::bind(&HttpController::QueryRoom, this, _1, _2);
	http_server->resource["/rooms"]["GET"] = std::bind(&HttpController::QueryRooms, this, _1, _2);
	http_server->resource["/room/open"]["POST"] = std::bind(&HttpController::OpenRoom, this, _1, _2);
	http_server->resource["/room/join"]["POST"] = std::bind(&HttpController::JoinRoom, this, _1, _2);
	http_server->resource["/room/leave"]["POST"] = std::bind(&HttpController::LeaveRoom, this, _1, _2);
}

void HttpController::QueryDefaultResource(HttpResponse response, HttpRequest request) {
	try {
		LOG_REQUEST_INFO("default resource");

		auto web_root_path = boost::filesystem::canonical("web");
		auto path = boost::filesystem::canonical(web_root_path / request->path);
		// Check if path is within web_root_path
		if (std::distance(web_root_path.begin(), web_root_path.end()) > std::distance(path.begin(), path.end()) ||
			!std::equal(web_root_path.begin(), web_root_path.end(), path.begin())) {
			LOG_ERROR("Query default resource, path must be within root path");
			throw std::invalid_argument("path must be within root path");
		}
		if (boost::filesystem::is_directory(path))
			path /= "index.html";

		SimpleWeb::CaseInsensitiveMultimap header;

		// Uncomment the following line to enable Cache-Control
		// header.emplace("Cache-Control", "max-age=86400");

		auto ifs = std::make_shared<std::ifstream>();
		ifs->open(path.string(), std::ifstream::in | std::ios::binary | std::ios::ate);

		if (*ifs) {
			auto length = ifs->tellg();
			ifs->seekg(0, std::ios::beg);

			header.emplace("Content-Length", to_string(length));
			response->write(header);

			// Trick to define a recursive function within this scope (for example purposes)
			class FileServer {
			public:
				static void read_and_send(const std::shared_ptr<HttpServer::Response>& response, const std::shared_ptr<std::ifstream>& ifs) {
					// Read and send 128 KB at a time
					static std::vector<char> buffer(131072); // Safe when server is running on one thread
					std::streamsize read_length;
					if ((read_length = ifs->read(&buffer[0], static_cast<std::streamsize>(buffer.size())).gcount()) > 0) {
						response->write(&buffer[0], read_length);
						if (read_length == static_cast<std::streamsize>(buffer.size())) {
							response->send([response, ifs](const SimpleWeb::error_code& ec) {
								if (!ec) {
									read_and_send(response, ifs);
								}
								else {
									LOG_ERROR("Query default resource, connection interrupted");
								}
								});
						}
					}
				}
			};
			FileServer::read_and_send(response, ifs);
		}
		else {
			LOG_ERROR("Query default resource, could not read file: %s", request->path.c_str());
			throw std::invalid_argument("could not read file");
		}
	}
	catch (const std::exception& e) {
		response->write(SimpleWeb::StatusCode::client_error_bad_request, "Could not open path " + request->path + ": " + e.what());
	}
}

void HttpController::QueryRoom(HttpResponse response, HttpRequest request) {
	LOG_REQUEST_INFO("query room");

	if (request->argument_size() < 1) {
		this->WriteJson(response, HttpStatus::ArgumentIncorrect);
		return;
	}

	const auto& rooms = ws_ctrl_->rooms_;
	auto roomid = request->argument_at(0);
	if (rooms.find(roomid) == rooms.cend()) {
		this->WriteJson(response, HttpStatus::RoomNotExisted);
		return;
	}

	this->WriteJson(response, HttpStatus::OK, rooms.at(roomid));
}

void HttpController::QueryRooms(HttpResponse response, HttpRequest request) {
	LOG_REQUEST_INFO("query rooms");

	this->WriteJson(response, HttpStatus::OK, ws_ctrl_->rooms_);
}

void HttpController::OpenRoom(HttpResponse response, HttpRequest request) {
	LOG_REQUEST_INFO("open room");

	json param_obj = json::parse(request->content.string(), nullptr, false);
	if (param_obj.is_discarded()) {
		this->WriteJson(response, HttpStatus::BodyParameterJsonInvalid);
		return;
	}

	if (!param_obj.contains("sessionid") || !param_obj.contains("roomid") ||
		!param_obj.contains("room_type")) {
		this->WriteJson(response, HttpStatus::ParameterIncorrect);
		return;
	}

	auto& rooms = ws_ctrl_->rooms_;
	auto sessionid = param_obj["sessionid"].get<SessionId>();
	auto roomid = param_obj["roomid"].get<RoomId>();
	auto room_type = static_cast<RoomType>(param_obj["room_type"].get<int>());
	bool force = false;
	if (param_obj.contains("force")) {
		force = param_obj["force"].get<int>();
	}

	// check if sessionid already in room
	RoomId existed_roomid;
	if (ws_ctrl_->IsSessionidExisted(sessionid, existed_roomid)) {
		json data_obj = { "roomid", existed_roomid };
		this->WriteJson(response, HttpStatus::SessionidAlreadyInRoom, data_obj);
		return;
	}

	// check if roomid already existed
	if (rooms.find(roomid) != rooms.cend()) {
		if (!force) {
			this->WriteJson(response, HttpStatus::RoomAlreadyExisted);
			return;
		}

		rooms.erase(roomid);
	}


	Room new_room {
		roomid,
		room_type,
		{ sessionid },
		room_type == RoomType::VideoBroadcasting ? sessionid : -1
	};
	rooms[roomid] = new_room;
	this->WriteJson(response, HttpStatus::OK);
}

void HttpController::JoinRoom(HttpResponse response, HttpRequest request) {
	LOG_REQUEST_INFO("join room");

	json param_obj = json::parse(request->content.string(), nullptr, false);
	if (param_obj.is_discarded()) {
		this->WriteJson(response, HttpStatus::BodyParameterJsonInvalid);
		return;
	}

	if (!param_obj.contains("sessionid") || !param_obj.contains("roomid")) {
		this->WriteJson(response, HttpStatus::ParameterIncorrect);
		return;
	}

	auto& rooms = ws_ctrl_->rooms_;
	auto sessionid = param_obj["sessionid"].get<SessionId>();
	auto roomid = param_obj["roomid"].get<RoomId>();

	// check if roomid already existed
	if (rooms.find(roomid) == rooms.cend()) {
		this->WriteJson(response, HttpStatus::RoomNotExisted);
		return;
	}

	// check if sessionid already in room
	RoomId existed_roomid;
	if (ws_ctrl_->IsSessionidExisted(sessionid, existed_roomid)) {
		json data_obj = { "roomid", existed_roomid };
		this->WriteJson(response, HttpStatus::SessionidAlreadyInRoom, data_obj);
		return;
	}

	auto& room = rooms[roomid];
	room.sessionids.emplace_back(sessionid);
	this->WriteJson(response, HttpStatus::OK, room);
}

void HttpController::LeaveRoom(HttpResponse response, HttpRequest request) {
	LOG_REQUEST_INFO("leave room");

	json param_obj = json::parse(request->content.string(), nullptr, false);
	if (param_obj.is_discarded()) {
		this->WriteJson(response, HttpStatus::BodyParameterJsonInvalid);
		return;
	}

	if (!param_obj.contains("sessionid")) {
		this->WriteJson(response, HttpStatus::ParameterIncorrect);
		return;
	}

	auto sessionid = param_obj["sessionid"].get<SessionId>();
	ws_ctrl_->LeaveRoom(sessionid);

	this->WriteJson(response, HttpStatus::OK);
}
