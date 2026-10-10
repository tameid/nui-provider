// Wire format tests.
//
// These are the contract tests for a frozen format. They deliberately pin the
// on-disk byte offsets and sizes: if one of these changes, a guest plugin built
// against the old header is now silently wrong, and that failure would show up
// as garbage poses rather than a build error.

#include <cstring>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nui/decode.h"
#include "nui/encode.h"
#include "nui/frame.h"

namespace {

constexpr uint16_t kMinimalFlags = nui::kFrameFlagSynthetic;

}  // namespace

TEST_CASE("NuiFrameHeader is 44 bytes", "[format]") {
  REQUIRE(sizeof(nui::NuiFrameHeader) == 44);
}

TEST_CASE("NuiJoint is 36 bytes", "[format]") {
  REQUIRE(sizeof(nui::NuiJoint) == 36);
}

TEST_CASE("NuiSkeleton covers all 20 joints", "[format]") {
  REQUIRE(nui::kJointCount == 20);
  REQUIRE(sizeof(nui::NuiSkeleton) == 82 + 36 * nui::kJointCount);
}

TEST_CASE("magic is XNUI little-endian on disk", "[format]") {
  nui::NuiFrameHeader header = nui::MakeHeader(1, 2, 3);
  uint8_t bytes[sizeof(header)];
  std::memcpy(bytes, &header, sizeof(header));
  REQUIRE(bytes[0] == 'X');
  REQUIRE(bytes[1] == 'N');
  REQUIRE(bytes[2] == 'U');
  REQUIRE(bytes[3] == 'I');
}

TEST_CASE("joint enum covers every joint with no gaps", "[format]") {
  for (uint16_t i = 0; i < nui::kJointCount; ++i) {
    REQUIRE(static_cast<uint16_t>(nui::Joint::kHead) + i == i);
  }
}

TEST_CASE("round trip preserves a frame", "[format][codec]") {
  nui::NuiFrameHeader header = nui::MakeHeader(42, 1000, 2000);
  header.flags = kMinimalFlags;

  std::vector<nui::NuiSkeleton> skeletons(2);
  for (size_t i = 0; i < skeletons.size(); ++i) {
    skeletons[i].tracking_id = static_cast<uint32_t>(i) + 7;
    skeletons[i].position[1] = 0.9f + static_cast<float>(i);
    skeletons[i].joints[0].position[0] = 1.5f;
    skeletons[i].joints[19].pos_confidence = 0.25f;
  }

  std::vector<uint8_t> buffer(nui::EncodedSize(2));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 2, kMinimalFlags,
                       buffer.data(), static_cast<uint32_t>(buffer.size()));
  REQUIRE(written == nui::EncodedSize(2));

  const nui::DecodedFrame decoded = nui::DecodeFrame(buffer.data(), written);
  REQUIRE(decoded.valid);
  REQUIRE(decoded.header.frame_id == 42);
  REQUIRE(decoded.header.capture_timestamp_us == 1000);
  REQUIRE(decoded.header.host_timestamp_us == 2000);
  REQUIRE(decoded.skeleton_count == 2);
  REQUIRE(decoded.skeletons[0].tracking_id == 7);
  REQUIRE(decoded.skeletons[1].position[1] == Catch::Approx(1.9f));
  REQUIRE(decoded.skeletons[0].joints[19].pos_confidence ==
          Catch::Approx(0.25f));
}

TEST_CASE("encode stamps magic, version and count", "[format][codec]") {
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  // Corrupt them; the encoder must not trust the caller.
  header.magic = 0;
  header.version = 99;
  header.num_skeletons = 77;

  std::vector<nui::NuiSkeleton> skeletons(1);
  std::vector<uint8_t> buffer(nui::EncodedSize(1));
  const uint32_t written = nui::EncodeFrame(header, skeletons.data(), 1,
                                            kMinimalFlags, buffer.data(),
                                            static_cast<uint32_t>(buffer.size()));
  const nui::DecodedFrame decoded = nui::DecodeFrame(buffer.data(), written);
  REQUIRE(decoded.valid);
  REQUIRE(decoded.header.magic == nui::kFrameMagic);
  REQUIRE(decoded.header.version == nui::kVersion1);
  REQUIRE(decoded.skeleton_count == 1);
}

TEST_CASE("encode refuses to overflow the buffer", "[format][codec]") {
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(4);
  std::vector<uint8_t> too_small(nui::EncodedSize(4) - 1);
  REQUIRE(nui::EncodeFrame(header, skeletons.data(), 4, 0, too_small.data(),
                           static_cast<uint32_t>(too_small.size())) == 0);
}

TEST_CASE("decode rejects a truncated frame", "[format][codec]") {
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(3);
  std::vector<uint8_t> buffer(nui::EncodedSize(3));
  const uint32_t written = nui::EncodeFrame(header, skeletons.data(), 3, 0,
                                            buffer.data(),
                                            static_cast<uint32_t>(buffer.size()));
  REQUIRE(written > 0);

  // A short read must read as invalid rather than as a short frame: this is the
  // case a torn file write produces.
  REQUIRE_FALSE(nui::DecodeFrame(buffer.data(), written - 1).valid);
  REQUIRE_FALSE(nui::DecodeFrame(buffer.data(), 0).valid);
  REQUIRE_FALSE(nui::DecodeFrame(nullptr, written).valid);
}

TEST_CASE("decode rejects a bad magic or version", "[format][codec]") {
  std::vector<nui::NuiSkeleton> skeletons(1);
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<uint8_t> buffer(nui::EncodedSize(1));
  const uint32_t written = nui::EncodeFrame(header, skeletons.data(), 1, 0,
                                            buffer.data(),
                                            static_cast<uint32_t>(buffer.size()));

  std::vector<uint8_t> corrupt = buffer;
  corrupt[0] = 'Z';
  REQUIRE_FALSE(nui::DecodeFrame(corrupt.data(), written).valid);

  corrupt = buffer;
  corrupt[4] = 0xFF;  // version low byte
  REQUIRE_FALSE(nui::DecodeFrame(corrupt.data(), written).valid);
}

TEST_CASE("a lying skeleton count cannot over-read", "[format][codec]") {
  // Hand-build a header claiming a huge count with only a small payload behind
  // it. The decoder must refuse rather than walk off the end.
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(1);
  std::vector<uint8_t> buffer(nui::EncodedSize(1));
  nui::EncodeFrame(header, skeletons.data(), 1, 0, buffer.data(),
                   static_cast<uint32_t>(buffer.size()));

  nui::NuiFrameHeader lying = header;
  lying.num_skeletons = 1000;
  std::memcpy(buffer.data(), &lying, sizeof(lying));

  REQUIRE_FALSE(nui::DecodeFrame(buffer.data(), static_cast<uint32_t>(buffer.size()))
                    .valid);
}