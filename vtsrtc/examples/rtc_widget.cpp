#include "rtc_widget.h"

#include <QAction>
#include <QBoxLayout>
#include <QDebug>
#include <QDir>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <fstream>
#if defined __aarch64__
#include <iostream>
#include <thread>
#endif

#define CHECK_ERRORCODE                                                      \
  if (code != RtcErrorCode::OK) {                                            \
    QMessageBox::warning(nullptr, tr("Warning"), tr(RtcErrorMessage(code))); \
    return;                                                                  \
  }

QListWidget* RtcWidget::recv_msg_listwgt_ = nullptr;
QStandardItemModel* RtcWidget::model_ = nullptr;
QTableView* RtcWidget::tableView_ = nullptr;
QStringList* RtcWidget::sourceid_list_ = nullptr;

RtcAudioRender* RtcWidget::rtc_audiorender_ = nullptr;
std::map<std::string, RtcVideoRender*> RtcWidget::source_render_;
QGridLayout* RtcWidget::render_layout_ = nullptr;
RtcWidget* RtcWidget::instance_ = nullptr;

void RtcWidget::HandleRoom(RtcRoomOperation room_operation, RtcRoomId roomid) {
  qDebug() << "-----> HandleRoom, room operation: " << room_operation
           << ", roomid: " << roomid;
}

void RtcWidget::HandleP2PState(RtcSessionId sessionid, RtcP2PState state) {
  qDebug() << "-----> HandleP2PState, sessionid: " << sessionid
           << ", state: " << state;
  if ((state == RtcP2PState::P2PDisconnected ||
       state == RtcP2PState::P2PClosed) &&
      instance_) {
    QMetaObject::invokeMethod(
        instance_, []() { instance_->ResetRender(); }, Qt::QueuedConnection);
  }
}

void RtcWidget::HandleDataChannelState(RtcSessionId sessionid,
                                       RtcDataChannelLabel label,
                                       RtcDataChannelState state) {
  qDebug() << "-----> HandleDataChannelState, sessionid: " << sessionid
           << ", label: " << label << ", state: " << state;
}

void RtcWidget::HandleServerConnectionState(RtcServerConnectionState state) {
  qDebug() << "-----> HandleServerConnectionState, state: " << state;
}

void RtcWidget::HandleSRSState(RtcSRSStreamurl streamurl, RtcP2PState state) {
  qDebug() << "-----> HandleSRSState, streamurl: " << streamurl
           << ", state: " << state;
}

void RtcWidget::HandleSRSResponse(RtcSRSStreamurl streamurl,
                                  RtcSRSResponse response) {
  qDebug() << "-----> HandleSRSResponse, streamurl: " << streamurl
           << ", state: " << response;
}

