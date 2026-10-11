/**
 * Recording session container, version 1 - ".nui" files.
 *
 * A session is a recorded sequence of frames, used for A/B testing and for
 * reproducing a bug without the hardware that caused it. It is deliberately
 * thin: after a fixed header, the payload is a sequence of frames in exactly
 * the frozen wire format of include/nui/frame.h, with no re-encoding.
 *
 * That is the whole design. Each frame's NuiFrameHeader already carries its
 * skeleton count, so playback reads a header, reads that many skeletons, and
 * repeats. There is no second length scheme to keep in sync, and a session file
 * and a live frame have identical bytes for the same data - which is what makes
 * a recorded session trustworthy as a reproduction.
 *
 * The header adds only what a recording knows and a single frame does not: how
 * many frames are in here, what produced them, and at what rate.
 *
 * Not compressed. A two-skeleton frame is 1648 bytes, so 30 Hz for a minute is
 * about 3 MB. If sessions grow large enough for that to matter, compress the
 * whole file rather than individual frames - frame-to-frame delta coding does
 * not survive the synthetic backend's already-tiny frames and adds a decoder
 * to the guest-facing side for no benefit.
 */

#ifndef NUI_SESSION_H_
#define NUI_SESSION_H_

#include <cstdint>

namespace nui {

// 'XNRS' little-endian.
constexpr uint32_t kSessionMagic = 0x53524E58;
constexpr uint16_t kSessionVersion = 1;

// Fixed width so the header can be read without parsing, and so a partially
// written session is still recognisable. Longer names are truncated.
constexpr uint32_t kBackendNameLength = 32;

#pragma pack(push, 1)

struct SessionHeader {
  uint32_t magic;             // kSessionMagic
  uint16_t version;           // kSessionVersion
  uint16_t flags;
  uint32_t frame_count;       // frames that follow
  uint32_t max_skeletons;     // largest num_skeletons in the file, for sizing
  uint32_t hz;                // capture rate the frames were produced at
  uint64_t created_unix_us;   // when recording started
  // NUL-padded, not necessarily NUL-terminated at the last byte.
  char backend[kBackendNameLength];
};
static_assert(sizeof(SessionHeader) == 60, "Size matters - it is on disk");

enum SessionFlags : uint16_t {
  // The recording was paced in real time (one frame per capture interval). If
  // absent, frames were produced as fast as possible and playback should not
  // assume wall-clock spacing.
  kSessionFlagRealtime = 1 << 0,
};

#pragma pack(pop)

constexpr bool SessionHeaderLooksValid(const SessionHeader& header) {
  return header.magic == kSessionMagic && header.version == kSessionVersion;
}

// Bytes one frame occupies inside a session: the frozen frame encoding, with no
// container overhead. Identical to the IPC .bin payload.
//
// Images are included, and their size is not knowable from the skeleton count
// alone - a frame's images carry their own geometry in NuiImage headers. The
// reader therefore walks the payload rather than assuming a size, and this
// helper is only for writers that already know their images.
constexpr uint32_t SessionFrameSize(uint32_t skeletons) {
  return sizeof(NuiFrameHeader) + skeletons * sizeof(NuiSkeleton);
}

}  // namespace nui

#endif  // NUI_SESSION_H_