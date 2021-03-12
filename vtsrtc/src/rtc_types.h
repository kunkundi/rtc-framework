#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

#define VTS_RTC_NAMESPACE_BEGIN namespace vts_rtc {
#define VTS_RTC_NAMESPACE_END }

VTS_RTC_NAMESPACE_BEGIN

struct VideoDevice;
struct Room;
using SessionId = unsigned int;
using SessionIds = std::vector<SessionId>;
using RoomId = std::string;
using Rooms = std::map<RoomId, Room>;
using VideoDevices = std::vector<VideoDevice>;
using VideoSourceId = std::string;
using RecvMessageHandler = std::function<void(SessionId, const std::string&)>;
using RecvFrameHandler = std::function<void(const VideoSourceId&, size_t, size_t, size_t, const std::vector<unsigned char>&)>;

struct RtcConfig {
	struct IceServer {
		std::vector<std::string> urls;
		std::string username;
		std::string password;
	};

	std::string api_server_url;
	std::string signaling_server_url;
	std::vector<IceServer> ice_servers;
};

struct VideoDeviceCapability {
	size_t width;
	size_t height;
	size_t max_fps;
	// TO DO
	//VideoType video_type;
};

struct VideoDevice {
	size_t device_index;
	std::string device_name;
	std::string device_uniqueid;
	std::string product_uniqueid;
	std::vector<VideoDeviceCapability> device_capabilities;
};

enum class RoomType {
	VideoBroadcasting = 0,  // one to many
	VideoConference         // many to many, not implemented yet
};

struct Room {
	RoomId roomid;
	RoomType room_type;
	SessionIds sessionids;
	SessionId broadcaster_sessionid = -1;
};

enum class RoomCode {
	OK = 0,
	InternalError,
	RoomNotExisted,
	RoomAlreadyExisted,
	AgentAlreadyInRoom,
	AgentNotLogined,
};

struct YUV420pFrame {
	size_t width;
	size_t height;
	size_t stride_Y;
	size_t stride_U;
	size_t stride_V;
	unsigned char* buffer;
	size_t sz_buffer;
};

VTS_RTC_NAMESPACE_END
