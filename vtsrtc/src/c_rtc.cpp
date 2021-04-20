#include "c_rtc.h"
#include "rtc.h"
#include <iostream>

#define CHECK_RTCAGENT_INITED if (!rtc_agent) { return RtcErrorCode::AgentNotInited; }

std::shared_ptr<vts_rtc::RtcAgent> rtc_agent = nullptr;

RtcErrorCode ConvertCode(vts_rtc::RoomCode roomcode) {
	return static_cast<RtcErrorCode>(roomcode);
}

RtcErrorCode RtcInitAgent(const char* config_filepath, RecvMessageHandler recv_msg_handler, RecvFrameHandler recv_frame_handler) {
	RtcDestoryAgent();

	auto msg_handler = [recv_msg_handler](vts_rtc::SessionId sessionid, const std::string& channel_label, const std::string& msg) {
		recv_msg_handler(sessionid, channel_label.c_str(), msg.c_str());
	};

	auto frame_handler = [recv_frame_handler](const vts_rtc::VideoSourceId& video_sourceid,
		size_t width, size_t height, size_t dimension, const std::vector<unsigned char>& framebuffer) {
		recv_frame_handler(video_sourceid.c_str(), width, height, dimension, framebuffer.data(), framebuffer.size());
	};

	rtc_agent = vts_rtc::RtcAgent::Create(std::string(config_filepath), msg_handler, frame_handler);

	return rtc_agent ? RtcErrorCode::OK : RtcErrorCode::Failed;
}

void RtcDestoryAgent() {
	if (rtc_agent) {
		rtc_agent = nullptr;
	}
}

RtcErrorCode RtcGetVideoDevices(RtcVideoDevices* out_video_devices, size_t* out_sz_video_devices) {
	CHECK_RTCAGENT_INITED

	auto video_devices = rtc_agent->GetVideoDevices();
	size_t len = video_devices.size();
	*out_sz_video_devices = len;
	*out_video_devices = new RtcVideoDevice[len];
	for (size_t i = 0; i < len; ++i) {
		const auto& device = video_devices[i];
		auto& out_device = (*out_video_devices)[i];

		out_device.device_index = device.device_index;
		out_device.device_name = new char[device.device_name.size() + 1];
		strcpy(out_device.device_name, device.device_name.c_str());
		out_device.device_uniqueid = new char[device.device_uniqueid.size() + 1];
		strcpy(out_device.device_uniqueid, device.device_uniqueid.c_str());
		out_device.product_uniqueid = new char[device.product_uniqueid.size() + 1];
		strcpy(out_device.product_uniqueid, device.product_uniqueid.c_str());
		size_t len2 = device.device_capabilities.size();
		out_device.sz_device_capabilities = len2;
		out_device.device_capabilities = new RtcVideoDeviceCapability[len2];
		for (size_t j = 0; j < len2; ++j) {
			const auto& device_capability = device.device_capabilities[j];
			auto& out_device_capability = out_device.device_capabilities[j];
			out_device_capability.width = device_capability.width;
			out_device_capability.height = device_capability.height;
			out_device_capability.max_fps = device_capability.max_fps;
			// TO DO
			//VideoType video_type;
		}
	}
	return RtcErrorCode::OK;
}

void RtcDestoryVideoDevices(RtcVideoDevices video_devices, size_t sz_video_devices) {
	for (size_t i = 0; i < sz_video_devices; ++i) {
		if (video_devices[i].device_name) {
			delete[] video_devices[i].device_name;
			video_devices[i].device_name = nullptr;
		}

		if (video_devices[i].device_uniqueid) {
			delete[] video_devices[i].device_uniqueid;
			video_devices[i].device_uniqueid = nullptr;
		}

		if (video_devices[i].product_uniqueid) {
			delete[] video_devices[i].product_uniqueid;
			video_devices[i].product_uniqueid = nullptr;
		}

		if (video_devices[i].device_capabilities) {
			delete[] video_devices[i].device_capabilities;
			video_devices[i].device_capabilities = nullptr;
		}
	}

	delete[] video_devices;
}

RtcErrorCode RtcAddDataChannel(RtcDataChannelLabel label, RtcDataChannelPriority priority, bool ordered, int max_retransmits) {
	CHECK_RTCAGENT_INITED

	return rtc_agent->AddDataChannel(std::string(label), static_cast<vts_rtc::DataChannelPriority>(priority),
		ordered, max_retransmits) ? RtcErrorCode::OK : RtcErrorCode::Failed;
}

RtcErrorCode RtcAddDeviceVideoSource(size_t device_index, const RtcVideoDeviceCapability* in_device_capability) {
	CHECK_RTCAGENT_INITED

	vts_rtc::VideoDeviceCapability device_capability {
		in_device_capability->width,
		in_device_capability->height,
		in_device_capability->max_fps
	};

	return rtc_agent->AddVideoSource(device_index, device_capability) ? RtcErrorCode::OK : RtcErrorCode::Failed;
}

