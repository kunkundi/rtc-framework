#include <absl/strings/match.h>
#include <system_wrappers/include/metrics.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>

#include "log_manager.h"
#include "jetsonh264_encoder_impl.h"

// QP scaling thresholds.
static const int kLowH264QpThreshold = 24;
static const int kHighH264QpThreshold = 37;

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
	EncodedImage *encoded_image = enc_impl_ptr->GetEncodedImage();

	if(!enc_impl_ptr || !enc || !encoded_image)
	{
		LOG_ERROR("Invalid ptr <%p><%p><%p>", enc_impl_ptr, enc, encoded_image);
		return false;
	}

	if(enc_impl_ptr->release_flag_)
	{
		LOG_WARN("JetsonH264Encoder is waiting for release, skip dequeing buffer from capture plane");
		return false;
	}

	if(!buffer)
	{
		LOG_ERROR("NvBuffer is null");
		return false;
	}

	if (v4l2_buf == NULL)
    {
        LOG_ERROR("Error while dequeing buffer from output plane, v4l2_buf is Null");
        return false;
    }

    if (buffer->planes[0].bytesused == 0)
    {
        LOG_ERROR("Got 0 size buffer in capture");
        return false;
    }

	encoded_image->set_size(buffer->planes[0].bytesused);
	encoded_image->SetTimestamp(v4l2_buf->timestamp.tv_sec);

	// Write to file
	if(enc_impl_ptr->save_stream_ && enc_impl_ptr->stream_file_->is_open())
		enc_impl_ptr->stream_file_->write((char *) buffer->planes[0].data, buffer->planes[0].bytesused);

	memcpy(encoded_image->data(), (uint8_t*)buffer->planes[0].data, buffer->planes[0].bytesused);
	
	RTPFragmentationHeader frag_header;
	auto nalu_indices = H264::FindNaluIndices((uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);
	auto nalu_size = nalu_indices.size();

	if (nalu_size == 0) {
		return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
	}

	frag_header.VerifyAndAllocateFragmentationHeader(nalu_size);
	for (auto i = 0; i < nalu_size; ++i) {
		frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
		frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
	}

	if((buffer->planes[0].data[4] & 0x1f) == 0x07)
	{
		H264BitstreamParser h264_bitstream_parser_;
		if (encoded_image->size() > 0) {
			h264_bitstream_parser_.ParseBitstream(
				encoded_image->data(), encoded_image->size());
			auto qp = h264_bitstream_parser_.GetLastSliceQp();			
			if (qp.has_value()) {
				encoded_image->qp_ = qp.value();
			}
		}
	}

	CodecSpecificInfo codec_specific;
	codec_specific.codecType = kVideoCodecH264;
	codec_specific.codecSpecific.H264.packetization_mode = enc_impl_ptr->GetH264PacketizationMode();
	codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
	codec_specific.codecSpecific.H264.idr_frame = 
			(encoded_image->_frameType == VideoFrameType::kVideoFrameKey);
	codec_specific.codecSpecific.H264.base_layer_sync = false;

	/* OUTPUT */
	enc_impl_ptr->GetEncodedImageCallback()->OnEncodedImage(
			*encoded_image, &codec_specific, &frag_header);

	// encoder qbuffer for capture plane
    if(enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
    {
        LOG_ERROR("Error while Qing buffer at capture plane" );
        return false;
    }

    return true;
}

JetsonH264EncoderImpl::JetsonH264EncoderImpl(const cricket::VideoCodec& codec) {
	RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));
	LOG_INFO("Create JetsonH264Encoder");
	std::string packetization_mode_string;
	if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
		&packetization_mode_string) &&
		packetization_mode_string == "1") {
		packetization_mode_ = H264PacketizationMode::NonInterleaved;
	}

	if(save_stream_)
	{
		stream_file_ = new std::ofstream("jetson_video.h264");
		if(!stream_file_->is_open()) LOG_ERROR("Create outfile failed");
	}
	
	SetV4L2LogLevel(0);
}

JetsonH264EncoderImpl::~JetsonH264EncoderImpl() {
	LOG_INFO("~JetsonH264EncoderImpl\n");
	// To do, need to release or not ?
	// Release();
}

int32_t JetsonH264EncoderImpl::Release() {
	LOG_INFO("Release() called for JetsonH264Encoder <%p>", jetsonh264_encoder);
	if (jetsonh264_encoder) {
		release_flag_ = true;
		int ret = 0;
		ret = jetsonh264_encoder->output_plane.setStreamStatus(false);
		if(ret < 0) LOG_ERROR("Set output plane status failed");
    	ret = jetsonh264_encoder->capture_plane.setStreamStatus(false);
		if(ret < 0) LOG_ERROR("Set capture plane status failed");
		jetsonh264_encoder->capture_plane.waitForDQThread(-1);
		buffer_count_ = 0;

		LOG_INFO("Delete JetsonH264Encoder");
		delete jetsonh264_encoder;
	}

	return WEBRTC_VIDEO_CODEC_OK;
}

