#pragma once

#include <map>
#include <vector>
#include <tuple>
#include <memory>
#include <string>

#include <api/peer_connection_interface.h> // NOLINT

#include "rtc_types.h"
#include "observer.hpp"
#include "rtc_videosink.hpp"

class RtcConnectionBase {
	friend class RtcConnectionManager;
	using PeerConnState = webrtc::PeerConnectionInterface::PeerConnectionState;
	using DataChannelState = webrtc::DataChannelInterface::DataState;

public:
	RtcConnectionBase();
	virtual ~RtcConnectionBase();

	PeerConnState GetPeerConnectionState() const;
	bool DataChannelExisted(const std::string& label) const;
	DataChannelState GetDataChannelState(const std::string& label) const;
	bool AddDataChannel(const std::string& label,
		const webrtc::DataChannelInit& datachannelinit);
	bool SendData(const std::string& channel_label, const std::string& msg);

protected:
	virtual void HandleIceConnectFailed() const = 0;
	virtual void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const = 0;
	virtual void HandleSdpCreateSucceed(const std::string& sdp) const = 0;
	virtual void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const = 0;
	virtual void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const = 0;

private:
	void InitDataChannelObserverCallbacks(
		rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel);
	void InitObserverCallbacks();

protected:
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn_ = nullptr;

private:
	// logic_thread_ is created in RtcAgent constructor method
	// @attention: call some method in logic_thread_ to avoid data
	// synchronization, mainly for peer_conn_, label_datachannel_map_ variable
	rtc::Thread* logic_thread_ = nullptr;

	std::map<std::string, rtc::scoped_refptr<webrtc::DataChannelInterface>>
		label_datachannel_map_;
	std::vector<std::unique_ptr<RtcVideoSink>> rtc_pc_videosinks_;

	PeerConnectionObserver peer_conn_observer_;
	std::vector<std::shared_ptr<DataChannelObserver>> datachannel_observers_;
	rtc::scoped_refptr<CreateSessionDescriptionObserver> create_sdp_observer_;
	rtc::scoped_refptr<SetSessionDescriptionObserver> set_sdp_observer_;
	rtc::scoped_refptr<SetRemoteDescriptionObserver> set_remote_sdp_observer_;
};

class RtcConnection : public RtcConnectionBase {
	friend class RtcConnectionManager;

public:
	explicit RtcConnection(
		vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid);
	virtual ~RtcConnection();

protected:
	void HandleIceConnectFailed() const override;
	void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const override;
	void HandleSdpCreateSucceed(const std::string& sdp) const override;
	void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const override;
	void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const override;

private:
	// local peer sessionid and remote peer sessionid
	const vts_rtc::SessionId local_sessionid_;
	const vts_rtc::SessionId remote_sessionid_;

	std::function<void(vts_rtc::SessionId)> on_iceconnect_failed = nullptr;
	// "candidate", "sdpMid", "sdpMLineIndex" for std::tuple
	std::function<void(vts_rtc::SessionId, const std::tuple<
		std::string, std::string, int>&)> on_ice_candidate_received_ = nullptr;
	std::function<void(vts_rtc::SessionId, const std::string&)>
		on_sdp_create_succeed_ = nullptr;
	vts_rtc::RecvMessageHandler on_dc_message_received_ = nullptr;
	vts_rtc::RecvFrameHandler on_frame_received_ = nullptr;
};

// Support publish RTC to SRS service
class Rtc2SRSConnection : public RtcConnectionBase {
	friend class RtcConnectionManager;

public:
	explicit Rtc2SRSConnection(const vts_rtc::SRSStreamurl& SRS_streamurl);
	virtual ~Rtc2SRSConnection();

	void SetSRSSessionid(const vts_rtc::SRSSessionId& SRS_sessionid);

protected:
	void HandleIceConnectFailed() const override;
	void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const override;
	void HandleSdpCreateSucceed(const std::string& sdp) const override;
	void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const override;
	void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const override;

private:
	vts_rtc::SRSSessionId SRS_sessionid_ = std::string("");
	vts_rtc::SRSStreamurl SRS_streamurl_;

	std::function<void(const vts_rtc::SRSSessionId&)>
		on_iceconnect_failed = nullptr;
	std::function<void(const std::string&)> on_sdp_create_succeed_ = nullptr;
	vts_rtc::RecvFrameHandler on_frame_received_ = nullptr;
	// @attention: DataChannelMessageReceived is not supported for now
};
