#include <absl/strings/match.h>
#include <system_wrappers/include/metrics.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>

#include "log_manager.h"
#include "rtc_base/logging.h"
#include "jetsonh264_encoder_impl.h"
#include "rtc_codec_pool.h"
#include <chrono>

using namespace std::chrono;

// QP scaling thresholds.
static const int kLowH264QpThreshold = 27;
static const int kHighH264QpThreshold = 34;
static int name_count = 0;
enum class H264EncoderImplEvent {
	H264EncoderEventInit = 0,
	H264EncoderEventError = 1,
	H264EncoderEventMax = 16,
};

namespace webrtc {

bool JetsonH264EncoderImpl::CapturePlaneDqCallback(struct v4l2_buffer *v4l2_buf, 
					NvBuffer * buffer, NvBuffer * shared_buffer, void *data) {
	
	JetsonH264EncoderImpl *enc_impl_ptr = (static_cast<JetsonH264EncoderImpl *>(data));
	NvVideoEncoder *enc = enc_impl_ptr->GetJetsonH264Encoder();
	EncodedImage encoded_image;
	const size_t new_capacity = CalcBufferSize(VideoType::kI420, enc_impl_ptr->width_, enc_impl_ptr->height_);
	encoded_image.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
	encoded_image._completeFrame = true;
	encoded_image._encodedWidth = enc_impl_ptr->width_;
	encoded_image._encodedHeight = enc_impl_ptr->height_;
	encoded_image.set_size(0);

	if(!enc_impl_ptr || !enc)
	{
		LOG_ERROR("Invalid ptr <%p><%p>", enc_impl_ptr, enc);
		return false;
	}

	if(!buffer)
	{
		LOG_ERROR("Encoder<%p> get null NvBuffer", enc);
		return false;
	}

	if (v4l2_buf == NULL)
    {
        LOG_ERROR("Encoder<%p> dequeue buffer from output plane failed, v4l2_buf is Null", enc);
        return false;
    }

    if (buffer->planes[0].bytesused == 0)
    {
        LOG_WARN("Encoder<%p> get 0 size buffer in capture plane, finish encode", enc);
        return false;
    }

	encoded_image.set_size(buffer->planes[0].bytesused);
	encoded_image.SetTimestamp(v4l2_buf->timestamp.tv_sec);
 	encoded_image.SetSpatialIndex(0);

	// Write to file
	if(enc_impl_ptr->save_stream_ && enc_impl_ptr->stream_file_->is_open())
		enc_impl_ptr->stream_file_->write((char *) buffer->planes[0].data, buffer->planes[0].bytesused);

	memcpy(encoded_image.data(), (uint8_t*)buffer->planes[0].data, buffer->planes[0].bytesused);
	
	if ((buffer->planes[0].data[4] & 0x1f) == 0x07) {
 		encoded_image._frameType = VideoFrameType::kVideoFrameKey;
		//LOG_WARN("Keyframe");
	} else if ((buffer->planes[0].data[4] & 0x1f) == 0x01) {
 		encoded_image._frameType = VideoFrameType::kVideoFrameDelta;
		//LOG_ERROR("Deltaframe");
	} else {
		encoded_image._frameType = VideoFrameType::kEmptyFrame;
		//LOG_ERROR("Emptyframe");
	}

	RTPFragmentationHeader frag_header;
	auto nalu_indices = H264::FindNaluIndices((uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);
	auto nalu_size = nalu_indices.size();

	if (nalu_size == 0) {
		LOG_ERROR("Encoder<%p> get 0 size Nalu", enc);
		return false;
	}

	frag_header.VerifyAndAllocateFragmentationHeader(nalu_size);
	for (auto i = 0; i < nalu_size; ++i) {
		frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
		frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
	}

	if((buffer->planes[0].data[4] & 0x1f) == 0x07)
	{
		H264BitstreamParser h264_bitstream_parser_;
		if (encoded_image.size() > 0) {
			h264_bitstream_parser_.ParseBitstream(
				encoded_image.data(), encoded_image.size());
			auto qp = h264_bitstream_parser_.GetLastSliceQp();			
			if (qp.has_value()) {
				encoded_image.qp_ = qp.value();
				LOG_WARN("QP = %d <%dx%d>", qp.value(), enc_impl_ptr->width_, enc_impl_ptr->height_);
			}
		}
	}

	CodecSpecificInfo codec_specific;
	codec_specific.codecType = kVideoCodecH264;
	codec_specific.codecSpecific.H264.packetization_mode = enc_impl_ptr->GetH264PacketizationMode();
	codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
	codec_specific.codecSpecific.H264.idr_frame = (encoded_image._frameType == VideoFrameType::kVideoFrameKey);
	codec_specific.codecSpecific.H264.base_layer_sync = false;

	/* OUTPUT */
	enc_impl_ptr->GetEncodedImageCallback()->OnEncodedImage(
			encoded_image, &codec_specific, &frag_header);

	// encoder qbuffer for capture plane
    if(enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
    {
        LOG_ERROR("Encoder<%p> queue buffer error at capture plane", enc);
        return false;
    }

    return true;
}

JetsonH264EncoderImpl::JetsonH264EncoderImpl(const cricket::VideoCodec& codec, const vts_rtc::RtcConfig& rtc_config)
	: rtc_config_(rtc_config) {
	RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));
	std::string packetization_mode_string;
	if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
		&packetization_mode_string) &&
		packetization_mode_string == "1") {
		packetization_mode_ = H264PacketizationMode::NonInterleaved;
	}

	if(rtc_config_.use_strategy) {
		auto iter = rtc_config_.strategy.begin();
		LOG_INFO("Resolution vs Bitrate strategy:");
		while(iter != rtc_config_.strategy.end()) {
			ResolutionBitrateLimits sub_sstrategy(iter->first, iter->second[0], iter->second[1], iter->second[2]);
			LOG_INFO("%ld %ld %ld %ld", iter->first, iter->second[0], iter->second[1], iter->second[2]);
			resolution_bitrate_limits_.push_back(sub_sstrategy);
			iter++;
		}
	}

	if(save_stream_)
	{
		stream_file_ = new std::ofstream("jetson_video.h264");
		if(!stream_file_->is_open()) LOG_ERROR("Create outfile failed");
	}
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() {
	LOG_INFO("Destroy JetsonH264Encoder <%p>", this);
}

