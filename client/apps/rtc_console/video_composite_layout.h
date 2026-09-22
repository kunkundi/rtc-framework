#pragma once

#include <vector>

namespace rtc_console {

enum class VideoTileId {
  Stereo,
  SurroundFront,
  SurroundRear,
  SurroundLeft,
  SurroundRight,
};

struct VideoRect {
  float x = 0.0f;
  float y = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

struct VideoTilePlacement {
  VideoTileId tile = VideoTileId::Stereo;
  VideoRect rect;
  bool picture_in_picture = false;
};

struct VideoCompositeLayout {
  std::vector<VideoTilePlacement> placements;
  VideoRect picture_in_picture_bounds;
};

VideoCompositeLayout ComputeVideoCompositeLayout(bool binocular_mode,
                                                 float canvas_width,
                                                 float canvas_height,
                                                 float stereo_aspect);

}  // 命名空间 rtc_console
