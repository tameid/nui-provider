/**
 * NUI wire format, version 1 - frame writer.
 *
 * Serialises a frame into a caller-supplied buffer. Does not own the file, the
 * buffering or the atomicity: that is src/ipc/file_writer.cpp's job. Keeping
 * this layer free of I/O is what makes the format testable without a disk.
 */

#ifndef NUI_ENCODE_H_
#define NUI_ENCODE_H_

#include <cstdint>
#include <cstring>

#include "nui/frame.h"

namespace nui {

// One image plus its pixels, as a backend hands it over.
struct EncodedImage {
  NuiImage header;
  const uint8_t* data;  // header.data_size bytes, may be null if that is 0
};

// Bytes EncodeFrame needs for `image_count` images totalling `image_bytes`
// (headers included).
inline uint32_t EncodedSize(uint32_t skeletons, uint32_t image_bytes) {
  return FrameSize(skeletons, image_bytes);
}

// Bytes EncodeFrame needs for a frame with no images.
inline uint32_t EncodedSize(uint32_t skeletons) { return FrameSize(skeletons); }

// Writes a frame into `out`, returning bytes written, or 0 if it does not fit.
//
// Images are written in the fixed order depth, ir, color, and the header counts
// are set from what was actually written rather than from the caller's header -
// the counts are how a reader walks the payload, so they must not be able to
// disagree with it.
inline uint32_t EncodeFrame(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
                            uint32_t skeleton_count, const EncodedImage* images,
                            uint32_t image_count, uint16_t flags, uint8_t* out,
                            uint32_t capacity) {
  uint32_t depth = 0, ir = 0, color = 0, image_bytes = 0;
  for (uint32_t i = 0; i < image_count; ++i) {
    const uint8_t format = images[i].header.format;
    const uint32_t data_size = images[i].header.data_size;
    // A payload whose size disagrees with its declared geometry would make the
    // reader walk off the end. Refuse rather than write a frame we cannot read.
    if (data_size != ImageDataSize(static_cast<ImageFormat>(format),
                                   images[i].header.width,
                                   images[i].header.height)) {
      return 0;
    }
    const uint32_t size = sizeof(NuiImage) + data_size;
    image_bytes += size;
    if (format == static_cast<uint8_t>(ImageFormat::kDepth16)) {
      ++depth;
    } else if (format == static_cast<uint8_t>(ImageFormat::kIr8)) {
      ++ir;
    } else {
      ++color;
    }
  }

  const uint32_t needed = EncodedSize(skeleton_count, image_bytes);
  if (capacity < needed) {
    return 0;
  }

  NuiFrameHeader stamped = header;
  stamped.magic = kFrameMagic;
  stamped.version = kVersion1;
  stamped.flags = flags;
  stamped.num_skeletons = skeleton_count;
  stamped.num_depth_frames = depth;
  stamped.num_ir_frames = ir;
  stamped.num_color_frames = color;

  uint8_t* at = out;
  std::memcpy(at, &stamped, sizeof(stamped));
  at += sizeof(stamped);

  if (skeleton_count && skeletons) {
    std::memcpy(at, skeletons, SkeletonsSize(skeleton_count));
    at += SkeletonsSize(skeleton_count);
  }

  // Written in the same fixed order the reader walks them.
  for (int pass = 0; pass < 3; ++pass) {
    for (uint32_t i = 0; i < image_count; ++i) {
      const ImageFormat want = pass == 0   ? ImageFormat::kDepth16
                               : pass == 1 ? ImageFormat::kIr8
                                           : ImageFormat::kColorBgra8;
      if (images[i].header.format != static_cast<uint8_t>(want)) {
        continue;
      }
      std::memcpy(at, &images[i].header, sizeof(NuiImage));
      at += sizeof(NuiImage);
      if (images[i].header.data_size && images[i].data) {
        std::memcpy(at, images[i].data, images[i].header.data_size);
        at += images[i].header.data_size;
      }
    }
  }
  return static_cast<uint32_t>(at - out);
}

// Frame with no images, for callers that only have skeletons.
inline uint32_t EncodeFrame(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
                            uint32_t skeleton_count, uint16_t flags, uint8_t* out,
                            uint32_t capacity) {
  return EncodeFrame(header, skeletons, skeleton_count, nullptr, 0, flags, out,
                     capacity);
}

// Fills in a header for `now_us` (host clock) and `capture_us` (sensor clock).
inline NuiFrameHeader MakeHeader(uint32_t frame_id, uint64_t capture_us,
                                 uint64_t host_us) {
  NuiFrameHeader header = {};
  header.magic = kFrameMagic;
  header.version = kVersion1;
  header.flags = 0;
  header.frame_id = frame_id;
  header.capture_timestamp_us = capture_us;
  header.host_timestamp_us = host_us;
  header.num_skeletons = 0;
  header.num_depth_frames = 0;
  header.num_ir_frames = 0;
  header.num_color_frames = 0;
  return header;
}

}  // namespace nui

#endif  // NUI_ENCODE_H_