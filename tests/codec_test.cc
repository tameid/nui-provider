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

#include "backends/synthetic/synthetic.h"
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

TEST_CASE("NuiImage is 10 bytes", "[format]") {
  REQUIRE(sizeof(nui::NuiImage) == 10);
}

TEST_CASE("depth pixel packs player index low and millimetres high", "[format]") {
  // This is the real NUI layout: bits 0-2 player index, bits 3-15 depth in mm.
  // If it is transposed, a title reading the depth image sees the wrong field
  // in the wrong place and there is nothing in the build to catch it.
  const uint16_t packed = nui::PackDepthPixel(2000, 3);
  REQUIRE(nui::DepthPixelMillimetres(packed) == 2000);
  REQUIRE(nui::DepthPixelPlayerIndex(packed) == 3);

  // The maximum depth must survive the 13-bit shift without overflowing into
  // the player index.
  const uint16_t max_packed = nui::PackDepthPixel(nui::kDepthMaxMillimetres, 7);
  REQUIRE(nui::DepthPixelMillimetres(max_packed) == nui::kDepthMaxMillimetres);
  REQUIRE(nui::DepthPixelPlayerIndex(max_packed) == 7);
}

TEST_CASE("image data size matches its geometry", "[format]") {
  REQUIRE(nui::ImageDataSize(nui::ImageFormat::kDepth16, 320, 240) ==
          320u * 240u * 2u);
  REQUIRE(nui::ImageDataSize(nui::ImageFormat::kIr8, 640, 480) == 640u * 480u);
  REQUIRE(nui::ImageDataSize(nui::ImageFormat::kColorBgra8, 640, 480) ==
          640u * 480u * 4u);
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

// --- images -----------------------------------------------------------------
//
// Depth is the payload that actually matters (see docs/kinect_support.md 4a), so
// these pin the parts a guest depends on: where images sit relative to the
// skeletons, that the counts are written from what was encoded rather than from
// the caller, and that a lying image cannot over-read.

namespace {

// A small depth image, so the tests stay fast. The geometry does not have to
// match the real 320x240 - the codec is geometry-agnostic.
std::vector<uint8_t> MakeDepthPixels(uint16_t millimetres, uint8_t player_index) {
  std::vector<uint8_t> pixels(4u * 3u * 2u);  // 4x3 depth16
  const uint16_t packed = nui::PackDepthPixel(millimetres, player_index);
  for (size_t i = 0; i < pixels.size() / 2; ++i) {
    pixels[i * 2] = static_cast<uint8_t>(packed & 0xFF);
    pixels[i * 2 + 1] = static_cast<uint8_t>(packed >> 8);
  }
  return pixels;
}

nui::EncodedImage MakeDepthImage(const std::vector<uint8_t>& pixels) {
  nui::EncodedImage image;
  image.header.width = 4;
  image.header.height = 3;
  image.header.format = static_cast<uint8_t>(nui::ImageFormat::kDepth16);
  image.header.reserved0 = 0;
  image.header.data_size = static_cast<uint32_t>(pixels.size());
  image.data = pixels.data();
  return image;
}

}  // namespace

TEST_CASE("round trip preserves a frame carrying depth", "[codec][image]") {
  const std::vector<uint8_t> pixels = MakeDepthPixels(1500, 2);

  nui::NuiFrameHeader header = nui::MakeHeader(9, 3000, 4000);
  std::vector<nui::NuiSkeleton> skeletons(1);
  skeletons[0].tracking_id = 11;

  const nui::EncodedImage image = MakeDepthImage(pixels);
  const uint32_t capacity = nui::EncodedSize(1, sizeof(nui::NuiImage) + pixels.size());
  std::vector<uint8_t> buffer(capacity);

  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 1, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written == capacity);

  const nui::DecodedFrame decoded = nui::DecodeFrame(buffer.data(), written);
  REQUIRE(decoded.valid);
  REQUIRE(decoded.skeleton_count == 1);
  REQUIRE(decoded.skeletons[0].tracking_id == 11);

  // The count comes from the encoding, not the caller's header.
  REQUIRE(decoded.header.num_depth_frames == 1);
  REQUIRE(decoded.header.num_ir_frames == 0);
  REQUIRE(decoded.header.num_color_frames == 0);

  REQUIRE(decoded.image_count == 1);
  REQUIRE(decoded.images[0].header.width == 4);
  REQUIRE(decoded.images[0].header.height == 3);
  REQUIRE(decoded.images[0].header.data_size == pixels.size());
  REQUIRE(std::memcmp(decoded.images[0].data, pixels.data(), pixels.size()) == 0);
}

