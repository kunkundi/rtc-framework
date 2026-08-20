#include "http_status_code.hpp"
#include "rtc_connection_manager.h"
#include "video/decode/rtc_decoder_factory.h"
#include "video/encode/rtc_encoder_factory.h"

#if defined __aarch64__
#ifdef USE_DEFAULT_JETSON_ENCODER
#include <modules/video_coding/codecs/nvidia/NvVideoEncoderFactory.h>
#endif
#endif

#include <limits>

vts_rtc::ErrorCode ConvertHttpCode(HttpStatus::Code http_code) {
  switch (http_code) {
    case HttpStatus::OK:
      return vts_rtc::ErrorCode::OK;
      break;
    case HttpStatus::RoomNotExisted:
      return vts_rtc::ErrorCode::RoomNotExisted;
      break;
    case HttpStatus::RoomAlreadyExisted:
      return vts_rtc::ErrorCode::RoomAlreadyExisted;
      break;
    case HttpStatus::SessionidAlreadyInRoom:
      return vts_rtc::ErrorCode::AgentAlreadyInRoom;
      break;
    default:
      return vts_rtc::ErrorCode::InternalError;
      break;
  }
}

RtcConnectionManager::RtcConnectionManager(
    const vts_rtc::RtcConfig& rtc_config,
    std::shared_ptr<RtcDeviceManager> device_manager,
    const vts_rtc::RoomHandler& room_handler,
    const vts_rtc::UserHandler& user_handler,
    const vts_rtc::P2PStateHandler& P2P_state_handler,
    const vts_rtc::DataChannelStateHandler& datachannel_state_handler,
    const vts_rtc::ServerConnectionStateHandler& serverconnection_state_handler,
    const vts_rtc::SRSStateHandler& SRS_state_handler,
    const vts_rtc::SRSResponseHandler& SRS_response_handler,
    const vts_rtc::RecvMessageHandler& recv_msg_handler,
    const vts_rtc::RecvAudioFrameHandler& recv_audioframe_handler,
    const vts_rtc::RecvFrameHandler& recv_frame_handler,
    const vts_rtc::ChannelNetworkStatsHandler& channel_network_stats_handler)
    : logic_thread_(rtc::Thread::Current()),
      rtc_config_(rtc_config),
      rtc_device_manager_(device_manager),
      room_handler_(room_handler),
      user_handler_(user_handler),
      P2P_state_handler_(P2P_state_handler),
      datachannel_state_handler_(datachannel_state_handler),
      serverconnection_state_handler_(serverconnection_state_handler),
      SRS_state_handler_(SRS_state_handler),
      SRS_publish_state_handler_(SRS_response_handler),
      recv_msg_handler_(recv_msg_handler),
      recv_audioframe_handler_(recv_audioframe_handler),
      recv_frame_handler_(recv_frame_handler),
      channel_network_stats_handler_(channel_network_stats_handler) {
  http_client_ = std::make_shared<HttpClient>(rtc_config_.api_server_url);
  if (!rtc_config_.SRS_api_server_url.empty()) {
    SRS_http_client_ =
        std::make_unique<HttpClient>(rtc_config_.SRS_api_server_url);
  }
  // 通过优化语句顺序，可以做到不加锁
  ws_io_context_ = std::make_shared<SimpleWeb::io_context>();
  stats_report_io_context_ = std::make_shared<SimpleWeb::io_context>();

  statistics_collector_ = std::make_shared<RtcStatistics>();
  statistics_collector_->SetStatisticsReportCallback(
      channel_network_stats_handler);
}

RtcConnectionManager::~RtcConnectionManager() {
  RTC_DCHECK_RUN_ON(logic_thread_);

  // reset callbacks
  room_handler_ = nullptr;
  user_handler_ = nullptr;
  serverconnection_state_handler_ = nullptr;

  this->LeaveRoom();

  if (worker_thread_) {
    worker_thread_->Invoke<void>(RTC_FROM_HERE, [this]() {
      adm_taskqueue_ = nullptr;
      audio_device_moudle_ = nullptr;
    });
  }

  // @attention: io_context stop method is thread-safe
  // @attention: io_context must be destoryed in other thread
  // 不能在ws_client_thread_线程中停止，因为ws_io_context_->run()已经阻塞住ws_client_thread_线程
  if (ws_io_context_) {
    ws_io_context_->stop();
  }

  if (stats_report_io_context_) {
    stats_report_io_context_->stop();
  }

  if (ws_client_thread_) {
    ws_client_thread_->Invoke<void>(RTC_FROM_HERE, [this]() {
      ping_timer_ = nullptr;
      pong_timer_ = nullptr;
      ws_client_ = nullptr;
    });
  }

  // TO DO
  // @attention: must be called here, otherwise crash (why!!!)
  rtpsender_priority_map_.clear();
}

bool RtcConnectionManager::Init() {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (logic_thread_ && InitPeerConnectionFactory()) {
    ws_client_thread_ = rtc::Thread::Create();
    ws_client_thread_->SetName("websocket-client", nullptr);
    ws_client_thread_->Start();

    ws_client_thread_->PostTask(RTC_FROM_HERE, [this]() { InitWebsocket(); });

    stats_report_thread_ = rtc::Thread::Create();
    stats_report_thread_->SetName("StatsReportThread", nullptr);
    stats_report_thread_->Start();

    // notify connecting to signaling server
    if (serverconnection_state_handler_) {
      serverconnection_state_handler_(
          vts_rtc::ServerConnectionState::Connecting);
    }

    return true;
  }

  return false;
}

bool RtcConnectionManager::InitPeerConnectionFactory() {
  RTC_DCHECK_RUN_ON(logic_thread_);

  network_thread_ = rtc::Thread::CreateWithSocketServer();
  network_thread_->SetName("network", nullptr);
  network_thread_->Start();

  worker_thread_ = rtc::Thread::Create();
  worker_thread_->SetName("worker", nullptr);
  worker_thread_->Start();

  signaling_thread_ = rtc::Thread::Create();
  signaling_thread_->SetName("signaling", nullptr);
  signaling_thread_->Start();

  // 关闭WebRTC debug日志
  rtc::LogMessage::LogToDebug(rtc::LS_NONE);
  rtc::LogMessage::SetLogToStderr(false);

#if 0
	webrtc_log_hook_ = new FileLog("logs");
	rtc::LogMessage::AddLogToStream(webrtc_log_hook_, rtc::LS_INFO);
#endif

  // To be improved
  // create dummy AudioDeviceModule for fixing initialization crash of VTS
  // apollo docker
  // @attention: invoke method will block the current thread until execution is
  // complete
  audio_device_moudle_ =
      worker_thread_->Invoke<rtc::scoped_refptr<webrtc::AudioDeviceModule>>(
          RTC_FROM_HERE, [this]() {
            adm_taskqueue_ = webrtc::CreateDefaultTaskQueueFactory();
            return webrtc::AudioDeviceModule::Create(
                webrtc::AudioDeviceModule::AudioLayer::kDummyAudio,
                adm_taskqueue_.get());
          });

  std::unique_ptr<webrtc::VideoEncoderFactory> video_encoder_factory = nullptr;
  std::unique_ptr<webrtc::VideoDecoderFactory> video_decoder_factory = nullptr;
  if (rtc_config_.use_NVENC) {
#ifdef USE_DEFAULT_JETSON_ENCODER
    LOG_INFO("Use jetson default H264 video encoder.");
    video_encoder_factory = webrtc::CreateNvVideoEncoderFactory();
#else
#if defined __aarch64__
    LOG_INFO("Use hardware H264 video encoder (%s).",
             rtc_config_.jetson_h264_encoder.c_str());
#elif defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
    LOG_INFO("Use hardware H264 video encoder (nvidia-nvenc).");
#else
    LOG_WARN("use_NVENC=true but this build has no CUDA/NVENC support, fallback to builtin video encoder.");
    video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
#endif
#if defined __aarch64__ || (defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER)
    video_encoder_factory = std::make_unique<webrtc::RtcEncoderFactory>();
    dynamic_cast<webrtc::RtcEncoderFactory*>(video_encoder_factory.get())
        ->setConfig(rtc_config_);
#endif
#endif
  } else {
    LOG_INFO("Use builtin video encoder.");
    video_encoder_factory = webrtc::CreateBuiltinVideoEncoderFactory();
  }

  if (rtc_config_.use_NVDEC) {
#if defined __aarch64__
    LOG_INFO("Use Nvidia H264 video decoder.");
    video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
#elif defined(VTSRTC_HAS_CUDA_DRIVER) && VTSRTC_HAS_CUDA_DRIVER
    LOG_INFO("Use Nvidia H264 video decoder.");
    video_decoder_factory = std::make_unique<webrtc::RtcDecoderFactory>();
#else
    LOG_WARN("use_NVDEC=true but this build has no CUDA/NVDEC support, fallback to builtin video decoder.");
    video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
#endif
  } else {
    LOG_INFO("Use builtin video decoder.");
    video_decoder_factory = webrtc::CreateBuiltinVideoDecoderFactory();
  }

  peer_conn_factory_ = webrtc::CreatePeerConnectionFactory(
      network_thread_.get(), worker_thread_.get(), signaling_thread_.get(),
      audio_device_moudle_, webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      std::move(video_encoder_factory), std::move(video_decoder_factory),
      nullptr, nullptr);

  if (!peer_conn_factory_) {
    // critical error
    LOG_ERROR("[WEBRTC] Create peer connection factory failed");
    return false;
  }

  return true;
}

