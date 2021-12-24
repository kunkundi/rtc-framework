#pragma once

#include <QObject>

class RtcAudioRender : QObject {
	Q_OBJECT

public:
	explicit RtcAudioRender(QObject* parent = nullptr);
	~RtcAudioRender();

	void OnAudioFrame(const char* sourceid, unsigned int sourcetype,
		size_t bits_per_sample, size_t sample_rate,
		size_t number_of_channels, size_t number_of_frames,
		const void* audio_data, size_t sz_audio_data) const;
};
