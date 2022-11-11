#include "packetizer.h"
#include "log/log_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <functional>
#include "log/log_manager.h"

/*                           Protocol
* version 1, max seq number = 2^16 = 65,535
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=1|        payload size       |    total sub packet number    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  main packet sequence number  |   sub packet sequence number  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|
                             payload
                                                                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/

/*                           Protocol
* version 2, max seq number = 2^32 = 4,294,967,295
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

Packetizer::Packetizer()
{
	split_thread_ = std::thread(std::bind(&Packetizer::Process, this));
}

Packetizer::~Packetizer()
{
	if (split_thread_.joinable())
	{
		split_thread_.join();
	}
}

int Packetizer::Split(const char* data, unsigned int len, PackUserParams* params)
{
	if (packetizer_queue_.Capacity() < kMaxSendQueueCapacity)
	{
		auto buffer = Buffer(data, len, params);
		if (buffer.GetBuffer())
		{
			packetizer_queue_.Push(buffer);
			sem_.signal();
		}
		else
		{
			LOG_ERROR("Try to push empty Buffer into packetizer queue");
			return PACK_INPUT_EMPTY_BUFFER;
		}
	}
	else
	{
		LOG_WARN("Send queue full");
		return PACK_SEND_QUEUE_FULL;
	}
    return PACK_OK;
}

int Packetizer::Resend(const char* data, unsigned int len, PackUserParams* params)
{
	if (resend_queue_.Capacity() < kMaxSendQueueCapacity)
	{
		auto buffer = Buffer(data, len, params);
		if (buffer.GetBuffer())
		{
			resend_queue_.Push(buffer);
			sem_.signal();
		}
		else
		{
			LOG_ERROR("Try to push empty Buffer into resend queue");
			return PACK_INPUT_EMPTY_BUFFER;
		}
	}
	else
	{
		LOG_WARN("Resend queue full");
		return PACK_SEND_QUEUE_FULL;
	}
	return PACK_OK;
}

void Packetizer::Stop()
{
	stop_ = true;
}

int Packetizer::DoPacketize(const char* data, unsigned int len, char* packet, unsigned int* size)
{
    if(data == nullptr)
    {
        return -1;
    }

    if(len > kMaxDataLen || len == 0)
    {
        return -1;
    }

    // version with placeholder: 2bytes
    unsigned short first_bit = version_ << 14 | len;
    char* value = (char*)&first_bit;
    *packet++ = value[1];
    *packet++ = value[0];

	// total sub packet number: 2bytes
    value = (char*)&total_sub_packet_numb_;
	*packet++ = value[1];
	*packet++ = value[0];

    // sequence number: 2bytes
    value = (char*)&main_packet_seq_;
    *packet++ = value[1];
    *packet++ = value[0];

    // sub packet sequence number: 2bytes
    value = (char*)&sub_packet_seq_;
    *packet++ = value[1];
    *packet++ = value[0];

    memcpy(packet, data, len);
    *size = len + kProtocolHeaderLen;

    return PACK_OK;
}

void Packetizer::Process()
{
	while (!stop_)
	{
		// Resend queue have higher priority than msg queue
		if (packetizer_queue_.Empty() && resend_queue_.Empty())
		{
			sem_.wait();
			continue;
		}

		if (!resend_queue_.Empty())
		{
			ResendPacket();
			continue;
		}

		if (!packetizer_queue_.Empty())
		{
			SendPacket();
		}
	}
}

int Packetizer::SendPacket()
{
	auto buffer = packetizer_queue_.Front();
	packetizer_queue_.Pop();

	unsigned int len = buffer.GetBufferSize();
	char* data = buffer.GetBuffer();
	if (nullptr == data)
	{
		LOG_ERROR("Invalid data, skip");
		return PACK_INPUT_EMPTY_BUFFER;
	}

	PackUserParams* params = buffer.GetUserPtr();
	if (nullptr == params)
	{
		LOG_ERROR("Invalid user params, skip");
		return PACK_INPUT_EMPTY_BUFFER;
	}

	// Get total sub packet number
	total_sub_packet_numb_ = len % kMaxDataLen ? (len / kMaxDataLen + 1) : len / kMaxDataLen;
	total_payload_size_ = len;

	while (len > 0)
	{
		if (len < kMaxDataLen)
		{
			char* slice = (char*)malloc(len);
			if (nullptr == slice)
			{
				LOG_ERROR("Malloc failed");
				continue;
			}
			memcpy(slice, data, len);
			payload_size_ = len;

			char packet_out[kMaxPacketSize];
			unsigned int size_out;
			DoPacketize(slice, len, packet_out, &size_out);

			if (params->packet_cb == nullptr)
			{
				LOG_ERROR("Invalid packet callback");
			}
			else
			{
				params->packet_cb(packet_out, size_out, params);
			}
			free(slice);

			len = 0;
			sub_packet_seq_++;
		}
		else
		{
			char* slice = (char*)malloc(kMaxDataLen);
			if (nullptr == slice)
			{
				LOG_ERROR("Malloc failed");
				continue;
			}
			memcpy(slice, data, kMaxDataLen);
			payload_size_ = kMaxDataLen;

			char packet_out[kMaxPacketSize];
			unsigned int size_out;
			DoPacketize(slice, kMaxDataLen, packet_out, &size_out);

			if (params->packet_cb == nullptr)
			{
				LOG_ERROR("Invalid packet callback");
			}
			else
			{
				params->packet_cb(packet_out, size_out, params);
			}
			free(slice);

			// move data pointer to next packet position
			data += kMaxDataLen;
			// unpacked length
			len -= kMaxDataLen;
			sub_packet_seq_++;
		}
	}

	payload_size_ = 0;
	sub_packet_seq_ = 0;
	main_packet_seq_++;

	return PACK_OK;
}

int Packetizer::ResendPacket()
{
	auto buffer = resend_queue_.Front();
	resend_queue_.Pop();

	unsigned int len = buffer.GetBufferSize();
	char* data = buffer.GetBuffer();
	if (nullptr == data)
	{
		LOG_ERROR("Invalid data, skip");
		return PACK_INPUT_EMPTY_BUFFER;
	}

	PackUserParams* params = buffer.GetUserPtr();
	if (nullptr == params)
	{
		LOG_ERROR("Invalid user params, skip");
		return PACK_INPUT_EMPTY_BUFFER;
	}

	if (params->packet_cb == nullptr)
	{
		LOG_ERROR("Invalid packet callback");
	}
	else
	{
		params->packet_cb(data, len, params);
	}

	return PACK_OK;
}