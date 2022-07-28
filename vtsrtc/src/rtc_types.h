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
using AudioSourceId = std::string;
using VideoDevices = std::vector<VideoDevice>;
using VideoSourceId = std::string;
using MediaSourceId = std::string;
enum class MediaSourceType {
	Rtc = 0,
	SRS
};

enum class RoomOperation {
	New,
	Delete
};

enum class P2PState {
	New,
	Connecting,
	Connected,
	Disconnected,
	Failed,
	Closed
};

enum class DataChannelState {
	Connecting,
	Open,
	Closing,
	Closed
};

enum class ServerConnectionState {
	Connecting,
	Connected,
	Logined,
	Disconnected,
	Reconnecting
};

enum class SRSResponse {
	SRSOK = 0,
	SRSAuthFailed,
	SRSStreamNotExisted,
	SRSStreamAlreadyExisted,
};

enum class MediaChannelType {
	Video = 0,
	Audio,
	data
};

struct AudioNetStats {
	std::string sourceid;
	double bitrate_bps;
};

struct VideoNetStats {
	std::string sourceid;

	unsigned int width;
	unsigned int height;
	unsigned int bitrate_bps;
	double fps;
	double loss_rate;
	double delay_ms;
	unsigned int key_frame_count;

	unsigned int fir_count;
	unsigned int pli_count;
	unsigned int nack_count;

	std::string codec_name;
};

struct NetStats {
	bool input;
	AudioNetStats audio_stats;
	VideoNetStats video_stats;
};

// RoomOperation, RoomId
using RoomHandler = std::function<void(enum RoomOperation, const RoomId&)>;
// user related callback
// TO DO
using UserHandler = std::function<void()>;
// remote sessionId, P2PState
using P2PStateHandler = std::function<void(SessionId, enum P2PState)>;
// srs url, P2PState
using SRSStateHandler = std::function<void(SRSStreamurl, enum P2PState)>;
// srs url, SRSResponse
using SRSResponseHandler = std::function<void(SRSStreamurl, enum SRSResponse)>;
// remote sessionid, label, state
using DataChannelStateHandler =
	std::function<void(SessionId, const std::string&, enum DataChannelState)>;
// ServerConnectionState
using ServerConnectionStateHandler =
	std::function<void(enum ServerConnectionState)>;
// remote sessionid, label, message
using RecvMessageHandler = std::function<void(SessionId, const std::string&,
	const std::string&)>;
// AudioSourceId, MediaSourceType, bits_per_sample, sample_rate,
// number_of_channels, number_of_frames, audio_data
using RecvAudioFrameHandler = std::function<void(const AudioSourceId&,
	enum MediaSourceType, size_t, size_t, size_t, size_t, const void*)>;
// VideoSourceId, MediaSourceType, width, height, dimension, video_data
using RecvFrameHandler = std::function<void(const VideoSourceId&,
	enum MediaSourceType, size_t, size_t, size_t, const std::vector<unsigned char>&)>;
// VideoSourceId, MediaSourceType, width, height, dimension, video_data
using ChannelNetworkStatsHandler = std::function<void(const NetStats&)>;
using ChannelNetworkStatsHandlerUS = std::function<void(const NetStats&)>;


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
	std::map<std::string, std::pair<long, long>> resolution_limit;
	std::map<long, std::vector<long>> strategy;
	long bitrate_maxmum = 2000000;

	bool use_NVENC = false;
	bool use_NVDEC = false;

	long ping_timeout = 2000;
	long pong_timeout = 4000;  // unit: milliseconds
	long reconnect_interval = 2500;
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
	AgentNotLogined,
};

struct PCMData {
	size_t bits_per_sample;
	size_t sample_rate;
	size_t number_of_channels;
	size_t number_of_frames;
	void* buffer;
	size_t sz_buffer;
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
