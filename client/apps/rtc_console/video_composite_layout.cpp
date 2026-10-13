#include "video_composite_layout.h"

#include <algorithm>

namespace rtc_console {
namespace {

constexpr float kTileGap = 4.0f;
constexpr float kPictureInPictureInset = 8.0f;
constexpr float kPictureInPictureWidthRatio = 0.28f;
constexpr float kPictureInPictureHeightRatio = 0.30f;
constexpr float kPictureInPictureMaxWidth = 384.0f;
constexpr float kSurroundGridAspect = 16.0f / 9.0f;

VideoTilePlacement MakePlacement(VideoTileId tile,
                                 const VideoRect& rect,
                                 bool picture_in_picture) {
  VideoTilePlacement placement;
  placement.tile = tile;
  placement.rect = rect;
  placement.picture_in_picture = picture_in_picture;
  return placement;
}

VideoRect FitBottomRight(float canvas_width,
                         float canvas_height,
                         float aspect) {
  const float inset_x = std::min(
      kPictureInPictureInset, std::max(0.0f, (canvas_width - 1.0f) * 0.5f));
  const float inset_y = std::min(
      kPictureInPictureInset, std::max(0.0f, (canvas_height - 1.0f) * 0.5f));
  const float max_width =
      std::max(1.0f, std::min(canvas_width - inset_x,
                  std::min(kPictureInPictureMaxWidth,
                       canvas_width * kPictureInPictureWidthRatio)));
  const float max_height =
      std::max(1.0f, std::min(canvas_height - inset_y,
                              canvas_height * kPictureInPictureHeightRatio));
  const float safe_aspect = aspect > 0.0f ? aspect : 1.0f;

  float width = max_width;
  float height = width / safe_aspect;
  if (height > max_height) {
    height = max_height;
    width = height * safe_aspect;
  }

  VideoRect rect;
  rect.x = std::max(0.0f, canvas_width - inset_x - width);
  rect.y = std::max(0.0f, canvas_height - inset_y - height);
  rect.width = std::max(1.0f, width);
  rect.height = std::max(1.0f, height);
  return rect;
}

void AppendGrid(const VideoRect& bounds,
                bool picture_in_picture,
                std::vector<VideoTilePlacement>* placements) {
  const float gap_x = std::min(
      kTileGap, std::max(0.0f, (bounds.width - 1.0f) * 0.5f));
  const float gap_y = std::min(
      kTileGap, std::max(0.0f, (bounds.height - 1.0f) * 0.5f));
  const float cell_width = std::max(0.5f, (bounds.width - gap_x) * 0.5f);
  const float cell_height = std::max(0.5f, (bounds.height - gap_y) * 0.5f);

  VideoRect front{bounds.x, bounds.y, cell_width, cell_height};
  VideoRect rear{bounds.x + cell_width + gap_x, bounds.y, cell_width,
                 cell_height};
  VideoRect left{bounds.x, bounds.y + cell_height + gap_y, cell_width,
                 cell_height};
  VideoRect right{bounds.x + cell_width + gap_x,
                  bounds.y + cell_height + gap_y, cell_width, cell_height};
  placements->push_back(MakePlacement(VideoTileId::SurroundFront, front,
                                      picture_in_picture));
  placements->push_back(MakePlacement(VideoTileId::SurroundRear, rear,
                                      picture_in_picture));
  placements->push_back(MakePlacement(VideoTileId::SurroundLeft, left,
                                      picture_in_picture));
  placements->push_back(MakePlacement(VideoTileId::SurroundRight, right,
                                      picture_in_picture));
}

}  // 匿名命名空间

VideoCompositeLayout ComputeVideoCompositeLayout(bool binocular_mode,
                                                 float canvas_width,
                                                 float canvas_height,
                                                 float stereo_aspect) {
  const float width = std::max(1.0f, canvas_width);
  const float height = std::max(1.0f, canvas_height);
  VideoCompositeLayout layout;

  if (binocular_mode) {
    layout.placements.push_back(MakePlacement(
        VideoTileId::Stereo, VideoRect{0.0f, 0.0f, width, height}, false));
    layout.picture_in_picture_bounds =
        FitBottomRight(width, height, kSurroundGridAspect);
    AppendGrid(layout.picture_in_picture_bounds, true, &layout.placements);
    return layout;
  }

  AppendGrid(VideoRect{0.0f, 0.0f, width, height}, false,
             &layout.placements);
  (void)stereo_aspect;
  layout.picture_in_picture_bounds = VideoRect{0, 0, 0, 0};
  return layout;
}

}  // 命名空间 rtc_console
