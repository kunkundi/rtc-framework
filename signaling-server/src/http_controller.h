#pragma once

#include "http_status_code.hpp"
#include "ws_controller.h"
#include <server_http.hpp>

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

class HttpController {
	using HttpRequest = std::shared_ptr<HttpServer::Request>;
	using HttpResponse = std::shared_ptr<HttpServer::Response>;
	
public:
	explicit HttpController(std::shared_ptr<HttpServer> http_server, std::shared_ptr<WsController> ws_ctrl);
	~HttpController() = default;
	
private:
	inline void WriteJson(HttpResponse response, HttpStatus::Code code) {
		json content_obj = {
			{ HttpStatus::status_field, code },
			{ HttpStatus::message_field, HttpStatus::reason_phrase(code) }
		};
		response->write(content_obj.dump());
	}

	template <typename T>
	inline void WriteJson(HttpResponse response, HttpStatus::Code code, T data) {
		json content_obj = {
			{ HttpStatus::status_field, code },
			{ HttpStatus::message_field, HttpStatus::reason_phrase(code) },
			{ HttpStatus::data_field, data }
		};
		response->write(content_obj.dump());
	}

	void QueryDefaultResource(HttpResponse response, HttpRequest request);
	void QueryRoom(HttpResponse response, HttpRequest request);
	void QueryRooms(HttpResponse response, HttpRequest request);
	void OpenRoom(HttpResponse response, HttpRequest request);
	void CloseRoom(HttpResponse response, HttpRequest request);
	void JoinRoom(HttpResponse response, HttpRequest request);
	void LeaveRoom(HttpResponse response, HttpRequest request);

private:
	std::shared_ptr<WsController> ws_ctrl_;
	std::string last_request_address_ = "";
	unsigned short last_request_port_ = 0;
	int last_same_request_ = 0;
};
