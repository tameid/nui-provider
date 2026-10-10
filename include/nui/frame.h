/**
 * NUI wire format, version 1.
 *
 * FROZEN. This is the contract between nui-provider (host side) and the guest
 * NUI plugin that runs inside xenia. Both sides compile against this header; a
 * change here is a protocol change and needs a version bump, not an edit.
 *
 * Design notes that matter when changing any of this:
 *
 *  - Little-endian and packed. The guest is a PowerPC/Xbox 360 CPU running
 *    big-endian, so the plugin byte-swaps on read. Do not "helpfully" reorder
 *    fields to be host-friendlier; that breaks the frozen contract.
 *
 *  - Positions are metres, world-relative, Kinect-camera style: +X right,
 *    +Y up, -Z into the screen. This matches what the Xbox 360 NUI API expects,
 *    so the plugin does not have to convert coordinate bases.
 *
 *  - Confidence is the single most important field in this format. Xbox 360
 *    Kinect titles gate hard on it, and a confidently-wrong joint is far worse
 *    than a low-confidence one: it produces gestures and scores that the title
 *    acts on. Emit 0 when you do not know. Never guess.
 *
 *  - Timestamps are microseconds. capture_timestamp_us is when the sensor
 *    sample was taken; host_timestamp_us is when the frame was handed to the
 *    writer. The difference is pipeline latency, and titles care about it.
 *
 * Spec: docs/kinect_support.md 3.2. Companion decoder: include/nui/decode.h.
 */

#ifndef NUI_FRAME_H_
#define NUI_FRAME_H_

#include <cstdint>

namespace nui {

// 'XNUI' little-endian, so the first four bytes on disk are X N U I.
constexpr uint32_t kFrameMagic = 0x49554E58;
constexpr uint16_t kVersion1 = 1;

// Xbox 360 NUI joint ids. This is the guest's set and its order; the guest
// indexes joints by this enum, so entries may not be reordered or removed
// without a version bump.
enum class Joint : uint8_t {
  kHead = 0,
  kNeck,
  kSpineChest,
  kPelvis,
  kShoulderLeft,
  kShoulderRight,
  kElbowLeft,
  kElbowRight,
  kWristLeft,
  kWristRight,
  kHandLeft,
  kHandRight,
  kHipLeft,
  kHipRight,
  kKneeLeft,
  kKneeRight,
  kAnkleLeft,
  kAnkleRight,
  kFootLeft,
  kFootRight,
  kCount,
};
constexpr uint16_t kJointCount = static_cast<uint16_t>(Joint::kCount);

// Quality/confidence range. Xbox 360 NUI reports these as 0.0-1.0 floats.
constexpr float kMinConfidence = 0.0f;
constexpr float kMaxConfidence = 1.0f;

#pragma pack(push, 1)

// One joint. orientation is a quaternion in xyzw order (that is the NUI order,
// not the more common wxyz).
struct NuiJoint {
  float position[3];           // metres, world space
  float orientation[4];        // quaternion xyzw
  float pos_confidence;        // 0..1
  float orient_confidence;     // 0..1
};
static_assert(sizeof(NuiJoint) == 36, "Size matters - it is on the wire");

// One tracked skeleton. tracking_id is assigned by the provider and kept stable
// for the life of a track; a guest title treats a changed id as a new player.
struct NuiSkeleton {
  uint32_t tracking_id;
  uint8_t seated;              // 0 standing, 1 seated
  uint8_t up_status;           // unknown / floor / ceiling / wall
  uint8_t flags;
  uint8_t reserved0;
  uint16_t reserved1;
  uint32_t reserved2;
  float position[3];           // pelvis position, metres
  float orientation[4];        // pelvis quaternion xyzw
  float up_vector[3];
  float up_confidence;
  float screen_position[2];    // normalised screen coords, 0..1
  float screen_rect[4];        // bounding rect of the skeleton on screen
  NuiJoint joints[kJointCount];
};
// Preamble is 82 bytes packed: 4 tracking_id, 4 bytes of uint8 flags, 2
// reserved1, 4 reserved2, 12 position, 16 orientation, 12 up_vector, 4
// up_confidence, 8 screen_position, 16 screen_rect.
static_assert(sizeof(NuiSkeleton) == 82 + 36 * kJointCount,
              "Size matters - it is on the wire");

// Frame header. Counts describe how many entries of each kind follow in the
// same file; anything not written is absent, and the guest must treat absence
// as "not available" rather than "zero".
struct NuiFrameHeader {
  uint32_t magic;              // kFrameMagic
  uint16_t version;            // kVersion1
  uint16_t flags;
  uint32_t frame_id;           // monotonic per provider
  uint64_t capture_timestamp_us;
  uint64_t host_timestamp_us;
  uint32_t num_skeletons;
  uint32_t num_depth_frames;
  uint32_t num_ir_frames;
  uint32_t num_color_frames;
};
static_assert(sizeof(NuiFrameHeader) == 44, "Size matters - it is on the wire");

enum FrameFlags : uint16_t {
  // At least one skeleton in this frame is provisional: tracking has not yet
  // confirmed it. A title should render it but not act on it.
  kFrameFlagProvisional = 1 << 0,
  // The skeletons in this frame are interpolated or predicted, not measured.
  // Used by latency compensation; see --predict-ms.
  kFrameFlagExtrapolated = 1 << 1,
  // No NUI hardware is behind these frames. Set by the synthetic backend so a
  // title or the plugin can tell a real device from a generated one.
  kFrameFlagSynthetic = 1 << 2,
};

enum UpStatus : uint8_t {
  kUpStatusUnknown = 0,
  kUpStatusFloor = 1,
  kUpStatusCeiling = 2,
  kUpStatusWall = 3,
};

enum SkeletonFlags : uint8_t {
  // This skeleton is tracked but its hands are too small/occluded for reliable
  // gesture input. Titles use this to suppress gesture recognition.
  kSkeletonFlagHandsUnreliable = 1 << 0,
  // The skeleton is only partly inside the frame of reference.
  kSkeletonFlagPartiallyTracked = 1 << 1,
  // Confidence fell below the point where this track is worth keeping; it is
  // reported one last time and the guest should drop it.
  kSkeletonFlagLost = 1 << 2,
};

#pragma pack(pop)

// Offset of the first skeleton in an encoded frame. No version argument: v1
// is the only version, and a parameter here would invite callers to pretend
// otherwise.
constexpr uint32_t PayloadOffset() { return sizeof(NuiFrameHeader); }

// Bytes needed for a frame carrying `skeletons` skeletons and no images.
constexpr uint32_t FrameSize(uint32_t skeletons) {
  return PayloadOffset() + skeletons * sizeof(NuiSkeleton);
}

// True if a header is worth parsing. Cheap, and rejects a truncated or
// half-written buffer before anyone reads a count out of it.
constexpr bool HeaderLooksValid(const NuiFrameHeader& header) {
  return header.magic == kFrameMagic && header.version == kVersion1;
}

}  // namespace nui

#endif  // NUI_FRAME_H_