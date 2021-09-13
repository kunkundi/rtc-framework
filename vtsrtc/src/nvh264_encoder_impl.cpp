#include <cuda.h>

#include <absl/strings/match.h>
#include <system_wrappers/include/metrics.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <common_video/h264/h264_common.h>
#include <nvEncodeAPI.h>

#include <string>

#include "log_manager.h"
#include "nvh264_encoder_impl.h"


namespace webrtc {

static const int index_of_GPU = 0;
static const GUID codec_guid = NV_ENC_CODEC_H264_GUID;
static const GUID preset_guid = NV_ENC_PRESET_P3_GUID;
static const NV_ENC_TUNING_INFO tuning_info =
	NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

// QP scaling thresholds.
static const int kLowH264QpThreshold = 24;
static const int kHighH264QpThreshold = 37;

enum class H264EncoderImplEvent {
	H264EncoderEventInit = 0,
	H264EncoderEventError = 1,
	H264EncoderEventMax = 16,
};

NvH264EncoderImpl::NvH264EncoderImpl(const cricket::VideoCodec& codec) {
	RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));

	std::string packetization_mode_string;
	if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
		&packetization_mode_string) &&
		packetization_mode_string == "1") {
		packetization_mode_ = H264PacketizationMode::NonInterleaved;
	}
}

NvH264EncoderImpl::~NvH264EncoderImpl() {
	Release();
}

int NvH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
	const VideoEncoder::Settings& settings) {
	ReportInit();

	if (!codec_settings || codec_settings->codecType != kVideoCodecH264) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	if (codec_settings->maxFramerate == 0) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	if (codec_settings->width < 1 || codec_settings->height < 1) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
	}

	// Release necessary in case of re-initializing.
	int32_t ret = Release();
	if (ret != WEBRTC_VIDEO_CODEC_OK) {
		ReportError();
		return ret;
	}

	auto num_of_streams =
		SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
	if (num_of_streams > 1) {
		return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
	}

	codec_ = *codec_settings;
	max_payload_size_ = settings.max_payload_size;

	// Code expects simulcastStream resolutions to be correct, make sure they are
	// filled even when there are no simulcast layers.
	if (codec_.numberOfSimulcastStreams == 0) {
		codec_.simulcastStream[0].width = codec_.width;
		codec_.simulcastStream[0].height = codec_.height;
	}

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
		// To be improved: WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE maybe better
		return WEBRTC_VIDEO_CODEC_ERROR;
	}

	const auto frame_width = codec_.simulcastStream[0].width;
	const auto frame_height = codec_.simulcastStream[0].height;

	// Create Nvidia encoder
	nvh264_encoder_ = std::make_unique<NvEncoderCuda>(cuda_context_,
		frame_width,
		frame_height,
		NV_ENC_BUFFER_FORMAT::NV_ENC_BUFFER_FORMAT_IYUV);

	NV_ENC_INITIALIZE_PARAMS init_params;
	init_params.version = NV_ENC_INITIALIZE_PARAMS_VER;
	NV_ENC_CONFIG encode_config = { NV_ENC_CONFIG_VER };
	init_params.encodeConfig = &encode_config;

	nvh264_encoder_->CreateDefaultEncoderParams(
		&init_params,
		codec_guid,
		preset_guid,
		tuning_info);

	init_params.encodeWidth = frame_width;
	init_params.encodeHeight = frame_height;
	init_params.encodeConfig->gopLength = NVENC_INFINITE_GOPLENGTH;
	init_params.encodeConfig->frameIntervalP = 1;
	// TO TEST: not tested yet