int JetsonH264EncoderImpl::InitEncode(const VideoCodec* codec_settings,
	const VideoEncoder::Settings& settings) {
	LOG_INFO("Init JetsonH264Encoder");
	release_flag_ = false;
    int32_t ret = 0;
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

	// // Release necessary in case of re-initializing.
	// ret = Release();
	// if (ret != WEBRTC_VIDEO_CODEC_OK) {
	// 	ReportError();
	// 	return ret;
	// }

	// TO DO: support SVC feature
	// auto num_of_streams =
	// 	SimulcastUtility::NumberOfSimulcastStreams(*codec_settings);
	// if (num_of_streams > 1) {
	// 	return WEBRTC_VIDEO_CODEC_ERR_SIMULCAST_PARAMETERS_NOT_SUPPORTED;
	// }

	codec_ = *codec_settings;
	max_payload_size_ = settings.max_payload_size;

	// Codec expects simulcastStream resolutions to be correct, make sure they are
	// filled even when there are no simulcast layers.
	if (codec_.numberOfSimulcastStreams == 0) {
		codec_.simulcastStream[0].width = codec_.width;
		codec_.simulcastStream[0].height = codec_.height;
	}

    jetsonh264_encoder = NvVideoEncoder::createVideoEncoder("enc0");
	LOG_INFO("JetsonH264Encoder created, address <%p>", jetsonh264_encoder);

	const auto frame_width = codec_.simulcastStream[0].width;
	const auto frame_height = codec_.simulcastStream[0].height;

    ret = jetsonh264_encoder->setCapturePlaneFormat(V4L2_PIX_FMT_H264, frame_width,
                                         frame_height, 2 * 1024 * 1024);
    if(ret < 0) LOG_ERROR("Could not set capture plane format");

    ret = jetsonh264_encoder->setOutputPlaneFormat(V4L2_PIX_FMT_YUV420M, frame_width,
                                        frame_height);
    if(ret < 0) LOG_ERROR("Could not set output plane format");

    // ret = jetsonh264_encoder->setBitrate(codec_settings->maxBitrate * 1000);
    // if(ret < 0) LOG_ERROR("Could not set encoder bitrate");

    ret = jetsonh264_encoder->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
    if(ret < 0) LOG_ERROR("Could not set encoder profile");

    // ret = jetsonh264_encoder->setLevel((uint32_t)V4L2_MPEG_VIDEO_H264_LEVEL_5_1);
    // if(ret < 0) LOG_ERROR("Could not set encoder level");

    // /* Set rate control mode for encoder */
    // ret = jetsonh264_encoder->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
    // if(ret < 0) LOG_ERROR("Could not set encoder rate control mode");
    // /* Set peak bitrate value for variable bitrate mode for encoder */
    // ret = jetsonh264_encoder->setPeakBitrate(codec_settings->maxBitrate * 1000);
    // if(ret < 0) LOG_ERROR("Could not set encoder peak bitrate");

    /* Set IDR frame interval for encoder */
    ret = jetsonh264_encoder->setIDRInterval(codec_settings->H264().keyFrameInterval);
    if(ret < 0) LOG_ERROR("Could not set encoder IDR interval");

    /* Set I frame interval for encoder */
    ret = jetsonh264_encoder->setIFrameInterval(60);
    if(ret < 0) LOG_ERROR("Could not set encoder I-Frame interval");

	ret = jetsonh264_encoder->setInsertSpsPpsAtIdrEnabled(true);
    if(ret < 0) printf("Could not set insertSPSPPSAtIDR\n");

    // /* Set framerate for encoder */
    ret = jetsonh264_encoder->setFrameRate(30, 1);
    if(ret < 0) LOG_ERROR("Could not set framerate");

	// ret = jetsonh264_encoder->setAlliFramesEncode(true);
    // if(ret < 0) LOG_ERROR("Could not set Alliframes encoding");

    uint32_t nMinQpI = kLowH264QpThreshold;
    uint32_t nMaxQpI = kHighH264QpThreshold;
    uint32_t nMinQpP = kLowH264QpThreshold;
    uint32_t nMaxQpP = kHighH264QpThreshold;
    uint32_t nMinQpB = kLowH264QpThreshold;
    uint32_t nMaxQpB = kHighH264QpThreshold;
    /* Set Min & Max qp range values for I/P/B-frames to be used by encoder */
    ret = jetsonh264_encoder->setQpRange(nMinQpI, nMaxQpI, nMinQpP, nMaxQpP, nMinQpB, nMaxQpB);
    if(ret < 0) LOG_ERROR("Could not set quantization parameters");


	ret = jetsonh264_encoder->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 1, false, true);
	if(ret < 0) LOG_ERROR("Could not setup output plane");
	ret = jetsonh264_encoder->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 1, true, false);
    if(ret < 0) LOG_ERROR("Could not setup capture plane");

    /* set encoder output plane STREAMON */
    ret = jetsonh264_encoder->output_plane.setStreamStatus(true);
    if(ret < 0) LOG_ERROR("Error in output plane streamon");
    /* set encoder capture plane STREAMON */
    ret = jetsonh264_encoder->capture_plane.setStreamStatus(true);
    if(ret < 0) LOG_ERROR("Error in capture plane streamon");


	/* Set encoder capture plane dq thread callback for blocking io mode */
    jetsonh264_encoder->capture_plane.setDQThreadCallback(CapturePlaneDqCallback);
	jetsonh264_encoder->capture_plane.startDQThread(this);

	/* Enqueue all the empty capture plane buffers. */
    for(uint32_t i = 0; i < jetsonh264_encoder->capture_plane.getNumBuffers(); i++)
    {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;

        ret = jetsonh264_encoder->capture_plane.qBuffer(v4l2_buf, NULL);
        if(ret < 0)  LOG_ERROR("Error while queueing buffer at capture plane");
    }

	const size_t new_capacity =
		CalcBufferSize(VideoType::kI420, frame_width, frame_height);
	encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
	encoded_image_._completeFrame = true;
	encoded_image_._encodedWidth = frame_width;
	encoded_image_._encodedHeight = frame_height;
	encoded_image_.set_size(0);
	return WEBRTC_VIDEO_CODEC_OK;
}

