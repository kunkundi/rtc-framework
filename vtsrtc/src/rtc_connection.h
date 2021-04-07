#pragma once

#include "rtc_types.h"
#include "observer.hpp"
#include "rtc_videosink.hpp"
#include <api/peer_connection_interface.h>

class RtcConnection {
	friend class RtcConnectionManager;
	using PeerConnState = webrtc::PeerConnectionInterface::PeerConnectionState;
	using DataChannelState = webrtc::DataChannelInterface::DataState;

public:
	explicit RtcConnection(vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid);
	~RtcConnection();

	PeerConnState GetPeerConnectionState() const;
	DataChannelState GetDataChannelState() const;

private:
	void InitObserverCallbacks();

private:
	// local peer sessionid and remote peer sessionid
	vts_rtc::SessionId local_sessionid_;
	vts_rtc::SessionId remote_sessionid_;
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn_ = nullptr;
	// attention: only one DataChannel is supported for now
	rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel_ = nullptr;
	std::vector<std::unique_ptr<RtcVideoSink>> rtc_pc_videosinks_;

	PeerConnectionObserver peer_conn_observer_;
	DataChannelObserver data_channel_observer_;
	rtc::scoped_refptr<CreateSessionDescriptionObserver> create_sdp_observer_;
	rtc::scoped_refptr<SetSessionDescriptionObserver> set_sdp_observer_;
	rtc::scoped_refptr<SetRemoteDescriptionObserver> set_remote_sdp_observer_;

	std::function<void(vts_rtc::SessionId)> on_iceconnect_failed = nullptr;
	// "candidate", "sdpMid", "sdpMLineIndex" for std::tuple
	std::function<void(vts_rtc::SessionId, const std::tuple<std::string, std::string, int>&)> on_ice_candidate_received_ = nullptr;
	std::function<void(vts_rtc::SessionId, const std::string&)> on_create_sdp_succeed_ = nullptr;
	vts_rtc::RecvMessageHandler on_dc_message_received_ = nullptr;
	vts_rtc::RecvFrameHandler on_frame_received_ = nullptr;
};