void RtcWidget::HandleChannelNetStats(RtcSessionId /*remote_sessionid*/,
                                      RtcNetStats params) {
  // 回调在 logic-thread 线程里触发，不能直接操作 Qt GUI 对象；
  // 同时 RtcNetStats 里的字符串是 const char*，生命周期只在回调期间有效，
  // 必须在这里先拷贝成我们自己的 QString/std::string 再投递到 GUI 线程。
  if (!tableView_ || !model_ || !sourceid_list_) {
    return;
  }

  // 立刻拷贝所有可能用到的字符串和数值，避免 use-after-free
  QString audio_source =
      params.audio_stats.sourceid
          ? QString::fromLocal8Bit(params.audio_stats.sourceid)
          : QString();
  QString video_source =
      params.video_stats.sourceid
          ? QString::fromLocal8Bit(params.video_stats.sourceid)
          : QString();
  QString codec_name =
      params.video_stats.codec_name
          ? QString::fromLocal8Bit(params.video_stats.codec_name)
          : QString();

  const bool input = params.input;
  const unsigned long bitrate_bps = params.video_stats.bitrate_bps;
  const unsigned int width = params.video_stats.width;
  const unsigned int height = params.video_stats.height;
  const unsigned int fps = params.video_stats.fps;
  const unsigned int loss_rate = params.video_stats.loss_rate;
  const unsigned int delay_ms = params.video_stats.delay_ms;
  const unsigned int key_frame_count = params.video_stats.key_frame_count;
  const unsigned int fir_count = params.video_stats.fir_count;
  const unsigned int pli_count = params.video_stats.pli_count;
  const unsigned int nack_count = params.video_stats.nack_count;

  QMetaObject::invokeMethod(
      tableView_,
      [audio_source, video_source, codec_name, input, bitrate_bps, width,
       height, fps, loss_rate, delay_ms, key_frame_count, fir_count, pli_count,
       nack_count]() {
        if (!RtcWidget::tableView_ || !RtcWidget::model_ ||
            !RtcWidget::sourceid_list_) {
          return;
        }

        // 维护 sourceid 列表
        if (!audio_source.isEmpty() &&
            !RtcWidget::sourceid_list_->contains(audio_source)) {
          RtcWidget::sourceid_list_->append(audio_source);
        }
        if (!video_source.isEmpty() &&
            !RtcWidget::sourceid_list_->contains(video_source)) {
          RtcWidget::sourceid_list_->append(video_source);
        }

        int row = RtcWidget::sourceid_list_->indexOf(video_source);
        if (row < 0) {
          return;
        }

        // 更新表格（目前 input / !input 两条路径逻辑一致）
        RtcWidget::model_->setItem(row, 0, new QStandardItem(video_source));
        RtcWidget::model_->setItem(
            row, 1, new QStandardItem(QString::number(bitrate_bps)));
        RtcWidget::model_->setItem(row, 2,
                                   new QStandardItem(QString::number(width)));
        RtcWidget::model_->setItem(row, 3,
                                   new QStandardItem(QString::number(height)));
        RtcWidget::model_->setItem(row, 4,
                                   new QStandardItem(QString::number(fps)));
        RtcWidget::model_->setItem(
            row, 5, new QStandardItem(QString::number(loss_rate)));
        RtcWidget::model_->setItem(
            row, 6, new QStandardItem(QString::number(delay_ms)));
        RtcWidget::model_->setItem(
            row, 7, new QStandardItem(QString::number(key_frame_count)));
        RtcWidget::model_->setItem(
            row, 8, new QStandardItem(QString::number(fir_count)));
        RtcWidget::model_->setItem(
            row, 9, new QStandardItem(QString::number(pli_count)));
        RtcWidget::model_->setItem(
            row, 10, new QStandardItem(QString::number(nack_count)));
        RtcWidget::model_->setItem(row, 11, new QStandardItem(codec_name));

        RtcWidget::tableView_->viewport()->update();
      },
      Qt::QueuedConnection);
}

void RtcWidget::HandleMessage(RtcSessionId remote_sessionid,
                              const char* channel_label, const char* msg,
                              size_t msg_size) {
  if (recv_msg_listwgt_) {
    QString item_text =
        QString("Receive size: %1 byte [from sessionid: %2, channel label: %3]")
            .arg(msg_size)
            .arg(remote_sessionid)
            .arg(QString::fromLocal8Bit(channel_label));
    recv_msg_listwgt_->insertItem(0, item_text);
  }
}

void RtcWidget::HandleAudioFrame(RtcSessionId remote_sessionid,
                                 RtcAudioSourceId sourceid,
                                 RtcMediaSourceType sourcetype,
                                 size_t bits_per_sample, size_t sample_rate,
                                 size_t number_of_channels,
                                 size_t number_of_frames,
                                 const void* audio_data, size_t sz_audio_data) {
  if (rtc_audiorender_) {
    rtc_audiorender_->OnAudioFrame(sourceid, sourcetype, bits_per_sample,
                                   sample_rate, number_of_channels,
                                   number_of_frames, audio_data, sz_audio_data);
  }
}

void RtcWidget::HandleFrame(RtcSessionId /*remote_sessionid*/,
                            RtcVideoSourceId sourceid,
                            RtcMediaSourceType sourcetype, size_t width,
                            size_t height, size_t dimension,
                            const unsigned char* buffer, size_t sz_buffer) {
  // 回调在线程 logic-thread 中，不能直接操作 Qt 控件
  if (!render_layout_ || !buffer || sz_buffer == 0) {
    return;
  }

  // 先在当前线程里拷贝一份数据，给 GUI 线程安全使用
  const std::string sid = sourceid ? std::string(sourceid) : std::string();
  QByteArray frameCopy(reinterpret_cast<const char*>(buffer),
                       static_cast<int>(sz_buffer));

  QMetaObject::invokeMethod(
      render_layout_,  // 目标对象，保证在 GUI 线程执行
      [sid, sourcetype, width, height, dimension, frameCopy]() {
        if (!RtcWidget::render_layout_) {
          return;
        }

        auto it = RtcWidget::source_render_.find(sid);
        RtcVideoRender* renderer = nullptr;

        if (it == RtcWidget::source_render_.end()) {
          // 为新的视频源创建一个渲染窗口，并加入布局
          renderer = new RtcVideoRender(640, 360);
          RtcWidget::source_render_[sid] = renderer;

          // 简单按两列网格布局排列所有源
          int index = static_cast<int>(RtcWidget::source_render_.size()) - 1;
          const int cols = 2;
          int row = index / cols;
          int col = index % cols;
          RtcWidget::render_layout_->addWidget(renderer, row, col);
        } else {
          renderer = it->second;
        }

        if (renderer && !frameCopy.isEmpty()) {
          renderer->OnFrame(
              sid.c_str(), static_cast<unsigned int>(sourcetype), width, height,
              dimension,
              reinterpret_cast<const unsigned char*>(frameCopy.constData()),
              static_cast<size_t>(frameCopy.size()));
        }
      },
      Qt::QueuedConnection);
}