void RtcConnectionManager::DestroyAllPeerConnection() {
  mtx_.lock();
  for (const auto& it : remotesessionid_rtcconn_map_) {
    if (it.second->peer_conn_) {
      LOG_WARN("Close peer connection <%d>", it.first);
      if (rtc_config_.netstats_report) {
        stats_report_thread_->PostTask(RTC_FROM_HERE, [this]() {
          if (stats_report_timer_) stats_report_timer_->cancel();
        });
      }
      statistics_collector_->Reset();

      if (webrtc_log_hook_) {
        delete webrtc_log_hook_;
        webrtc_log_hook_ = nullptr;
      }

      it.second->peer_conn_->Close();
      it.second->peer_conn_ = nullptr;
#if defined __aarch64__
      // if(rtc_config_.encode_params.use_codec_pool)
      // 	CodecPool::GetInstance()->Destroy();
#endif
    }
  }
  remotesessionid_rtcconn_map_.clear();
  mtx_.unlock();
}

void RtcConnectionManager::DestroyPeerConnection(
    vts_rtc::SessionId remote_sessionid) {
  mtx_.lock();
  for (auto it = remotesessionid_rtcconn_map_.begin();
       it != remotesessionid_rtcconn_map_.end();) {
    if (it->first == remote_sessionid) {
      if (it->second->peer_conn_) {
        LOG_WARN("Close peer connection <%d><%p>", it->first,
                 it->second->peer_conn_.get());
        if (rtc_config_.netstats_report) {
          stats_report_thread_->PostTask(RTC_FROM_HERE, [this]() {
            if (stats_report_timer_) stats_report_timer_->cancel();
          });
        }
        statistics_collector_->RemoveSessionMediaSsrcVsId(remote_sessionid);

        if (webrtc_log_hook_) {
          delete webrtc_log_hook_;
          webrtc_log_hook_ = nullptr;
        }

        it->second->peer_conn_->Close();
        it->second->peer_conn_ = nullptr;
#if defined __aarch64__
        // if (rtc_config_.encode_params.use_codec_pool)
        // 	CodecPool::GetInstance()->Destroy();
#endif
      }
      remotesessionid_rtcconn_map_.erase(it++);
    } else {
      it++;
    }
  }
  mtx_.unlock();
}

void RtcConnectionManager::InitWebsocket() {
  RTC_DCHECK_RUN_ON(ws_client_thread_.get());

  ws_client_ = std::make_shared<WsClient>(rtc_config_.signaling_server_url);
  ws_client_->io_service = ws_io_context_;

  ws_client_->on_open = [this](WsConnection conn) {
    // websocket-client线程（对应ws_client_thread_实例）
    LOG_INFO("Websocket onopen, remote peer: %s:%u",
             conn->remote_endpoint().address().to_string().c_str(),
             conn->remote_endpoint().port());

    network_disconnected_notified_ = false;
    lock_reconnect_ = false;
    if (reconnect_timer_) {
      reconnect_timer_->cancel();
    }

    {
      std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
      ws_conn_ = conn;
    }

    if (!ping_timer_) {
      ping_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
          *ws_io_context_, std::chrono::milliseconds(rtc_config_.ping_timeout));
    } else {
      ping_timer_->expires_after(
          std::chrono::milliseconds(rtc_config_.ping_timeout));
    }
    ping_timer_->async_wait(std::bind(&RtcConnectionManager::SetPingTimeout,
                                      this, std::placeholders::_1));

    // notify connected to signaling server
    if (serverconnection_state_handler_) {
      logic_thread_->PostTask(RTC_FROM_HERE, [this]() {
        serverconnection_state_handler_(
            vts_rtc::ServerConnectionState::Connected);
      });
    }
  };

  ws_client_
      ->on_message = [this](WsConnection conn,
                            std::shared_ptr<WsClient::InMessage> in_message) {
    // websocket-client线程（对应ws_client_thread_实例）
    auto msg_json = json::parse(in_message->string(), nullptr, false);
    if (msg_json.is_discarded()) {
      LOG_ERROR("Websocket onmessage, parse message failed, not vaild json");
      return;
    }

    if (!msg_json.contains("command")) {
      LOG_ERROR("Websocket onmessage, message do not contain command field");
      return;
    }

    auto command = msg_json["command"].get<std::string>();

    // filter heartbeat log info
    if (command != "take_heartbeat") {
      // LOG_INFO("Websocket onmessage, remote peer: %s:%u, "
      // 	"receive command [%s], receive message size: %llu",
      // 	conn->remote_endpoint().address().to_string().c_str(),
      // 	conn->remote_endpoint().port(), command.c_str(),
      // in_message->size());
    }

    if (command == "take_heartbeat") {
      if (pong_timer_) {
        pong_timer_->cancel();
      }
      ping_timer_->expires_after(
          std::chrono::milliseconds(rtc_config_.ping_timeout));
      ping_timer_->async_wait(std::bind(&RtcConnectionManager::SetPingTimeout,
                                        this, std::placeholders::_1));
    } else if (command == "take_info") {
      if (msg_json.contains("type")) {
        auto type = msg_json["type"].get<std::string>();
        if (type == "login_succeed") {
          if (msg_json.contains("sessionid")) {
            auto sessionid = msg_json["sessionid"].get<vts_rtc::SessionId>();
            std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
            current_sessionid_ =
                std::make_shared<vts_rtc::SessionId>(sessionid);

            // notify connected to signaling server
            if (serverconnection_state_handler_) {
              logic_thread_->PostTask(RTC_FROM_HERE, [this]() {
                serverconnection_state_handler_(
                    vts_rtc::ServerConnectionState::Logined);
              });
            }
          }
        }
      }
    } else if (command == "take_roominfo") {
      if (msg_json.contains("type") && msg_json.contains("roomid")) {
        auto type = msg_json["type"].get<std::string>();
        auto roomid = msg_json["roomid"].get<std::string>();

        if (room_handler_) {
          logic_thread_->PostTask(RTC_FROM_HERE, [this, type, roomid]() {
            if (type == "new") {
              room_handler_(vts_rtc::RoomOperation::New, roomid);
            } else if (type == "delete") {
              room_handler_(vts_rtc::RoomOperation::Delete, roomid);
            }
          });
        }
      }
    } else if (command == "take_configuration") {
      if (!msg_json.contains("type")) {
        LOG_ERROR(
            "Websocket onmessage, take configuration message do not contain "
            "type field");
        return;
      }

      auto type = msg_json["type"].get<std::string>();
      auto sdp = msg_json["sdp"].get<std::string>();
      auto from_sessionid = msg_json["from"].get<vts_rtc::SessionId>();
      auto to_sessionid = msg_json["to"].get<vts_rtc::SessionId>();

      auto roomid = msg_json["roomid"].get<vts_rtc::RoomId>();
      if (type == "forward_offer") {
        logic_thread_->PostTask(RTC_FROM_HERE, [this, from_sessionid, sdp]() {
          this->InteractRemotePeer(from_sessionid, false, sdp);
        });
      } else if (type == "forward_answer") {
        logic_thread_->PostTask(RTC_FROM_HERE, [this, from_sessionid, sdp]() {
          this->AckRemotePeerSdp(from_sessionid, sdp);
        });
      }
    } else if (command == "take_candidate") {
      logic_thread_->PostTask(RTC_FROM_HERE, [this, msg_json]() {
        auto from_sessionid = msg_json["from"].get<vts_rtc::SessionId>();
        auto to_sessionid = msg_json["to"].get<vts_rtc::SessionId>();

        if (remotesessionid_rtcconn_map_.find(from_sessionid) ==
            remotesessionid_rtcconn_map_.cend()) {
          LOG_ERROR(
              "Websocket onmessage, rtc connection of remote_sessionid: %u do "
              "not exist when take candidate",
              from_sessionid);
          return;
        }

        auto rtc_conn = remotesessionid_rtcconn_map_[from_sessionid];
        auto candidate = msg_json["candidate"].get<std::string>();
        auto sdp_mid = msg_json["sdp_mid"].get<std::string>();
        int sdp_mline_index = msg_json["sdp_mline_index"].get<int>();
        webrtc::SdpParseError error;
        std::unique_ptr<webrtc::IceCandidateInterface> candidate_object(
            webrtc::CreateIceCandidate(sdp_mid, sdp_mline_index, candidate,
                                       &error));
        if (!candidate_object) {
          LOG_ERROR(
              "Websocket onmessage, parse ice candidate failed, line: %s, "
              "description: %s",
              error.line.c_str(), error.description.c_str());
          return;
        }
        if (rtc_conn && rtc_conn->peer_conn_) {
          bool flag =
              rtc_conn->peer_conn_->AddIceCandidate(candidate_object.get());
          if (!flag) {
            LOG_ERROR(
                "Websocket onmessage, rtc connection add ice candidate failed");
          }
        }
      });
    }
  };

  ws_client_->on_error = [this](WsConnection conn,
                                const SimpleWeb::error_code& ec) {
    // websocket-client线程（对应ws_client_thread_实例）
    LOG_ERROR(
        "Websocket onerror, remote peer: %s:%u, error value: %d, error "
        "message: %s",
        conn->remote_endpoint().address().to_string().c_str(),
        conn->remote_endpoint().port(), ec.value(), ec.message().c_str());

    ReconnectWebsocket();
  };

  ws_client_->on_close = [this](WsConnection conn, int status,
                                const std::string& reason) {
    // websocket-client线程（对应ws_client_thread_实例）
    LOG_INFO(
        "Websocket onclose, remote peer: %s:%u, status value: %d, reason: %s",
        conn->remote_endpoint().address().to_string().c_str(),
        conn->remote_endpoint().port(), status, reason.c_str());

    ReconnectWebsocket();
  };

  ws_client_->start([]() { LOG_INFO("Websocket client is connecting..."); });
  ws_io_context_->run();
}

