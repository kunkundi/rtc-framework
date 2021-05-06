#include "http_controller.h"
#include <fstream>

int main(int argc, char* argv[]) {
	LogInst->init();

	// load config file
	unsigned short port = 8090;
	long client_ping_timeout = 3000;
	nlohmann::json server_cfg_obj;
	try {
		std::ifstream ifs("signaling-server.cfg");
		ifs >> server_cfg_obj;

		if (server_cfg_obj.contains("port")) {
			port = server_cfg_obj["port"].get<unsigned short>();
		}

		if (server_cfg_obj.contains("client_ping_timeout")) {
			client_ping_timeout = server_cfg_obj["client_ping_timeout"].get<long>();
		}
	}
	catch (const nlohmann::json::parse_error& exp) {
		LOG_WARN("Signaling server config file not found or not valid json, use default value");
	}

	auto io_context = std::make_shared<SimpleWeb::io_context>();

	auto ws_server = std::make_shared<WsServer>();
	auto ws_ctrl = std::make_shared<WsController>(io_context, client_ping_timeout, ws_server);

	auto http_server = std::make_shared<HttpServer>();
	http_server->config.port = port;
	http_server->io_service = io_context;
	HttpController http_ctrl(http_server, ws_ctrl);

	http_server->on_upgrade = [ws_server](std::unique_ptr<SimpleWeb::HTTP>& socket,
		std::shared_ptr<HttpServer::Request> request) {
		auto connection = std::make_shared<WsServer::Connection>(std::move(socket));
		connection->method = std::move(request->method);
		connection->path = std::move(request->path);
		connection->http_version = std::move(request->http_version);
		connection->header = std::move(request->header);
		ws_server->upgrade(connection);
	};
	
	LOG_INFO("Rtc signaling server started.");
	http_server->start();

	io_context->run();

    return 0;
}