RtcWidget::RtcWidget(const std::string& rtc_config_filepath,
                     const QString& pcmdata_filepath,
                     const QString& yuv_folderpath, QWidget* parent)
    : pcmdata_filepath_(pcmdata_filepath),
      yuv_folderpath_(yuv_folderpath),
      QWidget(parent) {
  instance_ = this;
  CreateUI();

  // auto code = RtcInitAgent(rtc_config_filepath.c_str(),
  // 	HandleRoom, HandleP2PState, HandleSRSState, HandleDataChannelState,
  // HandleServerConnectionState, 	HandleMessage, HandleAudioFrame,
  // HandleFrame);

  RtcInitParams st_params;
  st_params.config_filepath = rtc_config_filepath.c_str();
  st_params.room_handler = HandleRoom;
  st_params.P2P_state_handler = HandleP2PState;
  st_params.datachannel_state_handler = HandleDataChannelState;
  st_params.serverconnection_state_handler = HandleServerConnectionState;
  st_params.SRS_state_handler = HandleSRSState;
  st_params.SRS_response_handler = HandleSRSResponse;
  st_params.recv_msg_handler = HandleMessage;
  st_params.recv_audioframe_handler = HandleAudioFrame;
  st_params.recv_frame_handler = HandleFrame;
  st_params.channel_network_stats_handler = HandleChannelNetStats;
  auto code = RtcInitAgentV2(st_params);

  CHECK_ERRORCODE

  code = RtcAddDataChannel("datachannel", RtcPriorityType::High, true, -1);
  CHECK_ERRORCODE

  videosources_combobox_->clear();
  RtcVideoDevices video_devices = nullptr;
  size_t sz_video_devices = 0;

  code = RtcGetVideoDevices(&video_devices, &sz_video_devices);
  CHECK_ERRORCODE

  videosources_combobox_->addItem("Local YUV420p");

  for (size_t i = 0; i < sz_video_devices; ++i) {
    videosources_combobox_->addItem(
        QString::fromLocal8Bit(video_devices[i].device_name));
  }
  RtcDestoryVideoDevices(video_devices, sz_video_devices);
}

RtcWidget::~RtcWidget() {
  // 确保销毁前清理渲染相关资源
  ResetRender();
  instance_ = nullptr;

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
    delete[] static_cast<char*>(pcmdata.buffer);
  }

  for (auto& yuvframe : yuv_frames_) {
    delete[] yuvframe.buffer;
  }
}

void RtcWidget::ResetRender() {
  // 清理所有视频渲染窗口和布局中的控件
  if (!render_layout_) {
    return;
  }

  // 先删除 source_render_ 里记录的所有 RtcVideoRender
  for (auto& it : source_render_) {
    if (it.second) {
      render_layout_->removeWidget(it.second);
      it.second->deleteLater();
    }
  }
  source_render_.clear();

  // 再把 layout 里可能残留的其它 widget/item 也清掉
  QLayoutItem* item;
  while ((item = render_layout_->takeAt(0)) != nullptr) {
    if (QWidget* w = item->widget()) {
      w->deleteLater();
    }
    delete item;
  }
}

