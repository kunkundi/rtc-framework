#pragma once

#include "rtc_types.h"

#include <api/audio_options.h>
#include <api/media_stream_interface.h>
#include <api/notifier.h>
#include <api/scoped_refptr.h>

class RtcAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
public:
	explicit RtcAudioSource(const std::string& label,
		const cricket::AudioOptions& audio_options)
		: label_(label), options_(audio_options) {}

	virtual ~RtcAudioSource() = default;

	webrtc::MediaSourceInterface::SourceState state() const override {
		return webrtc::MediaSourceInterface::kLive;
	}

	bool remote() const override {
		return false;
	}

	void RegisterAudioObserver(AudioObserver* observer) override {
		// TO DO
	}

	void UnregisterAudioObserver(AudioObserver* observer) override {
		// TO DO
	}

	const cricket::AudioOptions options() const override {
		return options_;
	}

	void AddSink(webrtc::AudioTrackSinkInterface* sink) override {
		sinks_.emplace_back(sink);
	}

	void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override {
		sinks_.remove(sink);
	}

	void OnData(const vts_rtc::PCMData& pcmdata) {
		for (auto sink : sinks_) {
			sink->OnData(pcmdata.buffer,
				static_cast<int>(pcmdata.bits_per_sample),
				static_cast<int>(pcmdata.sample_rate),
				pcmdata.number_of_channels,
				pcmdata.number_of_frames,
				rtc::TimeMillis());
		}
	}

private:
	std::string label_;
	cricket::AudioOptions options_;
	std::list<webrtc::AudioTrackSinkInterface*> sinks_;
};

