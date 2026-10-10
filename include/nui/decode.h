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

struct DecodedFrame {
  NuiFrameHeader header = {};
  uint32_t skeleton_count = 0;
  // Points into the buffer passed to DecodeFrame. Valid only while that
  // buffer is.
  const NuiSkeleton* skeletons = nullptr;
  bool valid = false;
};

// Parses a frame from `data`. Never reads past `size`, and never trusts a
// count from the header without checking it against the bytes actually present:
// a truncated write must read as invalid, not as a short read of garbage.
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