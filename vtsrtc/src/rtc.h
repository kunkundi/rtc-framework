# pragma once

#include "rtc_types.h"
#include "rtc_connection_manager.h"

VTS_RTC_NAMESPACE_BEGIN

class RtcAgent {
public:
	static std::shared_ptr<RtcAgent> Create(
		const std::string& rtc_config_filepath,
		const RoomHandler& room_handler = nullptr,
		const UserHandler& user_handler = nullptr,
		const P2PStateHandler& P2P_state_handler = nullptr,
		const DataChannelStateHandler& datachannel_state_handler = nullptr,
		const ServerConnectionStateHandler& serverconnection_state_handler = nullptr,
		const SRSStateHandler& SRS_state_handler = nullptr,
		const SRSResponseHandler& SRS_response_handler = nullptr,
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvAudioFrameHandler& recv_audioframe_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr);
	static std::shared_ptr<RtcAgent> Create(
		const RtcConfig& rtc_config,
		const RoomHandler& room_handler = nullptr,
		const UserHandler& user_handler = nullptr,
		const P2PStateHandler& P2P_state_handler = nullptr,
		const DataChannelStateHandler& datachannel_state_handler = nullptr,
		const ServerConnectionStateHandler& serverconnection_state_handler = nullptr,
		const SRSStateHandler& SRS_state_handler = nullptr,
		const SRSResponseHandler& SRS_response_handler = nullptr,
		const RecvMessageHandler& recv_msg_handler = nullptr,
		const RecvAudioFrameHandler& recv_audioframe_handler = nullptr,
		const RecvFrameHandler& recv_frame_handler = nullptr);
	~RtcAgent();

	// video devices releated
	VideoDevices GetVideoDevices() const;

	// add data channel (attention: just for JoinRoom side, not work for OpenRoom side)
	bool AddDataChannel(const std::string& label,
		PriorityType priority = PriorityType::Low,
		bool ordered = true,
		int max_retransmits = -1);

	// add audio source
	bool AddAudioSource(const AudioSourceId& audio_sourceid,
		PriorityType priority = PriorityType::Low) const;

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
	ErrorCode OpenRoom(const RoomId& roomid,
		enum RoomType room_type = RoomType::VideoBroadcasting,
		bool force = false) const;
	// @retval OK, InternalError, AgentNotLogined, RoomNotExisted
	ErrorCode CloseRoom(const RoomId& roomid) const;
	// @retval OK, InternalError, AgentNotLogined, RoomNotExisted, AgentAlreadyInRoom
	ErrorCode JoinRoom(const RoomId& roomid) const;
	// @retval OK, InternalError, AgentNotLogined
	ErrorCode LeaveRoom() const;

	SessionIds QueryRemoteAgents() const;

	// SRS management
	// @retval OK, InternalError
	ErrorCode PublishToSRS(const SRSStreamurl& streamurl) const;
	// @retval OK
	ErrorCode UnpublishToSRS(const vts_rtc::SRSStreamurl& streamurl) const;
	// @retval OK, InternalError
	ErrorCode PlayFromSRS(const SRSStreamurl& streamurl) const;
	// @retval OK
	ErrorCode UnplayFromSRS(const vts_rtc::SRSStreamurl& streamurl) const;

	// send data
	bool SendData(SessionId sessionid, const std::string& channel_label,
		const std::string& msg) const;
	// broadcast data to all P2P connections
	bool BroadcastData(const std::string& channel_label,
		const std::string& msg) const;
	// send audio frame(PCM format)
	void SendAudioFrame(const AudioSourceId& audio_sourceid, const PCMData& pcmdata) const;
	// send frame
	void SendFrame(const VideoSourceId& video_sourceid, const YUV420pFrame& video_frame) const;

private:
	explicit RtcAgent(
		const RtcConfig& rtc_config,
		const RoomHandler& room_handler,
		const UserHandler& user_handler,
		const P2PStateHandler& P2P_state_handler,
		const DataChannelStateHandler& datachannel_state_handler,
		const ServerConnectionStateHandler& serverconnection_state_handler,
		const SRSStateHandler& SRS_state_handler,
		const SRSResponseHandler& SRS_response_handler,
		const RecvMessageHandler& recv_msg_handler,
		const RecvAudioFrameHandler& recv_audioframe_handler,
		const RecvFrameHandler& recv_frame_handler);
	// Init RtcAgent, mainly for RtcConnectionManager initialization
	bool Init();

private:
	std::unique_ptr<rtc::Thread> logic_thread_;
	std::shared_ptr<RtcDeviceManager> rtc_device_manager_;
	std::unique_ptr<RtcConnectionManager> rtc_conn_manager_;
};

VTS_RTC_NAMESPACE_END
