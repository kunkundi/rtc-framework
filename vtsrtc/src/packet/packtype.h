#ifndef _TYPEDEF_H_
#define _TYPEDEF_H_

#ifdef __cplusplus
extern "C" {
#endif

#define kMaxPacketSize 1170
#define kProtocolHeaderLen 8
#define kMaxDataLen 1162

// #define kMaxPacketSize 16384
// #define kProtocolHeaderLen 8
// #define kMaxDataLen 16376
#define kMaxSendQueueCapacity 64*1024*1024

#define PACK_OK					0xA0000000
#define PACK_SEND_QUEUE_FULL	0xA0000001
#define PACK_INPUT_EMPTY_BUFFER	0xA0000002
#define PACK_EMPTY_BUFFER		0xA0000003

struct PackUserParams;

typedef void(*PacketsCallback)(char* packet, unsigned int size, PackUserParams* user_ptr);
typedef void(*DataCallback)(char* data, unsigned int size, PackUserParams* user_ptr);

struct PackUserParams {
	PacketsCallback packet_cb;
	DataCallback data_cb;
	void* ptr1;
	void* ptr2;
};

# ifdef  __cplusplus
}
# endif
#endif