// 	init_params.encodeConfig->encodeCodecConfig.h264Config.sliceMode = 1;
// 	init_params.encodeConfig->encodeCodecConfig.h264Config.sliceModeData =
// 		max_payload_size_;

	nvh264_encoder_->CreateEncoder(&init_params);

	const size_t new_capacity =
		CalcBufferSize(VideoType::kI420, frame_width, frame_height);
	encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
	encoded_image_._completeFrame = true;
	encoded_image_._encodedWidth = frame_width;
	encoded_image_._encodedHeight = frame_height;
	encoded_image_.set_size(0);

	// TO DO
	SimulcastRateAllocator init_allocator(codec_);
	auto allocation =
		init_allocator.Allocate(VideoBitrateAllocationParameters(
			DataRate::KilobitsPerSec(codec_.startBitrate), codec_.maxFramerate));
	SetRates(RateControlParameters(allocation, codec_.maxFramerate));

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvH264EncoderImpl::Release() {
	if (nvh264_encoder_) {
		nvh264_encoder_->DestroyEncoder();
		nvh264_encoder_ = nullptr;
	}
	encoded_packets_.clear();
	encoded_image_.ClearEncodedData();

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t NvH264EncoderImpl::RegisterEncodeCompleteCallback(
	EncodedImageCallback* callback) {
	encoded_image_callback_ = callback;

	return WEBRTC_VIDEO_CODEC_OK;
}

void NvH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
// 	LOG_INFO("[WebRTC] SetRates: fps: %f, bitrate: %d",
// 		parameters.framerate_fps, parameters.bitrate.get_sum_bps());

	if (!nvh264_encoder_) {
		LOG_WARN("[WebRTC] SetRates failed when encoder is uninitialized.");
		return;
	}

	if (parameters.framerate_fps < 1.0) {
		LOG_WARN("[WebRTC] SetRates failed because framerate is invalid: %f",
			parameters.framerate_fps);
		return;
	}

	codec_.maxFramerate = static_cast<uint32_t>(parameters.framerate_fps);

	NV_ENC_RECONFIGURE_PARAMS reconfig_params;
	reconfig_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;

	NV_ENC_INITIALIZE_PARAMS init_params;
	NV_ENC_CONFIG encode_config = { NV_ENC_CONFIG_VER };
	init_params.encodeConfig = &encode_config;
	nvh264_encoder_->GetInitializeParams(&init_params);

	init_params.frameRateDen = 1;
	init_params.frameRateNum = init_params.frameRateDen * parameters.framerate_fps;
	init_params.encodeConfig->rcParams.maxBitRate =
		parameters.bitrate.GetBitrate(0, 0);

	reconfig_params.reInitEncodeParams = init_params;
	// TO TEST: not tested yet
// 	reconfig_params.resetEncoder = 1;
// 	reconfig_params.forceIDR = 1;

	nvh264_encoder_->Reconfigure(&reconfig_params);
}