void RtcWidget::CreateUI() {
  // create UI
  QBoxLayout* main_layout = new QVBoxLayout;

  // 视频源管理
  QGroupBox* videosource_groupbox =
      new QGroupBox(tr("Video Source Management"));
  QHBoxLayout* videosource_layout = new QHBoxLayout;
  QLabel* videosource_label = new QLabel(tr("Video Sources"));
  videosources_combobox_ = new QComboBox();
  videosource_layout->addWidget(videosource_label);
  videosource_layout->addWidget(videosources_combobox_);
  videosource_groupbox->setLayout(videosource_layout);
  videosource_groupbox->setFixedSize(660, 65);

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
  room_groupbox->setFixedSize(660, 100);

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
  SRS_groupbox->setFixedSize(660, 65);

  // 消息管理
  QGroupBox* msg_groupbox = new QGroupBox(tr("Message Management"));
  QGridLayout* msg_layout = new QGridLayout;
  QLabel* recv_msg_albel = new QLabel(tr("received message"));
  recv_msg_listwgt_ = new QListWidget;
  send_msg_edit_ = new QLineEdit("hello world");
  QPushButton* send_msg_btn = new QPushButton(tr("Send Message"));
  QPushButton* send_filemsg_btn = new QPushButton(tr("Send Message from file"));
  file_combobox_ = new QComboBox();
  file_combobox_->addItem("messagefile.txt");
  msg_layout->addWidget(recv_msg_albel, 0, 0, 1, 1);
  msg_layout->addWidget(recv_msg_listwgt_, 0, 1, 1, 3);
  msg_layout->addWidget(send_msg_edit_, 1, 0, 1, 3);
  msg_layout->addWidget(send_msg_btn, 1, 3, 1, 1);
  msg_layout->addWidget(file_combobox_, 2, 0, 1, 3);
  msg_layout->addWidget(send_filemsg_btn, 2, 3, 1, 1);
  msg_groupbox->setLayout(msg_layout);
  msg_groupbox->setFixedSize(660, 140);

  // 媒体统计
  tableView_ = new QTableView;
  sourceid_list_ = new QStringList({""});
  model_ = new QStandardItemModel();
  model_->setHorizontalHeaderLabels({"Source", "Bitrate", "Width", "Height",
                                     "Fps", "LossRate", "Delay", "KeyFrame",
                                     "Fir", "Pli", "Nack", "Codec"});
  model_->setVerticalHeaderLabels(*sourceid_list_);
  tableView_->setModel(model_);
  tableView_->verticalHeader()->setVisible(false);
  tableView_->setFixedSize(660, 120);

  // 渲染
  rtc_audiorender_ = new RtcAudioRender(this);
  QGroupBox* Render_groupbox = new QGroupBox(tr("Render"));
  render_layout_ = new QGridLayout();
  Render_groupbox->setLayout(render_layout_);
  Render_groupbox->setFixedSize(660, 390);

  main_layout->addWidget(videosource_groupbox);
  main_layout->addWidget(room_groupbox);
  main_layout->addWidget(SRS_groupbox);
  main_layout->addWidget(msg_groupbox);
  main_layout->addWidget(Render_groupbox);
  main_layout->addWidget(tableView_);

  this->setLayout(main_layout);
  this->setFixedSize(680, 950);

  // bind events
  connect(query_rooms_btn, SIGNAL(clicked()), this, SLOT(QueryRooms()));
  connect(open_room_btn, SIGNAL(clicked()), this, SLOT(OpenRoom()));
  connect(close_room_btn, SIGNAL(clicked()), this, SLOT(CloseRoom()));
  connect(join_room_btn, SIGNAL(clicked()), this, SLOT(JoinRoom()));
  connect(leave_room_btn, SIGNAL(clicked()), this, SLOT(LeaveRoom()));
  connect(publish_to_SRS_btn, SIGNAL(clicked()), this, SLOT(PublishToSRS()));
  connect(unpublish_to_SRS_btn, SIGNAL(clicked()), this,
          SLOT(UnpublishToSRS()));
  connect(play_from_SRS_btn, SIGNAL(clicked()), this, SLOT(PlayFromSRS()));
  connect(unplay_from_SRS_btn, SIGNAL(clicked()), this, SLOT(UnplayFromSRS()));
  connect(send_msg_btn, SIGNAL(clicked()), this, SLOT(SendMessage()));
  connect(send_filemsg_btn, SIGNAL(clicked()), this,
          SLOT(SendMessageFromFile()));
}

void RtcWidget::LoadPCMData() {
  if (pcmdatas_.size() > 0) {
    return;
  }

  size_t bits_per_sample = 16, sample_rate = 8000, number_of_channels = 1,
         number_of_frames = 80,  // send pcmdata per 10ms, 8000 * 0.01
      sz_buffer = 160;           // 16 * 1 * 80 / 8

  FILE* fp = fopen(pcmdata_filepath_.toLocal8Bit(), "rb");
  if (fp) {
    size_t num_read = 0;
    do {
      RtcPCMData pcmdata{bits_per_sample,     sample_rate,
                         number_of_channels,  number_of_frames,
                         new char[sz_buffer], sz_buffer};

      num_read = fread(pcmdata.buffer, 1, sz_buffer, fp);
      if (num_read == sz_buffer) {
        pcmdatas_.emplace_back(pcmdata);
      }
    } while (num_read == sz_buffer);

    fflush(fp);
    fclose(fp);
  }
}

