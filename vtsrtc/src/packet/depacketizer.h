#ifndef _DEPACKETIZER_H_
#define _DEPACKETIZER_H_

#include <thread>
#include <queue>
#include <map>
#include <mutex>
#include <fstream>
#include <iostream>
#include "buffer.h"
#include "semaphore.h"
#include "squeue.h"
#include "packtype.h"

class Depacketizer
{
public:
    Depacketizer();
    ~Depacketizer();

public:
    int Combine(const char* packet, unsigned int size, PackUserParams* params);
    void Stop();

private:
    Buffer DoDepacketize(const char* packet, unsigned int size, PackUserParams* user_ptr);
    void Process();

private:
    unsigned short GetVersion();
    unsigned short GetPayloadSize();
    unsigned short GetTotalSubPacketNumb();
    unsigned short GetMainPacketSeq();
    unsigned short GetSubPacketSeq();

    unsigned int GetTotalPayloadSize();

private:
    unsigned short version_ = 0x01;
    unsigned short main_packet_seq_ = 0x00;
    unsigned short total_sub_packet_numb_ = 0x00;
    unsigned short sub_packet_seq_ = 0x00;
    unsigned short payload_size_ = 0x00;

    std::map<unsigned int, std::map<unsigned int, Buffer>> incomplete_packet_;

	std::thread combine_thread_;
	SafeQueue depacketizer_queue_;
    std::mutex mutex_;
    bool stop_ = false;
	semaphore sem_;

    std::ofstream debug_file_;
};

#endif