#pragma once

#include "rtc.h"
#include "rtc_videorender.h"
#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QListWidget>

#define CHECK_RTCAGENT_VALID if (!rtc_agent_) { \
	QMessageBox::warning(nullptr, tr("Warning"), tr("RtcAgent create failed")); \
	return; \
}

class RtcWidget : public QWidget {
	Q_OBJECT

public:
	explicit RtcWidget(const std::string& rtc_config_filepath, const QString& yuv_folderpath, 
		QWidget* parent = 0);
	~RtcWidget();

private:
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
	std::unordered_map<vts_rtc::RoomCode, const char*> roomcode_map = {
		{ vts_rtc::RoomCode::OK, "OK" },
		{ vts_rtc::RoomCode::InternalError, "InternalError" },
		{ vts_rtc::RoomCode::RoomNotExisted, "RoomNotExisted" },
		{ vts_rtc::RoomCode::RoomAlreadyExisted, "RoomAlreadyExisted" },
		{ vts_rtc::RoomCode::AgentAlreadyInRoom, "AgentAlreadyInRoom" },
		{ vts_rtc::RoomCode::AgentNotLogined, "AgentNotLogined" }
	};

	bool external_feed_inited_ = false;
	QString yuv_folderpath_;
	std::vector<vts_rtc::YUV420pFrame> yuv_frames_;
	std::shared_ptr<vts_rtc::RtcAgent> rtc_agent_;
	QComboBox* videosources_combobox_;
	QLineEdit* open_room_edit_;
	QComboBox* rooms_combobox_;
	QListWidget* recv_msg_listwgt_;
	QLineEdit* send_msg_edit_;
	RtcVideoRender* rtc_videorender_;
};