int32_t JetsonH264EncoderImpl::Release() {
	LOG_INFO("Release() called for JetsonH264Encoder <%p>", jetsonh264_encoder);

	int ret = 0;
	// Enqueue empty buffer to notify encoder the process of encoding has been finished
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		NvBuffer *nvBuffer = jetsonh264_encoder->output_plane.getNthBuffer(0);
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = 0;
		v4l2_buf.m.planes = planes;

		for (uint32_t i = 0; i < (*nvBuffer).n_planes; i++)
		{
			NvBuffer::NvBufferPlane &plane = (*nvBuffer).planes[i];
			plane.bytesused = 0;
			if(i==0) plane.bytesused = 0;
			if(i==1) plane.bytesused = 0;
			if(i==2) plane.bytesused = 0;
		}

		ret = jetsonh264_encoder->output_plane.qBuffer(v4l2_buf, NULL);
		if(ret < 0) LOG_ERROR("Encoder qBuffer error");
	}

	ret = CodecPool::GetInstance()->ReleaseEncoder(width_, height_, jetsonh264_encoder);
	if(ret < 0) LOG_ERROR("Release encoder to codec pool failed");

	return WEBRTC_VIDEO_CODEC_OK;
}

int JetsonH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
	const VideoEncoder::Settings& settings) {
	LOG_INFO("Init JetsonH264Encoder");

    int ret = 0;
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

	codec_ = *codec_settings;
	max_payload_size_ = settings.max_payload_size;

	// Codec expects simulcastStream resolutions to be correct, make sure they are
	// filled even when there are no simulcast layers.
	if (codec_.numberOfSimulcastStreams == 0) {
		codec_.simulcastStream[0].width = codec_.width;
		codec_.simulcastStream[0].height = codec_.height;
	}

	width_ = codec_.width;
	height_= codec_.height;

	jetsonh264_encoder = CodecPool::GetInstance()->GetAvailableEncoder(codec_.width, codec_.height);
	if(!jetsonh264_encoder) {
		LOG_ERROR("No available encoder for resolution:<%dx%d>", codec_.width, codec_.height);
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}
	else {
		LOG_WARN("Get encoder for resolution:<%dx%d>", codec_.width, codec_.height);
	}

	/* Set encoder capture plane dq thread callback for blocking io mode */
    jetsonh264_encoder->capture_plane.setDQThreadCallback(CapturePlaneDqCallback);
	jetsonh264_encoder->capture_plane.startDQThread(this);
	
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::RegisterEncodeCompleteCallback(
	EncodedImageCallback* callback) {
	encoded_image_callback_ = callback;

	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::Encode(const VideoFrame& input_frame,
	const std::vector<VideoFrameType>* frame_types) {

	if(!jetsonh264_encoder)
	{
		ReportError();
		LOG_ERROR("jetsonh264_encoder is null");
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	if (!encoded_image_callback_)
	{
		LOG_ERROR("InitEncode() has been called, but a callback function "
			"has not been set with RegisterEncodeCompleteCallback()");
		ReportError();
		return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
	}

	if(encoder_id_ == 0)
		encoder_id_ = input_frame.id();

	auto frame_buffer = input_frame.video_frame_buffer()->ToI420();

	if (frame_types && (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
		LOG_WARN("Ignore empty frame");
		return WEBRTC_VIDEO_CODEC_OK;
	}

	// Request Key frame
	if (frame_types && (*frame_types)[0] == VideoFrameType::kVideoFrameKey) {
		LOG_WARN("Force IDR frame");
		jetsonh264_encoder->forceIDR();
	}

	int32_t ret = 0;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer = jetsonh264_encoder->output_plane.getNthBuffer(0);
	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.index = 0;
	v4l2_buf.m.planes = planes;

	for (uint32_t i = 0; i < (*nvBuffer).n_planes; i++)
	{
		NvBuffer::NvBufferPlane &plane = (*nvBuffer).planes[i];
		plane.bytesused = 0;
		if(i==0) {
			std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
			memcpy(plane.data, frame_buffer->DataY(), bytes_to_read);
			plane.bytesused = bytes_to_read;
		}
		if(i==1) {
			std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height ;
			memcpy(plane.data, frame_buffer->DataU(), bytes_to_read);
			plane.bytesused = bytes_to_read;
		}
		if(i==2) {
			std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
			memcpy(plane.data, frame_buffer->DataV(), bytes_to_read);
			plane.bytesused = bytes_to_read;
		}
	}

	v4l2_buf.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = input_frame.timestamp();

	ret = jetsonh264_encoder->output_plane.qBuffer(v4l2_buf, NULL);
	if(ret < 0) LOG_ERROR("Encoder qBuffer error <%p>", jetsonh264_encoder);

	ret = jetsonh264_encoder->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
	if(ret < 0) LOG_ERROR("Error DQing buffer at output plane");

	return WEBRTC_VIDEO_CODEC_OK;
}

VideoEncoder::EncoderInfo JetsonH264EncoderImpl::GetEncoderInfo() const {
	EncoderInfo info;
	info.supports_native_handle = false;
	info.implementation_name = "NvJetsonEncH264";
	info.scaling_settings =
		VideoEncoder::ScalingSettings(kLowH264QpThreshold, kHighH264QpThreshold);
	info.is_hardware_accelerated = true;
	info.has_internal_source = false;
	info.supports_simulcast = false;
	if(rtc_config_.use_strategy) {
		info.resolution_bitrate_limits = resolution_bitrate_limits_;
	}
	return info;
}

void JetsonH264EncoderImpl::SetFecControllerOverride(
	FecControllerOverride* fec_controller_override) {
}

void JetsonH264EncoderImpl::SetRates(const RateControlParameters& parameters) {
	int32_t ret = 0;

	if (!jetsonh264_encoder)
	{
		LOG_ERROR("JetsonH264Encoder is Null");
		return;
	}
	
	auto fps = static_cast<uint32_t>(parameters.framerate_fps);
	codec_.maxFramerate = fps;

	auto bitrate = parameters.bitrate.get_sum_bps();
	codec_.maxBitrate = bitrate;

	if (fps < 1 || bitrate < 1)
	{
		LOG_WARN("SetRates failed because framerate or bitrate is invalid");
		return;
	}

	if(fps_ != fps)
	{
		ret = jetsonh264_encoder->setFrameRate (fps, 1);
		if(ret < 0) LOG_ERROR("Could not set framerate");
		fps_ = fps;
		LOG_INFO("SetRates fps:%u <%dx%d>", fps, width_, height_);
	}
	if(bitrate_ != bitrate)
	{
		ret = jetsonh264_encoder->setPeakBitrate(bitrate);
    	if(ret < 0) LOG_ERROR("Could not set encoder bitrate");
		bitrate_ = bitrate;
		LOG_INFO("SetRates bitrate:%u <%dx%d>", bitrate, width_, height_);
	}
}

void JetsonH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {
	//LOG_WARN("Loss rate: %f", packet_loss_rate);
}

void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
	//LOG_WARN("rtt: %u", rtt_ms);
}

void JetsonH264EncoderImpl::OnLossNotification(
	const LossNotification& loss_notification) {
	auto delta = loss_notification.timestamp_of_last_decodable -
		loss_notification.timestamp_of_last_received;
	//LOG_INFO("Loss delta: %d", delta);
}

void JetsonH264EncoderImpl::ReportInit() {
	if (has_reported_init_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

	has_reported_init_ = true;
}

void JetsonH264EncoderImpl::ReportError() {
	if (has_reported_error_) {
		return;
	}

	RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventError),
		static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

	has_reported_error_ = true;
}

}  // namespace webrtc
