#pragma once

#include <map>
#include <vector>
#include <tuple>
#include <memory>
#include <string>

#include <api/peer_connection_interface.h> // NOLINT

#include "rtc_types.h"
#include "observer.hpp"
#include "rtc_audiosink.hpp"
#include "rtc_videosink.hpp"
#include "packet/pack.h"

class RtcConnectionBase : public std::enable_shared_from_this<RtcConnectionBase> {
	friend class RtcConnectionManager;

protected:
	using PeerConnState = webrtc::PeerConnectionInterface::PeerConnectionState;
	using RtcDataChannelState = webrtc::DataChannelInterface::DataState;

public:
	RtcConnectionBase();
	virtual ~RtcConnectionBase();

	void InitObserverCallbacks();
	PeerConnState GetPeerConnectionState() const;
	bool DataChannelExisted(const std::string& label) const;
	RtcDataChannelState GetDataChannelState(const std::string& label) const;
	bool AddDataChannel(const std::string& label,
		const webrtc::DataChannelInit& datachannelinit);
	bool SendData(const std::string& channel_label, const std::string& msg);

	static void PacketsCallback(char* packet, unsigned int size, PackUserParams* params);
	static void DataCallback(char* data, unsigned int size, PackUserParams* params);

protected:
	virtual void HandleP2PStateChanged(PeerConnState state) const = 0;
	virtual void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const = 0;
	virtual void HandleSdpCreateSucceed(const std::string& sdp) const = 0;
	virtual void HandleDataChannelStateChanged(
		const std::string& label, RtcDataChannelState state) const = 0;
	virtual void HandleNetStatsReport(
		const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) const = 0;
	virtual void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const = 0;
	virtual void HandleAudioFrameReceived(
		const vts_rtc::AudioSourceId& sourceid, size_t bits_per_sample,
		size_t sample_rate, size_t number_of_channels, size_t number_of_frames,
		const void* audio_data) const = 0;
	virtual void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const = 0;

private:
	void InitDataChannelObserverCallbacks(
		rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel);

protected:
	rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn_ = nullptr;

private:
	// logic_thread_ is created in RtcAgent constructor method
	// @attention: call some method in logic_thread_ to avoid data
	// synchronization, mainly for peer_conn_, label_datachannel_map_ variable
	rtc::Thread* logic_thread_ = nullptr;

	std::map<std::string, rtc::scoped_refptr<webrtc::DataChannelInterface>>
		label_datachannel_map_;
	std::map<std::string, PackUserParams*>
		label_ztchannel_map_;
	unsigned int resend_times_ = 0;

	std::vector<std::unique_ptr<RtcVideoSink>> rtc_pc_videosinks_;
	std::vector<std::unique_ptr<RtcAudioSink>> rtc_pc_audiosinks_;

	PeerConnectionObserver peer_conn_observer_;
	std::vector<std::shared_ptr<DataChannelObserver>> datachannel_observers_;
	rtc::scoped_refptr<CreateSessionDescriptionObserver> create_sdp_observer_;
	rtc::scoped_refptr<SetSessionDescriptionObserver> set_sdp_observer_;
	rtc::scoped_refptr<SetRemoteDescriptionObserver> set_remote_sdp_observer_;
	rtc::scoped_refptr<RtcChannelStatsObserver> rtc_channel_stats_observer_;
};

class RtcConnection : public RtcConnectionBase {
	friend class RtcConnectionManager;

public:
	explicit RtcConnection(
		vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid);
	virtual ~RtcConnection();

	void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const override;

protected:
	void HandleP2PStateChanged(PeerConnState state) const override;
	void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const override;
	void HandleSdpCreateSucceed(const std::string& sdp) const override;
	void HandleDataChannelStateChanged(
		const std::string& label, RtcDataChannelState state) const override;
	// void HandleNetStatsReport(vts_rtc::MediaChannelType media_type,
	// 	const vts_rtc::MediaSourceId& media_sourceid, 
	// 	const vts_rtc::NetStats& net_stats_params) const override;
	void HandleNetStatsReport(
		const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) const override;
	void HandleAudioFrameReceived(const vts_rtc::AudioSourceId& sourceid,
		size_t bits_per_sample, size_t sample_rate, size_t number_of_channels,
		size_t number_of_frames, const void* audio_data) const override;
	void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const override;

private:
	// local peer sessionid and remote peer sessionid
	const vts_rtc::SessionId local_sessionid_;
	const vts_rtc::SessionId remote_sessionid_;

	vts_rtc::P2PStateHandler on_P2P_state_changed_ = nullptr;
	// "candidate", "sdpMid", "sdpMLineIndex" for std::tuple
	std::function<void(vts_rtc::SessionId, const std::tuple<
		std::string, std::string, int>&)> on_ice_candidate_received_ = nullptr;
	std::function<void(vts_rtc::SessionId, const std::string&)>
		on_sdp_create_succeed_ = nullptr;
	vts_rtc::DataChannelStateHandler on_dc_state_changed_ = nullptr;
	std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport>&)> on_net_stats_report_ = nullptr;
	vts_rtc::RecvMessageHandler on_dc_message_received_ = nullptr;
	vts_rtc::RecvAudioFrameHandler on_audioframe_received_ = nullptr;
	vts_rtc::RecvFrameHandler on_frame_received_ = nullptr;
};

// Support publish RTC to SRS service
class Rtc2SRSConnection : public RtcConnectionBase {
	friend class RtcConnectionManager;

public:
	explicit Rtc2SRSConnection(const vts_rtc::SRSStreamurl& SRS_streamurl);
	virtual ~Rtc2SRSConnection();

	void SetSRSSessionid(const vts_rtc::SRSSessionId& SRS_sessionid);
	vts_rtc::SRSSessionId GetSRSSessionid() { return SRS_sessionid_; };

protected:
	void HandleP2PStateChanged(PeerConnState state) const override;
	void HandleIceCandidateReceived(const std::string& candidate,
		const std::string& sdp_mid, int sdp_mline_index) const override;
	void HandleSdpCreateSucceed(const std::string& sdp) const override;
	void HandleDataChannelStateChanged(
		const std::string& label, RtcDataChannelState state) const override;
	void HandleNetStatsReport(
		const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) const override;
	void HandleDataChannelMessageReceived(
		const std::string& label, const std::string& message) const override;
	void HandleAudioFrameReceived(const vts_rtc::AudioSourceId& sourceid,
		size_t bits_per_sample, size_t sample_rate, size_t number_of_channels,
		size_t number_of_frames, const void* audio_data) const override;
	void HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
		size_t width, size_t height, size_t dimension,
		const std::vector<unsigned char>& buffer) const override;

private:
	vts_rtc::SRSSessionId SRS_sessionid_ = std::string("");
	vts_rtc::SRSStreamurl SRS_streamurl_ = std::string("");;

	std::function<void(const vts_rtc::SRSStreamurl&, vts_rtc::P2PState)>
		on_P2P_state_changed_ = nullptr;
	std::function<void(const std::string&)> on_sdp_create_succeed_ = nullptr;
	vts_rtc::RecvAudioFrameHandler on_audioframe_received_ = nullptr;
	vts_rtc::RecvFrameHandler on_frame_received_ = nullptr;
	// @attention: DataChannelMessageReceived is not supported for now
};
