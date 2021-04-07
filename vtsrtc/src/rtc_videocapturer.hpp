#pragma once

#include "log_manager.h"
#include "rtc_videosource.hpp"
#include <modules/video_capture/video_capture_factory.h>
#include <rtc_base/thread.h>

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
		vcm_thread_->Stop();
	}

	void OnFrame(const webrtc::VideoFrame& frame) override {
		RtcVideoSource::OnFrame(frame);
	}

	void OnDiscardedFrame() override {
	}

private:
	RtcVideoCapturer() : video_capture_module_(nullptr) {
		vcm_thread_ = rtc::Thread::Create();
		vcm_thread_->SetName("video_capture_module", nullptr);
		vcm_thread_->Start();
	}

	bool Init(const std::string& device_uniqueid, const vts_rtc::VideoDeviceCapability& device_capability) {
		if (video_capture_module_) {
			LOG_ERROR("Rtc video capturer have been inited already, cannot be inited again.");
			return false;
		}
		
		video_capture_module_ = vcm_thread_->Invoke<rtc::scoped_refptr<webrtc::VideoCaptureModule>>(RTC_FROM_HERE,
			[device_uniqueid]() {
				return webrtc::VideoCaptureFactory::Create(device_uniqueid.c_str());
			});

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

		auto ret = vcm_thread_->Invoke<int32_t>(RTC_FROM_HERE,
			[this, &requested_capability]() {
				return video_capture_module_->StartCapture(requested_capability);
			});


		if (ret != 0) {
			LOG_ERROR("Start video capture failed");
			this->Destroy();
			return false;
		}

		return true;
	}

	void Destroy() {
		if (!video_capture_module_) { return; }

		vcm_thread_->Invoke<int32_t>(RTC_FROM_HERE,
			[this]() {
				return video_capture_module_->StopCapture();
			});

		video_capture_module_->DeRegisterCaptureDataCallback();

		vcm_thread_->Invoke<void>(RTC_FROM_HERE,
			[this]() {
				video_capture_module_ = nullptr;
			});
	}

private:
	std::unique_ptr<rtc::Thread> vcm_thread_;
	rtc::scoped_refptr<webrtc::VideoCaptureModule> video_capture_module_;
};