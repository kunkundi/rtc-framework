#include "depacketizer.h"
#include "log/log_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>

#define LOG_RECEIVE_DATA 0
/*                           Protocol
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=1|        payload size       |    total sub packet number    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  main packet sequence number  |   sub packet sequence number  |
+-+-+-+-+-+-+-+-+-+-+-+-+                                                                                                                                                 -+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|
                             payload
                                                                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

/*                           Protocol
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|        payload size       |    total sub packet number    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  main packet sequence number                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  sub packet sequence number                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|
                             payload
                                                                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

Depacketizer::Depacketizer()
{
	combine_thread_ = std::thread(std::bind(&Depacketizer::Process, this));
}

Depacketizer::~Depacketizer()
{
	if (combine_thread_.joinable())
	{
		combine_thread_.join();
	}
}

int Depacketizer::Combine(const char* packet, unsigned int size, PackUserParams* params)
{
	auto buffer = Buffer(packet, size, params);
	if (nullptr == buffer.GetBuffer())
	{
		return PACK_EMPTY_BUFFER;
	}

	depacketizer_queue_.Push(buffer);
	sem_.signal();
	return PACK_OK;
}

void Depacketizer::Stop()
{
	stop_ = true;
}

unsigned int Depacketizer::GetTotalPayloadSize()
{
	unsigned short total_sub_packet_numb = GetTotalSubPacketNumb();

	return total_sub_packet_numb * kMaxDataLen;
}

unsigned short Depacketizer::GetVersion()
{
	return version_;
}

unsigned short Depacketizer::GetPayloadSize()
{
	return payload_size_;
}

unsigned short Depacketizer::GetTotalSubPacketNumb()
{
	return total_sub_packet_numb_;
}

unsigned short Depacketizer::GetMainPacketSeq()
{
	return main_packet_seq_;
}

unsigned short Depacketizer::GetSubPacketSeq() 
{
	return sub_packet_seq_;
}

Buffer Depacketizer::DoDepacketize(const char* packet, unsigned int size, PackUserParams* user_ptr)
{
    if(packet == nullptr)
    {
		LOG_ERROR("Packet is nullptr");
        Buffer error;
        return error;
    }

    if(size == 0)
    {
		LOG_ERROR("Error packet size");
        Buffer error;
        return error;
    }
    
	// version with first 2byte: 2bytes
	unsigned short first_bit = 0x00;
	char* value = (char*)&first_bit;
	value[1] = *packet++;
	value[0] = *packet++;

	version_ = first_bit>>14;
	if (version_ != 1)
	{
		LOG_ERROR("Error packetizer version");
		Buffer error;
		return error;
	}

	payload_size_ = first_bit & (0xFFFF >> 2);

	if (payload_size_ < last_packet_payload_size_)
		last_packet_payload_size_ = payload_size_;

	// total sub packet number: 2bytes
	value = (char*)&total_sub_packet_numb_;
	value[1] = *packet++;
	value[0] = *packet++;

	// main packet sequence number: 2bytes
	value = (char*)&main_packet_seq_;
	value[1] = *packet++;
	value[0] = *packet++;

	// sub packet sequence number: 2bytes
	value = (char*)&sub_packet_seq_;
	value[1] = *packet++;
	value[0] = *packet++;

    return Buffer(packet, payload_size_, user_ptr);
}

void Depacketizer::Process()
{
	while (!stop_)
	{
		if (depacketizer_queue_.Empty())
		{
			sem_.wait();
			continue;
		}

		auto buffer = depacketizer_queue_.Front();
		if (nullptr == buffer.GetBuffer())
		{
			LOG_ERROR("Invalid buffer");
			continue;
		}

		PackUserParams* params = buffer.GetUserPtr();
		if (nullptr == params)
		{
			LOG_ERROR("Invalid user ptr");
			continue;
		}

		auto processed_buffer = DoDepacketize(buffer.GetBuffer(), buffer.GetBufferSize(), buffer.GetUserPtr());
		if (nullptr == processed_buffer.GetBuffer())
		{
			LOG_ERROR("Invalid buffer");
			continue;
		}

		incomplete_packet_[main_packet_seq_][sub_packet_seq_] = processed_buffer;

		if (incomplete_packet_[main_packet_seq_].size() == total_sub_packet_numb_)
		{
#if LOG_RECEIVE_DATA
			debug_file_.open("carinfo_receive.data", std::ios::out | std::ios::binary);
#endif
			unsigned int total_payload_size_real = GetTotalPayloadSize() - kMaxDataLen + 
				incomplete_packet_[main_packet_seq_][incomplete_packet_[main_packet_seq_].size() - 1].GetBufferSize();
			
			char* complete_packet = (char*)malloc(total_payload_size_real);
			if (nullptr == complete_packet)
			{
				LOG_ERROR("Malloc failed");
				continue;
			}
			
			unsigned int offset = 0;
			for (auto& it : incomplete_packet_[main_packet_seq_])
			{
#if LOG_RECEIVE_DATA
				debug_file_ << std::string(it.second.GetBuffer(), it.second.GetBufferSize());
#endif
				memcpy(complete_packet + offset, it.second.GetBuffer(), it.second.GetBufferSize());
				offset += it.second.GetBufferSize();
			}
#if LOG_RECEIVE_DATA
			debug_file_.close();
#endif
			if (params->data_cb == nullptr)
			{
				LOG_ERROR("Invalid raw data callback");
			}
			else
			{
				params->data_cb(complete_packet, total_payload_size_real, params);
			}
			incomplete_packet_.erase(main_packet_seq_);
			free(complete_packet);
		}

		depacketizer_queue_.Pop();
	}
}