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

#define CHECK_ERRORCODE if (code != RtcErrorCode::OK) {  \
		QMessageBox::warning(nullptr, tr("Warning"), tr(RtcErrorMessage(code))); \
		return; \
	}

QListWidget* RtcWidget::recv_msg_listwgt_ = nullptr;
RtcAudioRender* RtcWidget::rtc_audiorender_ = nullptr;
RtcVideoRender* RtcWidget::rtc_videorender_ = nullptr;

void RtcWidget::HandleRoom(RtcRoomOperation room_operation, RtcRoomId roomid) {
	qDebug() << "-----> HandleRoom, room operation: " << room_operation << ", roomid: " << roomid;
}

void RtcWidget::HandleP2PState(RtcSessionId sessionid, RtcP2PState state) {
	qDebug() << "-----> HandleP2PState, sessionid: " << sessionid << ", state: " << state;
}

void RtcWidget::HandleDataChannelState(RtcSessionId sessionid,
	RtcDataChannelLabel label, RtcDataChannelState state) {
	qDebug() << "-----> HandleDataChannelState, sessionid: " << sessionid <<
		", label: " << label << ", state: " << state;
}

void RtcWidget::HandleServerConnectionState(RtcServerConnectionState state) {
	qDebug() << "-----> HandleServerConnectionState, state: " << state;
}


void RtcWidget::HandleMessage(RtcSessionId remote_sessionid,
	const char* channel_label, const char* msg, size_t msg_size) {
	if (recv_msg_listwgt_) {
		QString item_text = QString("%1 [from sessionid: %2, channel label: %3]")
			.arg(QString::fromLocal8Bit(msg, msg_size))
			.arg(remote_sessionid)
			.arg(QString::fromLocal8Bit(channel_label));
		recv_msg_listwgt_->insertItem(0, item_text);
	}
}

void RtcWidget::HandleAudioFrame(RtcAudioSourceId sourceid,
	RtcMediaSourceType sourcetype,
	size_t bits_per_sample, size_t sample_rate,
	size_t number_of_channels, size_t number_of_frames,
	const void* audio_data, size_t sz_audio_data) {
	if (rtc_audiorender_) {
		rtc_audiorender_->OnAudioFrame(sourceid, sourcetype,
			bits_per_sample, sample_rate,
			number_of_channels, number_of_frames,
			audio_data, sz_audio_data);
	}
}

void RtcWidget::HandleFrame(RtcVideoSourceId sourceid,
	RtcMediaSourceType sourcetype,
	size_t width, size_t height, size_t dimension,
	const unsigned char* buffer, size_t sz_buffer) {
	if (rtc_videorender_) {
		rtc_videorender_->OnFrame(sourceid, sourcetype,
			width, height, dimension, buffer, sz_buffer);
	}
}

RtcWidget::RtcWidget(const std::string& rtc_config_filepath,
	const QString& pcmdata_filepath, const QString& yuv_folderpath,
	QWidget* parent)
	: pcmdata_filepath_(pcmdata_filepath), yuv_folderpath_(yuv_folderpath),
	QWidget(parent) {
	CreateUI();

	auto code = RtcInitAgent(rtc_config_filepath.c_str(),
		HandleRoom, HandleP2PState, HandleDataChannelState, HandleServerConnectionState,
		HandleMessage, HandleAudioFrame, HandleFrame);
	CHECK_ERRORCODE

	code = RtcAddDataChannel("datachannel", RtcPriorityType::High, true, -1);
	CHECK_ERRORCODE

	videosources_combobox_->clear();
	RtcVideoDevices video_devices = nullptr;
	size_t sz_video_devices = 0;

	code = RtcGetVideoDevices(&video_devices, &sz_video_devices);
	CHECK_ERRORCODE

	for (size_t i = 0; i < sz_video_devices; ++i) {
		videosources_combobox_->addItem(QString::fromLocal8Bit(video_devices[i].device_name));
	}
	RtcDestoryVideoDevices(video_devices, sz_video_devices);

	videosources_combobox_->addItem("Local YUV420p");
}

