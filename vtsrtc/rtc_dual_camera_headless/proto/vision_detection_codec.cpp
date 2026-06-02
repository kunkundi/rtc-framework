#include "vision_detection_codec.h"

#include "generated/vision_detection.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <cstring>
#include <sstream>

namespace vts_rtc {
namespace vision {
namespace {

using PbCapability = vtsrtc_vision_v1_Capability;
using PbClassInfo = vtsrtc_vision_v1_ClassInfo;
using PbClassMap = vtsrtc_vision_v1_ClassMap;
using PbCoordType = vtsrtc_vision_v1_CoordType;
using PbDetection = vtsrtc_vision_v1_Detection;
using PbDetectionFrame = vtsrtc_vision_v1_DetectionFrame;
using PbEnvelope = vtsrtc_vision_v1_VisionEnvelope;
using PbMessageType = vtsrtc_vision_v1_VisionMessageType;

std::string MakeError(const char* field, const char* reason) {
  std::ostringstream oss;
  oss << field << ": " << reason;
  return oss.str();
}

template <typename T, size_t N>
constexpr size_t ArraySize(const T (&)[N]) {
  return N;
}

template <typename T>
void Reset(T* target) {
  *target = T{};
}

template <size_t N>
bool CopyString(const std::string& source,
                char (&target)[N],
                const char* field,
                std::string* error) {
  if (source.size() >= N) {
    std::ostringstream oss;
    oss << field << " exceeds " << (N - 1) << " bytes";
    *error = oss.str();
    return false;
  }

  std::memset(target, 0, N);
  std::memcpy(target, source.c_str(), source.size());
  return true;
}

template <size_t N>
std::string ReadString(bool has_value, const char (&source)[N]) {
  return has_value ? std::string(source) : std::string();
}

PbMessageType ToPbMessageType(MessageType type) {
  switch (type) {
    case MessageType::Capability:
      return vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_CAPABILITY;
    case MessageType::ClassMap:
      return vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_CLASS_MAP;
    case MessageType::DetectionFrame:
      return vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_DETECTION_FRAME;
    case MessageType::Unknown:
    default:
      return vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_UNKNOWN;
  }
}

MessageType FromPbMessageType(PbMessageType type) {
  switch (type) {
    case vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_CAPABILITY:
      return MessageType::Capability;
    case vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_CLASS_MAP:
      return MessageType::ClassMap;
    case vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_DETECTION_FRAME:
      return MessageType::DetectionFrame;
    case vtsrtc_vision_v1_VisionMessageType_VISION_MESSAGE_TYPE_UNKNOWN:
    default:
      return MessageType::Unknown;
  }
}

PbCoordType ToPbCoordType(CoordType type) {
  switch (type) {
    case CoordType::NormU16Xywh:
      return vtsrtc_vision_v1_CoordType_COORD_TYPE_NORM_U16_XYWH;
    case CoordType::NormU16Xyxy:
      return vtsrtc_vision_v1_CoordType_COORD_TYPE_NORM_U16_XYXY;
    case CoordType::PixelI32Xywh:
      return vtsrtc_vision_v1_CoordType_COORD_TYPE_PIXEL_I32_XYWH;
    case CoordType::Unknown:
    default:
      return vtsrtc_vision_v1_CoordType_COORD_TYPE_UNKNOWN;
  }
}

CoordType FromPbCoordType(PbCoordType type) {
  switch (type) {
    case vtsrtc_vision_v1_CoordType_COORD_TYPE_NORM_U16_XYWH:
      return CoordType::NormU16Xywh;
    case vtsrtc_vision_v1_CoordType_COORD_TYPE_NORM_U16_XYXY:
      return CoordType::NormU16Xyxy;
    case vtsrtc_vision_v1_CoordType_COORD_TYPE_PIXEL_I32_XYWH:
      return CoordType::PixelI32Xywh;
    case vtsrtc_vision_v1_CoordType_COORD_TYPE_UNKNOWN:
    default:
      return CoordType::Unknown;
  }
}

bool IsKnownMessageType(MessageType type) {
  return type == MessageType::Capability || type == MessageType::ClassMap ||
         type == MessageType::DetectionFrame;
}

bool IsKnownCoordType(CoordType type) {
  return type == CoordType::NormU16Xywh || type == CoordType::NormU16Xyxy ||
         type == CoordType::PixelI32Xywh;
}

bool UsesU16Coords(CoordType type) {
  return type == CoordType::NormU16Xywh || type == CoordType::NormU16Xyxy;
}

void FillEnvelopeHeader(PbEnvelope* envelope,
                        MessageType type,
                        uint32_t seq) {
  envelope->has_magic = true;
  envelope->magic = kVisionDetectionMagic;
  envelope->has_protocol_major = true;
  envelope->protocol_major = kVisionProtocolMajor;
  envelope->has_protocol_minor = true;
  envelope->protocol_minor = kVisionProtocolMinor;
  envelope->has_type = true;
  envelope->type = ToPbMessageType(type);
  envelope->has_seq = true;
  envelope->seq = seq;
}

bool FillCapability(const Capability& source,
                    PbCapability* target,
                    std::string* error) {
  (void)error;
  Reset(target);
  target->has_max_detections_per_frame = true;
  target->max_detections_per_frame = source.max_detections_per_frame;
  target->has_supported_coord_types = true;
  target->supported_coord_types = source.supported_coord_types;
  target->has_max_source_id_size = true;
  target->max_source_id_size = source.max_source_id_size;
  target->has_max_label_size = true;
  target->max_label_size = source.max_label_size;
  return true;
}

bool FillClassInfo(const ClassInfo& source,
                   PbClassInfo* target,
                   std::string* error) {
  Reset(target);
  target->has_class_id = true;
  target->class_id = source.class_id;
  target->has_label = true;
  if (!CopyString(source.label, target->label, "class.label", error)) {
    return false;
  }
  target->has_color_argb = source.has_color_argb;
  target->color_argb = source.color_argb;
  return true;
}

bool FillClassMap(const ClassMap& source,
                  PbClassMap* target,
                  std::string* error) {
  if (source.classes.size() > ArraySize(target->classes)) {
    *error = MakeError("class_map.classes", "too many classes");
    return false;
  }

  Reset(target);
  target->has_map_version = true;
  target->map_version = source.map_version;
  target->has_model_name = true;
  if (!CopyString(source.model_name, target->model_name, "class_map.model_name",
                  error)) {
    return false;
  }
  target->has_model_version = true;
  if (!CopyString(source.model_version, target->model_version,
                  "class_map.model_version", error)) {
    return false;
  }

  target->classes_count = static_cast<pb_size_t>(source.classes.size());
  for (size_t i = 0; i < source.classes.size(); ++i) {
    if (!FillClassInfo(source.classes[i], &target->classes[i], error)) {
      return false;
    }
  }
  return true;
}

bool FillDetection(const Detection& source,
                   CoordType coord_type,
                   PbDetection* target,
                   std::string* error) {
  if (source.confidence > 1000) {
    *error = MakeError("detection.confidence", "must be in range 0..1000");
    return false;
  }
  if (UsesU16Coords(coord_type) &&
      (source.x > 65535 || source.y > 65535 || source.w > 65535 ||
       source.h > 65535)) {
    *error = MakeError("detection.coords",
                       "normalized coordinates must be in range 0..65535");
    return false;
  }

  Reset(target);
  target->has_class_id = true;
  target->class_id = source.class_id;
  target->has_confidence = true;
  target->confidence = source.confidence;
  target->has_x = true;
  target->x = source.x;
  target->has_y = true;
  target->y = source.y;
  target->has_w = true;
  target->w = source.w;
  target->has_h = true;
  target->h = source.h;
  target->has_detection_id = source.has_detection_id;
  target->detection_id = source.detection_id;
  target->has_track_id = source.has_track_id;
  target->track_id = source.track_id;
  target->has_flags = true;
  target->flags = source.flags;
  return true;
}

bool FillDetectionFrame(const DetectionFrame& source,
                        PbDetectionFrame* target,
                        std::string* error) {
  if (!IsKnownCoordType(source.coord_type)) {
    *error = MakeError("detection_frame.coord_type", "unknown coordinate type");
    return false;
  }
  if (source.detections.size() > ArraySize(target->detections)) {
    *error = MakeError("detection_frame.detections", "too many detections");
    return false;
  }

  Reset(target);
  target->has_source_id = true;
  if (!CopyString(source.source_id, target->source_id,
                  "detection_frame.source_id", error)) {
    return false;
  }
  target->has_frame_id = true;
  target->frame_id = source.frame_id;
  target->has_capture_ts_ms = true;
  target->capture_ts_ms = source.capture_ts_ms;
  target->has_frame_width = true;
  target->frame_width = source.frame_width;
  target->has_frame_height = true;
  target->frame_height = source.frame_height;
  target->has_class_map_version = true;
  target->class_map_version = source.class_map_version;
  target->has_coord_type = true;
  target->coord_type = ToPbCoordType(source.coord_type);

  target->detections_count = static_cast<pb_size_t>(source.detections.size());
  for (size_t i = 0; i < source.detections.size(); ++i) {
    if (!FillDetection(source.detections[i], source.coord_type,
                       &target->detections[i], error)) {
      return false;
    }
  }
  return true;
}

EncodeResult EncodeEnvelope(const PbEnvelope& envelope) {
  EncodeResult result;
  result.payload.resize(vtsrtc_vision_v1_VisionEnvelope_size);

  pb_ostream_t stream =
      pb_ostream_from_buffer(result.payload.data(), result.payload.size());
  if (!pb_encode(&stream, vtsrtc_vision_v1_VisionEnvelope_fields, &envelope)) {
    result.payload.clear();
    result.error_message = PB_GET_ERROR(&stream);
    return result;
  }

  result.payload.resize(stream.bytes_written);
  return result;
}

DecodeResult DecodeError(DecodeStatus status, const std::string& error) {
  DecodeResult result;
  result.status = status;
  result.error_message = error;
  return result;
}

Capability FromPbCapability(const PbCapability& source) {
  Capability result;
  result.max_detections_per_frame =
      source.has_max_detections_per_frame ? source.max_detections_per_frame : 0;
  result.supported_coord_types =
      source.has_supported_coord_types ? source.supported_coord_types : 0;
  result.max_source_id_size =
      source.has_max_source_id_size ? source.max_source_id_size : 0;
  result.max_label_size = source.has_max_label_size ? source.max_label_size : 0;
  return result;
}

ClassInfo FromPbClassInfo(const PbClassInfo& source) {
  ClassInfo result;
  result.class_id = source.has_class_id ? source.class_id : 0;
  result.label = ReadString(source.has_label, source.label);
  result.has_color_argb = source.has_color_argb;
  result.color_argb = source.color_argb;
  return result;
}

ClassMap FromPbClassMap(const PbClassMap& source) {
  ClassMap result;
  result.map_version = source.has_map_version ? source.map_version : 0;
  result.model_name = ReadString(source.has_model_name, source.model_name);
  result.model_version =
      ReadString(source.has_model_version, source.model_version);
  result.classes.reserve(source.classes_count);
  for (pb_size_t i = 0; i < source.classes_count; ++i) {
    result.classes.push_back(FromPbClassInfo(source.classes[i]));
  }
  return result;
}

Detection FromPbDetection(const PbDetection& source) {
  Detection result;
  result.class_id = source.has_class_id ? source.class_id : 0;
  result.confidence = source.has_confidence ? source.confidence : 0;
  result.x = source.has_x ? source.x : 0;
  result.y = source.has_y ? source.y : 0;
  result.w = source.has_w ? source.w : 0;
  result.h = source.has_h ? source.h : 0;
  result.has_detection_id = source.has_detection_id;
  result.detection_id = source.detection_id;
  result.has_track_id = source.has_track_id;
  result.track_id = source.track_id;
  result.flags = source.has_flags ? source.flags : 0;
  return result;
}

DetectionFrame FromPbDetectionFrame(const PbDetectionFrame& source) {
  DetectionFrame result;
  result.source_id = ReadString(source.has_source_id, source.source_id);
  result.frame_id = source.has_frame_id ? source.frame_id : 0;
  result.capture_ts_ms = source.has_capture_ts_ms ? source.capture_ts_ms : 0;
  result.frame_width = source.has_frame_width ? source.frame_width : 0;
  result.frame_height = source.has_frame_height ? source.frame_height : 0;
  result.class_map_version =
      source.has_class_map_version ? source.class_map_version : 0;
  result.coord_type = source.has_coord_type ? FromPbCoordType(source.coord_type)
                                            : CoordType::NormU16Xywh;
  result.detections.reserve(source.detections_count);
  for (pb_size_t i = 0; i < source.detections_count; ++i) {
    result.detections.push_back(FromPbDetection(source.detections[i]));
  }
  return result;
}

}  // namespace

Capability DefaultCapability() {
  Capability capability;
  capability.max_detections_per_frame =
      static_cast<uint32_t>(kMaxDetectionsPerFrame);
  capability.supported_coord_types = CoordTypeMask(CoordType::NormU16Xywh) |
                                     CoordTypeMask(CoordType::NormU16Xyxy) |
                                     CoordTypeMask(CoordType::PixelI32Xywh);
  capability.max_source_id_size =
      static_cast<uint32_t>(kMaxBoundedStringLength);
  capability.max_label_size = static_cast<uint32_t>(kMaxBoundedStringLength);
  return capability;
}

EncodeResult EncodeCapabilityEnvelope(uint32_t seq,
                                      const Capability& capability) {
  PbEnvelope envelope = vtsrtc_vision_v1_VisionEnvelope_init_zero;
  FillEnvelopeHeader(&envelope, MessageType::Capability, seq);
  envelope.has_capability = true;

  std::string error;
  if (!FillCapability(capability, &envelope.capability, &error)) {
    return EncodeResult{{}, error};
  }

  return EncodeEnvelope(envelope);
}

EncodeResult EncodeClassMapEnvelope(uint32_t seq, const ClassMap& class_map) {
  PbEnvelope envelope = vtsrtc_vision_v1_VisionEnvelope_init_zero;
  FillEnvelopeHeader(&envelope, MessageType::ClassMap, seq);
  envelope.has_class_map = true;

  std::string error;
  if (!FillClassMap(class_map, &envelope.class_map, &error)) {
    return EncodeResult{{}, error};
  }

  return EncodeEnvelope(envelope);
}

EncodeResult EncodeDetectionFrameEnvelope(uint32_t seq,
                                          const DetectionFrame& frame) {
  PbEnvelope envelope = vtsrtc_vision_v1_VisionEnvelope_init_zero;
  FillEnvelopeHeader(&envelope, MessageType::DetectionFrame, seq);
  envelope.has_detection_frame = true;

  std::string error;
  if (!FillDetectionFrame(frame, &envelope.detection_frame, &error)) {
    return EncodeResult{{}, error};
  }

  return EncodeEnvelope(envelope);
}

DecodeResult DecodeEnvelope(const uint8_t* data, size_t size) {
  if (size == 0) {
    return DecodeError(DecodeStatus::EmptyPayload, "empty payload");
  }
  if (!data) {
    return DecodeError(DecodeStatus::EmptyPayload, "null payload");
  }

  PbEnvelope pb_envelope = vtsrtc_vision_v1_VisionEnvelope_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(data, size);
  if (!pb_decode(&stream, vtsrtc_vision_v1_VisionEnvelope_fields,
                 &pb_envelope)) {
    return DecodeError(DecodeStatus::DecodeFailed, PB_GET_ERROR(&stream));
  }

  if (!pb_envelope.has_magic || pb_envelope.magic != kVisionDetectionMagic) {
    return DecodeError(DecodeStatus::InvalidEnvelope,
                       "missing or invalid magic");
  }
  if (!pb_envelope.has_protocol_major) {
    return DecodeError(DecodeStatus::InvalidEnvelope,
                       "missing protocol_major");
  }
  if (pb_envelope.protocol_major != kVisionProtocolMajor) {
    return DecodeError(DecodeStatus::UnsupportedProtocol,
                       "unsupported protocol_major");
  }
  if (!pb_envelope.has_type) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "missing type");
  }

