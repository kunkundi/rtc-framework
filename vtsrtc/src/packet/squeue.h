#pragma once

#include "buffer.h"
#include <queue>
#include <mutex>

class SafeQueue
{
public:
    SafeQueue()
    {

    }

    ~SafeQueue()
    {
        std::queue<Buffer> empty;
        std::swap(empty,queue_);
    }

    void Push(Buffer& buffer)
    {
        std::unique_lock<std::mutex>lock(mutex_);
        queue_.push(buffer);
        ++size_;
        capacity_ += buffer.GetBufferSize();
    }

    Buffer& Front()
    {
        std::unique_lock<std::mutex>lock(mutex_);
        return queue_.front();
    }

    void Pop()
    {
        std::unique_lock<std::mutex>lock(mutex_);
        capacity_ -= queue_.front().GetBufferSize();
        queue_.pop();
        --size_;
    }

    bool Empty()
    {
        std::unique_lock<std::mutex>lock(mutex_);
        return queue_.empty();
    }

    unsigned int Size()
    {
        return size_;
    }

    unsigned long Capacity()
    {
        return capacity_;
    }

private:
    std::queue<Buffer> queue_;
    std::mutex mutex_; 
    unsigned int size_ = 0;
    unsigned long capacity_ = 0;
};