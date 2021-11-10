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
using SRSSessionId = std::string;
using SRSStreamurl = std::string;
using VideoDevices = std::vector<VideoDevice>;
using VideoSourceId = std::string;
enum class VideoSourceType {
	Rtc = 0,
	SRS
};
using RecvMessageHandler = std::function<void(SessionId, const std::string&,
	const std::string&)>;
using RecvFrameHandler = std::function<void(const VideoSourceId&,
	enum VideoSourceType, size_t, size_t, size_t, const std::vector<unsigned char>&)>;
using NetworkDisconnectedHandler = std::function<void()>;

struct RtcConfig {
	struct IceServer {
		std::vector<std::string> urls;
		std::string username;
		std::string password;
	};

	std::string api_server_url;
	std::string signaling_server_url;
	std::string SRS_api_server_url;
	std::vector<IceServer> ice_servers;

	long ping_timeout = 2000;
	long pong_timeout = 4000;
	long reconnect_timeout = 2000;  // unit: milliseconds
};

struct VideoDeviceCapability {
	size_t width;
	size_t height;
	size_t max_fps;
	// TO DO
	// VideoType video_type;
};

struct VideoDevice {
	size_t device_index;
	std::string device_name;
	std::string device_uniqueid;
	std::string product_uniqueid;
	std::vector<VideoDeviceCapability> device_capabilities;
};

enum class PriorityType {
	VeryLow = 0,
	Low,
	Medium,
	High,
};

enum class RoomType {
	VideoBroadcasting = 0,  // one to many
	VideoConference,        // many to many, not implemented yet
};

struct Room {
	RoomId roomid;
	RoomType room_type;
	SessionIds sessionids;
	SessionId broadcaster_sessionid = -1;
};

enum class ErrorCode {
	OK = 0,
	InternalError,
	RoomNotExisted,
	RoomAlreadyExisted,
	AgentAlreadyInRoom,
	SRSAuthFailed,
	SRSStreamNotExisted,
	SRSStreamAlreadyExisted,
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
