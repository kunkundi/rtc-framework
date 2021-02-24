#include "log_manager.h"

LogManager::LogManager() {
}

LogManager::~LogManager() {
	log_.ClsFile( "rtc_agent" );
}

int LogManager::init() {
	log_.InitLog( true, vts::log::LEVEL_INFO );
	log_.CrtFile( "rtc_agent" );
	//log_.StartUpLoad( vts::log::TCP, "10.0.104.86:10086", "" );
	return 0;
}
