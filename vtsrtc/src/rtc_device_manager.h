#pragma once

#include "rtc_videocapturer.hpp"
#include <pc/video_track_source.h>

class RtcCameraCapturerTrackSource : public webrtc::VideoTrackSource {
public:
	static rtc::scoped_refptr<RtcCameraCapturerTrackSource> Create(const std::string& label,
		const std::string& device_uniqueid,
		const vts_rtc::VideoDeviceCapability& device_capability,
		vts_rtc::PriorityType priority) {
		auto video_capturer = RtcVideoCapturer::Create(device_uniqueid, device_capability);
		if (!video_capturer) { return nullptr; }

		return new rtc::RefCountedObject<RtcCameraCapturerTrackSource>(
			label, std::move(video_capturer), priority);
	}

	std::string GetLabel() const {
		return label_;
	}

	void SetLabel(const std::string& label) {
		label_ = label;
	}

	vts_rtc::PriorityType GetPriority() const {
		return priority_;
	}

	void SetPriority(vts_rtc::PriorityType priority) {
		priority_ = priority;
	}

protected:
	explicit RtcCameraCapturerTrackSource(const std::string& label,
		std::unique_ptr<RtcVideoCapturer> video_capturer,
		vts_rtc::PriorityType priority)
		: webrtc::VideoTrackSource(false), label_(label),
		video_capturer_(std::move(video_capturer)), priority_(priority) {
	}

private:
	rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
		return video_capturer_.get();
	}

private:
	std::string label_ = "camera_capturer";
	std::unique_ptr<RtcVideoCapturer> video_capturer_;
	vts_rtc::PriorityType priority_ = vts_rtc::PriorityType::Low;
};

class RtcDeviceManager {
	using RtcCCTrackSources = std::vector<rtc::scoped_refptr<RtcCameraCapturerTrackSource>>;

public:
	static vts_rtc::VideoDevices GetVideoDevices();
	bool AddVideoCapturer(size_t device_index,
		const vts_rtc::VideoDeviceCapability& device_capability,
		vts_rtc::PriorityType priority);
	RtcCCTrackSources GetVideoTrackSources() const;

private:
	RtcCCTrackSources track_sources_;
};
