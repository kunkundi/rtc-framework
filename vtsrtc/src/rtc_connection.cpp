#include "rtc_connection.h"

#include <utility>

/////////////////// BEGIN RtcConnectionBase ///////////////////
RtcConnectionBase::RtcConnectionBase() :
	logic_thread_(rtc::Thread::Current()) {
}

RtcConnectionBase::~RtcConnectionBase() {
	RTC_DCHECK_RUN_ON(logic_thread_);

	// reset all observers callbacks
	set_remote_sdp_observer_->ResetCallbacks();
	set_sdp_observer_->ResetCallbacks();
	create_sdp_observer_->ResetCallbacks();
	for (const auto& observer : datachannel_observers_) {
		observer->ResetCallbacks();
	}
	peer_conn_observer_.ResetCallbacks();

	// reset videosinks and audiosinks callbacks
	for (const auto& videosink : rtc_pc_videosinks_) {
		videosink->ResetCallbacks();
	}
	for (const auto& audiosink : rtc_pc_audiosinks_) {
		audiosink->ResetCallbacks();
	}

	for (const auto& label_datachannel : label_datachannel_map_) {
		auto datachannel = label_datachannel.second;
		if (datachannel) {
			datachannel->UnregisterObserver();
			datachannel->Close();
		}
	}

	if (peer_conn_) {
		peer_conn_->Close();
		peer_conn_ = nullptr;
	}

	rtc_pc_videosinks_.clear();
	rtc_pc_audiosinks_.clear();
	datachannel_observers_.clear();
	label_datachannel_map_.clear();
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

RtcConnectionBase::RtcDataChannelState RtcConnectionBase::GetDataChannelState(
	const std::string& label) const {
	RTC_DCHECK_RUN_ON(logic_thread_);

	if (!DataChannelExisted(label)) {
		return RtcDataChannelState::kClosed;
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
	if (datachannel->state() != RtcDataChannelState::kOpen) {
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

	std::weak_ptr<RtcConnectionBase> weak_self = shared_from_this();
	// 注意：lambda的捕获是立刻发生的，而不是等到函数调用的时候
	observer->on_statechange = [this, weak_self, datachannel]() {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		// signaling线程
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, datachannel]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] Data channel state changed, "
						"but rtc connection has been destroyed.");
					return;
				}

				if (!datachannel) {
					return;
				}

				LOG_INFO("[WEBRTC] Data channel (%s) on state change, new state: %s",
					datachannel->label().c_str(),
					webrtc::DataChannelInterface::DataStateString(datachannel->state()));

				HandleDataChannelStateChanged(datachannel->label(),
					datachannel->state());
			});
	};

	observer->on_message_ = [this, weak_self, datachannel](const webrtc::DataBuffer& buffer) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, datachannel, buffer]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] Data channel message received, "
						"but rtc connection has been destroyed.");
					return;
				}

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

	std::weak_ptr<RtcConnectionBase> weak_self = shared_from_this();
	peer_conn_observer_.on_P2PState_changed_ = [this, weak_self](PeerConnState state) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, state]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] P2P state changed, "
						"but rtc connection has been destroyed.");
					return;
				}

				HandleP2PStateChanged(state);
			});
	};

	peer_conn_observer_.on_addtrack_ = [this, weak_self](
		rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver,
		const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>&
		streams) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		// 注意：receiver和streams的生命周期和peer_conn_绑定
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, receiver, streams]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] P2P on addtrack, "
						"but rtc connection has been destroyed.");
					return;
				}

				if (!receiver) {
					return;
				}

				auto media_track = receiver->track();
				if (!media_track) {
					return;
				}

				if (media_track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
					auto audio_trackid =
						streams.size() > 0 && streams[0] ? streams[0]->id() : std::string("unknown");
					auto audio_track =
						static_cast<webrtc::AudioTrackInterface*>(media_track.get());
					auto rtc_audiosink = std::make_unique<RtcAudioSink>(audio_trackid);
					rtc_audiosink->on_audioframe_ = [this, weak_self](
						const vts_rtc::AudioSourceId& sourceid, size_t bits_per_sample,
						size_t sample_rate, size_t number_of_channels, size_t number_of_frames,
						const void* audio_data) {
						// IncomingAudioStream线程
						auto self = weak_self.lock();
						if (!self) {
							LOG_ERROR("[WEBRTC] On audio frame, "
								"but rtc connection has been destroyed.");
							return;
						}

						HandleAudioFrameReceived(sourceid, bits_per_sample,
							sample_rate, number_of_channels, number_of_frames, audio_data);
					};
					audio_track->AddSink(rtc_audiosink.get());
					rtc_pc_audiosinks_.emplace_back(std::move(rtc_audiosink));
				}
				else if (media_track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
					auto video_trackid =
						streams.size() > 0 && streams[0] ? streams[0]->id() : std::string("unknown");
					auto video_track =
						static_cast<webrtc::VideoTrackInterface*>(media_track.get());
					auto rtc_videosink = std::make_unique<RtcVideoSink>(video_trackid);
					rtc_videosink->on_frame_ = [this, weak_self](
						const vts_rtc::VideoSourceId& sourceid, size_t width, size_t height, size_t dimension,
						const std::vector<unsigned char>& buffer) {
						// IncomingVideoStream线程
						auto self = weak_self.lock();
						if (!self) {
							LOG_ERROR("[WEBRTC] On video frame, "
								"but rtc connection has been destroyed.");
							return;
						}

						HandleFrameReceived(sourceid, width, height, dimension, buffer);
					};
					video_track->AddOrUpdateSink(rtc_videosink.get(), rtc::VideoSinkWants());
					rtc_pc_videosinks_.emplace_back(std::move(rtc_videosink));
				}
			});
	};

	peer_conn_observer_.on_removetrack_ = [](
		rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
		// TO DO
	};

	peer_conn_observer_.on_datachannel_ =
		[this, weak_self](rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		// 注意：datachannel的生命周期和peer_conn_绑定
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, datachannel]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] P2P on datachannel, "
						"but rtc connection has been destroyed.");
					return;
				}

				if (!datachannel) {
					return;
				}

				auto label = datachannel->label();
				if (DataChannelExisted(label)) {
					return;
				}

				label_datachannel_map_[label] = datachannel;

				// @attention: it means the datachannel is already open
				HandleDataChannelStateChanged(datachannel->label(),
							datachannel->state());

				InitDataChannelObserverCallbacks(datachannel);
			});
	};

	peer_conn_observer_.on_ice_candidate_ =
		[this, weak_self](const webrtc::IceCandidateInterface* candidate) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		if (!candidate) {
			return;
		}

		std::string candidate_str;
		candidate->ToString(&candidate_str);
		std::string sdp_mid = candidate->sdp_mid();
		int sdp_mline_idx = candidate->sdp_mline_index();

		// 注意：不能直接传递candidate变量，因为candidate生命周期指在当前回调有效
		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, candidate_str, sdp_mid, sdp_mline_idx] {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] P2P on ice candidate, "
						"but rtc connection has been destroyed.");
					return;
				}

				HandleIceCandidateReceived(
					candidate_str, sdp_mid, sdp_mline_idx);
			});
	};

	set_sdp_observer_ =
		new rtc::RefCountedObject<SetSessionDescriptionObserver>();

	create_sdp_observer_ =
		new rtc::RefCountedObject<CreateSessionDescriptionObserver>();
	create_sdp_observer_->on_success_ =
		[this, weak_self](webrtc::SessionDescriptionInterface* desc_ptr) {
		auto self = weak_self.lock();
		if (!self) {
			return;
		}

		if (!desc_ptr) {
			return;
		}

		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self,
				desc = std::unique_ptr<webrtc::SessionDescriptionInterface>(desc_ptr)]() mutable {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] Create SDP on success, "
						"but rtc connection has been destroyed.");
					return;
				}

				if (!desc || !peer_conn_) {
					return;
				}

				std::string sdp;
				desc->ToString(&sdp);  // 注意：需要在desc.release()之前运行，否则desc会失效
				peer_conn_->SetLocalDescription(set_sdp_observer_.get(), desc.release());
				HandleSdpCreateSucceed(sdp);
			});
	};

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

