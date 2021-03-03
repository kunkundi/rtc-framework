#pragma once

#include "rtc_videocapturer.hpp"
#include <pc/video_track_source.h>

class RtcCameraCapturerTrackSource : public webrtc::VideoTrackSource {
public:
	static rtc::scoped_refptr<RtcCameraCapturerTrackSource> Create(const std::string& label, const std::string& device_uniqueid,
		const vts_rtc::VideoDeviceCapability& device_capability) {
		auto video_capturer = RtcVideoCapturer::Create(device_uniqueid, device_capability);
		if (!video_capturer) { return nullptr; }

		return new rtc::RefCountedObject<RtcCameraCapturerTrackSource>(label, std::move(video_capturer));
	}

	std::string GetLabel() const {
		return label_;
	}

	void SetLabel(const std::string& label) {
		label_ = label;
	}

protected:
	explicit RtcCameraCapturerTrackSource(const std::string& label, std::unique_ptr<RtcVideoCapturer> video_capturer)
		: webrtc::VideoTrackSource(false), label_(label), video_capturer_(std::move(video_capturer)) {
	}

private:
	rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
		return video_capturer_.get();
	}

private:
	std::string label_ = "camera_capturer";
	std::unique_ptr<RtcVideoCapturer> video_capturer_;
};

class RtcDeviceManager {
	using RtcCCTrackSources = std::vector<rtc::scoped_refptr<RtcCameraCapturerTrackSource>>;

public:
	static vts_rtc::VideoDevices GetVideoDevices();
	void AddVideoCapturer(size_t device_index, const vts_rtc::VideoDeviceCapability& device_capability);
	RtcCCTrackSources GetVideoTrackSources() const;

private:
	RtcCCTrackSources track_sources_;
};
