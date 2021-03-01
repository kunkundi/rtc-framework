#include "rtc_widget.h"
#include <QDir>
#include <QBoxLayout>
#include <QPushButton>
#include <QGroupBox>
#include <QLabel>
#include <QAction>
#include <QMessageBox>
#include <QThread>
#include <QDebug>

RtcWidget::RtcWidget(const std::string& rtc_config_filepath, const QString& yuv_folderpath, QWidget* parent)
	: yuv_folderpath_(yuv_folderpath), QWidget(parent) {
	CreateUI();

	auto recv_msg_handler = [this](vts_rtc::SessionId remote_sessionid, const std::string& msg) {
		QString item_text = QString("%1 [from: %2]").arg(QString::fromLocal8Bit(msg.c_str())).arg(remote_sessionid);
		recv_msg_listwgt_->insertItem(0, item_text);
	};

	auto recv_frame_handler = std::bind(&RtcVideoRender::OnFrame, rtc_videorender_, std::placeholders::_1, 
		std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5);

	rtc_agent_ = vts_rtc::RtcAgent::Create(rtc_config_filepath, recv_msg_handler, recv_frame_handler);

	if (rtc_agent_) {
		videosources_combobox_->clear();
		auto video_devices = rtc_agent_->GetVideoDevices();
		for (const auto& device : video_devices) {
			videosources_combobox_->addItem(QString::fromLocal8Bit(device.device_name.c_str()));
		}
		videosources_combobox_->addItem("Local YUV420p");
	}
}

RtcWidget::~RtcWidget() {
}

void RtcWidget::CreateUI() {
	// create UI
	QBoxLayout* main_layout = new QVBoxLayout;

	// 视频源管理
	QGroupBox* videosource_groupbox = new QGroupBox(tr("Video Source Management"));
	QHBoxLayout* videosource_layout = new QHBoxLayout;
	QLabel* videosource_label = new QLabel(tr("Video Sources"));
	videosources_combobox_ = new QComboBox();
	videosource_layout->addWidget(videosource_label);
	videosource_layout->addWidget(videosources_combobox_);
	videosource_groupbox->setLayout(videosource_layout);

	// 房间管理
	QGroupBox* room_groupbox = new QGroupBox(tr("Room Management"));
	QGridLayout* room_layout = new QGridLayout;
	open_room_edit_ = new QLineEdit("zhejianglab");
	QPushButton* open_room_btn = new QPushButton(tr("Open Room"));
	rooms_combobox_ = new QComboBox();
	QPushButton* query_rooms_btn = new QPushButton(tr("Query Rooms"));
	QPushButton* join_room_btn = new QPushButton(tr("Join Room"));
	QPushButton* leave_room_btn = new QPushButton(tr("Leave Room"));
	room_layout->addWidget(open_room_edit_, 0, 0, 1, 4);
	room_layout->addWidget(open_room_btn, 0, 4, 1, 1);
	room_layout->addWidget(rooms_combobox_, 1, 0, 1, 2);
	room_layout->addWidget(query_rooms_btn, 1, 2, 1, 1);
	room_layout->addWidget(join_room_btn, 1, 3, 1, 1);
	room_layout->addWidget(leave_room_btn, 1, 4, 1, 1);
	room_groupbox->setLayout(room_layout);

	// 消息管理
	QGroupBox* msg_groupbox = new QGroupBox(tr("Message Management"));
	QGridLayout* msg_layout = new QGridLayout;
	QLabel* recv_msg_albel = new QLabel(tr("received message"));
	recv_msg_listwgt_ = new QListWidget;
	send_msg_edit_ = new QLineEdit("hello world");
	QPushButton* send_msg_btn = new QPushButton(tr("Send Message"));
	msg_layout->addWidget(recv_msg_albel, 0, 0, 1, 1);
	msg_layout->addWidget(recv_msg_listwgt_, 0, 1, 1, 3);
	msg_layout->addWidget(send_msg_edit_, 1, 0, 1, 3);
	msg_layout->addWidget(send_msg_btn, 1, 3, 1, 1);
	msg_groupbox->setLayout(msg_layout);

	rtc_videorender_ = new RtcVideoRender();

	main_layout->addWidget(videosource_groupbox);
	main_layout->addWidget(room_groupbox);
	main_layout->addWidget(msg_groupbox);
	main_layout->addWidget(rtc_videorender_, 0, Qt::AlignCenter);

	this->setLayout(main_layout);
	this->setMinimumSize(800, 600);

	// bind events
	connect(query_rooms_btn, SIGNAL(clicked()), this, SLOT(QueryRooms()));
	connect(open_room_btn, SIGNAL(clicked()), this, SLOT(OpenRoom()));
	connect(join_room_btn, SIGNAL(clicked()), this, SLOT(JoinRoom()));
	connect(leave_room_btn, SIGNAL(clicked()), this, SLOT(LeaveRoom()));
	connect(send_msg_btn, SIGNAL(clicked()), this, SLOT(SendMessage()));
}

