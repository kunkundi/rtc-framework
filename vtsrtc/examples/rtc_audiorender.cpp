#include "rtc_audiorender.h"
#include <QDebug>

RtcAudioRender::RtcAudioRender(QObject* parent)
	: QObject(parent) {}

RtcAudioRender::~RtcAudioRender() {}

void RtcAudioRender::OnAudioFrame(const char* sourceid,
	unsigned int sourcetype, size_t bits_per_sample, size_t sample_rate,
	size_t number_of_channels, size_t number_of_frames,
	const void* audio_data, size_t sz_audio_data) const {
	qDebug() << "OnAudioFrame, sourceid: " << sourceid <<
		", sourcetype: " << sourcetype <<
		", bits_per_sample: " << bits_per_sample <<
		", sample_rate: " << sample_rate <<
		", number_of_channels: " << number_of_channels <<
		", number_of_frames: " << number_of_frames <<
		", sz_audio_data: " << sz_audio_data;
}
