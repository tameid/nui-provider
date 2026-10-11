/**
 * NUI wire format, version 1 - frame reader.
 *
 * The host-side mirror of what the guest plugin does, so tests and the frame
 * inspector can assert on exactly the bytes that cross the boundary. If the
 * plugin's decoder ever disagrees with this one, this one is the reference.
 */

#ifndef NUI_DECODE_H_
#define NUI_DECODE_H_

#include <cstring>

#include "nui/frame.h"

namespace nui {

// One image found in a decoded frame. `data` points into the caller's buffer.
struct DecodedImage {
  NuiImage header = {};
  const uint8_t* data = nullptr;
};

// Images a single frame can carry. Not a vector because the count is bounded by
// the header's three 32-bit fields and a frame with more than this is malformed,
// not merely unusual - a fixed array means a bad header cannot make us allocate.
constexpr uint32_t kMaxImagesPerFrame = 64;

struct DecodedFrame {
  NuiFrameHeader header = {};
  uint32_t skeleton_count = 0;
  // Points into the buffer passed to DecodeFrame. Valid only while that
  // buffer is.
  const NuiSkeleton* skeletons = nullptr;
  // Same lifetime rule as `skeletons`.
  uint32_t image_count = 0;
  DecodedImage images[kMaxImagesPerFrame] = {};
  bool valid = false;
};

// Parses a frame from `data`. Never reads past `size`, and never trusts a
// count from the header without checking it against the bytes actually present:
// a truncated write must read as invalid, not as a short read of garbage.
//
// Images are walked after the skeletons in the fixed order depth, ir, color -
// the same order EncodeFrame writes them. Each carries its own header, so its
// length is only knowable by reading it, which is why a short image invalidates
// the whole frame rather than being skipped.
inline DecodedFrame DecodeFrame(const uint8_t* data, uint32_t size) {
  DecodedFrame out = {};
  if (size < sizeof(NuiFrameHeader) || data == nullptr) {
    return out;
  }

  NuiFrameHeader header = {};
  std::memcpy(&header, data, sizeof(header));
  if (!HeaderLooksValid(header)) {
    return out;
  }

  const uint64_t available = size - sizeof(NuiFrameHeader);
  const uint64_t needed = static_cast<uint64_t>(header.num_skeletons) *
                          sizeof(NuiSkeleton);
  if (needed > available) {
    return out;
  }

  const uint64_t image_total = static_cast<uint64_t>(header.num_depth_frames) +
                               header.num_ir_frames + header.num_color_frames;
  if (image_total > kMaxImagesPerFrame) {
    return out;
  }

  uint64_t at = sizeof(NuiFrameHeader) + needed;
  for (uint64_t i = 0; i < image_total; ++i) {
    if (at + sizeof(NuiImage) > size) {
      return out;  // image header runs past the end
    }
    NuiImage image = {};
    std::memcpy(&image, data + at, sizeof(image));
    at += sizeof(image);

    if (at + image.data_size > size) {
      return out;  // pixel data runs past the end
    }
    // A declared size that disagrees with the geometry means the writer and the
    // reader disagree about the format; refuse rather than hand back a frame
    // whose pixels are not where they claim to be.
    if (image.data_size != ImageDataSize(static_cast<ImageFormat>(image.format),
                                         image.width, image.height)) {
      return out;
    }

    DecodedImage& slot = out.images[out.image_count++];
    slot.header = image;
    slot.data = data + at;
    at += image.data_size;
  }

  out.header = header;
  out.skeleton_count = header.num_skeletons;
  if (header.num_skeletons) {
    out.skeletons = reinterpret_cast<const NuiSkeleton*>(data + sizeof(header));
  }
  out.valid = true;
  return out;
}

}  // namespace nui

#endif  // NUI_DECODE_H_