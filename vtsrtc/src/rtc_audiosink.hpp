#pragma once

#include "rtc_types.h"
#include <api/media_stream_interface.h>
#include <string>

class RtcAudioSink : public webrtc::AudioTrackSinkInterface {
	friend class RtcConnectionBase;

public:
	explicit RtcAudioSink(const std::string& trackid)
		: trackid_(trackid) {}
	~RtcAudioSink() = default;

	void ResetCallbacks() {
		on_audioframe_ = nullptr;
	}

	// In this method, |absolute_capture_timestamp_ms|, when available, is
	// supposed to deliver the timestamp when this audio frame was originally
	// captured. This timestamp MUST be based on the same clock as
	// rtc::TimeMillis().
	void OnData(const void* audio_data,
		int bits_per_sample,
		int sample_rate,
		size_t number_of_channels,
		size_t number_of_frames,
		absl::optional<int64_t> absolute_capture_timestamp_ms) override {
		if (on_audioframe_) {
			on_audioframe_(trackid_, static_cast<size_t>(bits_per_sample),
				static_cast<size_t>(sample_rate), number_of_channels,
				number_of_frames, audio_data);
		}
	}

private:
	std::string trackid_;
	std::function<void(const vts_rtc::AudioSourceId&,
		size_t, size_t, size_t, size_t, const void*)> on_audioframe_ = nullptr;
};
