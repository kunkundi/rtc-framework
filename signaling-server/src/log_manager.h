#pragma once

#include "vtslog.h"

#define LOG_EVERY_N(num, n, fmt,...) \
	if(num % n == 0){ \
		LogInst->log_.Log(vts::log::LEVEL_INFO, __FILE__, __LINE__, 1,  LogInst->topic,  fmt, ##__VA_ARGS__ ); \
	};

class LogManager {
public:
	LogManager();
	virtual ~LogManager();
	static LogManager* GetInstance() {
		static LogManager instance;
		return &instance;
	}

	int init();
	vts::log::vtslog log_;
	std::string topic[1] = { "rtc_signaling_server" };

};
#define LogInst LogManager::GetInstance() 


#define LOG_INFO(fmt, ...) LogInst->log_.Log(vts::log::LEVEL_INFO, __FILE__, __LINE__, 1,  LogInst->topic,  fmt, ##__VA_ARGS__ )
#define LOG_ERROR(fmt, ...) LogInst->log_.Log(vts::log::LEVEL_ERROR, __FILE__, __LINE__, 1,  LogInst->topic,  fmt, ##__VA_ARGS__ );
#define LOG_WARN(fmt, ...) LogInst->log_.Log(vts::log::LEVEL_WARN, __FILE__, __LINE__, 1,  LogInst->topic,  fmt, ##__VA_ARGS__ );