void RtcConnection::HandleP2PStateChanged(PeerConnState state) const {
	if (on_P2P_state_changed_) {
		on_P2P_state_changed_(remote_sessionid_,
			static_cast<vts_rtc::P2PState>(state));
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

void RtcConnection::HandleDataChannelStateChanged(
	const std::string& label, RtcDataChannelState state) const {
	if (on_dc_state_changed_) {
		on_dc_state_changed_(remote_sessionid_, label,
			static_cast<vts_rtc::DataChannelState>(state));
	}
}

void RtcConnection::HandleDataChannelMessageReceived(
	const std::string& label, const std::string& message) const {
	if (on_dc_message_received_) {
		on_dc_message_received_(remote_sessionid_, label, message);
	}
}

void RtcConnection::HandleAudioFrameReceived(
	const vts_rtc::AudioSourceId& sourceid, size_t bits_per_sample,
	size_t sample_rate, size_t number_of_channels, size_t number_of_frames,
	const void* audio_data) const {
	if (on_audioframe_received_) {
		on_audioframe_received_(sourceid, vts_rtc::MediaSourceType::Rtc,
			bits_per_sample, sample_rate, number_of_channels, number_of_frames,
			audio_data);
	}
}

void RtcConnection::HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(sourceid, vts_rtc::MediaSourceType::Rtc,
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

void Rtc2SRSConnection::HandleP2PStateChanged(PeerConnState state) const {
	if (on_P2P_state_changed_) {
		on_P2P_state_changed_(SRS_streamurl_,
			static_cast<vts_rtc::P2PState>(state));
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

void Rtc2SRSConnection::HandleDataChannelStateChanged(
	const std::string& label, RtcDataChannelState state) const {
	// TO DO
}

void Rtc2SRSConnection::HandleDataChannelMessageReceived(
	const std::string& label, const std::string& message) const {
	// TO DO
}

void Rtc2SRSConnection::HandleAudioFrameReceived(
	const vts_rtc::AudioSourceId& sourceid, size_t bits_per_sample,
	size_t sample_rate, size_t number_of_channels, size_t number_of_frames,
	const void* audio_data) const {
	if (on_audioframe_received_) {
		on_audioframe_received_(sourceid, vts_rtc::MediaSourceType::SRS,
			bits_per_sample, sample_rate, number_of_channels, number_of_frames,
			audio_data);
	}
}

void Rtc2SRSConnection::HandleFrameReceived(
	const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(sourceid, vts_rtc::MediaSourceType::SRS,
			width, height, dimension, buffer);
	}
}
/////////////////// END Rtc2SRSConnection ///////////////////
