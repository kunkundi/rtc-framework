# pragma once

#include "rtc_types.h"
#include "rtc_connection_manager.h"

VTS_RTC_NAMESPACE_BEGIN

class RtcAgent {
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
	bool AddVideoSource(size_t device_index, const VideoDeviceCapability& device_capability) const;
	bool AddVideoSource(const VideoSourceId& video_sourceid) const;

	// room management
	// @retval OK, InternalError, RoomNotExisted
	RoomCode QueryRoom(const RoomId& roomid, Room& room) const;
	// @retval OK, InternalError
	RoomCode QueryRooms(Rooms& rooms) const;
	// @retval OK, InternalError, AgentNotLogined, RoomAlreadyExisted, AgentAlreadyInRoom
	RoomCode OpenRoom(const RoomId& roomid, enum RoomType room_type = RoomType::VideoBroadcasting) const;
	// @retval OK, InternalError, AgentNotLogined, RoomNotExisted, AgentAlreadyInRoom
	RoomCode JoinRoom(const RoomId& roomid) const;
	// @retval OK, InternalError, AgentNotLogined
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
	std::shared_ptr<RtcDeviceManager> rtc_device_manager_;
	std::unique_ptr<RtcConnectionManager> rtc_conn_manager_;
};

VTS_RTC_NAMESPACE_END
