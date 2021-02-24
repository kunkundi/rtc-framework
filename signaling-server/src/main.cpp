#include "http_controller.h"
#include <iostream>

int main(int argc, char* argv[]) {
	LogInst->init();

	auto ws_server = std::make_shared<WsServer>();
	auto ws_ctrl = std::make_shared<WsController>(ws_server);

	auto http_server = std::make_shared<HttpServer>();
	http_server->config.port = 8080;
	HttpController http_ctrl(http_server, ws_ctrl);

	http_server->on_upgrade = [ws_server](std::unique_ptr<SimpleWeb::HTTP>& socket, std::shared_ptr<HttpServer::Request> request) {
		auto connection = std::make_shared<WsServer::Connection>(std::move(socket));
		connection->method = std::move(request->method);
		connection->path = std::move(request->path);
		connection->http_version = std::move(request->http_version);
		connection->header = std::move(request->header);
		ws_server->upgrade(connection);
	};
	
	LOG_INFO("Rtc signaling server started.");
	http_server->start();

    return 0;
}
