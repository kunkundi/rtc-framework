#pragma once

/**
 * @brief 函数运行线程说明
 * 
 * RecvMessageHandler: logic-thread线程
 * RecvFrameHandler: IncomingVideoStream线程
 * NetworkDisconnectedHandler: logic-thread线程
 * 
 * Rtc前缀的功能函数: logic-thread线程
 */

#ifdef _WIN32
#ifdef RTC_DLL_EXPORTS
#define RTC_API __declspec(dllexport)
#else
#define RTC_API __declspec(dllimport)
#endif
#elif __linux__
#define RTC_API
#endif

#include <stddef.h>

typedef unsigned int RtcSessionId;
typedef RtcSessionId* RtcSessionIds;
typedef const char* RtcDataChannelLabel;
typedef const char* RtcAudioSourceId;
typedef const char* RtcVideoSourceId;
typedef char* RtcRoomId;
typedef char* RtcSRSStreamurl;
typedef char* RtcSRSSessionId;

typedef enum RtcMediaSourceType {
	Rtc = 0,
	SRS
} RtcMediaSourceType;

typedef enum RtcRoomOperation {
	RoomNew,
	RoomDelete
} RtcRoomOperation;

typedef enum RtcP2PState {
	P2PNew,
	P2PConnecting,
	P2PConnected,
	P2PDisconnected,
	P2PFailed,
	P2PClosed
} RtcP2PState;

typedef enum RtcDataChannelState {
	DataChannelConnecting,
	DataChannelOpen,
	DataChannelClosing,
	DataChannelClosed
} RtcDataChannelState;

typedef enum RtcServerConnectionState {
	ServerConnecting,
	ServerConnected,
	ServerLogined,
	ServerDisconnected,
	ServerReconnecting
} RtcServerConnectionState;

typedef enum RtcRoomType {
	VideoBroadcasting = 0,  // one to many
	VideoConference         // many to many, not implemented yet
} RtcRoomType;

typedef struct RtcRoom {
	RtcRoomId roomid;
	RtcRoomType room_type;
	RtcSessionIds sessionids;
	size_t sz_sessionids;
	RtcSessionId broadcaster_sessionid = -1;
} RtcRoom;

typedef RtcRoom* RtcRooms;

typedef enum RtcErrorCode {
	OK = 0,
	InternalError,
	RoomNotExisted,
	RoomAlreadyExisted,
	AgentAlreadyInRoom,
	SRSAuthFailed,
	SRSStreamNotExisted,
	SRSStreamAlreadyExisted,
	AgentNotLogined,
	AgentNotInited,
	Failed,
} RtcErrorCode;

typedef struct RtcVideoDeviceCapability {
	size_t width;
	size_t height;
	size_t max_fps;
	// TO DO
	// VideoType video_type;
} RtcVideoDeviceCapability;

typedef RtcVideoDeviceCapability* RtcVideoDeviceCapabilities;

typedef struct RtcVideoDevice {
	size_t device_index;
	char* device_name;
	char* device_uniqueid;
	char* product_uniqueid;
	RtcVideoDeviceCapabilities device_capabilities;
	size_t sz_device_capabilities;
} RtcVideoDevice;

typedef RtcVideoDevice* RtcVideoDevices;

typedef enum RtcPriorityType {
	VeryLow = 0,
	Low,
	Medium,
	High,
} RtcPriorityType;

typedef struct RtcPCMData {
	size_t bits_per_sample;
	size_t sample_rate;
	size_t number_of_channels;
	size_t number_of_frames;
	void* buffer;
	size_t sz_buffer;
} RtcPCMData;

typedef struct RtcYUV420pFrame {
	size_t width;
	size_t height;
	size_t stride_Y;
	size_t stride_U;
	size_t stride_V;
	unsigned char* buffer;
	size_t sz_buffer;
} RtcYUV420pFrame;

// room related callback
typedef void(*RoomHandler)(RtcRoomOperation, RtcRoomId);
// P2P related callback
typedef void(*P2PStateHandler)(RtcSessionId, RtcP2PState);
// Data Channel related callback
typedef void(*DataChannelStateHandler)(RtcSessionId,
	RtcDataChannelLabel, RtcDataChannelState);
// signaling server related callback
typedef void(*ServerConnectionStateHandler)(RtcServerConnectionState);

