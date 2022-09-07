#include "NvVideoEncoder.h"
#include <api/task_queue/default_task_queue_factory.h>
#include <api/create_peerconnection_factory.h>
#include <rtc_base/thread.h>
#include <mutex>

typedef bool(*dqThreadCallback) (struct v4l2_buffer * v4l2_buf,
    NvBuffer * buffer, NvBuffer * shared_buffer, void *data);

class CodecPool
{
public:
    static CodecPool* GetInstance()
    { 
        if(instance_ == nullptr)
            instance_ = new CodecPool;  
        return instance_;  
    }

    void Init();
    void Destroy();
    NvVideoEncoder* GetAvailableEncoder(int width, int height);
    void ReleaseEncoder(int width, int height, NvVideoEncoder* enc);
    void StopCreateEncoder();

private:
    CodecPool();

private:
    int InitTargetEncoder(int width, int height);
    NvVideoEncoder* CreateJetsonEncoder(int width, int height);

private:
    static CodecPool* instance_;

    std::map<int, int> resolution_map_ = {
        {432, 243}, 
        {576, 324},
        // {648, 363},
        {864, 486},
        {1152, 648}
    };

    std::unique_ptr<rtc::Thread> codecpool_thread_;
    dqThreadCallback callback_ = nullptr;
    void* user_ptr_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    std::mutex codecpool_mtx_;
    bool stop_ = false;
    bool inited_ = false;
};