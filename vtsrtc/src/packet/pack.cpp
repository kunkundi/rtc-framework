#include "pack.h"
#include "packtype.h"
#include "packetizer.h"
#include "depacketizer.h"
#include "log/log_manager.h"
#include <iostream>
#include <map>

Packetizer* g_packetizer = nullptr;
Depacketizer* g_depacketizer = nullptr;

void PackInit()
{
	if (g_packetizer == nullptr)
		g_packetizer = new Packetizer();

	if (g_depacketizer == nullptr)
		g_depacketizer = new Depacketizer();
}

void PackDestory()
{
	if (g_packetizer)
	{
		delete g_packetizer;
		g_packetizer = nullptr;
	}

	if (g_depacketizer)
	{
		delete g_depacketizer;
		g_depacketizer = nullptr;
	}
}

int PackSend(const char* data, unsigned int len, PackUserParams* params)
{
    return g_packetizer->Split(data, len, params);
}

int PackResend(const char* data, unsigned int len, PackUserParams* params)
{
	return g_packetizer->Resend(data, len, params);
}

int PackReceive(char* packet, unsigned int size, PackUserParams* params)
{
	return g_depacketizer->Combine(packet, size, params);
}
