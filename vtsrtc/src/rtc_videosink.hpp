#pragma once

#include "rtc_types.h"
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>

class RtcVideoSink : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
	friend class RtcConnectionBase;

public:
	explicit RtcVideoSink(const std::string& trackid)
		: trackid_(trackid) {}
	~RtcVideoSink() = default;

	void OnFrame(const webrtc::VideoFrame& frame) override {
		if (!on_frame_) { return; }

		size_t current_width = static_cast<size_t>(frame.width());
		size_t current_height = static_cast<size_t>(frame.height());
		if (width_ != current_width || height_ != current_height) {
			width_ = current_width;
			height_ = current_height;
			buffer_.resize(width_ * height_ * kDimension);
		}

		webrtc::ConvertFromI420(frame, webrtc::VideoType::kBGRA, 0, buffer_.data());
		on_frame_(trackid_, width_, height_, kDimension, buffer_);
	}

	void OnDiscardedFrame() override {}

private:
	const size_t kDimension = 4;
	std::string trackid_;
	size_t width_ = 0, height_ = 0;
	std::vector<unsigned char> buffer_;
	std::function<void(const vts_rtc::VideoSourceId&, size_t, size_t, size_t,
		const std::vector<unsigned char>&)> on_frame_ = nullptr;
};
