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

// Bytes EncodeFrame needs for this many skeletons. Excludes skeletons you
// intend to omit.
inline uint32_t EncodedSize(uint32_t skeletons) { return FrameSize(skeletons); }

// Writes a frame into `out`, returning bytes written, or 0 if it does not fit.
//
// `frame_id` is stamped by the caller rather than the writer because the
// provider owns the monotonic counter across backends.
inline uint32_t EncodeFrame(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
                            uint32_t skeleton_count, uint16_t flags, uint8_t* out,
                            uint32_t capacity) {
  const uint32_t needed = EncodedSize(skeleton_count);
  if (capacity < needed) {
    return 0;
  }

  NuiFrameHeader stamped = header;
  stamped.magic = kFrameMagic;
  stamped.version = kVersion1;
  stamped.flags = flags;
  stamped.num_skeletons = skeleton_count;

  std::memcpy(out, &stamped, sizeof(stamped));
  if (skeleton_count && skeletons) {
    std::memcpy(out + sizeof(stamped), skeletons,
                skeleton_count * sizeof(NuiSkeleton));
  }
  return needed;
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