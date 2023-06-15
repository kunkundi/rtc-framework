#include <absl/strings/match.h>
#include <system_wrappers/include/metrics.h>
#include <common_video/h264/h264_common.h>
#include <common_video/libyuv/include/webrtc_libyuv.h>
#include <modules/video_coding/utility/simulcast_utility.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <modules/video_coding/utility/simulcast_rate_allocator.h>

#include "log/log_manager.h"
#include "rtc_base/logging.h"
#include "jetsonh264_encoder_impl.h"
#include "rtc_codec_pool.h"
#include <chrono>

using namespace std::chrono;

// QP scaling thresholds.
static const int kLowH264QpThreshold = 27;
static const int kHighH264QpThreshold = 34;

enum class H264EncoderImplEvent
{
	H264EncoderEventInit = 0,
	H264EncoderEventError = 1,
	H264EncoderEventMax = 16,
};

namespace webrtc
{

	bool JetsonH264EncoderImpl::CapturePlaneDqCallbackWithCodecPool(struct v4l2_buffer *v4l2_buf,
																	NvBuffer *buffer, NvBuffer *shared_buffer, void *data)
	{

		JetsonH264EncoderImpl *enc_impl_ptr = (static_cast<JetsonH264EncoderImpl *>(data));
		NvVideoEncoder *enc = enc_impl_ptr->GetJetsonH264Encoder();
		EncodedImage *encoded_image = enc_impl_ptr->GetEncodedImage();

		if (!enc_impl_ptr || !enc || !encoded_image)
		{
			LOG_ERROR("Invalid ptr <%p><%p><%p>", enc_impl_ptr, enc, encoded_image);
			return false;
		}

		if (enc_impl_ptr->release_flag_)
		{
			return false;
		}

		if (!buffer)
		{
			enc_impl_ptr->stop_flag_ = true;
			return false;
		}

		if (v4l2_buf == NULL)
		{
			LOG_ERROR("Error while dequeing buffer from output plane, v4l2_buf is Null <%p><%p>", enc_impl_ptr, enc);
			return false;
		}

		if (buffer->planes[0].bytesused == 0)
		{
			LOG_ERROR("Got 0 size buffer in capture <%p><%p>", enc_impl_ptr, enc);
			return false;
		}

		encoded_image->set_size(buffer->planes[0].bytesused);
		encoded_image->SetTimestamp(v4l2_buf->timestamp.tv_sec);
		encoded_image->SetSpatialIndex(0);

		// Write to file
		if (enc_impl_ptr->save_stream_ && enc_impl_ptr->stream_file_->is_open())
			enc_impl_ptr->stream_file_->write((char *)buffer->planes[0].data, buffer->planes[0].bytesused);

		memcpy(encoded_image->data(), (uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);

		if ((buffer->planes[0].data[4] & 0x1f) == 0x07)
		{
			encoded_image->_frameType = VideoFrameType::kVideoFrameKey;
		}
		else if ((buffer->planes[0].data[4] & 0x1f) == 0x01)
		{
			encoded_image->_frameType = VideoFrameType::kVideoFrameDelta;
		}
		else
		{
			encoded_image->_frameType = VideoFrameType::kEmptyFrame;
		}

		RTPFragmentationHeader frag_header;
		auto nalu_indices = H264::FindNaluIndices((uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);
		auto nalu_size = nalu_indices.size();

		if (nalu_size == 0)
		{
			LOG_ERROR("Encoder<%p> get 0 size Nalu", enc);
			return false;
		}

		frag_header.VerifyAndAllocateFragmentationHeader(nalu_size);
		for (auto i = 0; i < nalu_size; ++i)
		{
			frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
			frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
		}

		if ((buffer->planes[0].data[4] & 0x1f) == 0x07)
		{
			H264BitstreamParser h264_bitstream_parser_;
			if (encoded_image->size() > 0)
			{
				h264_bitstream_parser_.ParseBitstream(
					encoded_image->data(), encoded_image->size());
				auto qp = h264_bitstream_parser_.GetLastSliceQp();
				if (qp.has_value())
				{
					encoded_image->qp_ = qp.value();
					// LOG_WARN("<%p> QP = %d <%ux%u>", enc, qp.value(), enc_impl_ptr->width_, enc_impl_ptr->height_);
				}
			}
		}

		CodecSpecificInfo codec_specific;
		codec_specific.codecType = kVideoCodecH264;
		codec_specific.codecSpecific.H264.packetization_mode = enc_impl_ptr->GetH264PacketizationMode();
		codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
		codec_specific.codecSpecific.H264.idr_frame = (encoded_image->_frameType == VideoFrameType::kVideoFrameKey);
		codec_specific.codecSpecific.H264.base_layer_sync = false;

		/* OUTPUT */
		enc_impl_ptr->GetEncodedImageCallback()->OnEncodedImage(
			*encoded_image, &codec_specific, &frag_header);

		// encoder qbuffer for capture plane
		if (enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
		{
			LOG_ERROR("Encoder<%p> queue buffer error at capture plane", enc);
			return false;
		}
		return true;
	}

	bool JetsonH264EncoderImpl::CapturePlaneDqCallbackWithoutCodecPool(struct v4l2_buffer *v4l2_buf,
																	   NvBuffer *buffer, NvBuffer *shared_buffer, void *data)
	{

		JetsonH264EncoderImpl *enc_impl_ptr = (static_cast<JetsonH264EncoderImpl *>(data));
		NvVideoEncoder *enc = enc_impl_ptr->GetJetsonH264Encoder();
		EncodedImage *encoded_image = enc_impl_ptr->GetEncodedImage();

		if (!enc_impl_ptr || !enc || !encoded_image)
		{
			LOG_ERROR("Invalid ptr <%p><%p><%p>", enc_impl_ptr, enc, encoded_image);
			return false;
		}

		if (enc_impl_ptr->release_flag_)
		{
			return false;
		}

		if (!buffer)
		{
			enc_impl_ptr->stop_flag_ = true;
			return false;
		}

		if (v4l2_buf == NULL)
		{
			LOG_ERROR("Error while dequeing buffer from output plane, v4l2_buf is Null <%p><%p>", enc_impl_ptr, enc);
			return false;
		}

		if (buffer->planes[0].bytesused == 0)
		{
			LOG_ERROR("Got 0 size buffer in capture <%p><%p>", enc_impl_ptr, enc);
			return false;
		}

		encoded_image->set_size(buffer->planes[0].bytesused);
		encoded_image->SetTimestamp(v4l2_buf->timestamp.tv_sec);
		encoded_image->SetSpatialIndex(0);

		// Write to file
		if (enc_impl_ptr->save_stream_ && enc_impl_ptr->stream_file_->is_open())
			enc_impl_ptr->stream_file_->write((char *)buffer->planes[0].data, buffer->planes[0].bytesused);

		memcpy(encoded_image->data(), (uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);

		if ((buffer->planes[0].data[4] & 0x1f) == 0x07)
		{
			encoded_image->_frameType = VideoFrameType::kVideoFrameKey;
		}
		else if ((buffer->planes[0].data[4] & 0x1f) == 0x01)
		{
			encoded_image->_frameType = VideoFrameType::kVideoFrameDelta;
		}
		else
		{
			encoded_image->_frameType = VideoFrameType::kEmptyFrame;
		}

		RTPFragmentationHeader frag_header;
		auto nalu_indices = H264::FindNaluIndices((uint8_t *)buffer->planes[0].data, buffer->planes[0].bytesused);
		auto nalu_size = nalu_indices.size();

		if (nalu_size == 0)
		{
			return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
		}

		frag_header.VerifyAndAllocateFragmentationHeader(nalu_size);
		for (auto i = 0; i < nalu_size; ++i)
		{
			frag_header.fragmentationOffset[i] = nalu_indices[i].payload_start_offset;
			frag_header.fragmentationLength[i] = nalu_indices[i].payload_size;
		}

		if ((buffer->planes[0].data[4] & 0x1f) == 0x07)
		{
			H264BitstreamParser h264_bitstream_parser_;
			if (encoded_image->size() > 0)
			{
				h264_bitstream_parser_.ParseBitstream(
					encoded_image->data(), encoded_image->size());
				auto qp = h264_bitstream_parser_.GetLastSliceQp();
				if (qp.has_value())
				{
					encoded_image->qp_ = qp.value();
					// LOG_WARN("<%p> QP = %d <%ux%u>", enc, qp.value(), enc_impl_ptr->width_, enc_impl_ptr->height_);
				}
			}
		}

		CodecSpecificInfo codec_specific;
		codec_specific.codecType = kVideoCodecH264;
		codec_specific.codecSpecific.H264.packetization_mode = enc_impl_ptr->GetH264PacketizationMode();
		codec_specific.codecSpecific.H264.temporal_idx = kNoTemporalIdx;
		codec_specific.codecSpecific.H264.idr_frame = (encoded_image->_frameType == VideoFrameType::kVideoFrameKey);
		codec_specific.codecSpecific.H264.base_layer_sync = false;

		/* OUTPUT */
		enc_impl_ptr->GetEncodedImageCallback()->OnEncodedImage(
			*encoded_image, &codec_specific, &frag_header);

		// encoder qbuffer for capture plane
		if (enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
		{
			LOG_ERROR("Error while Qing buffer at capture plane");
			return false;
		}

		return true;
	}

	JetsonH264EncoderImpl::JetsonH264EncoderImpl(const cricket::VideoCodec &codec, const vts_rtc::RtcConfig &rtc_config)
		: rtc_config_(rtc_config)
	{
		RTC_CHECK(absl::EqualsIgnoreCase(codec.name, cricket::kH264CodecName));
		std::string packetization_mode_string;
		if (codec.GetParam(cricket::kH264FmtpPacketizationMode,
						   &packetization_mode_string) &&
			packetization_mode_string == "1")
		{
			packetization_mode_ = H264PacketizationMode::NonInterleaved;
		}

		if (rtc_config_.use_strategy)
		{
			auto iter = rtc_config_.strategy.begin();
			LOG_INFO("Resolution vs Bitrate strategy:");
			while (iter != rtc_config_.strategy.end())
			{
				ResolutionBitrateLimits sub_sstrategy(iter->first, iter->second[0], iter->second[1], iter->second[2]);
				LOG_INFO("%ld %ld %ld %ld", iter->first, iter->second[0], iter->second[1], iter->second[2]);
				resolution_bitrate_limits_.push_back(sub_sstrategy);
				iter++;
			}
		}

		if (0 != rtc_config_.encode_params.qp_range.first && 0 != rtc_config_.encode_params.qp_range.second)
		{
			qp_range_ = rtc_config_.encode_params.qp_range;
			LOG_INFO("Set qp_range = [%u, %u]", qp_range_.first, qp_range_.second);
		}

		if (0 != rtc_config_.encode_params.qp_threshold.first && 0 != rtc_config_.encode_params.qp_threshold.second)
		{
			qp_threshold_ = rtc_config_.encode_params.qp_threshold;
			LOG_INFO("Set qp_threshold = [%u, %u]", qp_threshold_.first, qp_threshold_.second);
		}

		if (0 != rtc_config_.encode_params.I_frame_interval)
		{
			I_frame_interval_ = rtc_config_.encode_params.I_frame_interval;
			LOG_INFO("Set I_frame_interval = [%u]", I_frame_interval_);
		}

		if (!rtc_config_.encode_params.bitrate_mode.empty())
		{
			bitrate_mode_ = rtc_config_.encode_params.bitrate_mode;
			LOG_INFO("Set bitrate_mode = [%s]", bitrate_mode_.c_str());
		}

		if (save_stream_)
		{
			char filename[30];
			sprintf(filename, "jetson_video_%p.h264", this);
			stream_file_ = new std::ofstream(filename);
			if (!stream_file_->is_open())
				LOG_ERROR("Create outfile failed");
		}
	}

	JetsonH264EncoderImpl::~JetsonH264EncoderImpl()
	{
		CodecPool::GetInstance()->StopCreateEncoder();
	}

	int32_t JetsonH264EncoderImpl::Release()
	{
		if (rtc_config_.encode_params.use_codec_pool)
			return ReleaseWithCodecPool();
		else
			return ReleaseWithoutCodecPool();
	}

	int32_t JetsonH264EncoderImpl::ReleaseWithCodecPool()
	{
		if (jetsonh264_encoder_)
		{
			release_flag_ = true;
			stop_flag_ = true;

			int ret = 0;
			ret = jetsonh264_encoder_->output_plane.setStreamStatus(false);
			if (ret < 0)
				LOG_ERROR("Set output plane status failed");
			ret = jetsonh264_encoder_->capture_plane.setStreamStatus(false);
			if (ret < 0)
				LOG_ERROR("Set capture plane status failed");
			jetsonh264_encoder_->capture_plane.waitForDQThread(-1);
			delete jetsonh264_encoder_;

			CodecPool::GetInstance()->ReleaseEncoder(width_, height_, jetsonh264_encoder_);
			jetsonh264_encoder_ = nullptr;
		}

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t JetsonH264EncoderImpl::ReleaseWithoutCodecPool()
	{
		if (jetsonh264_encoder_)
		{
			release_flag_ = true;
			int ret = 0;
			ret = jetsonh264_encoder_->output_plane.setStreamStatus(false);
			if (ret < 0)
				LOG_ERROR("Set output plane status failed");
			ret = jetsonh264_encoder_->capture_plane.setStreamStatus(false);
			if (ret < 0)
				LOG_ERROR("Set capture plane status failed");
			jetsonh264_encoder_->capture_plane.waitForDQThread(-1);
			LOG_INFO("Release() called for JetsonH264Encoder <%p><%ux%u>", jetsonh264_encoder_, width_, height_);

			delete jetsonh264_encoder_;
		}

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int JetsonH264EncoderImpl::InitEncode(const VideoCodec *codec_settings,
										  const VideoEncoder::Settings &settings)
	{
		if (rtc_config_.encode_params.use_codec_pool)
			return InitEncodeWithCodecPool(codec_settings, settings);
		else
			return InitEncodeWithoutCodecPool(codec_settings, settings);
	}

	int JetsonH264EncoderImpl::InitEncodeWithCodecPool(const VideoCodec *codec_settings, const VideoEncoder::Settings &settings)
	{
		int ret = 0;
		ReportInit();

		if (!codec_settings || codec_settings->codecType != kVideoCodecH264)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		if (codec_settings->maxFramerate == 0)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		if (codec_settings->width < 1 || codec_settings->height < 1)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		codec_ = *codec_settings;

		// Codec expects simulcastStream resolutions to be correct, make sure they are
		// filled even when there are no simulcast layers.
		if (codec_.numberOfSimulcastStreams == 0)
		{
			codec_.simulcastStream[0].width = codec_.width;
			codec_.simulcastStream[0].height = codec_.height;
		}

		width_ = codec_.width;
		height_ = codec_.height;

		jetsonh264_encoder_ = CodecPool::GetInstance()->GetAvailableEncoder(codec_.width, codec_.height);
		if (!jetsonh264_encoder_)
		{
			LOG_ERROR("No available encoder for resolution:<%ux%u> <%p>", codec_.width, codec_.height, this);
			return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
		}

		/* Set encoder capture plane dq thread callback for blocking io mode */
		jetsonh264_encoder_->capture_plane.setDQThreadCallback(CapturePlaneDqCallbackWithCodecPool);
		jetsonh264_encoder_->capture_plane.startDQThread(this);

		for (uint32_t i = 0; i < jetsonh264_encoder_->capture_plane.getNumBuffers() - 1; i++)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));
			v4l2_buf.index = i;
			v4l2_buf.m.planes = planes;
			ret = jetsonh264_encoder_->capture_plane.qBuffer(v4l2_buf, NULL);
			if (ret < 0)
				LOG_ERROR("Error while queueing buffer at capture plane <%p>", jetsonh264_encoder_);
		}

		const size_t new_capacity =
			CalcBufferSize(VideoType::kI420, width_, height_);
		encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
		encoded_image_._completeFrame = true;
		encoded_image_._encodedWidth = width_;
		encoded_image_._encodedHeight = height_;
		encoded_image_.set_size(0);

		LOG_INFO("<%p> Init JetsonH264Encoder<%p><%ux%u> finish", this, jetsonh264_encoder_, width_, height_);

		release_flag_ = false;

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int JetsonH264EncoderImpl::InitEncodeWithoutCodecPool(const VideoCodec *codec_settings, const VideoEncoder::Settings &settings)
	{
		LOG_INFO("Init JetsonH264Encoder");
		release_flag_ = false;
		int32_t ret = 0;
		ReportInit();

		if (!codec_settings || codec_settings->codecType != kVideoCodecH264)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		if (codec_settings->maxFramerate == 0)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		if (codec_settings->width < 1 || codec_settings->height < 1)
		{
			ReportError();
			return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
		}

		codec_ = *codec_settings;

		// Codec expects simulcastStream resolutions to be correct, make sure they are
		// filled even when there are no simulcast layers.
		if (codec_.numberOfSimulcastStreams == 0)
		{
			codec_.simulcastStream[0].width = codec_.width;
			codec_.simulcastStream[0].height = codec_.height;
		}

		width_ = codec_.width;
		height_ = codec_.height;

		jetsonh264_encoder_ = NvVideoEncoder::createVideoEncoder("enc0");

		ret = jetsonh264_encoder_->setCapturePlaneFormat(V4L2_PIX_FMT_H264, width_,
														 height_, 2 * 1024 * 1024);
		if (ret < 0)
			LOG_ERROR("Could not set capture plane format");

		ret = jetsonh264_encoder_->setOutputPlaneFormat(V4L2_PIX_FMT_YUV420M, width_,
														height_);
		if (ret < 0)
			LOG_ERROR("Could not set output plane format");

		ret = jetsonh264_encoder_->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
		if (ret < 0)
			LOG_ERROR("Could not set encoder profile");

		ret = jetsonh264_encoder_->setLevel((uint32_t)V4L2_MPEG_VIDEO_H264_LEVEL_3_1);
		if (ret < 0)
			LOG_ERROR("Could not set encoder level");

		/* Set rate control mode for encoder */
		ret = jetsonh264_encoder_->setRateControlMode(
			bitrate_mode_ == "vbr" ? V4L2_MPEG_VIDEO_BITRATE_MODE_VBR : V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
		if (ret < 0)
			LOG_ERROR("Could not set encoder rate control mode");

		// /* Set IDR frame interval for encoder */
		// ret = jetsonh264_encoder_->setIDRInterval(codec_settings->H264().keyFrameInterval);
		// if(ret < 0) LOG_ERROR("Could not set encoder IDR interval");

		/* Set I frame interval for encoder */
		ret = jetsonh264_encoder_->setIFrameInterval(I_frame_interval_);
		if (ret < 0)
			LOG_ERROR("Could not set encoder I-Frame interval");

		ret = jetsonh264_encoder_->setInsertSpsPpsAtIdrEnabled(true);
		if (ret < 0)
			printf("Could not set insertSPSPPSAtIDR\n");

		// /* Set framerate for encoder */
		ret = jetsonh264_encoder_->setFrameRate(30, 1);
		if (ret < 0)
			LOG_ERROR("Could not set framerate");

		ret = jetsonh264_encoder_->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
		if (ret < 0)
			LOG_ERROR("Could not setHWPresetType");

		ret = jetsonh264_encoder_->setMaxPerfMode(1);
		if (ret < 0)
			LOG_ERROR("Could not setMaxPerfMode");

		uint32_t nMinQpI = qp_range_.first;
		uint32_t nMaxQpI = qp_range_.second;
		uint32_t nMinQpP = 35;
		uint32_t nMaxQpP = 45;
		uint32_t nMinQpB = 40;
		uint32_t nMaxQpB = 45;
		/* Set Min & Max qp range values for I/P/B-frames to be used by encoder */
		ret = jetsonh264_encoder_->setQpRange(nMinQpI, nMaxQpI, nMinQpP, nMaxQpP, nMinQpB, nMaxQpB);
		if (ret < 0)
			LOG_ERROR("Could not set quantization parameters");

		ret = jetsonh264_encoder_->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 1, false, true);
		if (ret < 0)
			LOG_ERROR("Could not setup output plane");
		ret = jetsonh264_encoder_->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 1, true, false);
		if (ret < 0)
			LOG_ERROR("Could not setup capture plane");

		/* set encoder output plane STREAMON */
		ret = jetsonh264_encoder_->output_plane.setStreamStatus(true);
		if (ret < 0)
			LOG_ERROR("Error in output plane streamon");
		/* set encoder capture plane STREAMON */
		ret = jetsonh264_encoder_->capture_plane.setStreamStatus(true);
		if (ret < 0)
			LOG_ERROR("Error in capture plane streamon");

		/* Set encoder capture plane dq thread callback for blocking io mode */
		jetsonh264_encoder_->capture_plane.setDQThreadCallback(CapturePlaneDqCallbackWithoutCodecPool);
		jetsonh264_encoder_->capture_plane.startDQThread(this);

		/* Enqueue all the empty capture plane buffers. */
		for (uint32_t i = 0; i < jetsonh264_encoder_->capture_plane.getNumBuffers(); i++)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];

			memset(&v4l2_buf, 0, sizeof(v4l2_buf));
			memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

			v4l2_buf.index = i;
			v4l2_buf.m.planes = planes;

			ret = jetsonh264_encoder_->capture_plane.qBuffer(v4l2_buf, NULL);
			if (ret < 0)
				LOG_ERROR("Error while queueing buffer at capture plane");
		}

		const size_t new_capacity =
			CalcBufferSize(VideoType::kI420, width_, height_);
		encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
		encoded_image_._completeFrame = true;
		encoded_image_._encodedWidth = width_;
		encoded_image_._encodedHeight = height_;
		encoded_image_.set_size(0);

		LOG_INFO("Init JetsonH264Encoder<%p><%ux%u> finish", jetsonh264_encoder_, width_, height_);

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t JetsonH264EncoderImpl::RegisterEncodeCompleteCallback(
		EncodedImageCallback *callback)
	{
		encoded_image_callback_ = callback;

		return WEBRTC_VIDEO_CODEC_OK;
	}

	int32_t JetsonH264EncoderImpl::Encode(const VideoFrame &input_frame,
										  const std::vector<VideoFrameType> *frame_types)
	{

		if (!jetsonh264_encoder_)
		{
			ReportError();
			LOG_ERROR("jetsonh264_encoder is null for <%ux%u> <%p>", width_, height_, this);
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

		if (frame_types && (*frame_types)[0] == VideoFrameType::kEmptyFrame)
		{
			LOG_WARN("Ignore empty frame");
			return WEBRTC_VIDEO_CODEC_OK;
		}

		// Request Key frame
		if (frame_types && (*frame_types)[0] == VideoFrameType::kVideoFrameKey)
		{
			jetsonh264_encoder_->forceIDR();
		}

		int32_t ret = 0;
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		NvBuffer *nvBuffer = jetsonh264_encoder_->output_plane.getNthBuffer(0);
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = 0;
		v4l2_buf.m.planes = planes;

		for (uint32_t i = 0; i < (*nvBuffer).n_planes; i++)
		{
			NvBuffer::NvBufferPlane &plane = (*nvBuffer).planes[i];
			plane.bytesused = 0;
			if (i == 0)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
				memcpy(plane.data, frame_buffer->DataY(), bytes_to_read);
				plane.bytesused = bytes_to_read;
			}
			if (i == 1)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
				memcpy(plane.data, frame_buffer->DataU(), bytes_to_read);
				plane.bytesused = bytes_to_read;
			}
			if (i == 2)
			{
				std::streamsize bytes_to_read = plane.fmt.bytesperpixel * plane.fmt.width * plane.fmt.height;
				memcpy(plane.data, frame_buffer->DataV(), bytes_to_read);
				plane.bytesused = bytes_to_read;
			}
		}

		v4l2_buf.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		v4l2_buf.timestamp.tv_sec = input_frame.timestamp();

		ret = jetsonh264_encoder_->output_plane.qBuffer(v4l2_buf, NULL);
		if (ret < 0)
			LOG_ERROR("Encoder qBuffer error <%p>", jetsonh264_encoder_);

		ret = jetsonh264_encoder_->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
			LOG_ERROR("Error DQing buffer at output plane");

		return WEBRTC_VIDEO_CODEC_OK;
	}

	VideoEncoder::EncoderInfo JetsonH264EncoderImpl::GetEncoderInfo() const
	{
		EncoderInfo info;
		info.supports_native_handle = false;
		info.implementation_name = "NvJetsonEncH264";
		info.scaling_settings =
			VideoEncoder::ScalingSettings(qp_threshold_.first, qp_threshold_.second);
		info.is_hardware_accelerated = true;
		info.has_internal_source = false;
		info.supports_simulcast = false;
		info.scaling_settings.min_pixels_per_frame = 360 * 180;
		if (rtc_config_.use_strategy)
		{
			info.resolution_bitrate_limits = resolution_bitrate_limits_;
		}
		return info;
	}

	void JetsonH264EncoderImpl::SetFecControllerOverride(
		FecControllerOverride *fec_controller_override)
	{
	}

	void JetsonH264EncoderImpl::SetRates(const RateControlParameters &parameters)
	{
		int32_t ret = 0;

		if (!jetsonh264_encoder_)
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

		if (fps_ != fps)
		{
			ret = jetsonh264_encoder_->setFrameRate(fps, 1);
			if (ret < 0)
				LOG_ERROR("Could not set framerate");
			fps_ = fps;
			// LOG_INFO("SetRates<%p> fps:%u <%dx%d>", jetsonh264_encoder_, fps, width_, height_);
		}
		if (bitrate_ != bitrate)
		{
			ret = jetsonh264_encoder_->setPeakBitrate(bitrate);
			if (ret < 0)
				LOG_ERROR("Could not set encoder bitrate");
			bitrate_ = bitrate;
			// LOG_INFO("SetRates<%p> bitrate:%u <%dx%d>", jetsonh264_encoder_, bitrate, width_, height_);
		}
	}

	void JetsonH264EncoderImpl::OnPacketLossRateUpdate(float packet_loss_rate)
	{
		// LOG_WARN("Loss rate: %f", packet_loss_rate);
	}

	void JetsonH264EncoderImpl::OnRttUpdate(int64_t rtt_ms)
	{
		// LOG_WARN("rtt: %u", rtt_ms);
	}

	void JetsonH264EncoderImpl::OnLossNotification(
		const LossNotification &loss_notification)
	{
		auto delta = loss_notification.timestamp_of_last_decodable -
					 loss_notification.timestamp_of_last_received;
		// LOG_INFO("Loss delta: %d", delta);
	}

	void JetsonH264EncoderImpl::ReportInit()
	{
		if (has_reported_init_)
		{
			return;
		}

		RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
								  static_cast<int>(H264EncoderImplEvent::H264EncoderEventInit),
								  static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

		has_reported_init_ = true;
	}

	void JetsonH264EncoderImpl::ReportError()
	{
		if (has_reported_error_)
		{
			return;
		}

		RTC_HISTOGRAM_ENUMERATION("WebRTC.Video.H264EncoderImpl.Event",
								  static_cast<int>(H264EncoderImplEvent::H264EncoderEventError),
								  static_cast<int>(H264EncoderImplEvent::H264EncoderEventMax));

		has_reported_error_ = true;
	}

} // namespace webrtc