void RtcConnectionManager::SetPingTimeout(const SimpleWeb::error_code& ec) {
  RTC_DCHECK_RUN_ON(ws_client_thread_.get());

  if (!ec) {
    // exclude SimpleWeb::asio::error::operation_aborted
    json heartbeat_json = {{"command", "take_heartbeat"}, {"type", "ping"}};
    {
      std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
      if (ws_conn_) {
        ws_conn_->send(heartbeat_json.dump());
      }
    }

    if (!pong_timer_) {
      pong_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
          *ws_io_context_, std::chrono::milliseconds(rtc_config_.pong_timeout));
    } else {
      pong_timer_->expires_after(
          std::chrono::milliseconds(rtc_config_.pong_timeout));
    }
    pong_timer_->async_wait([this](const SimpleWeb::error_code& ec) {
      // websocket-client线程（对应ws_client_thread_实例）
      if (!ec) {
        // this means client is disconnected from websocket server
        LOG_WARN("pong timeout, server not available");

        ReconnectWebsocket();
      }
    });
  }
}

void RtcConnectionManager::ReconnectWebsocket() {
  RTC_DCHECK_RUN_ON(ws_client_thread_.get());

  if (!network_disconnected_notified_) {
    {
      std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
      current_sessionid_ = nullptr;
      ws_conn_ = nullptr;
    }

    logic_thread_->PostTask(RTC_FROM_HERE, [this]() {
      // remove p2p connections when WebSocket disconnected
      DestroyAllPeerConnection();

      if (serverconnection_state_handler_) {
        // notify disconnected to signaling server
        serverconnection_state_handler_(
            vts_rtc::ServerConnectionState::Disconnected);

        // notify reconnecting to signaling server
        serverconnection_state_handler_(
            vts_rtc::ServerConnectionState::Reconnecting);
      }
    });

    network_disconnected_notified_ = true;
  }

  if (ping_timer_) {
    ping_timer_->cancel();
  }
  if (pong_timer_) {
    pong_timer_->cancel();
  }

  if (!lock_reconnect_) {
    lock_reconnect_ = true;
    if (!reconnect_timer_) {
      reconnect_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
          *ws_io_context_,
          std::chrono::milliseconds(rtc_config_.reconnect_interval));
    } else {
      reconnect_timer_->expires_after(
          std::chrono::milliseconds(rtc_config_.reconnect_interval));
    }
    reconnect_timer_->async_wait([this](const SimpleWeb::error_code& ec) {
      // websocket-client线程（对应ws_client_thread_实例）
      if (!ec) {
        lock_reconnect_ = false;
        ReconnectWebsocket();
      }
    });

    // stop first
    ws_client_->stop();
    ws_client_->start(
        []() { LOG_INFO("Websocket client is reconnecting..."); });
  }
}

bool RtcConnectionManager::AddDataChannel(const std::string& label,
                                          vts_rtc::PriorityType priority,
                                          bool ordered, int max_retransmits) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (label_datachannelinit_map_.find(label) !=
      label_datachannelinit_map_.cend()) {
    return false;
  }

  webrtc::DataChannelInit config;
  config.ordered = ordered;
  // if max_retransmits < 0, it means reliable
  if (max_retransmits >= 0) {
    config.maxRetransmits = max_retransmits;
  }
  config.priority = static_cast<webrtc::Priority>(priority);

  label_datachannelinit_map_[label] = config;

  return true;
}

bool RtcConnectionManager::AddAudioSource(
    const vts_rtc::AudioSourceId& audio_sourceid,
    vts_rtc::PriorityType priority) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (external_audiosources_.find(audio_sourceid) !=
      external_audiosources_.cend()) {
    return false;
  }

  external_audiosources_[audio_sourceid] =
      new rtc::RefCountedObject<RtcAudioSource>(audio_sourceid,
                                                cricket::AudioOptions());
  return true;
}

bool RtcConnectionManager::AddVideoSource(
    const vts_rtc::VideoSourceId& video_sourceid,
    vts_rtc::PriorityType priority) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (external_feed_tracksources_.find(video_sourceid) !=
      external_feed_tracksources_.cend()) {
    return false;
  }

  external_feed_tracksources_[video_sourceid] =
      new rtc::RefCountedObject<RtcExternalFeedTrackSource>(
          video_sourceid, std::make_unique<RtcVideoSource>(), priority);
  return true;
}

vts_rtc::SessionIds RtcConnectionManager::QueryRemoteAgents() const {
  RTC_DCHECK_RUN_ON(logic_thread_);

  vts_rtc::SessionIds remote_sessionids;
  remote_sessionids.reserve(remotesessionid_rtcconn_map_.size());
  for (const auto& sessionid_rtcconn : remotesessionid_rtcconn_map_) {
    remote_sessionids.emplace_back(sessionid_rtcconn.first);
  }
  return remote_sessionids;
}

