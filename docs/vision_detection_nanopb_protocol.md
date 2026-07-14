# YOLO Detection DataChannel Protocol

## Transport

Use a dedicated RTC DataChannel label:

```cpp
constexpr const char* kVisionDetectionChannelLabel = "vision.detect.v1";
```

Detection metadata is encoded with nanopb from `protocol/vision/schema/vision_detection.proto`
and sent through `RtcBroadcastData`. The payload is binary, so pass the encoded
buffer and its exact byte size.

The current C API preserves embedded zero bytes through the `msg_size` argument.
For browser/Web receivers, add a dedicated binary send API later so the underlying
WebRTC `DataBuffer` is marked as binary.

Recommended DataChannel settings for real-time detection overlays:

```cpp
RtcAddDataChannel(kVisionDetectionChannelLabel, RtcPriorityType::Medium, false, 0);
```

Use unordered and low retransmit count because old detection boxes are less useful
than the newest frame result.

## Message Types

All payloads are wrapped in `VisionEnvelope`.

Every sender must explicitly set:

```text
magic = 827606102  // ASCII "VDT1" as little-endian uint32: 0x31544456
protocol_major = 1
protocol_minor = 0
type = one of VisionMessageType
seq = monotonically increasing sequence number
```

`VISION_MESSAGE_TYPE_CAPABILITY`:
Sent when the channel opens. It announces receiver/sender limits.

`VISION_MESSAGE_TYPE_CLASS_MAP`:
Sent when the YOLO model is loaded or the class table changes. Detection frames
only carry `class_id`; receivers use the latest matching `map_version` to render
labels.

`VISION_MESSAGE_TYPE_DETECTION_FRAME`:
Sent for each frame that has YOLO output. It contains frame metadata and a bounded
list of detections.

## Coordinates

The default coordinate format is `COORD_TYPE_NORM_U16_XYWH`.

Each detection stores `x`, `y`, `w`, `h` as integers in the range `0..65535`,
normalized against the original frame size before encoding.

```text
pixel_x = x / 65535.0 * displayed_video_width
pixel_y = y / 65535.0 * displayed_video_height
pixel_w = w / 65535.0 * displayed_video_width
pixel_h = h / 65535.0 * displayed_video_height
```

If the receiver letterboxes or pillarboxes the video, map boxes into the actual
video content rectangle, not the whole widget.

## Synchronization

For exact overlays, the video frame and detection message should share the same
`source_id` and `frame_id`.

If the receiving video API does not expose `frame_id`, use `capture_ts_ms` and
drop stale detection data. A practical first threshold is `200..500 ms`.

## Compatibility Rules

Keep `protocol_major = 1` for compatible changes.

For v1.x evolution:

- Add new fields only as `optional` or `repeated`.
- Never reuse field numbers.
- Reserve field numbers/names when a field is removed.
- Keep bounded nanopb fields updated in `protocol/vision/schema/vision_detection.options`.
- Receivers must ignore unknown fields and unknown optional submessages.

Increment `protocol_major` only for incompatible changes, such as changing the
meaning of an existing field.

## Generation

Prefer importing nanopb through xmake:

```lua
add_requires("nanopb 0.4.9", {configs = {generator = true}})

target("your_target")
    add_packages("nanopb")
```

The `generator = true` config enables the nanopb code generator package
environment. Runtime-only users can omit that config.

Generate C sources through the repository wrapper. It locates nanopb 0.4.9 from
the xmake package and keeps output deterministic across platforms:

```bash
python tools/generate_nanopb.py --protocol vision
python tools/generate_nanopb.py --protocol vision --check
```

Expected generated files:

```text
protocol/vision/generated/vision_detection.pb.h
protocol/vision/generated/vision_detection.pb.c
```

Add the generated `.pb.c` file and generated include directory to the target:

```lua
target("rtc_vision_detection_protocol")
    add_packages("nanopb")
    add_includedirs(
        "protocol/vision/include",
        "protocol/vision/generated"
    )
    add_files(
        "protocol/vision/generated/vision_detection.pb.c",
        "protocol/vision/src/vision_detection_codec.cpp"
    )
```

## Sender Flow

1. Wait for `DataChannelOpen`.
2. Send `Capability`.
3. Send `ClassMap`.
4. For every YOLO result frame, send `DetectionFrame`.
5. Increment `VisionEnvelope.seq` for every envelope sent.

## Receiver Flow

1. Decode `VisionEnvelope`.
2. Validate that `has_magic`, `has_protocol_major`, and `has_type` are true.
3. Validate `magic == 827606102`, `protocol_major == 1`, and a known `type`.
4. Store `ClassMap` by `map_version`.
5. Store latest `DetectionFrame` by `source_id`.
6. Render boxes only when the frame is fresh or matches the current video frame.