/* void RtcWidget::LoadYUVData() {
        QStringList yuv_namefilters;
        yuv_namefilters << "*.yuv";
        QDir yuv_dir(yuv_folderpath_);
        auto files = yuv_dir.entryList(yuv_namefilters, QDir::Files |
QDir::Readable, QDir::Name);

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
                                new unsigned char[frame.height * frame.width * 3
/ 2], frame.height * frame.width * 3 / 2
                        };

                        fread(frame.buffer, 1, frame.height * frame.width * 3 /
2, fp); fflush(fp); fclose(fp); yuv_frames_.emplace_back(frame);
                }
        }
} */

//从UYVY中获取Y，并存到一个数组
void UYVYToYRow(const char* src_uyvy, char* dst_y, int width) {
  // Output a row of Y values.
  for (int x = 0; x < width - 1; x += 2) {
    dst_y[x] = src_uyvy[1];
    dst_y[x + 1] = src_uyvy[3];
    src_uyvy += 4;
  }
}
//从UYVY中获取UV，并分别存到2个数组
void UYVYToUVRow(const char* src_uyvy, int src_stride_uyvy, char* dst_u,
                 char* dst_v, int width) {
  // Output a row of UV values.
  for (int x = 0; x < width - 1; x += 2) {
    dst_u[0] = src_uyvy[0];
    dst_v[0] = src_uyvy[2];
    src_uyvy += 4;
    dst_u += 1;
    dst_v += 1;
  }
}

int UYVYToI420(const char* src_uyvy, int src_stride_uyvy, char* dst_y,
               int dst_stride_y, char* dst_u, int dst_stride_u, char* dst_v,
               int dst_stride_v, int width, int height) {
  for (int y = 0; y < height - 1; y += 2) {
    UYVYToUVRow(src_uyvy, src_stride_uyvy, dst_u, dst_v, width);
    UYVYToYRow(src_uyvy, dst_y, width);
    UYVYToYRow(src_uyvy + src_stride_uyvy, dst_y + dst_stride_y, width);
    src_uyvy += src_stride_uyvy * 2;
    dst_y += dst_stride_y * 2;
    dst_u += dst_stride_u;
    dst_v += dst_stride_v;
  }

  return 0;
}

void RtcWidget::LoadYUVData() {
  FILE* fp = fopen("zjlabs.yuv", "rb");
  while (!feof(fp)) {
    RtcYUV420pFrame frame{1280,
                          720,
                          frame.width,
                          frame.width / 2,
                          frame.width / 2,
                          new unsigned char[frame.height * frame.width * 3 / 2],
                          frame.height* frame.width * 3 / 2};
    fread(frame.buffer, 1, frame.height * frame.width * 3 / 2, fp);
    yuv_frames_.emplace_back(frame);
  }
  fflush(fp);
  fclose(fp);
}

