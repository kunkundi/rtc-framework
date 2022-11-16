#include "log_manager.h"

LogManager::LogManager() {
}

LogManager::~LogManager() {
	log_.ClsFile("rtc_agent");
}

int LogManager::init(std::string log_path) {
	if (!log_path.empty())
	{
		log_.InitLog(false, vts::log::LEVEL_INFO, log_path);
	}
	else
	{
		log_.InitLog(false, vts::log::LEVEL_INFO);
	}

	log_.CrtFile("rtc_agent");
	//log_.StartUpLoad(vts::log::TCP, "10.0.104.86:10086", "");
	return 0;
}