void RtcWidget::LoadYUVData() {
	QStringList yuv_namefilters;
	yuv_namefilters << "*.yuv";
	QDir yuv_dir(yuv_folderpath_);
	auto files = yuv_dir.entryList(yuv_namefilters, QDir::Files | QDir::Readable, QDir::Name);

	vts_rtc::YUV420pFrame frame;
	frame.width = 1280;
	frame.height = 720;
	frame.stride_Y = frame.width;
	frame.stride_U = frame.width / 2;
	frame.stride_V = frame.width / 2;
	frame.buffer.resize(frame.height * frame.width * 3 / 2);

	for (const auto& file : files) {
		QString yuv_filepath = yuv_folderpath_ + "/" + file;
		FILE* fp = fopen(yuv_filepath.toLocal8Bit(), "rb");
		if (fp) {
			fread(frame.buffer.data(), 1, frame.height * frame.width * 3 / 2, fp);
			fflush(fp);
			fclose(fp);
			yuv_frames_.emplace_back(frame);
		}
	}
}

void RtcWidget::SendFrame() {
	if (yuv_frames_.size() > 0) {
		auto send_thread = QThread::create([this]() {
			size_t idx = 0;
			while (true) {
				if (idx == yuv_frames_.size()) {
					idx = 0;
				}
				rtc_agent_->OnFrame("external_feed", yuv_frames_[idx++]);
				QThread::msleep(30);
			}
			});

		send_thread->start();
	}
}

void RtcWidget::QueryRooms() {
	CHECK_RTCAGENT_VALID

	vts_rtc::Rooms rooms;
	auto code = rtc_agent_->QueryRooms(rooms);
	if (code != vts_rtc::RoomCode::OK) {
		QMessageBox::warning(nullptr, tr("Warning"), tr(roomcode_map[code]));
		return;
	}

	rooms_combobox_->clear();
	for (const auto& room_kv : rooms) {
		rooms_combobox_->addItem(QString::fromLocal8Bit(room_kv.first.c_str()));
	}
}

void RtcWidget::OpenRoom() {
	CHECK_RTCAGENT_VALID

	auto current_idx = videosources_combobox_->currentIndex();
	if (current_idx == videosources_combobox_->count() - 1) {
		// YUV420p video source
		rtc_agent_->AddVideoSource("external_feed");

		if (!external_feed_inited_) {
			LoadYUVData();
			SendFrame();
			external_feed_inited_ = true;
		}
	}
	else {
		// Camera video source
		rtc_agent_->AddVideoSource(current_idx, { 1280, 720, 30 });
	}
	vts_rtc::RoomId roomid = std::string(open_room_edit_->text().toLocal8Bit());
	auto code = rtc_agent_->OpenRoom(roomid, vts_rtc::RoomType::VideoBroadcasting);
	if (code != vts_rtc::RoomCode::OK) {
		QMessageBox::warning(nullptr, tr("Warning"), tr(roomcode_map[code]));
		return;
	}
}

void RtcWidget::JoinRoom() {
	CHECK_RTCAGENT_VALID

	if (rooms_combobox_->currentIndex() == -1) {
		QMessageBox::warning(nullptr, tr("Warning"), tr("No room is selected"));
		return;
	}

	vts_rtc::RoomId roomid = std::string(rooms_combobox_->currentText().toLocal8Bit());
	auto code = rtc_agent_->JoinRoom(roomid);
	if (code != vts_rtc::RoomCode::OK) {
		QMessageBox::warning(nullptr, tr("Warning"), tr(roomcode_map[code]));
	}
}

void RtcWidget::LeaveRoom() {
	CHECK_RTCAGENT_VALID

	auto code = rtc_agent_->LeaveRoom();
	if (code != vts_rtc::RoomCode::OK) {
		QMessageBox::warning(nullptr, tr("Warning"), tr(roomcode_map[code]));
	}
}

void RtcWidget::SendMessage() {
	CHECK_RTCAGENT_VALID

	if (send_msg_edit_->text().isEmpty()) {
		QMessageBox::warning(nullptr, tr("Warning"), tr("Message is empty"));
		return;
	}

	std::string msg = std::string(send_msg_edit_->text().toLocal8Bit());
	bool succeed = rtc_agent_->Broadcast(msg);
	if (!succeed) {
		QMessageBox::warning(nullptr, tr("Warning"), tr("Send message failed"));
	}
}