// RtcSessionId, RtcDataChannelLabel, message, message size (unit: Byte)
typedef void(*RecvMessageHandler)(RtcSessionId, RtcDataChannelLabel,
	const char*, size_t);
// RtcAudioSourceId, RtcMediaSourceType, bits_per_sample, sample_rate,
// number_of_channels, number_of_frames, audio_data, audio_data_size
typedef void(*RecvAudioFrameHandler)(RtcAudioSourceId, RtcMediaSourceType,
	size_t, size_t, size_t, size_t, const void*, size_t);
// RtcVideoSourceId, RtcMediaSourceType, width, height, dimension,
// video_data, video_data_size
typedef void(*RecvFrameHandler)(RtcVideoSourceId, RtcMediaSourceType,
	size_t, size_t, size_t, const unsigned char*, size_t);

#ifdef  __cplusplus
extern "C" {
#endif

	/**
	 * @brief 初始化Rtc Agent
	 *
	 * @param config_filepath Rtc配置文件
	 * @param recv_msg_handler 收到远端消息的回调函数
	 * @param recv_frame_handler 收到远端图像帧的回调函数
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 初始化成功
	 *   @retval RtcErrorCode::Failed 初始化失败
	 * @attention 调用其他函数前，必须首先调用该函数
	 */
	RTC_API RtcErrorCode RtcInitAgent(const char* config_filepath,
		RoomHandler room_handler,
		P2PStateHandler P2P_state_handler,
		DataChannelStateHandler datachannel_state_handler,
		ServerConnectionStateHandler serverconnection_state_handler,
		RecvMessageHandler recv_msg_handler,
		RecvAudioFrameHandler recv_audioframe_handler,
		RecvFrameHandler recv_frame_handler);

	/**
	 * @brief 释放Rtc Agent资源
	 *
	 * @return void
	 * @attention 程序退出时，必须调用该函数，否则存在内存泄漏
	 */
	RTC_API void RtcDestoryAgent();

	/**
	 * @brief 获取设备信息
	 *
	 * @param video_devices 如果获取成功，则存放设备信息数组
	 * @param sz_video_devices 如果获取成功，则存放设备个数
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 获取设备失败
	 *   @retval RtcErrorCode::AgentNotInited 获取设备失败，因为Rtc Agent未成功初始化
	 */
	RTC_API RtcErrorCode RtcGetVideoDevices(RtcVideoDevices* video_devices,
		size_t* sz_video_devices);

	/**
	 * @brief 释放设备信息资源
	 *
	 * @param video_devices 待释放的设备数组指针
	 * @return void
	 * @attention RtcGetVideoDevices函数成功时，必须调用该函数释放房间资源，否则存在内存泄漏
	 */
	RTC_API void RtcDestoryVideoDevices(RtcVideoDevices video_devices,
		size_t sz_video_devices);

	/**
	 * @brief 加入房间时，增加数据通道
	 *
	 * @param label 数据通道的Label
	 * @param priority 优先级
	 * @param ordered 传递信息的顺序是否有保证
	 * @param max_retransmits 不可靠模式下消息允许尝试重发的最大次数，若为-1，则确保可靠
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 增加数据通道成功
	 *   @retval RtcErrorCode::AgentNotInited 增加数据通道失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 增加数据通道失败
	 * @attention: 必须由加入房间的一端调用才会生效，打开房间的一端调用不会生效
	 */
	RTC_API RtcErrorCode RtcAddDataChannel(RtcDataChannelLabel label,
		RtcPriorityType priority,
		bool ordered,
		int max_retransmits);

	/**
	 * @brief 增加来自外部的音频源
	 *
	 * @param audio_sourceid 音频源的唯一ID
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 增加源成功
	 *   @retval RtcErrorCode::AgentNotInited 增加源失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 增加源失败，audio_sourceid重复
	 * @attention 1. 函数参数必须与RtcSendAudioFrame函数和RecvAudioFrameHandler回调函数的audio_sourceid相一致
	 *            2. 针对WEB平台，当采用Unified Plan形式的SDP，接收端的MediaStreamTrack的id域是唯一的GUID，
	 *               并不具有业务含义，所以SDK约定一个track只属于一个stream，同时track和stream的label值相同，
	 *               通过访问接收端的MediaStream的id域可以对AudioTrack进行业务区分
	 */
	RTC_API RtcErrorCode RtcAddExternalAudioSource(
		RtcAudioSourceId audio_sourceid, RtcPriorityType priority);

	/**
	 * @brief 增加摄像头设备视频源
	 *
	 * @param device_index 设备index，来源于RtcGetVideoDevices函数查询的结果
	 * @param device_capability 指定摄像头捕获的属性，比如：分辨率、帧率
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 增加源成功
	 *   @retval RtcErrorCode::AgentNotInited 增加源失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 增加源失败，设备被占用
	 */
	RTC_API RtcErrorCode RtcAddDeviceVideoSource(size_t device_index,
		const RtcVideoDeviceCapability* device_capability,
		RtcPriorityType priority);

	/**
	 * @brief 增加来自外部的视频源（图像帧可能来自视频文件，或者是外部程序读取的摄像头捕获帧）
	 *
	 * @param video_sourceid 视频源的唯一ID
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 增加源成功
	 *   @retval RtcErrorCode::AgentNotInited 增加源失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 增加源失败，video_sourceid重复
	 * @attention 1. 函数参数必须与SendFrame函数和RecvFrameHandler回调函数的video_sourceid相一致
	 *            2. 针对WEB平台，当采用Unified Plan形式的SDP，接收端的MediaStreamTrack的id域是唯一的GUID，
	 *               并不具有业务含义，所以SDK约定一个track只属于一个stream，同时track和stream的label值相同，
	 *               通过访问接收端的MediaStream的id域可以对VideoTrack进行业务区分
	 */
	RTC_API RtcErrorCode RtcAddExternalVideoSource(
		RtcVideoSourceId video_sourceid, RtcPriorityType priority);

	/**
	 * @brief 请求房间的详细信息
	 *
	 * @param roomid 房间ID
	 * @param room 如果查询成功，则存放房间信息
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 查询成功
	 *   @retval RtcErrorCode::AgentNotInited 查询失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::InternalError 查询失败，服务器的问题
	 *   @retval RtcErrorCode::RoomNotExisted 查询失败，因为roomid不存在
	 */
	RTC_API RtcErrorCode RtcQueryRoom(const RtcRoomId roomid, RtcRoom* room);

	/**
	 * @brief 释放房间资源
	 *
	 * @param room 房间指针
	 * @return void
	 * @attention RtcQueryRoom函数成功时，必须调用该函数释放房间资源，否则存在内存泄漏
	 */
	RTC_API void RtcDestoryRoom(RtcRoom room);

	/**
	 * @brief 请求所有房间详细信息
	 *
	 * @param rooms 如果查询成功，则存放所有房间信息
	 * @param sz_rooms 如果查询成功，则存放房间个数
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 查询成功
	 *   @retval RtcErrorCode::AgentNotInited 查询失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::InternalError 查询失败，服务器的问题
	 */
	RTC_API RtcErrorCode RtcQueryRooms(RtcRooms* rooms, size_t* sz_rooms);

	/**
	 * @brief 释放所有房间资源
	 *
	 * @param rooms 房间数组指针
	 * @param sz_rooms 房间个数
	 * @return void
	 * @attention QueryRooms函数成功时，必须调用该函数释放所有房间资源，否则存在内存泄漏
	 */
	RTC_API void RtcDestoryRooms(RtcRooms rooms, size_t sz_rooms);

	/**
	 * @brief 打开房间
	 *
	 * @param roomid 房间ID
	 * @param room_type 房间类型
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 打开成功
	 *   @retval RtcErrorCode::AgentNotInited 打开失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::InternalError 打开失败，服务器的问题
	 *   @retval RtcErrorCode::AgentNotLogined 打开失败，Rtc Agent未成功登录
	 *   @retval RtcErrorCode::RoomAlreadyExisted 打开失败，房间ID已经存在
	 *   @retval RtcErrorCode::AgentAlreadyInRoom 打开失败，Rtc Agent已经在房间中，无法打开房间
	 */
	RTC_API RtcErrorCode RtcOpenRoom(const RtcRoomId roomid,
		RtcRoomType room_type, bool force);

	/**
	 * @brief 加入房间
	 *
	 * @param roomid 房间ID
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 加入成功
	 *   @retval RtcErrorCode::AgentNotInited 加入失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::InternalError 加入失败，服务器的问题
	 *   @retval RtcErrorCode::AgentNotLogined 加入失败，Rtc Agent未成功登录
	 *   @retval RtcErrorCode::RoomNotExisted 加入失败，房间ID不存在
	 *   @retval RtcErrorCode::AgentAlreadyInRoom 加入失败，Rtc Agent已经在房间中，无法加入房间
	 */	
	RTC_API RtcErrorCode RtcJoinRoom(const RtcRoomId roomid);

	/**
	 * @brief 离开房间
	 *
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 离开成功
	 *   @retval RtcErrorCode::AgentNotInited 离开失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::InternalError 离开失败，服务器的问题
	 *   @retval RtcErrorCode::AgentNotLogined 离开失败，Rtc Agent未成功登录
	 */
	RTC_API RtcErrorCode RtcLeaveRoom();

	/**
	 * @brief 将RTC流推送到SRS [https://github.com/ossrs/srs]
	 *
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 推流成功
	 *   @retval RtcErrorCode::InternalError 推流失败，服务器的问题
	 */
	RTC_API RtcErrorCode RtcPublishToSRS(const RtcSRSStreamurl SRS_streamurl);

	// Not supported
	RTC_API RtcErrorCode RtcUnpublishToSRS(const RtcSRSStreamurl SRS_streamurl,
		const RtcSRSSessionId SRS_sessionid);

	/**
	 * @brief 从SRS拉取RTC流
	 *
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 拉流成功
	 *   @retval RtcErrorCode::InternalError 拉流失败，服务器的问题
	 */
	RTC_API RtcErrorCode RtcPlayFromSRS(const RtcSRSStreamurl SRS_streamurl);

	// Not supported
	RTC_API RtcErrorCode RtcUnplayFromSRS(const RtcSRSStreamurl SRS_streamurl,
		const RtcSRSSessionId SRS_sessionid);

	/**
	 * @brief 发送消息到远端Rtc Agent
	 *
	 * @param remote_sessionid 远端Rtc Agent的sessionid
	 * @param channel_label 数据通道的Label
	 * @param msg 消息内容
	 * @param msg_size 消息大小
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 发送成功
	 *   @retval RtcErrorCode::AgentNotInited 发送失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 发送失败
	 */
	RTC_API RtcErrorCode RtcSendData(RtcSessionId remote_sessionid,
		RtcDataChannelLabel channel_label, const char* msg, size_t msg_size);

	/**
	 * @brief 广播消息到所有与本端连接的远端Rtc Agent
	 *
	 * @param channel_label 数据通道的Label
	 * @param msg 消息内容
	 * @param msg_size 消息大小
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 发送成功
	 *   @retval RtcErrorCode::AgentNotInited 发送失败，因为Rtc Agent未成功初始化
	 *   @retval RtcErrorCode::Failed 发送失败
	 * @attention 发送消息到远端Rtc Agent，至少一个发送成功便认为执行成功
	 */
	RTC_API RtcErrorCode RtcBroadcastData(RtcDataChannelLabel channel_label,
		const char* msg, size_t msg_size);

	/**
	 * @brief 发送音频帧
	 *
	 * @param audio_sourceid 音频源ID，与RtcAddExternalAudioSource函数参数相一致
	 * @param pcmdata PCM数据
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 发送成功
	 *   @retval RtcErrorCode::AgentNotInited 发送失败，因为Rtc Agent未成功初始化
	 */
	RTC_API RtcErrorCode RtcSendAudioFrame(RtcAudioSourceId audio_sourceid,
		const RtcPCMData* pcmdata);

	/**
	 * @brief 发送图像帧
	 *
	 * @param video_sourceid 视频源ID，与RtcAddExternalVideoSource函数参数相一致
	 * @param video_frame I420p帧数据
	 * @return 函数是否执行成功
	 *   @retval RtcErrorCode::OK 发送成功
	 *   @retval RtcErrorCode::AgentNotInited 发送失败，因为Rtc Agent未成功初始化
	 */
	RTC_API RtcErrorCode RtcSendFrame(RtcVideoSourceId video_sourceid,
		const RtcYUV420pFrame* video_frame);

	/**
	 * @brief ErrorCode转换为便于阅读的字符串
	 *
	 * @param code ErrorCode
	 * @return 可读的字符串
	 */
	RTC_API const char* RtcErrorMessage(RtcErrorCode code);

#ifdef  __cplusplus
}
#endif
