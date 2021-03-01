# pragma once

#ifdef _WIN32
#ifdef RTC_DLL_EXPORTS
#define RTC_API __declspec(dllexport)
#else
#define RTC_API __declspec(dllimport)
#endif
#elif __linux__
#define RTC_API
#endif

#include "rtc_types.h"
#include <memory>
#include <functional>

VTS_RTC_NAMESPACE_BEGIN

class RTC_API RtcAgent {
public:
	static std::shared_ptr<RtcAgent> Create(const std::string& rtc_config_filepath, 
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr);
	static std::shared_ptr<RtcAgent> Create(const RtcConfig& rtc_config, 
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr);
	~RtcAgent() = default;

	// video devices releated
	VideoDevices GetVideoDevices() const;

	// add video source, from camera capturer OR from external feed
	void AddVideoSource(size_t device_index, const VideoDeviceCapability& device_capability) const;
	void AddVideoSource(const VideoSourceId& video_sourceid) const;

	// room management
	/**
	 * QueryRoom: @retval OK, InternalError, RoomNotExisted
	 * QueryRooms: @retval OK, InternalError
	 * OpenRoom: @retval OK, InternalError, AgentNotLogined, RoomAlreadyExisted, AgentAlreadyInRoom
	 * JoinRoom: @retval OK, InternalError, AgentNotLogined, RoomNotExisted, AgentAlreadyInRoom
	 * LeaveRoom: @retval OK, InternalError, AgentNotLogined
	 */
	RoomCode QueryRoom(const RoomId& roomid, Room& room) const;
	RoomCode QueryRooms(Rooms& rooms) const;
	RoomCode OpenRoom(const RoomId& roomid, enum RoomType room_type = RoomType::VideoBroadcasting) const;
	RoomCode JoinRoom(const RoomId& roomid) const;
	RoomCode LeaveRoom() const;

	SessionIds QueryRemoteAgents() const;

	// data channel
	bool Send(const std::string& msg, SessionId remote_sessionid) const;
	bool Send(const std::string& msg, const SessionIds& remote_sessionids) const;
	bool Broadcast(const std::string& msg) const;

	// send frame
	void SendFrame(const VideoSourceId& video_sourceid, const YUV420pFrame& video_frame) const;

private:
	explicit RtcAgent(const RtcConfig& rtc_config, 
		const RecvMessageHandler& recv_msg_handler, 
		const RecvFrameHandler& recv_frame_handler);

private:
	class Impl;
	std::unique_ptr<Impl> pimpl_;
};

VTS_RTC_NAMESPACE_END
