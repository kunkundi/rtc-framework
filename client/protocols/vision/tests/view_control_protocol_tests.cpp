#include "rtc_vision/view_control_protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using vts_rtc::vision::DecodeViewControl;
using vts_rtc::vision::EncodeViewControl;
using vts_rtc::vision::ViewControlDecodeStatus;
using vts_rtc::vision::ViewMode;

void Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

void TestRoundTripAndGolden() {
  const auto encoded =
      EncodeViewControl(0x0102030405060708ULL, ViewMode::Surround);
  Check(static_cast<bool>(encoded), "encode surround view control");

  // 该消息在 v1 协议中的固定小端字节。
  const std::vector<uint8_t> golden = {
      0x56, 0x56, 0x43, 0x31, 0x01, 0x00, 0x00, 0x00, 0x08, 0x07,
      0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00,
  };
  Check(encoded.payload == golden, "view control golden vector");

  const auto decoded = DecodeViewControl(encoded.payload);
  Check(static_cast<bool>(decoded), "decode surround view control");
  Check(decoded.envelope.seq == 0x0102030405060708ULL,
        "decode view control sequence");
  Check(decoded.envelope.command.enable_view == ViewMode::Surround,
        "decode surround view mode");

  const auto binocular = EncodeViewControl(9, ViewMode::Binocular);
  Check(static_cast<bool>(binocular), "encode binocular view control");
  const auto decoded_binocular = DecodeViewControl(binocular.payload);
  Check(static_cast<bool>(decoded_binocular),
        "decode binocular view control");
  Check(decoded_binocular.envelope.command.enable_view == ViewMode::Binocular,
        "decode binocular view mode");
}

void TestEncodeRejectsUnknownMode() {
  Check(!EncodeViewControl(1, ViewMode::Unknown),
        "reject unknown view mode");
  Check(!EncodeViewControl(1, static_cast<ViewMode>(99)),
        "reject invalid view mode");
}

void TestRejectedPayloads() {
  const auto encoded = EncodeViewControl(7, ViewMode::Binocular);
  Check(static_cast<bool>(encoded), "prepare valid payload");

  Check(DecodeViewControl(nullptr, 0).status ==
            ViewControlDecodeStatus::EmptyPayload,
        "reject empty payload");

  std::vector<uint8_t> truncated = encoded.payload;
  truncated.pop_back();
  Check(DecodeViewControl(truncated).status ==
            ViewControlDecodeStatus::InvalidSize,
        "reject truncated payload");

  std::vector<uint8_t> oversized = encoded.payload;
  oversized.push_back(0);
  Check(DecodeViewControl(oversized).status ==
            ViewControlDecodeStatus::InvalidSize,
        "reject oversized payload");

  std::vector<uint8_t> bad_magic = encoded.payload;
  bad_magic[0] ^= 0xff;
  Check(DecodeViewControl(bad_magic).status ==
            ViewControlDecodeStatus::InvalidEnvelope,
        "reject invalid magic");

  std::vector<uint8_t> unsupported_major = encoded.payload;
  unsupported_major[4] = 2;
  unsupported_major[5] = 0;
  Check(DecodeViewControl(unsupported_major).status ==
            ViewControlDecodeStatus::UnsupportedProtocol,
        "reject unsupported protocol major");

  std::vector<uint8_t> unknown_mode = encoded.payload;
  unknown_mode[16] = 99;
  unknown_mode[17] = 0;
  unknown_mode[18] = 0;
  unknown_mode[19] = 0;
  Check(DecodeViewControl(unknown_mode).status ==
            ViewControlDecodeStatus::InvalidField,
        "reject unknown decoded view mode");
}

}  // 匿名命名空间

int main() {
  TestRoundTripAndGolden();
  TestEncodeRejectsUnknownMode();
  TestRejectedPayloads();
  std::cout << "rtc vision view control protocol tests passed" << std::endl;
  return 0;
}
