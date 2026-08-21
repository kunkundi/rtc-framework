#include "rtc_connection.h"
#include "api/data_channel_interface.h"
#include "pc/data_channel.h"
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
	rtc_channel_stats_observer_->ResetCallbacks();

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

	PackDestory();

	if (peer_conn_) {
		LOG_WARN("Close peer connection");
		peer_conn_->Close();
		peer_conn_ = nullptr;
	}

	rtc_pc_videosinks_.clear();
	rtc_pc_audiosinks_.clear();
	datachannel_observers_.clear();
	label_datachannel_map_.clear();
	
	for (auto iter = label_ztchannel_map_.begin(); iter != label_ztchannel_map_.end();)
	{
		free(iter->second);
		label_ztchannel_map_.erase(iter++);
	}
	label_ztchannel_map_.clear();
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

void RtcConnectionBase::PacketsCallback(char* packet, unsigned int size, PackUserParams* params)
{
	auto datachannel = rtc::scoped_refptr<webrtc::DataChannelInterface>((webrtc::DataChannel*)params->ptr1);

	if (datachannel->buffered_amount() + size >= 16 * 1024 * 1024)
	{
		PackResend(packet, size, params);
		((RtcConnectionBase*)(params->ptr2))->resend_times_++;
	}
	else
	{
		datachannel->Send(webrtc::DataBuffer(std::string(packet, size)));
		if (((RtcConnectionBase*)(params->ptr2))->resend_times_)
		{
			LOG_INFO("datachannel[%s] in congestion, resend successfully after retry %d times", datachannel->label().c_str(), ((RtcConnectionBase*)(params->ptr2))->resend_times_);
			((RtcConnectionBase*)(params->ptr2))->resend_times_ = 0;
		}
	}
}

void RtcConnectionBase::DataCallback(char* data, unsigned int size, PackUserParams* params)
{
	static_cast<RtcConnection*>(params->ptr2)->HandleDataChannelMessageReceived(rtc::scoped_refptr<webrtc::DataChannelInterface>((webrtc::DataChannel*)params->ptr1)->label(),
		std::string(data, size));
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

	if (bPacked_) {
		auto ret = PackSend(msg.c_str(), msg.length(), label_ztchannel_map_[channel_label]);
		return ret == PACK_OK ? true : false;
	}
	else {
		if (msg.size() > 256 * 1024) {
			LOG_ERROR("Datachannel [%s] send data failed, msg size is larger than 256KiB",
				channel_label.c_str());
			return false;
		}

		return datachannel->Send(webrtc::DataBuffer(msg));
	}
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

	PackInit();

	label_ztchannel_map_[datachannel->label().c_str()] = (PackUserParams*)malloc(sizeof(PackUserParams));
	label_ztchannel_map_[datachannel->label().c_str()]->packet_cb = PacketsCallback;
	label_ztchannel_map_[datachannel->label().c_str()]->data_cb = DataCallback;
	label_ztchannel_map_[datachannel->label().c_str()]->ptr1 = (void*)datachannel;
	label_ztchannel_map_[datachannel->label().c_str()]->ptr2 = (void*)this;

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

				auto dc_state = datachannel->state();
				LOG_INFO("[WEBRTC] Data channel (%s) on state change, new state: %s, error msg: %s",
					datachannel->label().c_str(),
					webrtc::DataChannelInterface::DataStateString(dc_state), datachannel->error().message());

				HandleDataChannelStateChanged(datachannel->label(), dc_state);
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

				if (bPacked_) {
					PackReceive((char*)buffer.data.data<char>(), buffer.data.size(), 
						label_ztchannel_map_[datachannel->label().c_str()]);
				}
				else {
					HandleDataChannelMessageReceived(datachannel->label(),
						std::string(buffer.data.data<char>(), buffer.data.size()));
				}
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
				if (!attached_media_track_ids_.insert(media_track->id()).second) {
					return;
				}

				std::string source_id = media_track->id();
				if (!streams.empty() && streams[0]) {
					source_id = streams[0]->id();
				} else {
					const std::vector<std::string> stream_ids = receiver->stream_ids();
					if (!stream_ids.empty()) {
						source_id = stream_ids[0];
					}
				}

				if (media_track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
					LOG_INFO("[WEBRTC] Remote audio track added: track=%s stream=%s",
						media_track->id().c_str(), source_id.c_str());
					if (!WantsAudioFrames()) {
						return;
					}
					auto audio_track =
						static_cast<webrtc::AudioTrackInterface*>(media_track.get());
					auto rtc_audiosink = std::make_unique<RtcAudioSink>(source_id);
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
					if (!WantsVideoFrames()) {
						return;
					}
					auto video_track =
						static_cast<webrtc::VideoTrackInterface*>(media_track.get());
					auto rtc_videosink = std::make_unique<RtcVideoSink>(source_id);
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

	rtc_channel_stats_observer_ = new rtc::RefCountedObject<RtcChannelStatsObserver>();
	rtc_channel_stats_observer_->on_stats_deliverd_ =
		[this, weak_self](const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
		auto self = weak_self.lock();
		if (!self) {
			LOG_WARN("[Stats] RtcChannelStatsObserver: weak_self is null");
			return;
		}

		logic_thread_->PostTask(RTC_FROM_HERE,
			[this, weak_self, report]() {
				auto self = weak_self.lock();
				if (!self) {
					LOG_ERROR("[WEBRTC] Report stats failed, "
						"because rtc connection has been destroyed.");
					return;
				}

				HandleNetStatsReport(report);
			});
		};
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
		LOG_INFO("P2P state changed, local sessionid: %d, "
			"remote sessionid: %d, state: %d",
			local_sessionid_, remote_sessionid_, state);
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
		LOG_INFO("Data channel state changed, local sessionid: %d, "
			"remote sessionid: %d, state: %d",
			local_sessionid_, remote_sessionid_, state);
		on_dc_state_changed_(remote_sessionid_, label,
			static_cast<vts_rtc::DataChannelState>(state));
	}
}

