#pragma once

#include "c_rtc.h"
#include "rtc_videorender.h"
#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QListWidget>

class RtcWidget : public QWidget {
	Q_OBJECT

public:
	explicit RtcWidget(const std::string& rtc_config_filepath, const QString& yuv_folderpath, 
		QWidget* parent = 0);
	~RtcWidget();

private:
	static void HandleMessage(RtcSessionId remote_sessionid, const char* channel_label, const char* msg, size_t msg_size);
	static void HandleFrame(RtcVideoSourceId sourceid, size_t width, size_t height, size_t dimension, 
		const unsigned char* buffer, size_t sz_buffer);
	static void HandleNetworkDisconnected();
	void CreateUI();
	void LoadYUVData();
	void SendFrame();

private slots:
	void QueryRooms();
	void OpenRoom();
	void JoinRoom();
	void LeaveRoom();
	void SendMessage();

private:
	bool external_feed_inited_ = false;
	QString yuv_folderpath_;
	std::vector<RtcYUV420pFrame> yuv_frames_;
	QComboBox* videosources_combobox_;
	QLineEdit* open_room_edit_;
	QComboBox* rooms_combobox_;
	static QListWidget* recv_msg_listwgt_;
	QLineEdit* send_msg_edit_;
	static RtcVideoRender* rtc_videorender_;
};
