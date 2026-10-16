#pragma once

#include <api/data_channel_interface.h>

#include <functional>
#include <memory>
#include <utility>

class DataChannelObserver : public webrtc::DataChannelObserver {
 public:
  using StateCallback = std::function<void()>;
  using MessageCallback = std::function<void(const webrtc::DataBuffer&)>;

  DataChannelObserver(StateCallback state_callback, MessageCallback message_callback)
      : on_statechange_(std::move(state_callback)),
        on_message_(std::move(message_callback)) {}

  static std::shared_ptr<DataChannelObserver> Register(
      webrtc::DataChannelInterface* channel, StateCallback state_callback,
      MessageCallback message_callback) {
    auto observer = std::make_shared<DataChannelObserver>(
        std::move(state_callback), std::move(message_callback));
    // 注册时可能同步交付已缓存的首包，必须先准备好回调。
    channel->RegisterObserver(observer.get());
    return observer;
  }

  void ResetCallbacks() {
    on_statechange_ = nullptr;
    on_message_ = nullptr;
  }

  void OnStateChange() override {
    if (on_statechange_) on_statechange_();
  }

  void OnMessage(const webrtc::DataBuffer& buffer) override {
    if (on_message_) on_message_(buffer);
  }

  void OnBufferedAmountChange(uint64_t) override {}

 private:
  StateCallback on_statechange_;
  MessageCallback on_message_;
};
