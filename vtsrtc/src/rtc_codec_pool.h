#include "NvVideoEncoder.h"

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
    int ReleaseEncoder(int width, int height, NvVideoEncoder* enc);

private:
    CodecPool();

private:
    int InitTargetEncoder(int width, int height);

    // level = 0:INFO 1:ERROR 2:WARN 3:DEBUG
	void SetV4L2LogLevel(int level);

private:
    static CodecPool* instance_;

    std::map<int, int> resolution_map_ = {
        {288, 162}, 
        {384, 216}, 
        {432, 243}, 
        {576, 324}, 
        {864, 486},
        {1152, 648}
    };

    dqThreadCallback callback_ = nullptr;
    void* user_ptr_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};