#pragma once

#include "log_manager.h"
#include "rtc_videosource.hpp"
#include <modules/video_capture/video_capture_factory.h>
#include <iostream>

class RtcVideoCapturer : public RtcVideoSource, public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
	static std::unique_ptr<RtcVideoCapturer> Create(const std::string& device_uniqueid, 
		const vts_rtc::VideoDeviceCapability& device_capability) {
		auto video_capturer = std::unique_ptr<RtcVideoCapturer>(new RtcVideoCapturer());
		if (!video_capturer->Init(device_uniqueid, device_capability)) {
			return nullptr;
		}
		return std::move(video_capturer);
	}

	virtual ~RtcVideoCapturer() {
		this->Destroy();
	}

	void OnFrame(const webrtc::VideoFrame& frame) override {
		RtcVideoSource::OnFrame(frame);
	}

	void OnDiscardedFrame() override {
	}

private:
	RtcVideoCapturer() = default;

	bool Init(const std::string& device_uniqueid, const vts_rtc::VideoDeviceCapability& device_capability) {
		video_capture_module_ = webrtc::VideoCaptureFactory::Create(device_uniqueid.c_str());
		if (!video_capture_module_) {
			LOG_ERROR("Create video capture module failed");
			return false;
		}

		video_capture_module_->RegisterCaptureDataCallback(this);

		webrtc::VideoCaptureCapability requested_capability;
		requested_capability.width = static_cast<int32_t>(device_capability.width);
		requested_capability.height = static_cast<int32_t>(device_capability.height);
		requested_capability.maxFPS = static_cast<int32_t>(device_capability.max_fps);
		// TO DO
		requested_capability.videoType = webrtc::VideoType::kI420;

		if (video_capture_module_->StartCapture(requested_capability) != 0) {
			LOG_ERROR("Start video capture failed");
			this->Destroy();
			return false;
		}

		return true;
	}

	void Destroy() {
		if (!video_capture_module_) { return; }
		video_capture_module_->StopCapture();
		video_capture_module_->DeRegisterCaptureDataCallback();
		video_capture_module_ = nullptr;
	}

private:
	rtc::scoped_refptr<webrtc::VideoCaptureModule> video_capture_module_ = nullptr;
};