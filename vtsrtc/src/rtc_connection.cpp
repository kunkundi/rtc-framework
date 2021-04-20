#include "rtc_connection.h"

RtcConnection::RtcConnection(vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid) 
	: local_sessionid_(local_sessionid), remote_sessionid_(remote_sessionid) {
	InitObserverCallbacks();
}

RtcConnection::~RtcConnection() {
	for (const auto& label_datachannel : label_datachannel_map_) {
		auto datachannel = label_datachannel.second;
		if (datachannel) {
			datachannel->UnregisterObserver();
			datachannel->Close();
		}
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

bool RtcConnection::DataChannelExisted(const std::string& label) const {
	if (label_datachannel_map_.find(label) == label_datachannel_map_.cend()) {
		return false;
	}
	
	return true;
}

RtcConnection::DataChannelState RtcConnection::GetDataChannelState(const std::string& label) const {
	if (!DataChannelExisted(label)) {
		return DataChannelState::kClosed;
	}

	return label_datachannel_map_.at(label)->state();
}

bool RtcConnection::AddDataChannel(const std::string& label, const webrtc::DataChannelInit& datachannelinit) {
	if (DataChannelExisted(label) || !peer_conn_) {
		return false;
	}

	auto datachannel = peer_conn_->CreateDataChannel(label, &datachannelinit);
	label_datachannel_map_[label] = datachannel;

	InitDataChannelObserverCallbacks(datachannel);

	return true;
}

bool RtcConnection::SendData(const std::string& channel_label, const std::string& msg) {
	if (!DataChannelExisted(channel_label)) {
		LOG_ERROR("Send data failed, data channel (%s) not existed", channel_label.c_str());
		return false;
	}

	auto datachannel = label_datachannel_map_[channel_label];
	if (datachannel->state() != DataChannelState::kOpen) {
		LOG_ERROR("Send data failed, data channel (%s) not opened", channel_label.c_str());
		return false;
	}

	return datachannel->Send(webrtc::DataBuffer(msg));
}

void RtcConnection::InitDataChannelObserverCallbacks(rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) {
	if (!datachannel) {
		return;
	}

	auto observer = std::make_shared<DataChannelObserver>();
	datachannel_observers_.emplace_back(observer);
	datachannel->RegisterObserver(observer.get());

	observer->on_statechange = [this, datachannel]() {
		LOG_INFO("[WEBRTC] Data channel (%s) on state change, new state: %s", datachannel->label().c_str(),
			webrtc::DataChannelInterface::DataStateString(datachannel->state()));
	};

	observer->on_message_ = [this, datachannel](const webrtc::DataBuffer& buffer) {
		if (on_dc_message_received_) {
			on_dc_message_received_(remote_sessionid_, datachannel->label(), 
				std::string(buffer.data.data<char>(), buffer.data.size()));
		}
	};
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

	peer_conn_observer_.on_datachannel_ = [this](rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) {
		if (!datachannel) {
			return;
		}

		auto label = datachannel->label();
		if (DataChannelExisted(label)) {
			return;
		}

		label_datachannel_map_[label] = datachannel;

		InitDataChannelObserverCallbacks(datachannel);
	};

	peer_conn_observer_.on_ice_candidate_ = [this](const webrtc::IceCandidateInterface* candidate) {
		if (on_ice_candidate_received_) {
			std::string candidate_str;
			candidate->ToString(&candidate_str);

			on_ice_candidate_received_(remote_sessionid_, std::make_tuple(candidate_str, candidate->sdp_mid(), candidate->sdp_mline_index()));
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
