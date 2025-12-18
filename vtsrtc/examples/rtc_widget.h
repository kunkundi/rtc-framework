#pragma once

#include <QComboBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QWidget>
#include <mutex>

#include "c_rtc.h"
#include "rtc_audiorender.h"
#include "rtc_videorender.h"


class RtcWidget : public QWidget {
  Q_OBJECT

 public:
  explicit RtcWidget(const std::string& rtc_config_filepath,
                     const QString& pcmdata_filepath,
                     const QString& yuv_folderpath, QWidget* parent = 0);
  ~RtcWidget();

 private:
  static void HandleRoom(RtcRoomOperation room_operation, RtcRoomId roomid);
  static void HandleP2PState(RtcSessionId sessionid, RtcP2PState state);
  static void HandleDataChannelState(RtcSessionId sessionid,
                                     RtcDataChannelLabel label,
                                     RtcDataChannelState state);
  static void HandleServerConnectionState(RtcServerConnectionState state);
  static void HandleSRSState(RtcSRSStreamurl streamurl, RtcP2PState state);
  static void HandleSRSResponse(RtcSRSStreamurl streamurl,
                                RtcSRSResponse response);
  static void HandleChannelNetStats(RtcSessionId remote_sessionid,
                                    RtcNetStats params);

  static void HandleMessage(RtcSessionId remote_sessionid,
                            const char* channel_label, const char* msg,
                            size_t msg_size);
  static void HandleAudioFrame(RtcSessionId remote_sessionid,
                               RtcAudioSourceId sourceid,
                               RtcMediaSourceType sourcetype,
                               size_t bits_per_sample, size_t sample_rate,
                               size_t number_of_channels,
                               size_t number_of_frames, const void* audio_data,
                               size_t sz_audio_data);
  static void HandleFrame(RtcSessionId remote_sessionid,
                          RtcVideoSourceId sourceid,
                          RtcMediaSourceType sourcetype, size_t width,
                          size_t height, size_t dimension,
                          const unsigned char* buffer, size_t sz_buffer);
  void CreateUI();
  void LoadPCMData();
  void LoadYUVData();
  void SendAudioFrame();
  void SendFrame();
  void AddAudioSource();
  void AddVideoSource();
  void ResetRender();  // 断开连接后重置视频渲染

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
  void SendMessageFromFile();

 private:
  bool audio_source_added_ = false, external_feed_inited_ = false,
       video_source_added_ = false, send_frame_flag = true;
  QString pcmdata_filepath_, yuv_folderpath_;
  std::vector<RtcPCMData> pcmdatas_;
  std::vector<RtcYUV420pFrame> yuv_frames_;
  std::mutex stop_audiothread_mtx_, stop_videothread_mtx_;
  bool stop_audiothread_ = false, stop_videothread_ = false;
  QThread *audiothread_, *videothread_;
  QComboBox* videosources_combobox_;
  QLineEdit* open_room_edit_;
  QComboBox* rooms_combobox_;
  QComboBox* file_combobox_;
  QLineEdit* SRS_streamurl_edit_;
  static QListWidget* recv_msg_listwgt_;
  QLineEdit* send_msg_edit_;
  static QStandardItemModel* model_;
  static QTableView* tableView_;
  static QStringList* sourceid_list_;
  static std::map<std::string, RtcVideoRender*> source_render_;
  static QGridLayout* render_layout_;
  static RtcAudioRender* rtc_audiorender_;
  static RtcWidget* instance_;
};