vts_rtc::ErrorCode RtcConnectionManager::QueryRoom(
    const vts_rtc::RoomId& roomid, vts_rtc::Room& room) const {
  RTC_DCHECK_RUN_ON(logic_thread_);

  try {
    LOG_INFO("Http client query room info by roomid: %s", roomid.c_str());
    auto response = http_client_->request("GET", "/room/" + roomid);
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR(
          "Http client query room info, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client query room info, error code: %d, error message: %s",
             status, message.c_str());

    if (status_code == HttpStatus::OK) {
      json data_obj = result_obj[HttpStatus::data_field];
      room = data_obj.get<vts_rtc::Room>();
    }

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client query room info occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::QueryRooms(
    vts_rtc::Rooms& rooms) const {
  RTC_DCHECK_RUN_ON(logic_thread_);

  try {
    LOG_INFO("Http client query rooms");
    auto response = http_client_->request("GET", "/rooms");
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR(
          "Http client query rooms, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client query rooms, error code: %d, error message: %s",
             status, message.c_str());

    if (status_code == HttpStatus::OK) {
      json data_obj = result_obj[HttpStatus::data_field];
      rooms = data_obj.get<vts_rtc::Rooms>();
    }

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client query rooms occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::OpenRoom(
    const vts_rtc::RoomId& roomid, enum vts_rtc::RoomType room_type,
    bool force) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  auto copy_sessionid = -1;
  {
    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (current_sessionid_) {
      copy_sessionid = *current_sessionid_;
    }
  }

  if (copy_sessionid == -1) {
    LOG_ERROR("Http client cannot open room, agent not logined");
    return vts_rtc::ErrorCode::AgentNotLogined;
  }

  try {
    std::map<vts_rtc::RoomType, const char*> roomtype_map = {
        {vts_rtc::RoomType::VideoBroadcasting, "VideoBroadcasting"},
        {vts_rtc::RoomType::VideoConference, "VideoConference"}};
    LOG_INFO(
        "Http client open room by sessionid: %d, roomid: %s, room type: %s",
        copy_sessionid, roomid.c_str(), roomtype_map[room_type]);

    json room_obj = {{"sessionid", copy_sessionid},
                     {"roomid", roomid},
                     {"room_type", room_type},
                     {"force", force ? 1 : 0}};
    auto response =
        http_client_->request("POST", "/room/open", room_obj.dump());
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR("Http client open room, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client open room, error code: %d, error message: %s", status,
             message.c_str());

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client open room occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::CloseRoom(
    const vts_rtc::RoomId& roomid) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  auto copy_sessionid = -1;
  {
    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (current_sessionid_) {
      copy_sessionid = *current_sessionid_;
    }
  }

  if (copy_sessionid == -1) {
    LOG_ERROR("Http client cannot close room, agent not logined");
    return vts_rtc::ErrorCode::AgentNotLogined;
  }

  try {
    LOG_INFO("Http client close room by sessionid: %d, roomid: %s",
             copy_sessionid, roomid.c_str());

    json room_obj = {{"sessionid", copy_sessionid}, {"roomid", roomid}};
    auto response =
        http_client_->request("POST", "/room/close", room_obj.dump());
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR("Http client close room, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client close room, error code: %d, error message: %s",
             status, message.c_str());

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client close room occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::JoinRoom(
    const vts_rtc::RoomId& roomid) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  auto copy_sessionid = -1;
  {
    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (current_sessionid_) {
      copy_sessionid = *current_sessionid_;
    }
  }

  if (copy_sessionid == -1) {
    LOG_ERROR("Http client cannot join room, agent not logined");
    return vts_rtc::ErrorCode::AgentNotLogined;
  }

  try {
    LOG_INFO("Http client join room by sessionid: %d, roomid: %s",
             copy_sessionid, roomid.c_str());

    json room_obj = {{"sessionid", copy_sessionid}, {"roomid", roomid}};
    auto response =
        http_client_->request("POST", "/room/join", room_obj.dump());
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR("Http client join room, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client join room, error code: %d, error message: %s", status,
             message.c_str());

    if (status == HttpStatus::OK) {
      json data_obj = result_obj[HttpStatus::data_field];
      auto room = data_obj.get<vts_rtc::Room>();
      switch (room.room_type) {
        case vts_rtc::RoomType::VideoBroadcasting:
          this->InteractRemotePeer(room.broadcaster_sessionid, true, "");
          break;
        case vts_rtc::RoomType::VideoConference:
          for (auto sessionid : room.sessionids) {
            if (copy_sessionid != sessionid) {
              this->InteractRemotePeer(sessionid, true, "");
            }
          }
          break;
        default:
          break;
      }
    }

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client join room occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::LeaveRoom() {
  RTC_DCHECK_RUN_ON(logic_thread_);

  auto copy_sessionid = -1;
  {
    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (current_sessionid_) {
      copy_sessionid = *current_sessionid_;
    }
  }

  if (copy_sessionid == -1) {
    LOG_ERROR("Http client cannot leave room, agent not logined");
    return vts_rtc::ErrorCode::AgentNotLogined;
  }

  try {
    LOG_INFO("Http client leave room by sessionid: %d", copy_sessionid);

    json room_obj = {{"sessionid", copy_sessionid}};
    auto response =
        http_client_->request("POST", "/room/leave", room_obj.dump());
    json result_obj = json::parse(response->content.string(), nullptr, false);
    if (result_obj.is_discarded()) {
      LOG_ERROR("Http client leave room, parse content failed, not valid json");
      return vts_rtc::ErrorCode::InternalError;
    }

    auto status = result_obj[HttpStatus::status_field].get<int>();
    auto status_code = static_cast<HttpStatus::Code>(status);
    auto message = result_obj[HttpStatus::message_field].get<std::string>();
    LOG_INFO("Http client leave room, error code: %d, error message: %s",
             status, message.c_str());

    if (status == HttpStatus::OK) {
      DestroyAllPeerConnection();
    }

    return ConvertHttpCode(status_code);
  } catch (const SimpleWeb::system_error& e) {
    LOG_ERROR("Http client leave room occurs error: %s", e.what());
    return vts_rtc::ErrorCode::InternalError;
  }
}

vts_rtc::ErrorCode RtcConnectionManager::PublishToSRS(
    const vts_rtc::SRSStreamurl& streamurl) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (!SRS_http_client_) {
    return vts_rtc::ErrorCode::InternalError;
  }

  auto SRS_conn = std::make_shared<Rtc2SRSConnection>(streamurl, 0, 0);
  SRS_conn->InitObserverCallbacks();

  webrtc::PeerConnectionInterface::RTCConfiguration peer_conn_config;
  peer_conn_config.disable_link_local_networks = true;
  peer_conn_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // peer_conn_config.enable_dtls_srtp = true;
  webrtc::PeerConnectionDependencies depends(&SRS_conn->peer_conn_observer_);

  auto peer_conn = peer_conn_factory_->CreatePeerConnection(peer_conn_config,
                                                            std::move(depends));
  if (!peer_conn) {
    LOG_ERROR("Publish RTC to SRS, create peer connection failed");
    return vts_rtc::ErrorCode::InternalError;
  }

  SRS_conn->peer_conn_ = peer_conn;

  webrtc::RtpTransceiverInit rtp_transceiver_init;
  rtp_transceiver_init.direction = webrtc::RtpTransceiverDirection::kSendOnly;

  auto audio_track_num = external_audiosources_.size();
  while (audio_track_num--)
    peer_conn->AddTransceiver(cricket::MEDIA_TYPE_AUDIO, rtp_transceiver_init);

  auto video_track_num = external_feed_tracksources_.size();
  while (video_track_num--)
    peer_conn->AddTransceiver(cricket::MEDIA_TYPE_VIDEO, rtp_transceiver_init);

  this->AddAudioTrack2PeerConnection(peer_conn);
  this->AddVideoTrack2PeerConnection(peer_conn);

  // create offer (declare the directional attribute by using RtpTransceiver
  // API instead of RTCOfferAnswerOptions parameters for Unified Plan)
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_conn->CreateOffer(SRS_conn->create_sdp_observer_.get(), options);

  SRS_conn->on_P2P_state_changed_ =
      [this](const vts_rtc::SRSStreamurl& SRS_streamurl,
             vts_rtc::P2PState state) {
        if (state == vts_rtc::P2PState::Failed) {
          LOG_INFO("Reconnect SRS remote peer failed, remote streamurl: %s",
                   SRS_streamurl.c_str());

          logic_thread_->PostTask(
              RTC_FROM_HERE, [this, SRS_streamurl, state]() {
                // @attention: must run in logic thread, otherwise cannot
                // re-create PeerConnection

                if (SRS_state_handler_) {
                  SRS_state_handler_(SRS_streamurl, state);
                }

                auto iter = std::find_if(
                    SRS_publish_conns_.begin(), SRS_publish_conns_.end(),
                    [&SRS_streamurl](std::shared_ptr<Rtc2SRSConnection> conn) {
                      return conn->SRS_streamurl_ == SRS_streamurl;
                    });
                if (iter != SRS_publish_conns_.end()) {
                  *iter = nullptr;
                  SRS_publish_conns_.erase(iter);
                }
              });
        }
      };

  std::weak_ptr<Rtc2SRSConnection> weak_SRS_conn(SRS_conn);
  SRS_conn->on_sdp_create_succeed_ = [this, weak_SRS_conn](
                                         const std::string& offer_sdp) {
    logic_thread_->PostTask(RTC_FROM_HERE, [this, weak_SRS_conn, offer_sdp]() {
      auto shared_SRS_conn = weak_SRS_conn.lock();
      if (!shared_SRS_conn) {
        LOG_ERROR("Publish RTC to SRS failed, SRS_conn is null!");
        return;
      }

      auto raw_SRS_conn = shared_SRS_conn.get();
      auto RemoveCurrecntConnection = [this, raw_SRS_conn]() {
        auto iter = std::find_if(
            SRS_publish_conns_.begin(), SRS_publish_conns_.end(),
            [raw_SRS_conn](std::shared_ptr<Rtc2SRSConnection> conn) {
              return conn.get() == raw_SRS_conn;
            });
        if (iter != SRS_publish_conns_.end()) {
          *iter = nullptr;
          SRS_publish_conns_.erase(iter);
        }
      };

      try {
        auto streamurl = shared_SRS_conn->SRS_streamurl_;
        LOG_INFO("Publish RTC to SRS with streamurl: %s", streamurl.c_str());

        json publisher_obj = {{"streamurl", streamurl}, {"sdp", offer_sdp}};
        auto response = SRS_http_client_->request("POST", "/rtc/v1/publish/",
                                                  publisher_obj.dump());
        json result_obj =
            json::parse(response->content.string(), nullptr, false);
        if (result_obj.is_discarded()) {
          LOG_ERROR(
              "SRS http client publish RTC to SRS, parse content failed, "
              "not valid json");
          RemoveCurrecntConnection();
          return;
        }

        auto code = result_obj["code"].get<int>();
        if (code == 0) {
          if (SRS_publish_state_handler_) {
            SRS_publish_state_handler_(streamurl, vts_rtc::SRSResponse::SRSOK);
          }
          auto answer_sdp = result_obj["sdp"].get<std::string>();
          auto SRS_sessionid =
              result_obj["sessionid"].get<vts_rtc::SRSSessionId>();

          shared_SRS_conn->SetSRSSessionid(SRS_sessionid);

          webrtc::SdpParseError error;
          auto remote_sdp = webrtc::CreateSessionDescription(
              webrtc::SdpType::kAnswer, answer_sdp, &error);
          if (!remote_sdp) {
            LOG_ERROR(
                "Publish RTC to SRS, create answer SDP failed, line: %s, "
                "description: %s",
                error.line.c_str(), error.description.c_str());
            RemoveCurrecntConnection();
            return;
          }

          if (shared_SRS_conn->peer_conn_) {
            shared_SRS_conn->peer_conn_->SetRemoteDescription(
                std::move(remote_sdp),
                shared_SRS_conn->set_remote_sdp_observer_);
          }
        } else {
          LOG_ERROR("Publish RTC to SRS failed, error code: %d", code);
          RemoveCurrecntConnection();
          if (code == 400 && SRS_publish_state_handler_) {
            SRS_publish_state_handler_(
                streamurl, vts_rtc::SRSResponse::SRSStreamAlreadyExisted);
          }
        }
      } catch (const SimpleWeb::system_error& e) {
        LOG_ERROR("Publish RTC to SRS occurs error: %s", e.what());
        RemoveCurrecntConnection();
      }
    });
  };

  SRS_publish_conns_.emplace_back(SRS_conn);

  return vts_rtc::ErrorCode::OK;
}

vts_rtc::ErrorCode RtcConnectionManager::UnpublishRtc2SRS(
    const vts_rtc::SRSStreamurl& streamurl) {
  auto iter =
      std::find_if(SRS_publish_conns_.begin(), SRS_publish_conns_.end(),
                   [&streamurl](std::shared_ptr<Rtc2SRSConnection> conn) {
                     return conn->SRS_streamurl_ == streamurl;
                   });
  if (iter != SRS_publish_conns_.end()) {
    try {
      LOG_INFO("Unpublish RTC to SRS with streamurl: %s, sessionid: %s",
               streamurl.c_str(), (*iter)->GetSRSSessionid().c_str());

      json publisher_obj = {{"streamurl", streamurl},
                            {"sessionid", (*iter)->GetSRSSessionid()}};
      auto response = SRS_http_client_->request("POST", "/rtc/v1/unpublish/",
                                                publisher_obj.dump());
      json result_obj = json::parse(response->content.string(), nullptr, false);
      if (result_obj.is_discarded()) {
        LOG_ERROR(
            "SRS http client unpublish RTC to SRS, parse content failed, "
            "not valid json");
        return vts_rtc::ErrorCode::InternalError;
      }

      auto code = result_obj["code"].get<int>();
      if (code == 0) {
        LOG_WARN("Unpublish RTC to SRS success, error code: %d", code);
      } else {
        LOG_ERROR("Unpublish RTC to SRS success, error code: %d", code);
      }
    } catch (const SimpleWeb::system_error& e) {
      LOG_ERROR("Unpublish RTC to SRS success occurs error: %s", e.what());
      auto a = e;
    }

    *iter = nullptr;
    SRS_publish_conns_.erase(iter);
  }

  return vts_rtc::ErrorCode::OK;
}

vts_rtc::ErrorCode RtcConnectionManager::PlayFromSRS(
    const vts_rtc::SRSStreamurl& streamurl) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (!SRS_http_client_) {
    return vts_rtc::ErrorCode::InternalError;
  }

  auto SRS_conn = std::make_shared<Rtc2SRSConnection>(streamurl, 0, 0);
  SRS_conn->InitObserverCallbacks();

  webrtc::PeerConnectionInterface::RTCConfiguration peer_conn_config;
  peer_conn_config.disable_link_local_networks = true;
  peer_conn_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  // peer_conn_config.enable_dtls_srtp = true;
  webrtc::PeerConnectionDependencies depends(&SRS_conn->peer_conn_observer_);
  auto peer_conn = peer_conn_factory_->CreatePeerConnection(peer_conn_config,
                                                            std::move(depends));
  if (!peer_conn) {
    LOG_ERROR("Play RTC from SRS, create peer connection failed");
    return vts_rtc::ErrorCode::InternalError;
  }

  webrtc::RtpTransceiverInit rtp_transceiver_init;
  rtp_transceiver_init.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
  peer_conn->AddTransceiver(cricket::MEDIA_TYPE_AUDIO, rtp_transceiver_init);
  peer_conn->AddTransceiver(cricket::MEDIA_TYPE_VIDEO, rtp_transceiver_init);

  // create offer (declare the directional attribute by using RtpTransceiver
  // API instead of RTCOfferAnswerOptions parameters for Unified Plan)
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  peer_conn->CreateOffer(SRS_conn->create_sdp_observer_.get(), options);

  SRS_conn->peer_conn_ = peer_conn;

  std::weak_ptr<RtcConnectionManager> weak_self = shared_from_this();
  SRS_conn->on_audioframe_received_ =
      [this, weak_self](
          const vts_rtc::SessionId, const vts_rtc::AudioSourceId& sourceid,
          enum vts_rtc::MediaSourceType type, size_t bits_per_sample,
          size_t sample_rate, size_t number_of_channels,
          size_t number_of_frames, const void* audio_data) {
        auto self = weak_self.lock();
        if (!self) {
          LOG_ERROR(
              "[WEBRTC] SRS connection on_audioframe_received, "
              "but rtc connection manager has been destroyed.");
          return;
        }

        recv_audioframe_handler_(0, sourceid, type, bits_per_sample,
                                 sample_rate, number_of_channels,
                                 number_of_frames, audio_data);
      };
  SRS_conn->on_frame_received_ = [this, weak_self](
                                     const vts_rtc::SessionId,
                                     const vts_rtc::VideoSourceId& sourceid,
                                     enum vts_rtc::MediaSourceType type,
                                     size_t width, size_t height,
                                     size_t dimension,
                                     const std::vector<unsigned char>& buffer) {
    auto self = weak_self.lock();
    if (!self) {
      LOG_ERROR(
          "[WEBRTC] SRS connection on_frame_received, "
          "but rtc connection manager has been destroyed.");
      return;
    }

    recv_frame_handler_(0, sourceid, type, width, height, dimension, buffer);
  };

  SRS_conn->on_P2P_state_changed_ =
      [this](const vts_rtc::SRSStreamurl& SRS_streamurl,
             vts_rtc::P2PState state) {
        if (state == vts_rtc::P2PState::Failed) {
          LOG_INFO("Reconnect SRS remote peer failed, remote streamurl: %s",
                   SRS_streamurl.c_str());

          logic_thread_->PostTask(RTC_FROM_HERE, [this, SRS_streamurl]() {
            // @attention: must run in logic thread, otherwise cannot re-create
            // PeerConnection
            auto iter = std::find_if(
                SRS_play_conns_.begin(), SRS_play_conns_.end(),
                [&SRS_streamurl](std::shared_ptr<Rtc2SRSConnection> conn) {
                  return conn->SRS_streamurl_ == SRS_streamurl;
                });
            if (iter != SRS_play_conns_.end()) {
              *iter = nullptr;
              SRS_play_conns_.erase(iter);
            }
          });
        }
      };

  std::weak_ptr<Rtc2SRSConnection> weak_SRS_conn(SRS_conn);
  SRS_conn->on_sdp_create_succeed_ = [this, weak_SRS_conn](
                                         const std::string& offer_sdp) {
    logic_thread_->PostTask(RTC_FROM_HERE, [this, weak_SRS_conn, offer_sdp]() {
      auto shared_SRS_conn = weak_SRS_conn.lock();
      if (!shared_SRS_conn) {
        LOG_ERROR("Play RTC from SRS failed, SRS_conn is null!");
        return;
      }

      auto raw_SRS_conn = shared_SRS_conn.get();
      auto RemoveCurrecntConnection = [this, raw_SRS_conn]() {
        auto iter = std::find_if(
            SRS_play_conns_.begin(), SRS_play_conns_.end(),
            [raw_SRS_conn](std::shared_ptr<Rtc2SRSConnection> conn) {
              return conn.get() == raw_SRS_conn;
            });
        if (iter != SRS_play_conns_.end()) {
          *iter = nullptr;
          SRS_play_conns_.erase(iter);
        }
      };

      try {
        auto streamurl = shared_SRS_conn->SRS_streamurl_;
        LOG_INFO("Play RTC from SRS with streamurl: %s", streamurl.c_str());

        json play_obj = {{"streamurl", streamurl}, {"sdp", offer_sdp}};
        auto response =
            SRS_http_client_->request("POST", "/rtc/v1/play/", play_obj.dump());
        json result_obj =
            json::parse(response->content.string(), nullptr, false);
        if (result_obj.is_discarded()) {
          LOG_ERROR(
              "SRS http client play RTC from SRS, parse content failed, "
              "not valid json");
          RemoveCurrecntConnection();
          return;
        }

        auto code = result_obj["code"].get<int>();
        if (code == 0) {
          auto answer_sdp = result_obj["sdp"].get<std::string>();
          auto SRS_sessionid =
              result_obj["sessionid"].get<vts_rtc::SRSSessionId>();

          shared_SRS_conn->SetSRSSessionid(SRS_sessionid);

          webrtc::SdpParseError error;
          auto remote_sdp = webrtc::CreateSessionDescription(
              webrtc::SdpType::kAnswer, answer_sdp, &error);
          if (!remote_sdp) {
            LOG_ERROR(
                "Play RTC from SRS, create answer SDP failed, line: %s, "
                "description: %s",
                error.line.c_str(), error.description.c_str());
            RemoveCurrecntConnection();
            return;
          }

          if (shared_SRS_conn->peer_conn_) {
            shared_SRS_conn->peer_conn_->SetRemoteDescription(
                std::move(remote_sdp),
                shared_SRS_conn->set_remote_sdp_observer_);
          }
        } else {
          LOG_ERROR("Play RTC from SRS failed, error code: %d", code);
          RemoveCurrecntConnection();
        }
      } catch (const SimpleWeb::system_error& e) {
        LOG_ERROR("Play RTC from SRS occurs error: %s", e.what());
        RemoveCurrecntConnection();
      }
    });
  };

  SRS_play_conns_.emplace_back(SRS_conn);

  return vts_rtc::ErrorCode::OK;
}

vts_rtc::ErrorCode RtcConnectionManager::UnplayFromSRS(
    const vts_rtc::SRSStreamurl& streamurl) {
  // TO DO
  return vts_rtc::ErrorCode::OK;
}

bool RtcConnectionManager::SendData(vts_rtc::SessionId sessionid,
                                    const std::string& channel_label,
                                    const std::string& msg) const {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (remotesessionid_rtcconn_map_.find(sessionid) ==
      remotesessionid_rtcconn_map_.cend()) {
    return false;
  }

  return remotesessionid_rtcconn_map_.at(sessionid)->SendData(channel_label,
                                                              msg);
}

bool RtcConnectionManager::BroadcastData(const std::string& channel_label,
                                         const std::string& msg) const {
  RTC_DCHECK_RUN_ON(logic_thread_);

  bool succeed = false;
  for (const auto& sessionid_rtcconn : remotesessionid_rtcconn_map_) {
    succeed |= sessionid_rtcconn.second->SendData(channel_label, msg);
  }

  return succeed;
}

void RtcConnectionManager::SendAudioFrame(
    const vts_rtc::AudioSourceId& audio_sourceid,
    const vts_rtc::PCMData& pcmdata) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (external_audiosources_.find(audio_sourceid) !=
      external_audiosources_.cend()) {
    external_audiosources_[audio_sourceid]->OnData(pcmdata);
  }
}

void RtcConnectionManager::SendFrame(
    const vts_rtc::VideoSourceId& video_sourceid,
    const vts_rtc::YUV420pFrame& frame) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (external_feed_tracksources_.find(video_sourceid) !=
      external_feed_tracksources_.cend()) {
    auto frame_build = BuildAndLimitFrameSize(video_sourceid, frame);
    external_feed_tracksources_[video_sourceid]->video_source_->OnFrame(
        frame_build);
  }
}

webrtc::VideoFrame RtcConnectionManager::BuildAndLimitFrameSize(
    const vts_rtc::VideoSourceId& video_sourceid,
    const vts_rtc::YUV420pFrame& frame) {
  const int64_t capture_time_us = rtc::TimeMicros();
  const int64_t capture_time_ms = capture_time_us / 1000;
  const uint32_t capture_timestamp_rtp =
      static_cast<uint32_t>(capture_time_us * 90 / 1000);

  // @attention: copy frame data
  auto I420buffer = webrtc::I420Buffer::Copy(
      frame.width, frame.height, frame.buffer, frame.stride_Y,
      frame.buffer + frame.stride_Y * frame.height, frame.stride_U,
      frame.buffer + frame.stride_Y * frame.height +
          frame.stride_U * ((frame.height + 1) / 2),
      frame.stride_V);

  auto iter = rtc_config_.resolution_limit.find(video_sourceid);
  if (iter != rtc_config_.resolution_limit.end()) {
    int out_width = iter->second.first;
    int out_height = iter->second.second;
    if (frame.width > out_width || frame.height > out_height) {
      auto scaled_buffer = webrtc::I420Buffer::Create(out_width, out_height);
      scaled_buffer->ScaleFrom(*I420buffer);
      auto frame_build = webrtc::VideoFrame::Builder()
                             .set_video_frame_buffer(scaled_buffer)
                             .set_rotation(webrtc::kVideoRotation_0)
                             .set_timestamp_us(capture_time_us)
                             .set_timestamp_rtp(capture_timestamp_rtp)
                             .set_ntp_time_ms(capture_time_ms)
                             .build();
      // if (frame.has_update_rect()) {
      // 	auto new_rect =
      // frame.update_rect().ScaleWithFrame(frame.width(), frame.height(),
      // 0, 0, frame.width(), frame.height(), out_width, out_height);
      // 	frame_build.set_update_rect(new_rect);
      // }
      return frame_build;
    } else {
      auto frame_build = webrtc::VideoFrame::Builder()
                             .set_video_frame_buffer(I420buffer)
                             .set_rotation(webrtc::kVideoRotation_0)
                             .set_timestamp_us(capture_time_us)
                             .set_timestamp_rtp(capture_timestamp_rtp)
                             .set_ntp_time_ms(capture_time_ms)
                             .build();
      return frame_build;
    }
  } else {
    auto frame_build = webrtc::VideoFrame::Builder()
                           .set_video_frame_buffer(I420buffer)
                           .set_rotation(webrtc::kVideoRotation_0)
                           .set_timestamp_us(capture_time_us)
                           .set_timestamp_rtp(capture_timestamp_rtp)
                           .set_ntp_time_ms(capture_time_ms)
                           .build();
    return frame_build;
  }
}

void RtcConnectionManager::SetRtpSendersPriority() {
  RTC_DCHECK_RUN_ON(logic_thread_);

  const int configured_min_bitrate_bps =
      static_cast<int>(std::min<unsigned int>(
          rtc_config_.encode_params.bitrate_minmum,
          static_cast<unsigned int>(std::numeric_limits<int>::max())));
  const int configured_max_bitrate_bps =
      static_cast<int>(std::min<unsigned int>(
          rtc_config_.encode_params.bitrate_maxmum,
          static_cast<unsigned int>(std::numeric_limits<int>::max())));

  for (const auto& rtpsender_priority : rtpsender_priority_map_) {
    auto& rtpsender = rtpsender_priority.first;
    auto rtpparams = rtpsender->GetParameters();
    if (rtpparams.encodings.size() > 0) {
      std::map<vts_rtc::PriorityType, double> bitrate_priority_map = {
          {vts_rtc::PriorityType::VeryLow, 0.5},
          {vts_rtc::PriorityType::Low, 1.0},
          {vts_rtc::PriorityType::Medium, 2.0},
          {vts_rtc::PriorityType::High, 4.0}};

      rtpparams.encodings[0].bitrate_priority =
          bitrate_priority_map[rtpsender_priority.second];
      rtpparams.encodings[0].network_priority =
          static_cast<webrtc::Priority>(rtpsender_priority.second);
      if (configured_max_bitrate_bps > 0) {
        rtpparams.encodings[0].max_bitrate_bps = configured_max_bitrate_bps;
      }
      if (configured_min_bitrate_bps > 0) {
        rtpparams.encodings[0].min_bitrate_bps = configured_min_bitrate_bps;
      }
      auto error = rtpsender->SetParameters(rtpparams);
      if (!error.ok()) {
        LOG_WARN("Set priority of rtpsender (%s) failed, reason: %s",
                 rtpsender->id().c_str(), error.message());
      }
    } else {
      LOG_WARN("Set priority of rtpsender (%s) failed, reason: encodings empty",
               rtpsender->id().c_str());
    }
  }
}

void RtcConnectionManager::AddAudioTrack2PeerConnection(
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  // @attention
  // 由于接收端的MediaStreamTrack的id域是唯一的GUID，并不具有业务含义，
  // 所以此处约定一个track只属于一个stream，同时track和stream的label值相同，
  // 通过访问接收端的MediaStream的id域作为VideoSourceId值

  // Add external audio tracks
  for (const auto& id_audiosource : external_audiosources_) {
    auto audio_sourceid = id_audiosource.first;
    auto audio_source = id_audiosource.second;
    auto audio_track = peer_conn_factory_->CreateAudioTrack(audio_sourceid,
                                                            audio_source.get());

    auto rtpsender_error = peer_conn->AddTrack(audio_track, {audio_sourceid});
    if (rtpsender_error.ok()) {
      auto rtpsender = rtpsender_error.value();
      if (rtpsender) {
      }
    } else {
      LOG_ERROR("[WEBRTC] Add track (%s) failed, reason: %s",
                audio_sourceid.c_str(), rtpsender_error.error().message());
    }
  }
}

void RtcConnectionManager::AddVideoTrack2PeerConnection(
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_conn) {
  RTC_DCHECK_RUN_ON(logic_thread_);
  rtpsender_priority_map_.clear();

  // @attention
  // 由于接收端的MediaStreamTrack的id域是唯一的GUID，并不具有业务含义，
  // 所以此处约定一个track只属于一个stream，同时track和stream的label值相同，
  // 通过访问接收端的MediaStream的id域作为VideoSourceId值

  // Add camera capturer video tracks
  if (rtc_device_manager_) {
    auto video_track_sources = rtc_device_manager_->GetVideoTrackSources();
    for (const auto& track_source : video_track_sources) {
      auto tracklabel = track_source->GetLabel();
      auto video_track =
          peer_conn_factory_->CreateVideoTrack(tracklabel, track_source.get());
      // 帧率优先
      video_track->set_content_hint(
          webrtc::VideoTrackInterface::ContentHint::kFluid);
      auto rtpsender_error = peer_conn->AddTrack(video_track, {tracklabel});
      if (rtpsender_error.ok()) {
        auto rtpsender = rtpsender_error.value();
        if (rtpsender) {
          rtpsender_priority_map_[rtpsender] = track_source->GetPriority();
        }
      } else {
        LOG_ERROR("[WEBRTC] Add track (%s) failed, reason: %s",
                  tracklabel.c_str(), rtpsender_error.error().message());
      }
    }
  }

  // Add external feed video tracks
  for (const auto& id_tracksource : external_feed_tracksources_) {
    auto track_source = id_tracksource.second;
    auto tracklabel = track_source->label_;
    auto video_track =
        peer_conn_factory_->CreateVideoTrack(tracklabel, track_source.get());
    // 帧率优先
    video_track->set_content_hint(
        webrtc::VideoTrackInterface::ContentHint::kFluid);
    auto rtpsender_error = peer_conn->AddTrack(video_track, {tracklabel});
    if (rtpsender_error.ok()) {
      auto rtpsender = rtpsender_error.value();
      if (rtpsender) {
        rtpsender_priority_map_[rtpsender] = track_source->priority_;
      }
      LOG_INFO("[WEBRTC] Add track (%s) success", tracklabel.c_str());
    } else {
      LOG_ERROR("[WEBRTC] Add track (%s) failed, reason: %s",
                tracklabel.c_str(), rtpsender_error.error().message());
    }
  }
}

void RtcConnectionManager::InitStatsReport() {
  if (!stats_report_timer_) {
    stats_report_timer_ = std::make_shared<SimpleWeb::asio::steady_timer>(
        *stats_report_io_context_, std::chrono::milliseconds(1000));
  }
  LOG_WARN("Start stats report timer");

  StatsReport(stats_report_timer_);

  stats_report_io_context_->run();
}

void RtcConnectionManager::StatsReport(SteadyTimer steady_timer) {
  RTC_DCHECK_RUN_ON(stats_report_thread_.get());
  std::weak_ptr<RtcConnectionManager> weak_self = shared_from_this();
  steady_timer->expires_from_now(std::chrono::milliseconds(1000));
  steady_timer->async_wait(
      [this, steady_timer, weak_self](const SimpleWeb::error_code& ec) {
        auto self = weak_self.lock();
        if (!self) {
          return;
        }
        mtx_.lock();
        size_t conn_count = remotesessionid_rtcconn_map_.size();
        size_t stats_called = 0;
        for (const auto& rtccon_obj : remotesessionid_rtcconn_map_) {
          if (rtccon_obj.second->peer_conn_ != nullptr) {
            stats_called++;
            rtccon_obj.second->peer_conn_->GetStats(
                rtccon_obj.second->rtc_channel_stats_observer_);
          }
        }
        mtx_.unlock();
        LOG_INFO(
            "[Stats] StatsReport called, TotalConnections: %zu, "
            "GetStatsCalled: %zu",
            conn_count, stats_called);
        StatsReport(steady_timer);
      });
}

void RtcConnectionManager::InteractRemotePeer(
    vts_rtc::SessionId remote_sessionid, bool offer_peer,
    const std::string& remote_sdp) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  std::shared_ptr<RtcConnection> rtc_conn = nullptr;
  {
    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (!current_sessionid_) {
      LOG_ERROR("Interact remote peer, but current_sessionid_ is null");
      P2P_state_handler_(remote_sessionid, vts_rtc::P2PState::Closed);
      return;
    }
    rtc_conn =
        std::make_shared<RtcConnection>(*current_sessionid_, remote_sessionid);
  }
  rtc_conn->InitObserverCallbacks();
  std::weak_ptr<RtcConnectionManager> weak_self = shared_from_this();

  rtc_conn->on_P2P_state_changed_ = [this, rtc_conn, weak_self](
                                        vts_rtc::SessionId remote_sessionid,
                                        vts_rtc::P2PState state) {
    RTC_DCHECK_RUN_ON(logic_thread_);
    auto self = weak_self.lock();
    if (!self) {
      LOG_ERROR(
          "[WEBRTC] Rtc connection on_P2P_state_changed_, "
          "but rtc connection manager has been destroyed.");
      return;
    }

    if (P2P_state_handler_) {
      P2P_state_handler_(remote_sessionid, state);
    }

    if (state == vts_rtc::P2PState::Connected && current_sessionid_) {
      LOG_INFO(
          "[Stats] P2P Connected, netstats_report: %d, stats_report_inited: %d",
          rtc_config_.netstats_report, stats_report_inited_);
      if (rtc_config_.netstats_report && !stats_report_inited_) {
        LOG_INFO("[Stats] Starting stats report initialization");
        stats_report_thread_->PostTask(RTC_FROM_HERE,
                                       [this]() { InitStatsReport(); });
        stats_report_inited_ = true;
      }

      // get ssrc and sourceid info for statistics
      if (rtc_conn->peer_conn_) {
        auto rtpsenders = rtc_conn->peer_conn_->GetSenders();
        // LOG_INFO("[Stats] Found %zu RTP senders", rtpsenders.size());
        for (auto it : rtpsenders) {
          uint32_t ssrc = it->ssrc();
          std::string track_id = it->id();
          bool is_external = external_feed_tracksources_.find(track_id) !=
                             external_feed_tracksources_.end();
          // LOG_INFO("[Stats] RTP Sender: track_id=%s, ssrc=%u,
          // is_external=%d",
          //          track_id.c_str(), ssrc, is_external);

          // 注册所有发送端，不仅仅是external feed
          // 使用track_id作为sourceid
          statistics_collector_->AddSessionSendersMediaSsrcVsId(
              remote_sessionid, ssrc, track_id);
        }

        auto rtpreceivers = rtc_conn->peer_conn_->GetReceivers();
        // LOG_INFO("[Stats] Found %zu RTP receivers", rtpreceivers.size());
        receiver_tracksources_id_vs_ssrc_.clear();
        for (auto it : rtpreceivers) {
          auto streamids = it->stream_ids();
          auto encoding_obj = it->GetParameters().encodings;
          // LOG_INFO(
          //     "[Stats] RTP Receiver: streamids.size()=%zu, "
          //     "encodings.size()=%zu",
          //     streamids.size(), encoding_obj.size());

          if (!streamids.empty() && !encoding_obj.empty()) {
            uint32_t ssrc = encoding_obj[0].ssrc.value();
            std::string sourceid = streamids[0];
            LOG_INFO("[Stats] Registering receiver: sourceid=%s, ssrc=%u",
                     sourceid.c_str(), ssrc);
            statistics_collector_->AddSessionReceiversMediaSsrcVsId(
                remote_sessionid, sourceid, ssrc);
          } else {
            // LOG_WARN(
            //     "[Stats] RTP Receiver skipped: streamids or encodings
            //     empty");
          }
        }
      } else {
        // LOG_WARN("[Stats] peer_conn_ is null, cannot register SSRC
        // mappings");
      }
    }

    if (state == vts_rtc::P2PState::Failed) {
      // @attention: must run in logic thread, otherwise cannot re-create
      // PeerConnection
      LOG_WARN("P2P connection <%u> state Failed", remote_sessionid);
      DestroyPeerConnection(remote_sessionid);
    }
  };

  rtc_conn->on_net_stats_report_ =
      [this, remote_sessionid, weak_self](
          const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
        LOG_INFO(
            "[Stats] on_net_stats_report_ callback called for SessionID: %u",
            remote_sessionid);
        statistics_collector_->OnStatisticsReport(remote_sessionid, report);
      };

  rtc_conn->on_sdp_create_succeed_ = [this, offer_peer](
                                         vts_rtc::SessionId remote_sessionid,
                                         const std::string& sdp) {
    RTC_DCHECK_RUN_ON(logic_thread_);

    std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
    if (current_sessionid_ && ws_conn_) {
      json msg_obj = {{"command", "take_configuration"},
                      {"type", offer_peer ? "offer" : "answer"},
                      {"sdp", sdp},
                      {"from", *current_sessionid_},
                      {"to", remote_sessionid},
                      {"roomid", "not_needed"}};

      ws_conn_->send(msg_obj.dump());
    }
  };

  rtc_conn->on_ice_candidate_received_ =
      [this](vts_rtc::SessionId remote_sessionid,
             const std::tuple<std::string, std::string, int>& ice_candidate) {
        RTC_DCHECK_RUN_ON(logic_thread_);

        std::lock_guard<std::mutex> lg(cursessionid_wsconn_mtx_);
        if (current_sessionid_ && ws_conn_) {
          json msg_obj = {{"command", "take_candidate"},
                          {"candidate", std::get<0>(ice_candidate)},
                          {"sdp_mid", std::get<1>(ice_candidate)},
                          {"sdp_mline_index", std::get<2>(ice_candidate)},
                          {"from", *current_sessionid_},
                          {"to", remote_sessionid}};

          ws_conn_->send(msg_obj.dump());
        }
      };

  // rtc_conn->on_dc_state_changed_ = datachannel_state_handler_;
  rtc_conn->on_dc_state_changed_ = [this, remote_sessionid, weak_self](
                                       vts_rtc::SessionId sessionId,
                                       const std::string& datachannel_label,
                                       vts_rtc::DataChannelState state) {
    auto self = weak_self.lock();
    if (!self) {
      LOG_ERROR(
          "[WEBRTC] Rtc connection on_dc_state_changed_, "
          "but rtc connection manager has been destroyed.");
      return;
    }
    // WebRTC内部不存在DataChannel的重连机制，同时本端和远端的DataChannel状态
    // 并非完全一致（存在本端DataChannel已关闭，对端1.5分钟才感知到关闭），故暂且
    // 选择关闭P2P连接来通知上层业务进行重连
    if (state == vts_rtc::DataChannelState::Closed) {
      DestroyPeerConnection(remote_sessionid);
    }
    datachannel_state_handler_(sessionId, datachannel_label, state);
  };

  rtc_conn->on_dc_message_received_ = recv_msg_handler_;

  if (recv_audioframe_handler_) {
    rtc_conn->on_audioframe_received_ =
        [this, remote_sessionid, weak_self](
            const vts_rtc::SessionId, const vts_rtc::AudioSourceId& sourceid,
            enum vts_rtc::MediaSourceType type, size_t bits_per_sample,
            size_t sample_rate, size_t number_of_channels,
            size_t number_of_frames, const void* audio_data) {
          auto self = weak_self.lock();
          if (!self) {
            LOG_ERROR(
                "[WEBRTC] Rtc connection on_audioframe_received, "
                "but rtc connection manager has been destroyed.");
            return;
          }

          recv_audioframe_handler_(
              remote_sessionid, sourceid, type, bits_per_sample, sample_rate,
              number_of_channels, number_of_frames, audio_data);
        };
  }
  if (recv_frame_handler_) {
    rtc_conn->on_frame_received_ =
        [this, remote_sessionid, weak_self](
            const vts_rtc::SessionId, const vts_rtc::VideoSourceId& sourceid,
            enum vts_rtc::MediaSourceType type, size_t width, size_t height,
            size_t dimension, const std::vector<unsigned char>& buffer) {
          auto self = weak_self.lock();
          if (!self) {
            LOG_ERROR(
                "[WEBRTC] Rtc connection on_frame_received, "
                "but rtc connection manager has been destroyed.");
            return;
          }

          recv_frame_handler_(remote_sessionid, sourceid, type, width, height,
                              dimension, buffer);
        };
  }

  webrtc::PeerConnectionInterface::RTCConfiguration peer_conn_config;
  for (const auto& ice_server : rtc_config_.ice_servers) {
    webrtc::PeerConnectionInterface::IceServer webrtc_ice_server;
    webrtc_ice_server.urls = ice_server.urls;
    webrtc_ice_server.username = ice_server.username;
    webrtc_ice_server.password = ice_server.password;
    peer_conn_config.servers.emplace_back(webrtc_ice_server);
  }

  peer_conn_config.continual_gathering_policy =
      webrtc::PeerConnectionInterface::GATHER_CONTINUALLY;
  peer_conn_config.disable_ipv6 = true;
  peer_conn_config.disable_link_local_networks = true;
  peer_conn_config.tcp_candidate_policy =
      webrtc::PeerConnectionInterface::kTcpCandidatePolicyDisabled;
  peer_conn_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  webrtc::PeerConnectionDependencies depends(&rtc_conn->peer_conn_observer_);
  rtc_conn->peer_conn_ = peer_conn_factory_->CreatePeerConnection(
      peer_conn_config, std::move(depends));
  if (!rtc_conn->peer_conn_) {
    LOG_ERROR("Interact remote peer, create peer connection failed");
    P2P_state_handler_(remote_sessionid, vts_rtc::P2PState::Failed);
    return;
  }

  webrtc::BitrateSettings bitratelimit;
  if (rtc_config_.encode_params.bitrate_minmum > 0) {
    bitratelimit.min_bitrate_bps = static_cast<int>(std::min<unsigned int>(
        rtc_config_.encode_params.bitrate_minmum,
        static_cast<unsigned int>(std::numeric_limits<int>::max())));
  }
  if (rtc_config_.encode_params.bitrate_start > 0) {
    bitratelimit.start_bitrate_bps = static_cast<int>(std::min<unsigned int>(
        rtc_config_.encode_params.bitrate_start,
        static_cast<unsigned int>(std::numeric_limits<int>::max())));
  }
  if (rtc_config_.encode_params.bitrate_maxmum > 0) {
    bitratelimit.max_bitrate_bps = static_cast<int>(std::min<unsigned int>(
        rtc_config_.encode_params.bitrate_maxmum,
        static_cast<unsigned int>(std::numeric_limits<int>::max())));
  }
  rtc_conn->peer_conn_->SetBitrate(bitratelimit);

  this->AddAudioTrack2PeerConnection(rtc_conn->peer_conn_);
  this->AddVideoTrack2PeerConnection(rtc_conn->peer_conn_);

  const auto configure_media_directions = [this, &rtc_conn]() {
    for (const auto& transceiver : rtc_conn->peer_conn_->GetTransceivers()) {
      const bool has_sender =
          transceiver->sender() && transceiver->sender()->track();
      bool wants_receiver = false;
      if (transceiver->media_type() == cricket::MEDIA_TYPE_AUDIO) {
        wants_receiver = static_cast<bool>(recv_audioframe_handler_);
      } else if (transceiver->media_type() == cricket::MEDIA_TYPE_VIDEO) {
        wants_receiver = static_cast<bool>(recv_frame_handler_);
      }

      if (has_sender && wants_receiver) {
        transceiver->SetDirection(
            webrtc::RtpTransceiverDirection::kSendRecv);
      } else if (has_sender) {
        transceiver->SetDirection(webrtc::RtpTransceiverDirection::kSendOnly);
      } else if (wants_receiver) {
        transceiver->SetDirection(webrtc::RtpTransceiverDirection::kRecvOnly);
      } else {
        transceiver->SetDirection(webrtc::RtpTransceiverDirection::kInactive);
      }
    }
  };

  configure_media_directions();

  if (offer_peer) {
    // Add data channels (just for offer side for now)
    for (const auto& label_dcinit : label_datachannelinit_map_) {
      bool succeed =
          rtc_conn->AddDataChannel(label_dcinit.first, label_dcinit.second);
      if (!succeed) {
        LOG_ERROR("Add data channel (%s) failed", label_dcinit.first.c_str());
      }
    }

    // create offer (declare the directional attribute by using
    // RtpTransceiver API instead of RTCOfferAnswerOptions parameters
    // for Unified Plan)
    webrtc::RtpTransceiverInit audio_transceiver_init;
    audio_transceiver_init.direction =
        recv_audioframe_handler_
            ? webrtc::RtpTransceiverDirection::kRecvOnly
            : webrtc::RtpTransceiverDirection::kInactive;
    webrtc::RtpTransceiverInit video_transceiver_init;
    video_transceiver_init.direction = recv_frame_handler_
                                           ? webrtc::RtpTransceiverDirection::kRecvOnly
                                           : webrtc::RtpTransceiverDirection::kInactive;
    for (int i = 0; i < 12; ++i) {
      rtc_conn->peer_conn_->AddTransceiver(cricket::MEDIA_TYPE_AUDIO,
                                           audio_transceiver_init);
      rtc_conn->peer_conn_->AddTransceiver(cricket::MEDIA_TYPE_VIDEO,
                                           video_transceiver_init);
    }

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    rtc_conn->peer_conn_->CreateOffer(rtc_conn->create_sdp_observer_.get(),
                                      options);
  } else {
    // set remote offer SDP
    webrtc::SdpParseError error;
    auto remote_session_description = webrtc::CreateSessionDescription(
        webrtc::SdpType::kOffer, remote_sdp, &error);
    if (!remote_session_description) {
      LOG_ERROR(
          "Interact remote peer, create SDP failed, line: %s, description: %s",
          error.line.c_str(), error.description.c_str());
      return;
    }
    rtc_conn->peer_conn_->SetRemoteDescription(
        std::move(remote_session_description),
        rtc_conn->set_remote_sdp_observer_);
    configure_media_directions();

    // create answer
    webrtc::RtpTransceiverInit audio_transceiver_init;
    audio_transceiver_init.direction =
        recv_audioframe_handler_
            ? webrtc::RtpTransceiverDirection::kRecvOnly
            : webrtc::RtpTransceiverDirection::kInactive;
    webrtc::RtpTransceiverInit video_transceiver_init;
    video_transceiver_init.direction = recv_frame_handler_
                                           ? webrtc::RtpTransceiverDirection::kRecvOnly
                                           : webrtc::RtpTransceiverDirection::kInactive;
    for (int i = 0; i < 12; ++i) {
      rtc_conn->peer_conn_->AddTransceiver(cricket::MEDIA_TYPE_AUDIO,
                                           audio_transceiver_init);
      rtc_conn->peer_conn_->AddTransceiver(cricket::MEDIA_TYPE_VIDEO,
                                           video_transceiver_init);
    }

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    rtc_conn->peer_conn_->CreateAnswer(rtc_conn->create_sdp_observer_.get(),
                                       options);

    this->SetRtpSendersPriority();
  }

  mtx_.lock();
  LOG_WARN("Add PeerConnection <%u><%p> to manager map", remote_sessionid,
           rtc_conn->peer_conn_.get());
  remotesessionid_rtcconn_map_[remote_sessionid] = rtc_conn;
  mtx_.unlock();
}

void RtcConnectionManager::AckRemotePeerSdp(vts_rtc::SessionId remote_sessionid,
                                            const std::string& remote_sdp) {
  RTC_DCHECK_RUN_ON(logic_thread_);

  if (remotesessionid_rtcconn_map_.find(remote_sessionid) ==
      remotesessionid_rtcconn_map_.cend()) {
    LOG_ERROR(
        "Ack remote peer SDP, rtc connection of remote_sessionid: "
        "%u do not exist",
        remote_sessionid);
    return;
  }

  auto rtc_conn = remotesessionid_rtcconn_map_[remote_sessionid];
  webrtc::SdpParseError error;
  auto remote_session_description = webrtc::CreateSessionDescription(
      webrtc::SdpType::kAnswer, remote_sdp, &error);
  if (!remote_session_description) {
    LOG_ERROR(
        "Ack remote peer SDP, create SDP failed, line: %s, description: %s",
        error.line.c_str(), error.description.c_str());
    // remotesessionid_rtcconn_map_.erase(remote_sessionid);
    DestroyPeerConnection(remote_sessionid);
    return;
  }
  if (rtc_conn && rtc_conn->peer_conn_) {
    rtc_conn->peer_conn_->SetRemoteDescription(
        std::move(remote_session_description),
        rtc_conn->set_remote_sdp_observer_);
  }

  this->SetRtpSendersPriority();
}
