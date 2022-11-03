#pragma once

#ifndef _BUFFER_H_
#define _BUFFER_H_

#include <stdlib.h>
#include <string.h>
#include "packtype.h"

class Buffer
{
public:
	Buffer()
	{

	}

	Buffer(const char* buffer, unsigned int buffer_size, PackUserParams* user_ptr)
	{
		buffer_ = (char*)malloc(buffer_size);
		if (buffer_)
		{
			memcpy(buffer_, buffer, buffer_size);
			buffer_size_ = buffer_size;
		}

		user_ptr_ = (PackUserParams*)malloc(sizeof(PackUserParams));
		if (user_ptr_)
		{
			user_ptr_->packet_cb = user_ptr->packet_cb;
			user_ptr_->data_cb = user_ptr->data_cb;
			user_ptr_->ptr1 = user_ptr->ptr1;
			user_ptr_->ptr2 = user_ptr->ptr2;
		}
	}

	~Buffer()
	{
		if (buffer_)
		{
			free(buffer_);
			buffer_ = nullptr;
		}

		if (user_ptr_)
		{
			free(user_ptr_);
			user_ptr_ = nullptr;
		}

		buffer_size_ = 0;
	}

	Buffer(const Buffer& obj)
	{
		this->buffer_ = (char*)malloc(obj.buffer_size_);
		if (this->buffer_)
		{
			memcpy(this->buffer_, obj.buffer_, obj.buffer_size_);
			this->buffer_size_ = obj.buffer_size_;
		}

		this->user_ptr_ = (PackUserParams*)malloc(sizeof(PackUserParams));
		if (this->user_ptr_)
		{
			this->user_ptr_->packet_cb = obj.user_ptr_->packet_cb;
			this->user_ptr_->data_cb = obj.user_ptr_->data_cb;
			this->user_ptr_->ptr1 = obj.user_ptr_->ptr1;
			this->user_ptr_->ptr2 = obj.user_ptr_->ptr2;
		}
	}

	Buffer& operator = (const Buffer& rhs)
	{
		if (this == &rhs) {
			return *this;
		}

		this->buffer_ = (char*)malloc(rhs.buffer_size_);
		if (this->buffer_)
		{
			memcpy(this->buffer_, rhs.buffer_, rhs.buffer_size_);
			this->buffer_size_ = rhs.buffer_size_;
		}

		this->user_ptr_ = (PackUserParams*)malloc(sizeof(PackUserParams));
		if (this->user_ptr_)
		{
			this->user_ptr_->packet_cb = rhs.user_ptr_->packet_cb;
			this->user_ptr_->data_cb = rhs.user_ptr_->data_cb;
			this->user_ptr_->ptr1 = rhs.user_ptr_->ptr1;
			this->user_ptr_->ptr2 = rhs.user_ptr_->ptr2;
		}

		return *this;
	}

public:
	char* GetBuffer()
	{
		return buffer_;
	}

	unsigned int GetBufferSize()
	{
		return buffer_size_;
	}

	PackUserParams* GetUserPtr()
	{
		return user_ptr_;
	}

private:
    char* buffer_;
    unsigned int buffer_size_;
	PackUserParams* user_ptr_;
};

#endif