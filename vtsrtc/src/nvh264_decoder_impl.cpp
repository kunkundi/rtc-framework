#include <cuda.h>

#include <absl/strings/match.h>
#include <system_wrappers/include/metrics.h>
#include <common_video/include/video_frame_buffer.h>
#include <third_party/libyuv/include/libyuv.h>

#include <limits>

#include "log_manager.h"
#include "nvh264_decoder_impl.h"

namespace webrtc {

static const int index_of_GPU = 0;

enum class H264DecoderImplEvent {
	H264DecoderEventInit = 0,
	H264DecoderEventError = 1,
	H264DecoderEventMax = 16,
};

NvH264DecoderImpl::NvH264DecoderImpl(const cricket::VideoCodec& codec) {
	RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));
}

NvH264DecoderImpl::~NvH264DecoderImpl() {
	Release();
}

int32_t NvH264DecoderImpl::InitDecode(const VideoCodec* codec_settings,
	int32_t number_of_cores) {
	ReportInit();

	if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	// Release necessary in case of re-initializing.
	int32_t ret = Release();
	if (ret != WEBRTC_VIDEO_CODEC_OK) {
		ReportError();
		return ret;
	}
	RTC_DCHECK(!cuda_context_);

	// Init cuda context
	int num_of_GPUs = 0;
	CUdevice cuda_device;
	bool cuda_ctx_succeed = (index_of_GPU >= 0 &&
		cuInit(0) == CUresult::CUDA_SUCCESS &&
		cuDeviceGetCount(&num_of_GPUs) == CUresult::CUDA_SUCCESS &&
		(num_of_GPUs > 0 && index_of_GPU < num_of_GPUs) &&
		cuDeviceGet(&cuda_device, index_of_GPU) == CUresult::CUDA_SUCCESS &&
		cuCtxCreate(&cuda_context_, 0, cuda_device) == CUresult::CUDA_SUCCESS);
	if (!cuda_ctx_succeed) {
		LOG_ERROR("Init CUDA context failed");
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	// Create Nvidia decoder
	width_ = codec_settings->width;
	height_ = codec_settings->height;
	nvh264_decoder_ = std::make_unique<NvDecoder>(
		cuda_context_, false, cudaVideoCodec::cudaVideoCodec_H264, true);

	if (codec_settings && codec_settings->buffer_pool_size) {
		if (!I420buffer_pool_.Resize(*codec_settings->buffer_pool_size)) {
			return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
		}
	}

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvH264DecoderImpl::Release() {
	cuda_context_ = nullptr;
	nvh264_decoder_.reset(nullptr);

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvH264DecoderImpl::Decode(const EncodedImage& input_image,
	bool missing_frames,
	int64_t render_time_ms) {
	if (input_image._encodedWidth > 0 &&
		input_image._encodedHeight > 0 &&
		(input_image._encodedWidth != width_ ||
			input_image._encodedHeight != height_)) {
		LOG_INFO("[WebRTC] input_image resolution changed from "
			"(%d x %d) to (%d x %d)", width_, height_,
			input_image._encodedWidth, input_image._encodedHeight);

		// To be improved
		width_ = input_image._encodedWidth;
		height_ = input_image._encodedHeight;
		nvh264_decoder_ = std::make_unique<NvDecoder>(
			cuda_context_, false, cudaVideoCodec::cudaVideoCodec_H264, true);
	}

	if (!nvh264_decoder_) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	if (!decoded_image_callback_) {
		LOG_ERROR("InitDecode() has been called, but a callback function "
			"has not been set with RegisterDecodeCompleteCallback()");
		ReportError();
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	if (!input_image.data() || !input_image.size()) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	if (input_image.size() >
		static_cast<size_t>(std::numeric_limits<int>::max())) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	auto output_format = nvh264_decoder_->GetOutputFormat();
	if (output_format == cudaVideoSurfaceFormat_P016 ||
		output_format == cudaVideoSurfaceFormat_YUV444_16Bit) {
		// TO DO: not implemented yet
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERROR;
	}


	h264_bitstream_parser_.ParseBitstream(input_image.data(), input_image.size());
	auto qp = h264_bitstream_parser_.GetLastSliceQp();

	int num_frame_returned = nvh264_decoder_->Decode(
		input_image.data(), input_image.size());
	uint8_t* nvdec_framebuf;
	for (size_t i = 0; i < num_frame_returned; ++i) {
		nvdec_framebuf = nvh264_decoder_->GetFrame();

		int frame_width = nvh264_decoder_->GetWidth();
		int frame_height = nvh264_decoder_->GetHeight();

		int dst_stride_Y = frame_width;
		int dst_stride_U = dst_stride_Y / 2;
		int dst_stride_V = dst_stride_U;

		auto I420buffer = I420buffer_pool_.CreateBuffer(
			frame_width, frame_height, dst_stride_Y, dst_stride_U, dst_stride_V);

		int converted_ret = -1;
		if (output_format == cudaVideoSurfaceFormat_NV12) {
			int src_stride_Y = frame_width;
			int src_stride_UV = src_stride_Y;

			converted_ret = libyuv::NV12ToI420(nvdec_framebuf, src_stride_Y,
				nvdec_framebuf + src_stride_Y * frame_height, src_stride_UV,
				I420buffer->MutableDataY(), dst_stride_Y,
				I420buffer->MutableDataU(), dst_stride_U,
				I420buffer->MutableDataV(), dst_stride_V,
				frame_width, frame_height);
		} else if (output_format == cudaVideoSurfaceFormat_YUV444) {
			// TO TEST: not tested yet
			int src_stride_Y = frame_width;
			int src_stride_U = src_stride_Y;
			int src_stride_V = src_stride_U;

			converted_ret = libyuv::I444ToI420(nvdec_framebuf, src_stride_Y,
				nvdec_framebuf + src_stride_Y * frame_height, src_stride_U,
				nvdec_framebuf + (src_stride_Y + src_stride_U) * frame_height, src_stride_V,
				I420buffer->MutableDataY(), dst_stride_Y,
				I420buffer->MutableDataU(), dst_stride_U,
				I420buffer->MutableDataV(), dst_stride_V,
				frame_width, frame_height);
		}

		if (converted_ret != 0) {
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERROR;
		}

		auto decoded_frame = VideoFrame::Builder()
			.set_video_frame_buffer(I420buffer)
			.set_timestamp_rtp(input_image.Timestamp())
			.build();

		// TODO(nisse): Timestamp and rotation are all zero here. Change decoder
		// interface to pass a VideoFrameBuffer instead of a VideoFrame?
		decoded_image_callback_->Decoded(decoded_frame, absl::nullopt, qp);
	}

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvH264DecoderImpl::RegisterDecodeCompleteCallback(
	DecodedImageCallback* callback) {
	decoded_image_callback_ = callback;

	return WEBRTC_VIDEO_CODEC_OK;
}

bool NvH264DecoderImpl::PrefersLateDecoding() const {
	return true;
}

const char* NvH264DecoderImpl::ImplementationName() const {
	return "NvDecH264";
}

void NvH264DecoderImpl::ReportInit() {
	if (has_reported_init_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264DecoderImpl.Event",
		static_cast<int>(H264DecoderImplEvent::H264DecoderEventInit),
		static_cast<int>(H264DecoderImplEvent::H264DecoderEventMax));

	has_reported_init_ = true;
}

void NvH264DecoderImpl::ReportError() {
	if (has_reported_error_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264DecoderImpl.Event",
		static_cast<int>(H264DecoderImplEvent::H264DecoderEventError),
		static_cast<int>(H264DecoderImplEvent::H264DecoderEventMax));

	has_reported_error_ = true;
}

}  // namespace webrtc