RtcErrorCode RtcAddExternalVideoSource(RtcVideoSourceId video_sourceid) {
	CHECK_RTCAGENT_INITED

	return rtc_agent->AddVideoSource(std::string(video_sourceid)) ? RtcErrorCode::OK : RtcErrorCode::Failed;
}

RtcErrorCode RtcQueryRoom(const RtcRoomId roomid, RtcRoom* out_room) {
	CHECK_RTCAGENT_INITED

	vts_rtc::Room room;
	auto roomcode = rtc_agent->QueryRoom(std::string(roomid), room);
	if (roomcode == vts_rtc::RoomCode::OK) {
		out_room->roomid = new char[strlen(roomid) + 1];
		strcpy(out_room->roomid, roomid);
		out_room->room_type = static_cast<RtcRoomType>(room.room_type);
		auto len = room.sessionids.size();
		out_room->sz_sessionids = len;
		out_room->sessionids = new RtcSessionId[len];
		for (size_t i = 0; i < len; ++i) {
			out_room->sessionids[i] = room.sessionids[i];
		}
		out_room->broadcaster_sessionid = room.broadcaster_sessionid;
	}
	return ConvertCode(roomcode);
}

void RtcDestoryRoom(RtcRoom room) {
	if (room.roomid) {
		delete[] room.roomid;
		room.roomid = nullptr;
	}

	if (room.sessionids) {
		delete[] room.sessionids;
		room.sessionids = nullptr;
	}
}

RtcErrorCode RtcQueryRooms(RtcRooms* out_rooms, size_t* out_sz_rooms) {
	CHECK_RTCAGENT_INITED

	vts_rtc::Rooms rooms;
	auto roomcode = rtc_agent->QueryRooms(rooms);
	if (roomcode == vts_rtc::RoomCode::OK) {
		auto len = rooms.size();
		*out_sz_rooms = len;
		*out_rooms = new RtcRoom[len];
		size_t cnt = 0;
		for (const auto& roomid_room : rooms) {
			const auto& room = roomid_room.second;
			auto& out_room = (*out_rooms)[cnt++];

			out_room.roomid = new char[room.roomid.size() + 1];
			strcpy(out_room.roomid, room.roomid.c_str());
			out_room.room_type = static_cast<RtcRoomType>(room.room_type);
			auto len2 = room.sessionids.size();
			out_room.sz_sessionids = len2;
			out_room.sessionids = new RtcSessionId[len2];
			for (size_t i = 0; i < len2; ++i) {
				out_room.sessionids[i] = room.sessionids[i];
			}
			out_room.broadcaster_sessionid = room.broadcaster_sessionid;
		}
	}
	return ConvertCode(roomcode);
}

void RtcDestoryRooms(RtcRooms rooms, size_t sz_rooms) {
	for (size_t i = 0; i < sz_rooms; ++i) {
		RtcDestoryRoom(rooms[i]);
	}
	delete[] rooms;
}

RtcErrorCode RtcOpenRoom(const RtcRoomId roomid, RtcRoomType room_type) {
	CHECK_RTCAGENT_INITED

	auto roomcode = rtc_agent->OpenRoom(std::string(roomid), static_cast<vts_rtc::RoomType>(room_type));
	return ConvertCode(roomcode);
}

RtcErrorCode RtcJoinRoom(const RtcRoomId roomid) {
	CHECK_RTCAGENT_INITED

	auto roomcode = rtc_agent->JoinRoom(std::string(roomid));
	return ConvertCode(roomcode);
}

RtcErrorCode RtcLeaveRoom() {
	CHECK_RTCAGENT_INITED

	auto roomcode = rtc_agent->LeaveRoom();
	return ConvertCode(roomcode);
}

RtcErrorCode RtcSendData(RtcDataChannelLabel channel_label, const char* msg) {
	CHECK_RTCAGENT_INITED

	return rtc_agent->SendData(std::string(channel_label), std::string(msg)) ? RtcErrorCode::OK : RtcErrorCode::Failed;
}

RtcErrorCode RtcSendFrame(RtcVideoSourceId video_sourceid, const RtcYUV420pFrame* in_video_frame) {
	CHECK_RTCAGENT_INITED

	vts_rtc::YUV420pFrame video_frame {
		in_video_frame->width,
		in_video_frame->height,
		in_video_frame->stride_Y,
		in_video_frame->stride_U,
		in_video_frame->stride_V,
		in_video_frame->buffer, // attention: no copy for performance
		in_video_frame->sz_buffer
	};

	rtc_agent->SendFrame(std::string(video_sourceid), video_frame);
	return RtcErrorCode::OK;
}
