#include "rtc_connection.h"

#include <utility>

/////////////////// BEGIN RtcConnectionBase ///////////////////
RtcConnectionBase::RtcConnectionBase() :
	logic_thread_(rtc::Thread::Current()) {
	InitObserverCallbacks();
}

RtcConnectionBase::~RtcConnectionBase() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	for (const auto& label_datachannel : label_datachannel_map_) {
		auto datachannel = label_datachannel.second;
		if (datachannel) {
			datachannel->UnregisterObserver();
			datachannel->Close();
		}
	}

	// TO DO
	// @attention: temporary code, otherwise crash, but not best solution
	for (auto iter = rtc_pc_videosinks_.begin();
		iter != rtc_pc_videosinks_.end(); ++iter) {
		(*iter)->on_frame_ = nullptr;
	}

	if (peer_conn_) {
		peer_conn_->Close();
	}
}

RtcConnectionBase::PeerConnState RtcConnectionBase::GetPeerConnectionState()
	const {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (peer_conn_) {
		return peer_conn_->peer_connection_state();
	}
	return PeerConnState::kClosed;
}

bool RtcConnectionBase::DataChannelExisted(const std::string& label) const {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (label_datachannel_map_.find(label) == label_datachannel_map_.cend()) {
		return false;
	}

	return true;
}

RtcConnectionBase::DataChannelState RtcConnectionBase::GetDataChannelState(
	const std::string& label) const {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (!DataChannelExisted(label)) {
		return DataChannelState::kClosed;
	}

	return label_datachannel_map_.at(label)->state();
}

bool RtcConnectionBase::AddDataChannel(const std::string& label,
	const webrtc::DataChannelInit& datachannelinit) {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (DataChannelExisted(label) || !peer_conn_) {
		return false;
	}

	auto datachannel = peer_conn_->CreateDataChannel(label, &datachannelinit);
	label_datachannel_map_[label] = datachannel;

	InitDataChannelObserverCallbacks(datachannel);

	return true;
}

bool RtcConnectionBase::SendData(const std::string& channel_label,
	const std::string& msg) {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (!DataChannelExisted(channel_label)) {
		LOG_ERROR("Send data failed, data channel (%s) not existed",
			channel_label.c_str());
		return false;
	}

	auto datachannel = label_datachannel_map_[channel_label];
	if (datachannel->state() != DataChannelState::kOpen) {
		LOG_ERROR("Send data failed, data channel (%s) not opened",
			channel_label.c_str());
		return false;
	}

	return datachannel->Send(webrtc::DataBuffer(msg));
}

void RtcConnectionBase::InitDataChannelObserverCallbacks(
	rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (!datachannel) {
		return;
	}

	auto observer = std::make_shared<DataChannelObserver>();
	datachannel_observers_.emplace_back(observer);
	datachannel->RegisterObserver(observer.get());

	observer->on_statechange = [datachannel]() {
		LOG_INFO("[WEBRTC] Data channel (%s) on state change, new state: %s",
			datachannel->label().c_str(),
			webrtc::DataChannelInterface::DataStateString(datachannel->state()));
	};

	observer->on_message_ = [this, datachannel](const webrtc::DataBuffer& buffer) {
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, datachannel, buffer]() {
				if (!datachannel) {
					return;
				}

				HandleDataChannelMessageReceived(datachannel->label(),
					std::string(buffer.data.data<char>(), buffer.data.size()));
			});
	};
}

