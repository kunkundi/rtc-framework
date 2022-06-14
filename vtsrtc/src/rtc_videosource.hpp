#pragma once

#include "rtc_types.h"
#include <media/base/video_adapter.h>
#include <media/base/video_broadcaster.h>
#include <api/video/i420_buffer.h>
#include <api/media_stream_interface.h>

class RtcVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
public:
	RtcVideoSource() = default;
	virtual ~RtcVideoSource() = default;

	void OnFrame(const webrtc::VideoFrame& frame) {
		int cropped_width = 0, cropped_height = 0;
		int out_width = 0, out_height = 0;

		if ((!video_adapter_.AdaptFrameResolution(frame.width(), frame.height(), frame.timestamp_us() * 1000,
			&cropped_width, &cropped_height, &out_width, &out_height)) || 
			(!video_broadcaster_.frame_wanted())){
			// Drop frame in order to respect frame rate constraint.
			return;
		}

		if (out_width != frame.width() || out_height != frame.height()) {
			// Video adapter has requested a down-scale. Allocate a new buffer and return scaled version.
			// For simplicity, only scale here without cropping.
			// TO DO
			auto scaled_buffer = webrtc::I420Buffer::Create(out_width, out_height);
			scaled_buffer->ScaleFrom(*frame.video_frame_buffer()->ToI420());
			auto new_frame_builder = webrtc::VideoFrame::Builder()
				.set_video_frame_buffer(scaled_buffer)
				.set_rotation(webrtc::kVideoRotation_0)
				.set_timestamp_us(frame.timestamp_us())
				.set_id(frame.id())
				.set_timestamp_rtp(frame.timestamp());
			if (frame.has_update_rect()) {
				auto new_rect = frame.update_rect().ScaleWithFrame(frame.width(), frame.height(),
					0, 0, frame.width(), frame.height(), out_width, out_height);
				new_frame_builder.set_update_rect(new_rect);
			}

			video_broadcaster_.OnFrame(new_frame_builder.build());
		}
		else {
			video_broadcaster_.OnFrame(frame);
		}
	}

	void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink, const rtc::VideoSinkWants& wants) override {
		video_broadcaster_.AddOrUpdateSink(sink, wants);
		video_adapter_.OnSinkWants(video_broadcaster_.wants());
	}

	void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
		video_broadcaster_.RemoveSink(sink);
		video_adapter_.OnSinkWants(video_broadcaster_.wants());
	}

private:
	cricket::VideoAdapter video_adapter_;
	rtc::VideoBroadcaster video_broadcaster_;
};
