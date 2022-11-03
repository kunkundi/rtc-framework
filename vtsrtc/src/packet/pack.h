#pragma once

#include "packtype.h"

#ifdef _WIN32
#ifdef ZT_DLL_EXPORTS
#define PACK_API 
#else
#define PACK_API 
#endif
#elif __linux__
#define PACK_API
#endif

#ifdef  __cplusplus
extern "C" {
#endif

	PACK_API void PackInit();
    PACK_API void PackDestory();
    PACK_API int PackSend(const char* data, unsigned int len, PackUserParams* params);
    PACK_API int PackResend(const char* data, unsigned int len, PackUserParams* params);
    PACK_API int PackReceive(char* packet, unsigned int size, PackUserParams* params);

#ifdef  __cplusplus
}
#endif
