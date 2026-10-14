#pragma once

#include "log/log_manager.h"
#include "rtc_types.h"
#include <atomic>
#include <media/base/video_adapter.h>
#include <media/base/video_broadcaster.h>
#include <api/video/i420_buffer.h>
#include <api/media_stream_interface.h>
#include <rtc_base/time_utils.h>
#include <string>
#include <utility>

class RtcVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame> {
public:
	explicit RtcVideoSource(std::string source_id = std::string())
		: source_id_(std::move(source_id)) {}
	virtual ~RtcVideoSource() = default;

	static std::pair<int, int> FitFrameSize(int width, int height,
		int max_width, int max_height) {
		if (width <= 0 || height <= 0 || max_width <= 0 || max_height <= 0 ||
			(width <= max_width && height <= max_height)) {
			return {width, height};
		}
		int divisor = width;
		int remainder = height;
		while (remainder != 0) {
			const int next_remainder = divisor % remainder;
			divisor = remainder;
			remainder = next_remainder;
		}
		const int ratio_width = width / divisor;
		const int ratio_height = height / divisor;
		const int width_multiple = max_width / ratio_width;
		const int height_multiple = max_height / ratio_height;
		int multiple = width_multiple < height_multiple ? width_multiple : height_multiple;
		multiple -= multiple % 2;
		if (multiple == 0) {
			return {width, height};
		}
		return {ratio_width * multiple, ratio_height * multiple};
	}

	void OnFrame(const webrtc::VideoFrame& frame) {
		int cropped_width = 0, cropped_height = 0;
		int out_width = 0, out_height = 0;

		const bool frame_wanted = video_broadcaster_.frame_wanted();
		const bool adapted = frame_wanted &&
			video_adapter_.AdaptFrameResolution(
				frame.width(), frame.height(), frame.timestamp_us() * 1000,
				&cropped_width, &cropped_height, &out_width, &out_height);
		if (!frame_wanted || !adapted) {
			LogDroppedFrame(frame_wanted);
			return;
		}
		const auto output_size = FitFrameSize(frame.width(), frame.height(),
			out_width, out_height);
		out_width = output_size.first;
		out_height = output_size.second;

		if (out_width != frame.width() || out_height != frame.height()) {
			auto scaled_buffer = webrtc::I420Buffer::Create(out_width, out_height);
			scaled_buffer->ScaleFrom(*frame.video_frame_buffer()->ToI420());
			auto new_frame_builder = webrtc::VideoFrame::Builder()
				.set_video_frame_buffer(scaled_buffer)
				.set_rotation(webrtc::kVideoRotation_0)
				.set_timestamp_us(frame.timestamp_us())
				.set_id(frame.id())
				.set_timestamp_rtp(frame.timestamp())
				.set_ntp_time_ms(frame.ntp_time_ms());
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
	void LogDroppedFrame(bool frame_wanted) {
		if (frame_wanted) {
			adaptation_drop_count_.fetch_add(1, std::memory_order_relaxed);
		} else {
			no_sink_drop_count_.fetch_add(1, std::memory_order_relaxed);
		}

		const int64_t now_ms = rtc::TimeMillis();
		int64_t last_log_ms = last_drop_log_ms_.load(std::memory_order_relaxed);
		if (now_ms - last_log_ms < 1000 ||
			!last_drop_log_ms_.compare_exchange_strong(
				last_log_ms, now_ms, std::memory_order_relaxed)) {
			return;
		}

		const uint64_t adaptation_drops =
			adaptation_drop_count_.exchange(0, std::memory_order_relaxed);
		const uint64_t no_sink_drops =
			no_sink_drop_count_.exchange(0, std::memory_order_relaxed);
		LOG_WARN(
			"[WebRTC][source] id=%s dropped frames: adaptation=%llu no_sink=%llu",
			source_id_.empty() ? "unknown" : source_id_.c_str(),
			static_cast<unsigned long long>(adaptation_drops),
			static_cast<unsigned long long>(no_sink_drops));
	}

	cricket::VideoAdapter video_adapter_;
	rtc::VideoBroadcaster video_broadcaster_;
	std::string source_id_;
	std::atomic<uint64_t> adaptation_drop_count_{0};
	std::atomic<uint64_t> no_sink_drop_count_{0};
	std::atomic<int64_t> last_drop_log_ms_{0};
};