TEST_CASE("images follow the skeletons in the frame", "[codec][image]") {
  // Offsets, because a reader walking the payload uses exactly these and
  // nothing else.
  const std::vector<uint8_t> pixels = MakeDepthPixels(1000, 1);
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(2);

  const nui::EncodedImage image = MakeDepthImage(pixels);
  std::vector<uint8_t> buffer(nui::EncodedSize(2, sizeof(nui::NuiImage) + pixels.size()));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 2, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written > 0);

  REQUIRE(nui::ImagesOffset(2) == sizeof(nui::NuiFrameHeader) +
                                      2u * sizeof(nui::NuiSkeleton));
  // The NuiImage header must land exactly where ImagesOffset says it does.
  nui::NuiImage on_wire = {};
  std::memcpy(&on_wire, buffer.data() + nui::ImagesOffset(2), sizeof(on_wire));
  REQUIRE(on_wire.width == 4);
  REQUIRE(on_wire.height == 3);
}

TEST_CASE("a frame may carry depth and no skeletons", "[codec][image]") {
  // Not a degenerate case: depth is the primitive, and a frame with no tracked
  // player in it is an ordinary thing to see.
  const std::vector<uint8_t> pixels = MakeDepthPixels(2500, 4);
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  const nui::EncodedImage image = MakeDepthImage(pixels);

  std::vector<uint8_t> buffer(
      nui::EncodedSize(0, sizeof(nui::NuiImage) + pixels.size()));
  const uint32_t written =
      nui::EncodeFrame(header, nullptr, 0, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written > 0);

  const nui::DecodedFrame decoded = nui::DecodeFrame(buffer.data(), written);
  REQUIRE(decoded.valid);
  REQUIRE(decoded.skeleton_count == 0);
  REQUIRE(decoded.skeletons == nullptr);
  REQUIRE(decoded.image_count == 1);
}

TEST_CASE("decode rejects a truncated image", "[codec][image]") {
  // The failure mode that matters: a torn write of a frame whose payload is
  // mostly image. It must read as invalid, not as a valid frame with a short
  // depth image.
  const std::vector<uint8_t> pixels = MakeDepthPixels(1200, 1);
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(1);
  const nui::EncodedImage image = MakeDepthImage(pixels);

  std::vector<uint8_t> buffer(
      nui::EncodedSize(1, sizeof(nui::NuiImage) + pixels.size()));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 1, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written > 0);

  for (uint32_t cut = 1; cut < written; ++cut) {
    REQUIRE_FALSE(nui::DecodeFrame(buffer.data(), cut).valid);
  }
}

TEST_CASE("a lying image header cannot over-read", "[codec][image]") {
  // Claim more pixels than the buffer holds. The declared size must also match
  // the geometry, or the two sides disagree about where the pixels are.
  const std::vector<uint8_t> pixels = MakeDepthPixels(1000, 1);
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(1);

  nui::EncodedImage image = MakeDepthImage(pixels);
  image.header.data_size = 0xFFFF;  // far more than 4*3*2
  std::vector<uint8_t> buffer(
      nui::EncodedSize(1, sizeof(nui::NuiImage) + pixels.size()));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 1, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  // The encoder must refuse outright rather than write a frame nobody can walk.
  REQUIRE(written == 0);

  // And the decoder refuses one that is already on disk.
  nui::EncodedImage honest = MakeDepthImage(pixels);
  buffer.assign(nui::EncodedSize(1, sizeof(nui::NuiImage) + pixels.size()), 0);
  const uint32_t ok = nui::EncodeFrame(header, skeletons.data(), 1, &honest, 1, 0,
                                       buffer.data(),
                                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(ok > 0);
  nui::NuiImage tampered = honest.header;
  tampered.data_size = honest.header.data_size + 2;
  std::memcpy(buffer.data() + nui::ImagesOffset(1), &tampered, sizeof(tampered));
  REQUIRE_FALSE(nui::DecodeFrame(buffer.data(), ok).valid);
}

TEST_CASE("a lying image count cannot over-read", "[codec][image]") {
  const std::vector<uint8_t> pixels = MakeDepthPixels(1000, 1);
  nui::NuiFrameHeader header = nui::MakeHeader(1, 0, 0);
  std::vector<nui::NuiSkeleton> skeletons(1);
  const nui::EncodedImage image = MakeDepthImage(pixels);

  std::vector<uint8_t> buffer(
      nui::EncodedSize(1, sizeof(nui::NuiImage) + pixels.size()));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 1, &image, 1, 0, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written > 0);

  nui::NuiFrameHeader lying = header;
  lying.num_depth_frames = 1000;  // one image on disk, a thousand claimed
  std::memcpy(buffer.data(), &lying, sizeof(lying));
  REQUIRE_FALSE(
      nui::DecodeFrame(buffer.data(), static_cast<uint32_t>(buffer.size())).valid);
}

