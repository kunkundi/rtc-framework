#pragma once

#include "rtc_vision/view_control_protocol.h"

#include <stdint.h>

#include <map>
#include <mutex>

namespace rtc_console {

class VideoViewState {
 public:
  vts_rtc::vision::ViewMode Get(uint32_t sessionid) const;
  void Set(uint32_t sessionid, vts_rtc::vision::ViewMode mode);
  void Erase(uint32_t sessionid);
  void Clear();

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, vts_rtc::vision::ViewMode> modes_by_session_;
};

}  // 命名空间 rtc_console
