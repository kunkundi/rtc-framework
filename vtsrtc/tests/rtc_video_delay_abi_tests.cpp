#include "c_rtc.h"

#include <cstddef>
#include <type_traits>

struct LegacyVideoNetStats {
  const char* sourceid;
  unsigned int width;
  unsigned int height;
  unsigned long int bitrate_bps;
  unsigned int fps;
  unsigned int loss_rate;
  unsigned int delay_ms;
  unsigned int key_frame_count;
  unsigned int fir_count;
  unsigned int pli_count;
  unsigned int nack_count;
  const char* codec_name;
};

struct LegacyNetStats {
  bool input;
  RtcAudioNetStats audio_stats;
  LegacyVideoNetStats video_stats;
};

static_assert(sizeof(RtcVideoNetStats) == sizeof(LegacyVideoNetStats), "ABI size");
static_assert(alignof(RtcVideoNetStats) == alignof(LegacyVideoNetStats), "ABI alignment");
static_assert(offsetof(RtcVideoNetStats, delay_ms) ==
                  offsetof(LegacyVideoNetStats, delay_ms), "ABI delay offset");
static_assert(offsetof(RtcVideoNetStats, codec_name) ==
                  offsetof(LegacyVideoNetStats, codec_name), "ABI codec offset");
static_assert(sizeof(RtcNetStats) == sizeof(LegacyNetStats), "ABI callback size");
static_assert(offsetof(RtcNetStats, video_stats) ==
                  offsetof(LegacyNetStats, video_stats), "ABI callback offset");
static_assert(std::is_standard_layout<RtcVideoDelayStatsV1>::value, "C layout");

void ReceiveDelay(RtcSessionId, RtcVideoSourceId, const RtcVideoDelayStatsV1*) {}

int main() {
  RtcSetVideoDelayStatsHandlerV1(&ReceiveDelay);
  RtcSetVideoDelayStatsHandlerV1(nullptr);
  RtcDestoryAgent();
}