RtcWidget::~RtcWidget() {
	RtcDestoryAgent();

	{
		std::lock_guard<std::mutex> lg(stop_audiothread_mtx_);
		stop_audiothread_ = true;
	}

	{
		std::lock_guard<std::mutex> lg(stop_videothread_mtx_);
		stop_videothread_ = true;
	}

	for (auto& pcmdata : pcmdatas_) {
		delete[] pcmdata.buffer;
	}

	for (auto& yuvframe : yuv_frames_) {
		delete[] yuvframe.buffer;
	}
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
	QPushButton* close_room_btn = new QPushButton(tr("Close Room"));
	room_layout->addWidget(open_room_edit_, 0, 0, 1, 3);
	room_layout->addWidget(open_room_btn, 0, 3, 1, 1);
	room_layout->addWidget(close_room_btn, 0, 4, 1, 1);
	room_layout->addWidget(rooms_combobox_, 1, 0, 1, 2);
	room_layout->addWidget(query_rooms_btn, 1, 2, 1, 1);
	room_layout->addWidget(join_room_btn, 1, 3, 1, 1);
	room_layout->addWidget(leave_room_btn, 1, 4, 1, 1);
	room_groupbox->setLayout(room_layout);

	// SRS管理
	QGroupBox* SRS_groupbox = new QGroupBox(tr("SRS Management"));
	QHBoxLayout* SRS_layout = new QHBoxLayout;
	SRS_streamurl_edit_ = new QLineEdit("webrtc://47.96.251.52/AR/livestream");
	QPushButton* publish_to_SRS_btn = new QPushButton(tr("Publish to SRS"));
	QPushButton* unpublish_to_SRS_btn = new QPushButton(tr("Unpublish to SRS"));
	QPushButton* play_from_SRS_btn = new QPushButton(tr("Play from SRS"));
	QPushButton* unplay_from_SRS_btn = new QPushButton(tr("Unplay from SRS"));
	SRS_layout->addWidget(SRS_streamurl_edit_);
	SRS_layout->addWidget(publish_to_SRS_btn);
	SRS_layout->addWidget(unpublish_to_SRS_btn);
	SRS_layout->addWidget(play_from_SRS_btn);
	SRS_layout->addWidget(unplay_from_SRS_btn);
	SRS_groupbox->setLayout(SRS_layout);

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

	rtc_audiorender_ = new RtcAudioRender(this);
	rtc_videorender_ = new RtcVideoRender();

	main_layout->addWidget(videosource_groupbox);
	main_layout->addWidget(room_groupbox);
	main_layout->addWidget(SRS_groupbox);
	main_layout->addWidget(msg_groupbox);
	main_layout->addWidget(rtc_videorender_, 0, Qt::AlignCenter);

	this->setLayout(main_layout);
	this->setMinimumSize(800, 600);

	// bind events
	connect(query_rooms_btn, SIGNAL(clicked()), this, SLOT(QueryRooms()));
	connect(open_room_btn, SIGNAL(clicked()), this, SLOT(OpenRoom()));
	connect(close_room_btn, SIGNAL(clicked()), this, SLOT(CloseRoom()));
	connect(join_room_btn, SIGNAL(clicked()), this, SLOT(JoinRoom()));
	connect(leave_room_btn, SIGNAL(clicked()), this, SLOT(LeaveRoom()));
	connect(publish_to_SRS_btn, SIGNAL(clicked()), this, SLOT(PublishToSRS()));
	connect(unpublish_to_SRS_btn, SIGNAL(clicked()), this, SLOT(UnpublishToSRS()));
	connect(play_from_SRS_btn, SIGNAL(clicked()), this, SLOT(PlayFromSRS()));
	connect(unplay_from_SRS_btn, SIGNAL(clicked()), this, SLOT(UnplayFromSRS()));
	connect(send_msg_btn, SIGNAL(clicked()), this, SLOT(SendMessage()));
}

void RtcWidget::LoadPCMData() {
	if (pcmdatas_.size() > 0) {
		return;
	}

	size_t bits_per_sample = 16,
		sample_rate = 8000,
		number_of_channels = 1,
		number_of_frames = 80, // send pcmdata per 10ms, 8000 * 0.01
		sz_buffer = 160; // 16 * 1 * 80 / 8

	FILE* fp = fopen(pcmdata_filepath_.toLocal8Bit(), "rb");
	if (fp) {
		size_t num_read = 0;
		do {
			RtcPCMData pcmdata {
				bits_per_sample,
				sample_rate,
				number_of_channels,
				number_of_frames,
				new char[sz_buffer],
				sz_buffer
			};

			num_read = fread(pcmdata.buffer, 1, sz_buffer, fp);
			if (num_read == sz_buffer) {
				pcmdatas_.emplace_back(pcmdata);
			}
		} while (num_read == sz_buffer);

		fflush(fp);
		fclose(fp);
	}
}

void RtcWidget::LoadYUVData() {
	QStringList yuv_namefilters;
	yuv_namefilters << "*.yuv";
	QDir yuv_dir(yuv_folderpath_);
	auto files = yuv_dir.entryList(yuv_namefilters, QDir::Files | QDir::Readable, QDir::Name);

	for (const auto& file : files) {
		QString yuv_filepath = yuv_folderpath_ + "/" + file;
		FILE* fp = fopen(yuv_filepath.toLocal8Bit(), "rb");
		if (fp) {
			RtcYUV420pFrame frame {
				1280,
				720,
				frame.width,
				frame.width / 2,
				frame.width / 2,
				new unsigned char[frame.height * frame.width * 3 / 2],
				frame.height * frame.width * 3 / 2
			};

			fread(frame.buffer, 1, frame.height * frame.width * 3 / 2, fp);
			fflush(fp);
			fclose(fp);
			yuv_frames_.emplace_back(frame);
		}
	}
}

void RtcWidget::SendAudioFrame() {
	if (pcmdatas_.size() > 0) {
		audiothread_ = QThread::create([this]() {
			size_t idx = 0;
			bool need_stop = false;
			while (!need_stop) {
				if (idx == pcmdatas_.size()) {
					idx = 0;
				}
				RtcSendAudioFrame("external_audio", &pcmdatas_[idx++]);
				QThread::msleep(10);

				{
					std::lock_guard<std::mutex> lg(stop_audiothread_mtx_);
					need_stop = stop_audiothread_;
				}
			}
			});

		audiothread_->start();
	}
}

