#pragma once

#include "c_rtc.h"
#include "rtc_audiorender.h"
#if !defined  __aarch64__
#include "rtc_videorender.h"
#endif
#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QListWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <mutex>

class RtcWidget : public QWidget {
	Q_OBJECT

public:
	explicit RtcWidget(const std::string& rtc_config_filepath,
		const QString& pcmdata_filepath, const QString& yuv_folderpath,
		QWidget* parent = 0);
	~RtcWidget();

private:
	static void HandleRoom(RtcRoomOperation room_operation, RtcRoomId roomid);
	static void HandleP2PState(RtcSessionId sessionid, RtcP2PState state);
	static void HandleDataChannelState(RtcSessionId sessionid,
		RtcDataChannelLabel label, RtcDataChannelState state);
	static void HandleServerConnectionState(RtcServerConnectionState state);
	static void HandleSRSState(RtcSRSStreamurl streamurl, RtcP2PState state);
	static void HandleSRSResponse(RtcSRSStreamurl streamurl, RtcSRSResponse response);
	static void HandleChannelNetStats(RtcNetStats params);

	static void HandleMessage(RtcSessionId remote_sessionid,
		const char* channel_label, const char* msg, size_t msg_size);
	static void HandleAudioFrame(RtcAudioSourceId sourceid,
		RtcMediaSourceType sourcetype,
		size_t bits_per_sample, size_t sample_rate,
		size_t number_of_channels, size_t number_of_frames,
		const void* audio_data, size_t sz_audio_data);
	static void HandleFrame(RtcVideoSourceId sourceid,
		RtcMediaSourceType sourcetype,
		size_t width, size_t height, size_t dimension,
		const unsigned char* buffer, size_t sz_buffer);
	void CreateUI();
	void LoadPCMData();
	void LoadYUVData();
	void SendAudioFrame();
	void SendFrame();
	void AddAudioSource();
	void AddVideoSource();

private slots:
	void QueryRooms();
	void OpenRoom();
	void CloseRoom();
	void JoinRoom();
	void LeaveRoom();
	void PublishToSRS();
	void UnpublishToSRS();
	void PlayFromSRS();
	void UnplayFromSRS();
	void SendMessage();

private:
	bool audio_source_added_ = false,
		external_feed_inited_ = false,
		video_source_added_ = false,
		send_frame_flag = true;
	QString pcmdata_filepath_, yuv_folderpath_;
	std::vector<RtcPCMData> pcmdatas_;
	std::vector<RtcYUV420pFrame> yuv_frames_;
	std::mutex stop_audiothread_mtx_, stop_videothread_mtx_;
	bool stop_audiothread_ = false, stop_videothread_ = false;
	QThread* audiothread_, *videothread_;
	QComboBox* videosources_combobox_;
	QLineEdit* open_room_edit_;
	QComboBox* rooms_combobox_;
	QLineEdit* SRS_streamurl_edit_;
	static QListWidget* recv_msg_listwgt_;
	QLineEdit* send_msg_edit_;
	static QStandardItemModel* model_;
	static QTableView* tableView_;
	static QStringList* sourceid_list_;
#if !defined  __aarch64__
	static std::map<std::string, RtcVideoRender*> source_render_;
#endif
	static RtcAudioRender* rtc_audiorender_;
};