void RtcConnectionBase::InitObserverCallbacks() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	peer_conn_observer_.on_iceconnect_failed =
		std::bind(&RtcConnectionBase::HandleIceConnectFailed, this);

	peer_conn_observer_.on_addtrack_ = [this](
		rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
		const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
		streams) {
			auto media_track = receiver->track();
			if (!media_track) {
				return;
			}

			if (media_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
				auto video_trackid =
					streams.size() > 0 ? streams[0]->id() : std::string("unknown");
				auto video_track =
					static_cast<webrtc::VideoTrackInterface*>(media_track.get());
				auto rtc_videosink = std::make_unique<RtcVideoSink>(video_trackid);
				rtc_videosink->on_frame_ = std::bind(
					&RtcConnectionBase::HandleFrameReceived, this,
					std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
					std::placeholders::_4, std::placeholders::_5);
				video_track->AddOrUpdateSink(rtc_videosink.get(), rtc::VideoSinkWants());
				rtc_pc_videosinks_.emplace_back(std::move(rtc_videosink));
			}
			else if (media_track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
				// TO DO
			}
	};

	peer_conn_observer_.on_removetrack_ = [](
		rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
		// TO DO
	};

	peer_conn_observer_.on_datachannel_ =
		[this](rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) {
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, datachannel]() {
				if (!datachannel) {
					return;
				}

				auto label = datachannel->label();
				if (DataChannelExisted(label)) {
					return;
				}

				label_datachannel_map_[label] = datachannel;

				InitDataChannelObserverCallbacks(datachannel);
			});
	};

	peer_conn_observer_.on_ice_candidate_ =
		[this](const webrtc::IceCandidateInterface* candidate) {
		std::string candidate_str;
		candidate->ToString(&candidate_str);

		HandleIceCandidateReceived(
			candidate_str, candidate->sdp_mid(), candidate->sdp_mline_index());
	};

	create_sdp_observer_ =
		new rtc::RefCountedObject<CreateSessionDescriptionObserver>();
	create_sdp_observer_->on_success_ =
		[this](webrtc::SessionDescriptionInterface* desc) {
		if (!desc) {
			return;
		}

		peer_conn_->SetLocalDescription(set_sdp_observer_.get(), desc);

		std::string sdp;
		desc->ToString(&sdp);
		HandleSdpCreateSucceed(sdp);
	};

	set_sdp_observer_ =
		new rtc::RefCountedObject<SetSessionDescriptionObserver>();

	set_remote_sdp_observer_ =
		new rtc::RefCountedObject<SetRemoteDescriptionObserver>();
}
/////////////////// END RtcConnectionBase ///////////////////


/////////////////// BEGIN RtcConnection ///////////////////
RtcConnection::RtcConnection(
	vts_rtc::SessionId local_sessionid, vts_rtc::SessionId remote_sessionid) :
	local_sessionid_(local_sessionid), remote_sessionid_(remote_sessionid) {
}

RtcConnection::~RtcConnection() {
}

void RtcConnection::HandleIceConnectFailed() const {
	if (on_iceconnect_failed) {
		on_iceconnect_failed(remote_sessionid_);
	}
}

void RtcConnection::HandleIceCandidateReceived(const std::string& candidate,
	const std::string& sdp_mid, int sdp_mline_index) const {
	if (on_ice_candidate_received_) {
		on_ice_candidate_received_(remote_sessionid_,
			std::make_tuple(candidate, sdp_mid, sdp_mline_index));
	}
}

void RtcConnection::HandleSdpCreateSucceed(const std::string& sdp) const {
	if (on_sdp_create_succeed_) {
		on_sdp_create_succeed_(remote_sessionid_, sdp);
	}
}

void RtcConnection::HandleDataChannelMessageReceived(
	const std::string& label, const std::string& message) const {
	if (on_dc_message_received_) {
		on_dc_message_received_(remote_sessionid_, label, message);
	}
}

void RtcConnection::HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(sourceid, vts_rtc::VideoSourceType::Rtc,
			width, height, dimension, buffer);
	}
}
/////////////////// END RtcConnection ///////////////////


/////////////////// BEGIN Rtc2SRSConnection ///////////////////
Rtc2SRSConnection::Rtc2SRSConnection(
	const vts_rtc::SRSStreamurl& SRS_streamurl) :
	SRS_streamurl_(SRS_streamurl) {
}

Rtc2SRSConnection::~Rtc2SRSConnection() {
	on_frame_received_ = nullptr;
}

void Rtc2SRSConnection::SetSRSSessionid(
	const vts_rtc::SRSSessionId& SRS_sessionid) {
	SRS_sessionid_ = SRS_sessionid;
}

void Rtc2SRSConnection::HandleIceConnectFailed() const {
	if (on_iceconnect_failed) {
		on_iceconnect_failed(SRS_streamurl_);
	}
}

void Rtc2SRSConnection::HandleIceCandidateReceived(
	const std::string& candidate, const std::string& sdp_mid, int sdp_mline_index)
	const {
}

void Rtc2SRSConnection::HandleSdpCreateSucceed(const std::string& sdp) const {
	if (on_sdp_create_succeed_) {
		on_sdp_create_succeed_(sdp);
	}
}

void Rtc2SRSConnection::HandleDataChannelMessageReceived(
	const std::string& label, const std::string& message) const {
	// TO DO
}

void Rtc2SRSConnection::HandleFrameReceived(
	const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(sourceid, vts_rtc::VideoSourceType::SRS,
			width, height, dimension, buffer);
	}
}
/////////////////// END Rtc2SRSConnection ///////////////////
