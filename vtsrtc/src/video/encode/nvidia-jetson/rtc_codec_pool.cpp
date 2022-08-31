#include "rtc_codec_pool.h"
#include "log/log_manager.h"
#include <chrono>

using namespace std::chrono;
static std::multimap<int, std::pair<bool, NvVideoEncoder*>> encoder_pool_;
CodecPool* CodecPool::instance_ = nullptr;

NvVideoEncoder* CodecPool::GetAvailableEncoder(int width, int height) {
    int resolution = width * height;
    codecpool_mtx_.lock();
    auto low_bound_iter = encoder_pool_.lower_bound(resolution);
    auto upper_bound_iter = encoder_pool_.upper_bound(resolution);

    while (low_bound_iter != encoder_pool_.end() && low_bound_iter != upper_bound_iter)
	{
		if (low_bound_iter->second.first)
		{
            low_bound_iter->second.first = false;
            codecpool_mtx_.unlock();
			return low_bound_iter->second.second;
		}
		++low_bound_iter;
	}

    codecpool_mtx_.unlock();
    return nullptr;
}

void CodecPool::ReleaseEncoder(int width, int height, NvVideoEncoder* enc) {
    codecpool_thread_->PostTask(RTC_FROM_HERE, [this, width, height, enc]() {
        RTC_DCHECK_RUN_ON(codecpool_thread_.get());
        int resolution = width * height;

        auto low_bound_iter = encoder_pool_.lower_bound(resolution);
        auto upper_bound_iter = encoder_pool_.upper_bound(resolution);

        while (low_bound_iter != encoder_pool_.end() && low_bound_iter != upper_bound_iter)
        {
            if (low_bound_iter->second.first == false && low_bound_iter->second.second == enc)
            {
                low_bound_iter->second.second = nullptr;
                if(!stop_) {
                    low_bound_iter->second.second = CreateJetsonEncoder(width, height);
                    low_bound_iter->second.first = true;
                }
            }
            ++low_bound_iter;
        }
	});
}

void CodecPool::StopCreateEncoder() {
    stop_ = true;
}

CodecPool::CodecPool() {
}

void CodecPool::Init() {
    codecpool_thread_ = rtc::Thread::Create();
	codecpool_thread_->SetName("codecpool", nullptr);
	codecpool_thread_->Start();
    stop_ = false;

    for(auto it: resolution_map_) {
        LOG_INFO("Init encoder <%dx%d>", it.first, it.second);
        InitTargetEncoder(it.first, it.second);
    }

    inited_ = true;
}

void CodecPool::Destroy() {
    if(inited_) {
        codecpool_thread_->Stop();
        for(const auto& it:encoder_pool_) {
            if(it.second.first == true && it.second.second) {
                delete it.second.second;
            }
        }
    encoder_pool_.clear();
    inited_ = false;
    LOG_INFO("Destroy codec pool");
    }
}

int CodecPool::InitTargetEncoder(int width, int height) {
    NvVideoEncoder *enc = CreateJetsonEncoder(width, height);
    encoder_pool_.insert(std::make_pair(width * height, std::make_pair(true, enc)));
    return 0;
}

NvVideoEncoder* CodecPool::CreateJetsonEncoder(int width, int height) {
    int ret = 0;
    NvVideoEncoder *enc = nullptr;

	const auto target_width = width;
	const auto target_height = height;

	enc = NvVideoEncoder::createVideoEncoder("enc0");
    if(!enc) {
        LOG_ERROR("Cannot create jetson encoder");
        return nullptr;
    }

    ret = enc->setCapturePlaneFormat(V4L2_PIX_FMT_H264, target_width, target_height, 2 * 1024 * 1024);
    if(ret < 0) LOG_ERROR("Could not set capture plane format");

    ret = enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV420M, target_width, target_height);
    if(ret < 0) LOG_ERROR("Could not set output plane format");

    ret = enc->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    if(ret < 0) LOG_ERROR("Could not set encoder profile");

    ret = enc->setLevel((uint32_t)V4L2_MPEG_VIDEO_H264_LEVEL_3_1);
    if(ret < 0) LOG_ERROR("Could not set encoder level");

    /* Set rate control mode for encoder */
    ret = enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
    if(ret < 0) LOG_ERROR("Could not set encoder rate control mode");

    /* Set IDR frame interval for encoder */
    ret = enc->setIDRInterval(3000);
    if(ret < 0) LOG_ERROR("Could not set encoder IDR interval");

    /* Set I frame interval for encoder */
    ret = enc->setIFrameInterval(150);
    if(ret < 0) LOG_ERROR("Could not set encoder I-Frame interval");

	ret = enc->setInsertSpsPpsAtIdrEnabled(true);
    if(ret < 0) printf("Could not set insertSPSPPSAtIDR\n");

    // /* Set framerate for encoder */
    ret = enc->setFrameRate(30, 1);
    if(ret < 0) LOG_ERROR("Could not set framerate");

	ret = enc->setHWPresetType(V4L2_ENC_HW_PRESET_ULTRAFAST);
	if(ret < 0) LOG_ERROR("Could not setHWPresetType");

	ret = enc->setMaxPerfMode(1);
	if(ret < 0) LOG_ERROR("Could not setMaxPerfMode");

    uint32_t nMinQpI = 15;
    uint32_t nMaxQpI = 45;
    uint32_t nMinQpP = 25;
    uint32_t nMaxQpP = 45;
    uint32_t nMinQpB = 40;
    uint32_t nMaxQpB = 45;
    /* Set Min & Max qp range values for I/P/B-frames to be used by encoder */
    ret = enc->setQpRange(nMinQpI, nMaxQpI, nMinQpP, nMaxQpP, nMinQpB, nMaxQpB);
    if(ret < 0) LOG_ERROR("Could not set quantization parameters");

    ret = enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 1, false, true);
	if(ret < 0) LOG_ERROR("Could not setup output plane");
	ret = enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 1, true, false);
    if(ret < 0) LOG_ERROR("Could not setup capture plane");

    /* set encoder output plane STREAMON */
    ret = enc->output_plane.setStreamStatus(true);
    if(ret < 0) LOG_ERROR("Error in output plane streamon");
    /* set encoder capture plane STREAMON */
    ret = enc->capture_plane.setStreamStatus(true);
    if(ret < 0) LOG_ERROR("Error in capture plane streamon");

    return enc;
}