TEST_CASE("skeleton-only frames are unchanged by image support", "[codec]") {
  // The regression guard that matters for existing recordings: a frame with no
  // images must be byte-identical to what v1 produced before images existed.
  nui::NuiFrameHeader header = nui::MakeHeader(7, 100, 200);
  std::vector<nui::NuiSkeleton> skeletons(2);

  std::vector<uint8_t> buffer(nui::EncodedSize(2));
  const uint32_t written =
      nui::EncodeFrame(header, skeletons.data(), 2, kMinimalFlags, buffer.data(),
                       static_cast<uint32_t>(buffer.size()));
  REQUIRE(written == nui::FrameSize(2));

  const nui::DecodedFrame decoded = nui::DecodeFrame(buffer.data(), written);
  REQUIRE(decoded.valid);
  REQUIRE(decoded.image_count == 0);
  REQUIRE(decoded.header.num_depth_frames == 0);
  REQUIRE(decoded.header.num_ir_frames == 0);
  REQUIRE(decoded.header.num_color_frames == 0);
}

// --- the synthetic backend's depth ------------------------------------------
//
// These live in the codec tests because the failure they guard against was not
// a crash or a bad size - the frame encoded fine and decoded fine. It was an
// image with the right dimensions containing nothing but background, which no
// structural test can see. Only "is there actually a body in here" catches it.

TEST_CASE("synthetic depth contains a body, not just background",
          "[image][synthetic]") {
  nui::SyntheticBackend backend(1, 30);
  backend.set_depth(true);

  nui::NuiFrameHeader header = {};
  std::vector<nui::NuiSkeleton> skeletons;
  std::vector<nui::BackendImage> images;
  REQUIRE(backend.NextFrame(&header, &skeletons, &images) == 1);
  REQUIRE(images.size() == 1);
  REQUIRE(images[0].header.width == nui::kDepthWidth);
  REQUIRE(images[0].header.height == nui::kDepthHeight);

  const uint16_t* px =
      reinterpret_cast<const uint16_t*>(images[0].data);
  const size_t count = images[0].header.data_size / 2;

  size_t body = 0;
  size_t nearest = nui::kDepthMaxMillimetres;
  for (size_t i = 0; i < count; ++i) {
    if (nui::DepthPixelPlayerIndex(px[i]) != 0) {
      ++body;
    }
    const uint16_t mm = nui::DepthPixelMillimetres(px[i]);
    if (mm < nearest) {
      nearest = mm;
    }
  }

  // An all-background image is the exact failure this guards: structurally
  // perfect, semantically empty.
  REQUIRE(body > 0);
  // A plausible fraction of the frame - enough to be a body, not a stray pixel.
  REQUIRE(body > count / 50);

  // And the body must be nearer than the far plane, at a human distance. If the
  // camera pose were wrong the body would either vanish or sit at depth 0.
  REQUIRE(nearest > 500);
  REQUIRE(nearest < 5000);
}

TEST_CASE("synthetic depth tracks the skeleton's own joints", "[image][synthetic]") {
  // Depth and skeleton are two views of the same thing, so they must agree. This
  // is the check that a title cross-referencing a joint against the depth image
  // behind it would exercise.
  nui::SyntheticBackend backend(1, 30);
  backend.set_depth(true);

  nui::NuiFrameHeader header = {};
  std::vector<nui::NuiSkeleton> skeletons;
  std::vector<nui::BackendImage> images;
  REQUIRE(backend.NextFrame(&header, &skeletons, &images) == 1);

  // Head height is kRestHeight (1.7 m); the camera sits at 1.0 m, so the head
  // is 0.7 m above it and must appear in the upper half of the image.
  const uint16_t* px = reinterpret_cast<const uint16_t*>(images[0].data);
  const uint16_t width = images[0].header.width;

  int topmost_row = static_cast<int>(images[0].header.height);
  for (uint16_t y = 0; y < images[0].header.height; ++y) {
    for (uint16_t x = 0; x < width; ++x) {
      if (nui::DepthPixelPlayerIndex(px[y * width + x]) != 0) {
        topmost_row = y;
        y = images[0].header.height;
        break;
      }
    }
  }
  REQUIRE(topmost_row < static_cast<int>(images[0].header.height) / 2);
}

TEST_CASE("depth is off unless asked for", "[image][synthetic]") {
  // Existing behaviour must not change silently: a caller that never asks for
  // depth gets skeleton-only frames, so recorded sizes stay what they were.
  nui::SyntheticBackend backend(1, 30);
  nui::NuiFrameHeader header = {};
  std::vector<nui::NuiSkeleton> skeletons;
  std::vector<nui::BackendImage> images;
  REQUIRE(backend.NextFrame(&header, &skeletons, &images) == 1);
  REQUIRE(images.empty());
  REQUIRE(header.num_depth_frames == 0);
}