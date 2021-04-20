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
	bool DataChannelExisted(const std::string& label) const;
	DataChannelState GetDataChannelState(const std::string& label) const;
	bool AddDataChannel(const std::string& label, const webrtc::DataChannelInit& datachannelinit);
	bool SendData(const std::string& channel_label, const std::string& msg);

private:
	void InitDataChannelObserverCallbacks(rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel);
	void InitObserverCallbacks();

private:
	// local peer sessionid and remote peer sessionid
	vts_rtc::SessionId local_sessionid_;
	vts_rtc::SessionId remote_sessionid_;
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn_ = nullptr;
	std::map<std::string, rtc::scoped_refptr<webrtc::DataChannelInterface>> label_datachannel_map_;
	std::vector<std::unique_ptr<RtcVideoSink>> rtc_pc_videosinks_;

	PeerConnectionObserver peer_conn_observer_;
	std::vector<std::shared_ptr<DataChannelObserver>> datachannel_observers_;
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