void RtcWidget::SendFrame() {
	if (yuv_frames_.size() > 0) {
		videothread_ = QThread::create([this]() {
			size_t idx = 0;
			bool need_stop = false;
			while (!need_stop) {
				if (idx == yuv_frames_.size()) {
					idx = 0;
				}
				RtcSendFrame("external_feed", &yuv_frames_[idx++]);
				QThread::msleep(30);

				{
					std::lock_guard<std::mutex> lg(stop_videothread_mtx_);
					need_stop = stop_videothread_;
				}
			}
			});

		videothread_->start();
	}
}

void RtcWidget::AddAudioSource() {
	auto code = RtcAddExternalAudioSource("external_audio", RtcPriorityType::High);
	CHECK_ERRORCODE
	
	LoadPCMData();
	SendAudioFrame();
}

void RtcWidget::AddVideoSource() {
	auto current_idx = videosources_combobox_->currentIndex();
	if (current_idx == videosources_combobox_->count() - 1) {
		// YUV420p video source
		auto code = RtcAddExternalVideoSource("external_feed", RtcPriorityType::High);
		//CHECK_ERRORCODE

		if (!external_feed_inited_) {
			LoadYUVData();
			SendFrame();
			external_feed_inited_ = true;
		}
	}
	else {
		// Camera video source
		RtcVideoDeviceCapability device_capability{ 1280, 720, 30 };
		auto code = RtcAddDeviceVideoSource(current_idx, &device_capability, RtcPriorityType::High);
		//CHECK_ERRORCODE

// 		code = RtcAddExternalVideoSource("external_feed", RtcPriorityType::High);
// 		CHECK_ERRORCODE
// 
// 		if (!external_feed_inited_) {
// 			LoadYUVData();
// 			SendFrame();
// 			external_feed_inited_ = true;
// 		}
	}
}

void RtcWidget::QueryRooms() {
	RtcRooms rooms = nullptr;
	size_t sz_rooms = 0;
	auto code = RtcQueryRooms(&rooms, &sz_rooms);
	CHECK_ERRORCODE

	rooms_combobox_->clear();
	for (size_t i = 0; i < sz_rooms; ++i) {
		rooms_combobox_->addItem(QString::fromLocal8Bit(rooms[i].roomid));
	}

	RtcDestoryRooms(rooms, sz_rooms);
	rooms = nullptr;
}

void RtcWidget::OpenRoom() {
	QByteArray roomid = open_room_edit_->text().toLocal8Bit();
	auto code = RtcOpenRoom(roomid.data(), RtcRoomType::VideoBroadcasting, false);
	CHECK_ERRORCODE

	if (!video_source_added_) {
		this->AddVideoSource();
		video_source_added_ = true;
	}

	if (!audio_source_added_) {
		this->AddAudioSource();
		audio_source_added_ = true;
	}
}

void RtcWidget::CloseRoom() {
	QByteArray roomid = open_room_edit_->text().toLocal8Bit();
	auto code = RtcCloseRoom(roomid.data());
	CHECK_ERRORCODE
}

void RtcWidget::JoinRoom() {
	if (rooms_combobox_->currentIndex() == -1) {
		QMessageBox::warning(nullptr, tr("Warning"), tr("No room is selected"));
		return;
	}

	QByteArray roomid = rooms_combobox_->currentText().toLocal8Bit();
	auto code = RtcJoinRoom(roomid.data());
	CHECK_ERRORCODE
}

void RtcWidget::LeaveRoom() {
	auto code = RtcLeaveRoom();
	CHECK_ERRORCODE
}

void RtcWidget::PublishToSRS() {
	if (!video_source_added_) {
		this->AddVideoSource();
		video_source_added_ = true;
	}

	if (!audio_source_added_) {
		this->AddAudioSource();
		audio_source_added_ = true;
	}

	QByteArray SRS_streamurl = SRS_streamurl_edit_->text().toLocal8Bit();
	auto code = RtcPublishToSRS(SRS_streamurl.data());
	CHECK_ERRORCODE
}

void RtcWidget::UnpublishToSRS() {
	qDebug() << "Unpublish to SRS";
}

void RtcWidget::PlayFromSRS() {
	QByteArray SRS_streamurl = SRS_streamurl_edit_->text().toLocal8Bit();
	auto code = RtcPlayFromSRS(SRS_streamurl.data());
	CHECK_ERRORCODE
}

void RtcWidget::UnplayFromSRS() {
	qDebug() << "Unplay from SRS";
}

void RtcWidget::SendMessage() {
	if (send_msg_edit_->text().isEmpty()) {
		QMessageBox::warning(nullptr, tr("Warning"), tr("Message is empty"));
		return;
	}

	QByteArray msg = send_msg_edit_->text().toLocal8Bit();
	auto code = RtcBroadcastData("datachannel", msg.data(), msg.size());
	CHECK_ERRORCODE
}
