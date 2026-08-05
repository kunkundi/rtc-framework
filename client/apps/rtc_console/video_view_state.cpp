#include "video_view_state.h"

namespace rtc_console {

vts_rtc::vision::ViewMode VideoViewState::Get(uint32_t sessionid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = modes_by_session_.find(sessionid);
  if (it == modes_by_session_.end()) {
    return vts_rtc::vision::ViewMode::Binocular;
  }
  return it->second;
}

void VideoViewState::Set(uint32_t sessionid,
                         vts_rtc::vision::ViewMode mode) {
  if (sessionid == 0 ||
      (mode != vts_rtc::vision::ViewMode::Binocular &&
       mode != vts_rtc::vision::ViewMode::Surround)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  modes_by_session_[sessionid] = mode;
}

void VideoViewState::Erase(uint32_t sessionid) {
  std::lock_guard<std::mutex> lock(mutex_);
  modes_by_session_.erase(sessionid);
}

void VideoViewState::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  modes_by_session_.clear();
}

}  // 命名空间 rtc_console
