#pragma once

#include "log_manager.h"
#include <iostream>
#include <functional>
#include <api/peer_connection_interface.h>

class PeerConnectionObserver : public webrtc::PeerConnectionObserver {
	friend class RtcConnectionBase;
	using P2PSignalingState = webrtc::PeerConnectionInterface::SignalingState;
	using P2PIceGatheringState = webrtc::PeerConnectionInterface::IceGatheringState;
	using P2PPeerConnectionState = webrtc::PeerConnectionInterface::PeerConnectionState;
	using P2PIceConnectionState = webrtc::PeerConnectionInterface::IceConnectionState;

public:
	void ResetCallbacks() {
		on_P2PState_changed_ = nullptr;
		on_addtrack_ = nullptr;
		on_removetrack_ = nullptr;
		on_datachannel_ = nullptr;
		on_ice_candidate_ = nullptr;
	}

	// Triggered when the SignalingState changed.
	void OnSignalingChange(P2PSignalingState new_state) override {
		std::map<P2PSignalingState, const char*> state_map = {
			{ P2PSignalingState::kStable, "Stable" },
			{ P2PSignalingState::kHaveLocalOffer, "HaveLocalOffer" },
			{ P2PSignalingState::kHaveLocalPrAnswer, "HaveLocalPrAnswer" },
			{ P2PSignalingState::kHaveRemoteOffer, "HaveRemoteOffer" },
			{ P2PSignalingState::kHaveRemotePrAnswer, "HaveRemotePrAnswer" },
			{ P2PSignalingState::kClosed, "Closed" }
		};
		LOG_INFO("[WEBRTC] On signaling change, new state: %s", state_map[new_state]);
	}

	// Triggered when media is received on a new stream from remote peer.
	void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
		if (stream) { LOG_INFO("[WEBRTC] On add stream, streamid: %s", stream->id().c_str()); }
	}

	// Triggered when a remote peer closes a stream.
	void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
		if (stream) { LOG_INFO("[WEBRTC] On remove stream, streamid: %s", stream->id().c_str()); }
	}

	void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, 
		const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {
		if (receiver) {
			auto media_track = receiver->track();
			if (media_track) {
				LOG_INFO("[WEBRTC] On add track, receiverid: %s, media trackid: %s", receiver->id().c_str(), media_track->id().c_str());
			}
			else {
				LOG_INFO("[WEBRTC] On add track, receiverid: %s", receiver->id().c_str());
			}
		}
		
		if (on_addtrack_) { on_addtrack_(receiver, streams); }
	}

	void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
		LOG_INFO("[WEBRTC] On track");
	}

	void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {
		if (receiver) {
			auto media_track = receiver->track();
			if (media_track) {
				LOG_INFO("[WEBRTC] On remove track, receiverid: %s, media trackid: %s", receiver->id().c_str(), media_track->id().c_str());
			}
			else {
				LOG_INFO("[WEBRTC] On remove track, receiverid: %s", receiver->id().c_str());
			}
		}

		if (on_removetrack_) { on_removetrack_(receiver); }
	}

	// Triggered when a remote peer opens a data channel.
	void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) override {	
		if (datachannel) {
			LOG_INFO("[WEBRTC] On data channel, id: %d, label: %s, protocol: %s", 
				datachannel->id(), datachannel->label().c_str(), datachannel->protocol().c_str());

			if (on_datachannel_) { on_datachannel_(datachannel); }
		}
	}

	void OnRenegotiationNeeded() override {
		LOG_INFO("[WEBRTC] On renegotiation needed");
	}

	// Called any time the IceGatheringState changes.
	void OnIceGatheringChange(P2PIceGatheringState new_state) override {
		std::map<P2PIceGatheringState, const char*> state_map = {
			{ P2PIceGatheringState::kIceGatheringNew, "IceGatheringNew" },
			{ P2PIceGatheringState::kIceGatheringGathering, "IceGatheringGathering" },
			{ P2PIceGatheringState::kIceGatheringComplete, "IceGatheringComplete" }
		};
		LOG_INFO("[WEBRTC] On ICE gathering change, new state: %s", state_map[new_state]);
	}

	void OnStandardizedIceConnectionChange(P2PIceConnectionState new_state) override {
		std::map<P2PIceConnectionState, const char*> state_map = {
			{ P2PIceConnectionState::kIceConnectionNew, "IceConnectionNew" },
			{ P2PIceConnectionState::kIceConnectionChecking, "IceConnectionChecking" },
			{ P2PIceConnectionState::kIceConnectionConnected, "IceConnectionConnected" },
			{ P2PIceConnectionState::kIceConnectionCompleted, "IceConnectionCompleted" },
			{ P2PIceConnectionState::kIceConnectionFailed, "IceConnectionFailed" },
			{ P2PIceConnectionState::kIceConnectionDisconnected, "IceConnectionDisconnected" },
			{ P2PIceConnectionState::kIceConnectionClosed, "IceConnectionClosed" },
			{ P2PIceConnectionState::kIceConnectionMax, "IceConnectionMax" },
		};
		LOG_INFO("[WEBRTC] On standardized iceconnection change, new state: %s", state_map[new_state]);
	}

	// A new ICE candidate has been gathered.
	void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
		if (candidate) {
			LOG_INFO("[WEBRTC] On ICE candidate, server url: %s, sdp_mid: %s, sdp_mline_index: %d", 
				candidate->server_url().c_str(), candidate->sdp_mid().c_str(), candidate->sdp_mline_index());

			if (on_ice_candidate_) { on_ice_candidate_(candidate); }
		}
	}

	void OnIceCandidateError(const std::string& host_candidate, const std::string& url, int error_code, const std::string& error_text) override {
		LOG_ERROR("[WEBRTC] On ICE candidate error, host_candidate: %s, url: %s, error_code: %d, error_text: %s", 
			host_candidate.c_str(), url.c_str(), error_code, error_text.c_str());
	}

	void OnIceCandidateError(const std::string& address, int port, const std::string& url, int error_code, const std::string& error_text) override {
		LOG_ERROR("[WEBRTC] On ICE candidate error, address: %s, port: %d, url: %s, error_code: %d, error_text: %s",
			address.c_str(), port, url.c_str(), error_code, error_text.c_str());
	}

	void OnConnectionChange(P2PPeerConnectionState new_state) override {
		std::map<P2PPeerConnectionState, const char*> state_map = {
			{ P2PPeerConnectionState::kNew, "New" },
			{ P2PPeerConnectionState::kConnecting, "Connecting" },
			{ P2PPeerConnectionState::kConnected, "Connected" },
			{ P2PPeerConnectionState::kDisconnected, "Disconnected" },
			{ P2PPeerConnectionState::kFailed, "Failed" },
			{ P2PPeerConnectionState::kClosed, "Closed" }
		};
		LOG_INFO("[WEBRTC] On connection change, new state: %s", state_map[new_state]);

		if (on_P2PState_changed_) {
			on_P2PState_changed_(new_state);
		}
	}