int32_t JetsonH264EncoderImpl::RegisterEncodeCompleteCallback(
	EncodedImageCallback* callback) {
	encoded_image_callback_ = callback;

	return WEBRTC_VIDEO_CODEC_OK;
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

	auto bitrate = parameters.bitrate.GetBitrate(0, 0);
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
		LOG_INFO("SetRates fps:%u", fps);
	}
	if(bitrate_ != bitrate)
	{
		ret = jetsonh264_encoder->setBitrate(bitrate);
    	if(ret < 0) LOG_ERROR("Could not set encoder bitrate");
		bitrate_ = bitrate;
		LOG_INFO("SetRates bitrate:%u", bitrate);
	}
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

	auto frame_buffer = input_frame.video_frame_buffer()->ToI420();

	RTC_DCHECK_EQ(encoded_image_._encodedWidth, frame_buffer->width());
	RTC_DCHECK_EQ(encoded_image_._encodedHeight, frame_buffer->height());

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
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(buffer_count_ < jetsonh264_encoder->output_plane.getNumBuffers()){
		nvBuffer = jetsonh264_encoder->output_plane.getNthBuffer(buffer_count_);
		v4l2_buf.index = buffer_count_ ;
		buffer_count_++;

	}else{
		ret = jetsonh264_encoder->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0) {
			LOG_ERROR("Error DQing buffer at output plane");
			return -1;
		}

	}

	char *data;

	for (uint32_t i = 0; i < (*nvBuffer).n_planes; i++)
	{
		//LOG_INFO("Go through every plane");
		NvBuffer::NvBufferPlane &plane = (*nvBuffer).planes[i];
		//data = (char *) plane.data;
		plane.bytesused = 0;
		if(i==0)
			//for (uint32_t j = 0; j < plane.fmt.height; j++)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
				memcpy(plane.data, frame_buffer->DataY(), bytes_to_read);
				//plane.data += plane.fmt.stride;
				plane.bytesused = bytes_to_read;
				//plane.bytesused = plane.fmt.stride * plane.fmt.width * plane.fmt.height;
			}
		if(i==1)
			//for (uint32_t j = 0; j < plane.fmt.height; j++)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height ;
				memcpy(plane.data, frame_buffer->DataU(), bytes_to_read);
				//plane.data += plane.fmt.stride;
				plane.bytesused = bytes_to_read;
				//plane.bytesused = plane.fmt.stride * plane.fmt.width * plane.fmt.height;
			}
		if(i==2)
			//for (uint32_t j = 0; j < plane.fmt.height; j++)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
				memcpy(plane.data, frame_buffer->DataV(), bytes_to_read);
				//plane.data += plane.fmt.stride;
				plane.bytesused = bytes_to_read;
				//plane.bytesused = plane.fmt.stride * plane.fmt.width * plane.fmt.height;
			}
	}

	v4l2_buf.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = input_frame.timestamp();

	ret = jetsonh264_encoder->output_plane.qBuffer(v4l2_buf, NULL);
	if(ret < 0) LOG_ERROR("Encoder qBuffer error");

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

	return info;
}

void JetsonH264EncoderImpl::SetFecControllerOverride(
	FecControllerOverride* fec_controller_override) {
}

void JetsonH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate) {
/*	LOG_INFO("[WEBRTC] OnPacketLossRateUpdate: %f", packet_loss_rate);*/
}

void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms) {
/*	LOG_INFO("[WEBRTC] OnRttUpdate: %d", rtt_ms);*/
}

void JetsonH264EncoderImpl::OnLossNotification(
	const LossNotification& loss_notification) {
	auto delta = loss_notification.timestamp_of_last_decodable -
		loss_notification.timestamp_of_last_received;
	//LOG_INFO("Loss delta: %d", delta);
}

void JetsonH264EncoderImpl::SetV4L2LogLevel(int level)
{
	log_level = level;
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