void RtcConnection::HandleNetStatsReport(
	const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) const {
	if (on_net_stats_report_) {
		on_net_stats_report_(report);
	} else {
		LOG_WARN("[Stats] HandleNetStatsReport: on_net_stats_report_ callback is not set!");
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
		on_audioframe_received_(remote_sessionid_, sourceid, vts_rtc::MediaSourceType::Rtc,
			bits_per_sample, sample_rate, number_of_channels, number_of_frames,
			audio_data);
	}
}

void RtcConnection::HandleFrameReceived(const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(remote_sessionid_, sourceid, vts_rtc::MediaSourceType::Rtc,
			width, height, dimension, buffer);
	}
}

bool RtcConnection::WantsAudioFrames() const {
	return static_cast<bool>(on_audioframe_received_);
}

bool RtcConnection::WantsVideoFrames() const {
	return static_cast<bool>(on_frame_received_);
}
/////////////////// END RtcConnection ///////////////////


/////////////////// BEGIN Rtc2SRSConnection ///////////////////
Rtc2SRSConnection::Rtc2SRSConnection(
	const vts_rtc::SRSStreamurl& SRS_streamurl,
	vts_rtc::SessionId local_sessionid, 
	vts_rtc::SessionId remote_sessionid) :
	SRS_streamurl_(SRS_streamurl), 
	local_sessionid_(local_sessionid), 
	remote_sessionid_(remote_sessionid) {
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

void Rtc2SRSConnection::HandleNetStatsReport(
	const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) const {
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
		on_audioframe_received_(remote_sessionid_, sourceid, vts_rtc::MediaSourceType::SRS,
			bits_per_sample, sample_rate, number_of_channels, number_of_frames,
			audio_data);
	}
}

void Rtc2SRSConnection::HandleFrameReceived(
	const vts_rtc::VideoSourceId& sourceid,
	size_t width, size_t height, size_t dimension,
	const std::vector<unsigned char>& buffer) const {
	if (on_frame_received_) {
		on_frame_received_(remote_sessionid_, sourceid, vts_rtc::MediaSourceType::SRS,
			width, height, dimension, buffer);
	}
}

bool Rtc2SRSConnection::WantsAudioFrames() const {
	return static_cast<bool>(on_audioframe_received_);
}

bool Rtc2SRSConnection::WantsVideoFrames() const {
	return static_cast<bool>(on_frame_received_);
}
/////////////////// END Rtc2SRSConnection ///////////////////
