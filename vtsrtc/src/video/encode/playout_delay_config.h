#pragma once

#include <utility>

#include "log/log_manager.h"
#include "rtc_types.h"

namespace webrtc {

struct ConfiguredPlayoutDelay {
  bool enabled = false;
  int min_ms = -1;
  int max_ms = -1;
};

inline ConfiguredPlayoutDelay ResolveConfiguredPlayoutDelay(
    const vts_rtc::EncodeParamsConfig& encode_params,
    const char* encoder_name) {
  ConfiguredPlayoutDelay resolved;
  int min_ms = encode_params.playout_delay_min_ms;
  int max_ms = encode_params.playout_delay_max_ms;
  const char* name = encoder_name ? encoder_name : "unknown";

  if (min_ms < 0 && max_ms < 0) {
    return resolved;
  }

  if (min_ms < 0) {
    min_ms = max_ms;
    LOG_WARN(
        "[WEBRTC] %s playout delay min is unset, reuse max=%d ms as both "
        "bounds",
        name, max_ms);
  }

  if (max_ms < 0) {
    max_ms = min_ms;
    LOG_WARN(
        "[WEBRTC] %s playout delay max is unset, reuse min=%d ms as both "
        "bounds",
        name, min_ms);
  }

  if (max_ms < min_ms) {
    std::swap(min_ms, max_ms);
    LOG_WARN(
        "[WEBRTC] %s playout delay config was reversed, normalized to [%d, "
        "%d] ms",
        name, min_ms, max_ms);
  }

  resolved.enabled = true;
  resolved.min_ms = min_ms;
  resolved.max_ms = max_ms;
  return resolved;
}

}  // namespace webrtc