  const MessageType type = FromPbMessageType(pb_envelope.type);
  if (!IsKnownMessageType(type)) {
    return DecodeError(DecodeStatus::InvalidEnvelope, "unknown message type");
  }

  DecodeResult result;
  result.status = DecodeStatus::Ok;
  result.envelope.seq = pb_envelope.has_seq ? pb_envelope.seq : 0;
  result.envelope.type = type;

  switch (type) {
    case MessageType::Capability:
      if (!pb_envelope.has_capability) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "capability envelope has no capability payload");
      }
      result.envelope.capability = FromPbCapability(pb_envelope.capability);
      break;
    case MessageType::ClassMap:
      if (!pb_envelope.has_class_map) {
        return DecodeError(DecodeStatus::UnexpectedPayload,
                           "class_map envelope has no class_map payload");
      }
      result.envelope.class_map = FromPbClassMap(pb_envelope.class_map);
      break;
    case MessageType::DetectionFrame:
      if (!pb_envelope.has_detection_frame) {
        return DecodeError(
            DecodeStatus::UnexpectedPayload,
            "detection_frame envelope has no detection_frame payload");
      }
      result.envelope.detection_frame =
          FromPbDetectionFrame(pb_envelope.detection_frame);
      break;
    case MessageType::Unknown:
    default:
      return DecodeError(DecodeStatus::InvalidEnvelope, "unknown message type");
  }

  return result;
}

}  // namespace vision
}  // namespace vts_rtc
