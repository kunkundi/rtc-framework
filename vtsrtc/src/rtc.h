# pragma once

#include "rtc_types.h"
#include "rtc_connection_manager.h"

VTS_RTC_NAMESPACE_BEGIN

class RtcAgent {
public:
	static std::shared_ptr<RtcAgent> Create(const std::string& rtc_config_filepath, 
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr,
		const NetworkDisconnectedHandler& network_disconnected_handler = nullptr);
	static std::shared_ptr<RtcAgent> Create(const RtcConfig& rtc_config, 
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr,
		const NetworkDisconnectedHandler& network_disconnected_handler = nullptr);
	~RtcAgent();

	// video devices releated
	VideoDevices GetVideoDevices() const;

	// add data channel (attention: just for JoinRoom side, not work for OpenRoom side)
	bool AddDataChannel(const std::string& label,
		PriorityType priority = PriorityType::Low,
		bool ordered = true,
		int max_retransmits = -1);

	// add video source, from camera capturer OR from external feed
	bool AddVideoSource(size_t device_index, const VideoDeviceCapability& device_capability,
		PriorityType priority = PriorityType::Low) const;
	bool AddVideoSource(const VideoSourceId& video_sourceid,
		PriorityType priority = PriorityType::Low) const;

	// room management
	// @retval OK, InternalError, RoomNotExisted
	ErrorCode QueryRoom(const RoomId& roomid, Room& room) const;
	// @retval OK, InternalError
	ErrorCode QueryRooms(Rooms& rooms) const;
	// @retval OK, InternalError, AgentNotLogined, RoomAlreadyExisted, AgentAlreadyInRoom
	ErrorCode OpenRoom(const RoomId& roomid, enum RoomType room_type = RoomType::VideoBroadcasting) const;
	// @retval OK, InternalError, AgentNotLogined, RoomNotExisted, AgentAlreadyInRoom
	ErrorCode JoinRoom(const RoomId& roomid) const;
	// @retval OK, InternalError, AgentNotLogined
	ErrorCode LeaveRoom() const;

	SessionIds QueryRemoteAgents() const;

	// SRS management
	// @retval OK, InternalError
	ErrorCode PublishToSRS(const SRSStreamurl& streamurl) const;
	// @retval OK
	ErrorCode UnpublishToSRS(const vts_rtc::SRSStreamurl& streamurl,
		const vts_rtc::SRSSessionId& sessionid) const;
	// @retval OK, InternalError
	ErrorCode PlayFromSRS(const SRSStreamurl& streamurl) const;
	// @retval OK
	ErrorCode UnplayFromSRS(const vts_rtc::SRSStreamurl& streamurl,
		const vts_rtc::SRSSessionId& sessionid) const;

	// send data
	bool SendData(const std::string& channel_label, const std::string& msg) const;
	// send frame
	void SendFrame(const VideoSourceId& video_sourceid, const YUV420pFrame& video_frame) const;

private:
	explicit RtcAgent(const RtcConfig& rtc_config, 
		const RecvMessageHandler& recv_msg_handler, 
		const RecvFrameHandler& recv_frame_handler,
		const NetworkDisconnectedHandler& network_disconnected_handler);
	// Init RtcAgent, mainly for RtcConnectionManager initialization
	bool Init();

private:
	std::unique_ptr<rtc::Thread> logic_thread_;
	std::shared_ptr<RtcDeviceManager> rtc_device_manager_;
	std::unique_ptr<RtcConnectionManager> rtc_conn_manager_;
};

VTS_RTC_NAMESPACE_END