void RtcWidget::SendAudioFrame() {
  if (pcmdatas_.size() > 0) {
#if defined __aarch64__
    std::thread audioThread([this]() {
#else
    audiothread_ = QThread::create([this]() {
#endif
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
#if defined __aarch64__
    audioThread.join();
#else
    audiothread_->start();
#endif
  }
}

void RtcWidget::SendFrame() {
  if (yuv_frames_.size() > 0) {
#if defined __aarch64__
    std::thread videoThread([this]() {
#else
    videothread_ = QThread::create([this]() {
#endif
      size_t idx = 0;
      bool need_stop = false;
      while (!need_stop) {
        if (idx == yuv_frames_.size()) {
          idx = 0;
        }

        RtcSendFrame("merged_image", &yuv_frames_[idx]);
        idx++;
        QThread::msleep(30);
        {
          std::lock_guard<std::mutex> lg(stop_videothread_mtx_);
          need_stop = stop_videothread_;
        }
      }
    });
#if defined __aarch64__
    videoThread.detach();
#else
    videothread_->start();
#endif
  }
}

void RtcWidget::AddAudioSource() {
  auto code =
      RtcAddExternalAudioSource("external_audio", RtcPriorityType::High);
  CHECK_ERRORCODE
#if !defined __aarch64__
  LoadPCMData();
  SendAudioFrame();
#endif
}

void RtcWidget::AddVideoSource() {
  auto current_idx = videosources_combobox_->currentIndex();
  if (current_idx == 0) {
    // YUV420p video source
    auto code =
        RtcAddExternalVideoSource("merged_image", RtcPriorityType::High);
    CHECK_ERRORCODE

    if (!external_feed_inited_) {
      LoadYUVData();
      SendFrame();
      external_feed_inited_ = true;
    }
  } else {
    // Camera video source
    // current_idx is combobox index, device_index = current_idx - 1 (because
    // index 0 is "Local YUV420p")
    size_t device_index = current_idx - 1;
    RtcVideoDeviceCapability device_capability{1280, 720, 30};
    auto code = RtcAddDeviceVideoSource(device_index, &device_capability,
                                        RtcPriorityType::High);
    // CHECK_ERRORCODE

    // 		code = RtcAddExternalVideoSource("external_feed",
    // RtcPriorityType::High); 		CHECK_ERRORCODE
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

  // 	if (!video_source_added_) {
  // 		this->AddVideoSource();
  // 		video_source_added_ = true;
  // 	}
  //
  // 	if (!audio_source_added_) {
  // 		this->AddAudioSource();
  // 		audio_source_added_ = true;
  // 	}
}

void RtcWidget::CloseRoom() {
  QByteArray roomid = open_room_edit_->text().toLocal8Bit();
  QMessageBox messageBox(
      QMessageBox::Warning, "Warining",
      "Are you sure you want to delete the room " + roomid + "?",
      QMessageBox::Yes | QMessageBox::No, NULL);
  ;
  int result = messageBox.exec();

  if (result == QMessageBox::Yes) {
    auto code = RtcCloseRoom(roomid.data());
    CHECK_ERRORCODE;
  }
}

void RtcWidget::JoinRoom() {
  if (rooms_combobox_->currentIndex() == -1) {
    QMessageBox::warning(nullptr, tr("Warning"), tr("No room is selected"));
    return;
  }
  if (!video_source_added_) {
    this->AddVideoSource();
    video_source_added_ = true;
  }

  if (!audio_source_added_) {
    this->AddAudioSource();
    audio_source_added_ = true;
  }
  QByteArray roomid = rooms_combobox_->currentText().toLocal8Bit();
  auto code = RtcJoinRoom(roomid.data());
  CHECK_ERRORCODE
}

void RtcWidget::LeaveRoom() {
  auto code = RtcLeaveRoom();
  CHECK_ERRORCODE

  // 离开房间后重置视频渲染窗口
  ResetRender();

  // 重置视频源相关状态和下拉选项
  video_source_added_ = false;
  external_feed_inited_ = false;
  send_frame_flag = true;

  if (videosources_combobox_) {
    // 恢复到默认的第一个视频源（通常是 "Local YUV420p"）
    videosources_combobox_->setCurrentIndex(0);
  }
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
  QByteArray SRS_streamurl = SRS_streamurl_edit_->text().toLocal8Bit();
  auto code = RtcUnpublishToSRS(SRS_streamurl.data());
  CHECK_ERRORCODE
}

void RtcWidget::PlayFromSRS() {
  QByteArray SRS_streamurl = SRS_streamurl_edit_->text().toLocal8Bit();
  auto code = RtcPlayFromSRS(SRS_streamurl.data());
  CHECK_ERRORCODE
}

void RtcWidget::UnplayFromSRS() { qDebug() << "Unplay from SRS"; }

void RtcWidget::SendMessage() {
  if (send_msg_edit_->text().isEmpty()) {
    QMessageBox::warning(nullptr, tr("Warning"), tr("Message is empty"));
    return;
  }

  QByteArray msg = send_msg_edit_->text().toLocal8Bit();
  auto code = RtcBroadcastData("datachannel", msg.data(), msg.size());
  CHECK_ERRORCODE
}

void RtcWidget::SendMessageFromFile() {
  std::ifstream fin;
  fin.open("messagefile.txt", std::ios::in | std::ios::binary);
  fin.seekg(0, std::ios::end);
  unsigned int file_size = fin.tellg();
  fin.seekg(0, std::ios::beg);
  char* file_msg = (char*)malloc(file_size);
  fin.read(file_msg, file_size);
  fin.close();
  auto code = RtcBroadcastData("datachannel", file_msg, file_size);
  CHECK_ERRORCODE
}
