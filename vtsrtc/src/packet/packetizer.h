#ifndef _PACKETIZER_H_
#define _PACKETIZER_H_

#include <thread>
#include <queue>
#include <mutex>
#include "buffer.h"
#include "squeue.h"
#include "packtype.h"
#include "semaphore.h"

class Packetizer
{
public:
    Packetizer();
    ~Packetizer();

public:
    int Split(const char* data, unsigned int len, PackUserParams* params);
    int Resend(const char* data, unsigned int len, PackUserParams* params);
    void Stop();

private:
    int DoPacketize(const char* data, unsigned int len, char* packet, unsigned int* size);
    void Process();
    int SendPacket();
    int ResendPacket();

private:
    unsigned short version_ = 0x01;
    unsigned short payload_size_ = 0x00;

    unsigned short total_sub_packet_numb_ = 0x00;
    unsigned short main_packet_seq_ = 0x00;
    unsigned short sub_packet_seq_ = 0x00;

    unsigned int total_payload_size_ = 0x00;

    std::thread split_thread_;
    SafeQueue packetizer_queue_;
    SafeQueue resend_queue_;
    std::mutex mutex_;
    bool stop_ = false;
    semaphore sem_;
};

#endif