int32_t NvH264EncoderImpl::Encode(const VideoFrame& input_frame,
	const std::vector<VideoFrameType>* frame_types) {
	if (!nvh264_encoder_) {
		ReportError();
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	if (!encoded_image_callback_) {
		LOG_ERROR("InitEncode() has been called, but a callback function "
			"has not been set with RegisterEncodeCompleteCallback()");
		ReportError();
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	auto frame_buffer = input_frame.video_frame_buffer()->ToI420();

	RTC_DCHECK_EQ(encoded_image_._encodedWidth, frame_buffer->width());
	RTC_DCHECK_EQ(encoded_image_._encodedHeight, frame_buffer->height());

	if (!frame_types || (*frame_types)[0] != VideoFrameType::kEmptyFrame) {
		const NvEncInputFrame* encoder_inputframe =
			nvh264_encoder_->GetNextInputFrame();

		NvEncoderCuda::CopyToDeviceFrame(cuda_context_,
			(void*)frame_buffer->DataY(), // NOLINT
			0,
			(CUdeviceptr)encoder_inputframe->inputPtr,
			encoder_inputframe->pitch,
			nvh264_encoder_->GetEncodeWidth(),
			nvh264_encoder_->GetEncodeHeight(),
			CU_MEMORYTYPE_HOST,
			encoder_inputframe->bufferFormat,
			encoder_inputframe->chromaOffsets,
			encoder_inputframe->numChromaPlanes);

		nvh264_encoder_->EncodeFrame(encoded_packets_);

		if (encoded_packets_.size() < 1) {
			return WEBRTC_VIDEO_CODEC_ERROR;
		}

		for (const auto& packet : encoded_packets_) {
			encoded_image_.set_size(packet.size());
/*			encoded_image_.playout_delay_ = { 0, 0 };*/
			encoded_image_.SetTimestamp(input_frame.timestamp());
 			encoded_image_.ntp_time_ms_ = input_frame.ntp_time_ms();
 			encoded_image_.capture_time_ms_ = input_frame.render_time_ms();
 			encoded_image_.rotation_ = input_frame.rotation();
 			encoded_image_.SetColorSpace(input_frame.color_space());
 			encoded_image_.content_type_ =
 				codec_.mode == VideoCodecMode::kScreensharing ?
 				VideoContentType::SCREENSHARE : VideoContentType::UNSPECIFIED;
 			encoded_image_.SetSpatialIndex(0);
 			if ((packet[4] & 0x1f) == 0x07) {
 				encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
 			} else if ((packet[4] & 0x1f) == 0x01) {
 				encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;
 			} else {
 				encoded_image_._frameType = VideoFrameType::kEmptyFrame;
 			}

			memcpy(encoded_image_.data(), packet.data(), packet.size());

			RTPFragmentationHeader frag_header;
			auto nalu_indices = H264::FindNaluIndices(packet.data(), packet.size());
			auto nalu_size = nalu_indices.size();

			if (nalu_size == 0) {
				return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
			}

			frag_header.VerifyAndAllocateFragmentationHeader(nalu_size);
			for (auto i = 0; i < nalu_size; ++i) {
				frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
				frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
			}

			if (encoded_image_.size() > 0) {
				h264_bitstream_parser_.ParseBitstream(
					encoded_image_.data(), encoded_image_.size());
				auto qp = h264_bitstream_parser_.GetLastSliceQp();
				if (qp.has_value()) {
					encoded_image_.qp_ = qp.value();
				}
			}

			CodecSpecificInfo codec_specific;
			codec_specific.codecType = kVideoCodecH264;
			codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
			codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
			codec_specific.codecSpecific.H264.idr_frame =
				(encoded_image_._frameType == VideoFrameType::kVideoFrameKey);
			codec_specific.codecSpecific.H264.base_layer_sync = false;

			encoded_image_callback_->OnEncodedImage(
				encoded_image_, &codec_specific, &frag_header);
		}
	}

	return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo NvH264EncoderImpl::GetEncoderInfo() const {
	EncoderInfo info;
	info.supports_native_handle = false;
	info.implementation_name = "NvEncH264";
	info.scaling_settings =
		VideoEncoder::ScalingSettings(kLowH264QpThreshold, kHighH264QpThreshold);
	info.is_hardware_accelerated = true;
	info.has_internal_source = false;
	info.supports_simulcast = false;

	return info;
}

void NvH264EncoderImpl::SetFecControllerOverride(
	FecControllerOverride* fec_controller_override) {
}

void NvH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {
/*	LOG_INFO("[WEBRTC] OnPacketLossRateUpdate: %f", packet_loss_rate);*/
}

void NvH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
/*	LOG_INFO("[WEBRTC] OnRttUpdate: %d", rtt_ms);*/
}

void NvH264EncoderImpl::OnLossNotification(
	const LossNotification& loss_notification) {
// 	auto delta = loss_notification.timestamp_of_last_decodable -
// 		loss_notification.timestamp_of_last_received;
// 	LOG_INFO("[WEBRTC] OnLossNotification timestamp between"
// 		"last decodable and last received frame: %d", delta);
}

void NvH264EncoderImpl::ReportInit() {
	if (has_reported_init_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

	has_reported_init_ = true;
}

void NvH264EncoderImpl::ReportError() {
	if (has_reported_error_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventError),
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

	has_reported_error_ = true;
}

}  // namespace webrtc
