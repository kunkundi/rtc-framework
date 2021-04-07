#include "rtc_connection.h"

RtcConnection::RtcConnection(vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid) 
	: local_sessionid_(local_sessionid), remote_sessionid_(remote_sessionid) {
	InitObserverCallbacks();
}

RtcConnection::~RtcConnection() {
	if (data_channel_) {
		data_channel_->UnregisterObserver();
		data_channel_->Close();
	}
	
	if (peer_conn_) {
		peer_conn_->Close();
	}
}

RtcConnection::PeerConnState RtcConnection::GetPeerConnectionState() const {
	if (peer_conn_) {
		return peer_conn_->peer_connection_state();
	}
	return PeerConnState::kClosed;
}

RtcConnection::DataChannelState RtcConnection::GetDataChannelState() const {
	if (data_channel_) {
		return data_channel_->state();
	}
	return DataChannelState::kClosed;
}

void RtcConnection::InitObserverCallbacks() {
	peer_conn_observer_.on_iceconnect_failed = [this]() {
		if (on_iceconnect_failed) {
			on_iceconnect_failed(remote_sessionid_);
		}
	};

	peer_conn_observer_.on_addtrack_ = [this](rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
		const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) {
			auto media_track = receiver->track();
			if (on_frame_received_ && media_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
				rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
					static_cast<webrtc::VideoTrackInterface*>(media_track.get()));
				if (video_track) {
					auto rtc_videosink = std::make_unique<RtcVideoSink>(video_track->id());
					rtc_videosink->on_frame_ = on_frame_received_;
					video_track->AddOrUpdateSink(rtc_videosink.get(), rtc::VideoSinkWants());
					rtc_pc_videosinks_.emplace_back(std::move(rtc_videosink));
				}
			}
	};

	peer_conn_observer_.on_data_channel_ = [this](rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
		data_channel_ = data_channel;
		data_channel_->RegisterObserver(&data_channel_observer_);
	};

	peer_conn_observer_.on_ice_candidate_ = [this](const webrtc::IceCandidateInterface* candidate) {
		if (on_ice_candidate_received_) {
			std::string candidate_str;
			candidate->ToString(&candidate_str);

			on_ice_candidate_received_(remote_sessionid_, std::make_tuple(candidate_str, candidate->sdp_mid(), candidate->sdp_mline_index()));
		}
	};

	data_channel_observer_.on_statechange = [this]() {
		LOG_INFO("[WEBRTC] Data channel on state change, new state: %s", webrtc::DataChannelInterface::DataStateString(GetDataChannelState()));
	};

	data_channel_observer_.on_message_ = [this](const webrtc::DataBuffer& buffer) {
		if (on_dc_message_received_) {
			on_dc_message_received_(remote_sessionid_, std::string(buffer.data.data<char>(), buffer.data.size()));
		}
	};

	create_sdp_observer_ = new rtc::RefCountedObject<CreateSessionDescriptionObserver>();
	create_sdp_observer_->on_success_ = [this](webrtc::SessionDescriptionInterface* desc) {
		if (on_create_sdp_succeed_) {
			std::string sdp;
			desc->ToString(&sdp);
			on_create_sdp_succeed_(remote_sessionid_, sdp);
		}
		peer_conn_->SetLocalDescription(set_sdp_observer_.get(), desc);
	};

	set_sdp_observer_ = new rtc::RefCountedObject<SetSessionDescriptionObserver>();

	set_remote_sdp_observer_ = new rtc::RefCountedObject<SetRemoteDescriptionObserver>();
}
