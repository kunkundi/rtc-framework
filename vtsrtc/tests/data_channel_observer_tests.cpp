#include "data_channel_observer.h"

#include <rtc_base/ref_counted_object.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

class BufferedDataChannel : public webrtc::DataChannelInterface {
 public:
  void RegisterObserver(webrtc::DataChannelObserver* observer) override {
    observer_ = observer;
    observer_->OnStateChange();
    observer_->OnMessage(webrtc::DataBuffer(std::string("car_1")));
  }
  void UnregisterObserver() override { observer_ = nullptr; }
  void Deliver(const std::string& payload) {
    if (observer_) observer_->OnMessage(webrtc::DataBuffer(payload));
  }
  std::string label() const override { return "device.identity.v1"; }
  bool reliable() const override { return true; }
  int id() const override { return 1; }
  DataState state() const override { return kOpen; }
  uint32_t messages_sent() const override { return 0; }
  uint64_t bytes_sent() const override { return 0; }
  uint32_t messages_received() const override { return 0; }
  uint64_t bytes_received() const override { return 0; }
  uint64_t buffered_amount() const override { return 0; }
  void Close() override {}
  bool Send(const webrtc::DataBuffer&) override { return true; }

 private:
  webrtc::DataChannelObserver* observer_ = nullptr;
};

void TestQueuedIdentityAndBinaryPayload() {
  rtc::scoped_refptr<BufferedDataChannel> channel =
      new rtc::RefCountedObject<BufferedDataChannel>();
  int state_events = 0;
  int messages = 0;
  std::string received;
  auto observer = DataChannelObserver::Register(channel.get(),
      [&] { ++state_events; }, [&](const webrtc::DataBuffer& buffer) {
        ++messages;
        received.assign(buffer.data.data<char>(), buffer.data.size());
      });
  Check(state_events == 1, "注册期间的通道状态不能丢失");
  Check(messages == 1 && received == "car_1", "注册期间缓存的设备身份首包不能丢失");
  const std::string binary("\x01\0\x02\0\x03", 5);
  channel->Deliver(binary);
  Check(messages == 2 && received == binary, "后续二进制消息必须保留零字节和准确长度");

  observer->ResetCallbacks();
  channel->Deliver("late message");
  observer->OnStateChange();
  Check(messages == 2 && state_events == 1, "关闭后的迟到回调不能进入业务逻辑");
  channel->UnregisterObserver();
  observer.reset();
  channel->Deliver("after unregister");
}

void TestEmptyCallbacks() {
  rtc::scoped_refptr<BufferedDataChannel> channel =
      new rtc::RefCountedObject<BufferedDataChannel>();
  auto observer = DataChannelObserver::Register(channel.get(), {}, {});
  channel->Deliver("ignored");
  observer->ResetCallbacks();
  observer->ResetCallbacks();
  channel->UnregisterObserver();
}

}

int main() {
  TestQueuedIdentityAndBinaryPayload();
  TestEmptyCallbacks();
  std::cout << "data channel observer tests passed" << std::endl;
  return 0;
}