private:
	std::function<void(P2PPeerConnectionState)> on_P2PState_changed_ = nullptr;
	std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>, 
		const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&)> on_addtrack_ = nullptr;
	std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>)> on_removetrack_ = nullptr;
	std::function<void(rtc::scoped_refptr<webrtc::DataChannelInterface>)> on_datachannel_ = nullptr;
	std::function<void(const webrtc::IceCandidateInterface*)> on_ice_candidate_ = nullptr;
};

class DataChannelObserver : public webrtc::DataChannelObserver {
	friend class RtcConnectionBase;

public:
	void ResetCallbacks() {
		on_statechange = nullptr;
		on_message_ = nullptr;
	}

	// The data channel state have changed.
	void OnStateChange() override {
		if (on_statechange) { on_statechange(); }
	}

	//  A data buffer was successfully received.
	void OnMessage(const webrtc::DataBuffer& buffer) override {
		LOG_INFO("[WEBRTC] Data channel on message, buffer size: %d", buffer.size());

		if (on_message_) { on_message_(buffer); }
	}

	// The data channel's buffered_amount has changed.
	void OnBufferedAmountChange(uint64_t sent_data_size) override {
		LOG_INFO("[WEBRTC] Data channel on buffered amount change, sent data size: %llu", sent_data_size);
	}

private:
	std::function<void()> on_statechange = nullptr;
	std::function<void(const webrtc::DataBuffer&)> on_message_ = nullptr;
};

// Create SessionDescription events.
class CreateSessionDescriptionObserver : public webrtc::CreateSessionDescriptionObserver {
	friend class RtcConnectionBase;

public:
	void ResetCallbacks() {
		on_success_ = nullptr;
	}

	// Successfully created a session description.
	void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
		LOG_INFO("[WEBRTC] Create SDP on success, sessionid: %s, session version: %s", desc->session_id().c_str(), desc->session_version().c_str());

		if (on_success_) { on_success_(desc); }
	}

	// Failure to create a session description.
	void OnFailure(webrtc::RTCError error) override {
		LOG_ERROR("[WEBRTC] Create SDP on failure, error message: %s", error.message());
	}

private:
	std::function<void(webrtc::SessionDescriptionInterface*)> on_success_ = nullptr;
};

// Set SessionDescription events.
class SetSessionDescriptionObserver : public webrtc::SetSessionDescriptionObserver {
public:
	void ResetCallbacks() {}

	// Successfully set a session description.
	void OnSuccess() override {
		LOG_INFO("[WEBRTC] Set SDP on success");
	}

	// Failure to set a sesion description.
	void OnFailure(webrtc::RTCError error) override {
		LOG_ERROR("[WEBRTC] Set SDP on failure, error message: %s", error.message());
	}
};

class SetRemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
public:
	void ResetCallbacks() {}

	void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
		LOG_INFO("[WEBRTC] Set remote SDP on complete, error message: %s", error.message());
	}
};
