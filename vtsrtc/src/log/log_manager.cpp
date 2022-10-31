#include "log_manager.h"

LogManager::LogManager() {
}

LogManager::~LogManager() {
	log_.ClsFile("rtc_agent");
}

int LogManager::init() {
#if defined  __aarch64__
	log_.InitLog(false, vts::log::LEVEL_INFO, "/data/log/");
#else
	log_.InitLog(false, vts::log::LEVEL_INFO);
#endif
	log_.CrtFile("rtc_agent");
	//log_.StartUpLoad(vts::log::TCP, "10.0.104.86:10086", "");
	return 0;
}
