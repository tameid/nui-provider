#include "backends/synthetic/synthetic.h"

#include <algorithm>
#include <cmath>

#include "nui/encode.h"

namespace nui {
namespace {

// Rest pose, metres, world space (+X right, +Y up, -Z into the screen). A
// standing adult roughly 1.7 m, arms down. Roughly human proportions so a
// title that draws a stick figure looks right.
constexpr float kRestHeight = 1.7f;

constexpr float kPi = 3.14159265358979323846f;

// Depth-camera intrinsics for the retail 320x240 stream. A real backend reads
// these from the device's factory calibration; a synthetic one has to assert
// them, and asserting the documented values is more honest than inventing
// convenient ones.
constexpr float kFocalLengthPixels = 285.0f;
constexpr float kFarMillimetres = 4000;  // beyond the sensor's useful range
constexpr uint32_t kMaxTrackedPlayers = 7;  // 3 bits

// Where the camera sits, in the same world space the skeletons are expressed in.
//
// This is not redundant. Skeleton coordinates are metric and camera-relative in
// the NUI model, but the rest pose above is built with y = 0 at the feet and the
// body at z = 0, which puts it *on* the origin. A renderer that assumes the
// camera is at the origin therefore projects the whole body at depth 0 and
// clips it away - which is exactly what an all-background depth image looks
// like, and why the first run of this produced no body at all.
//
// A real backend gets this from the device pose. Asserting it here keeps the
// synthetic body standing where a player actually stands: about 2.5 m in front
// of the camera, head comfortably inside the vertical field of view.
constexpr float kCameraZ = -2.5f;   // camera looks down +Z at the body
constexpr float kCameraHeight = 1.0f;

// Limb radius, so a projected limb has thickness. Halved with distance by
// DrawBone to stay roughly constant in pixels.
constexpr float kLimbRadiusMetres = 0.05f;

// Projects a world point into depth-image pixel coordinates.
//
// World space is +X right, +Y up, -Z into the screen. The camera is at
// (0, kCameraHeight, kCameraZ) looking towards +Z, so a point's depth is its
// distance along +Z from the camera - not simply -z. Getting that wrong is what
// made the first depth render come out empty.
bool ProjectPoint(const float world[3], float focal_px, float cx, float cy,
                  float* out_x, float* out_y, uint16_t* out_depth_mm) {
  const float z = world[2] - kCameraZ;
  if (z <= 0.01f) {
    return false;  // behind or level with the camera
  }
  *out_x = cx + (world[0] / z) * focal_px;
  // Height relative to the camera, then flipped: +Y is up, image Y is down.
  *out_y = cy - ((world[1] - kCameraHeight) / z) * focal_px;
  const float mm = z * 1000.0f;
  if (mm <= 0.0f || mm > static_cast<float>(kDepthMaxMillimetres)) {
    return false;
  }
  *out_depth_mm = static_cast<uint16_t>(mm);
  return true;
}

// Writes a disc of `radius_px` at (x, y). Nearer samples win, which is what a
// real depth sensor reports for a limb occluding a torso.
void Splat(uint16_t* px, int x, int y, uint16_t depth_mm, uint8_t player_index,
           float radius_px) {
  const int r = static_cast<int>(radius_px + 0.5f);
  for (int dy = -r; dy <= r; ++dy) {
    const int py = y + dy;
    if (py < 0 || py >= kDepthHeight) {
      continue;
    }
    for (int dx = -r; dx <= r; ++dx) {
      const int pxx = x + dx;
      if (pxx < 0 || pxx >= kDepthWidth) {
        continue;
      }
      if (dx * dx + dy * dy > r * r) {
        continue;  // outside the disc
      }
      uint16_t& target = px[static_cast<size_t>(py) * kDepthWidth + pxx];
      if (DepthPixelMillimetres(target) > depth_mm) {
        target = PackDepthPixel(depth_mm, player_index);
      }
    }
  }
}

// Draws a limb as a chain of discs between two projected joints. Sampling along
// the segment is what closes the gaps a two-point splat would leave, which a
// title doing silhouette extraction would read as a broken body.
void DrawBone(uint16_t* px, const float from[3], const float to[3], float focal_px,
              float cx, float cy, uint8_t player_index) {
  float ax, ay, bx, by;
  uint16_t az, bz;
  if (!ProjectPoint(from, focal_px, cx, cy, &ax, &ay, &az) ||
      !ProjectPoint(to, focal_px, cx, cy, &bx, &by, &bz)) {
    return;
  }

  // Limb radius shrinks with distance, so it stays roughly constant in pixels
  // the way a real limb does.
  const float depth_m = static_cast<float>(az) / 1000.0f;
  const float radius_px = (kLimbRadiusMetres / std::max(depth_m, 0.1f)) * focal_px;
  const int radius = std::max(1, static_cast<int>(radius_px + 0.5f));

  // Steps scaled so consecutive splats overlap rather than leaving gaps.
  const float span = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
  const int steps = std::max(1, static_cast<int>(span / std::max(1, radius)));

  for (int i = 0; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const int x = static_cast<int>(ax + (bx - ax) * t + 0.5f);
    const int y = static_cast<int>(ay + (by - ay) * t + 0.5f);
    const auto depth = static_cast<uint16_t>(az + (bz - az) * t);
    Splat(px, x, y, depth, player_index, static_cast<float>(radius));
  }
}

}  // namespace

SyntheticBackend::SyntheticBackend(uint32_t skeleton_count, uint32_t hz)
    : skeleton_count_(skeleton_count ? skeleton_count : 1),
      hz_(hz ? hz : 30) {}

void SyntheticBackend::Reset() {
  frame_id_ = 0;
  capture_us_ = 0;
}

uint32_t SyntheticBackend::NextFrame(NuiFrameHeader* header,
                                     std::vector<NuiSkeleton>* skeletons,
                                     std::vector<BackendImage>* images) {
  const uint64_t step_us = 1000000ull / hz_;

  skeletons->assign(skeleton_count_, NuiSkeleton{});
  for (uint32_t i = 0; i < skeleton_count_; ++i) {
    FillSkeleton(&(*skeletons)[i], i, capture_us_);
  }

  *header = MakeHeader(frame_id_, capture_us_, capture_us_);
  header->flags = kFrameFlagSynthetic;

  images->clear();
  if (depth_enabled_) {
    RenderDepth(*skeletons);
    BackendImage depth;
    depth.header.width = kDepthWidth;
    depth.header.height = kDepthHeight;
    depth.header.format = static_cast<uint8_t>(ImageFormat::kDepth16);
    depth.header.reserved0 = 0;
    depth.header.data_size =
        static_cast<uint32_t>(depth_pixels_.size());
    depth.data = depth_pixels_.data();
    images->push_back(depth);
  }

  ++frame_id_;
  capture_us_ += step_us;
  return skeleton_count_;
}

void SyntheticBackend::RenderDepth(const std::vector<NuiSkeleton>& skeletons) {
  // Background first: the far plane, player index 0. A real sensor reports no
  // depth (black) where it cannot see, but a uniform far value is easier to
  // tell apart from a bug that never wrote the buffer at all.
  const size_t pixels = static_cast<size_t>(kDepthWidth) * kDepthHeight;
  depth_pixels_.resize(pixels * 2);
  uint16_t* px = reinterpret_cast<uint16_t*>(depth_pixels_.data());
  for (size_t i = 0; i < pixels; ++i) {
    px[i] = PackDepthPixel(static_cast<uint16_t>(kFarMillimetres), 0);
  }

  // Pinhole projection matching the 360's depth camera, so a joint at a known
  // world position lands where a real camera would put it. Constants are the
  // retail 320x240 depth intrinsics.
  const float focal_px = kFocalLengthPixels;
  const float cx = kDepthWidth * 0.5f;
  const float cy = kDepthHeight * 0.5f;

  // Bones, as joint index pairs. Drawing limbs rather than points is what makes
  // the image look like a body: a title doing background subtraction needs
  // contiguous regions, not a scatter of dots.
  static constexpr Joint kBones[][2] = {
      {Joint::kHead, Joint::kNeck},
      {Joint::kNeck, Joint::kSpineChest},
      {Joint::kSpineChest, Joint::kPelvis},
      {Joint::kShoulderLeft, Joint::kShoulderRight},
      {Joint::kShoulderLeft, Joint::kElbowLeft},
      {Joint::kElbowLeft, Joint::kWristLeft},
      {Joint::kWristLeft, Joint::kHandLeft},
      {Joint::kShoulderRight, Joint::kElbowRight},
      {Joint::kElbowRight, Joint::kWristRight},
      {Joint::kWristRight, Joint::kHandRight},
      {Joint::kPelvis, Joint::kHipLeft},
      {Joint::kHipLeft, Joint::kKneeLeft},
      {Joint::kKneeLeft, Joint::kAnkleLeft},
      {Joint::kAnkleLeft, Joint::kFootLeft},
      {Joint::kPelvis, Joint::kHipRight},
      {Joint::kHipRight, Joint::kKneeRight},
      {Joint::kKneeRight, Joint::kAnkleRight},
      {Joint::kAnkleRight, Joint::kFootRight},
  };

  for (const NuiSkeleton& s : skeletons) {
    // Player index is 1-based in NUI: 0 means "no player".
    const uint8_t player_index = static_cast<uint8_t>(
        std::min<uint32_t>(s.tracking_id, kMaxTrackedPlayers));

    for (const auto& bone : kBones) {
      const NuiJoint& a = s.joints[static_cast<uint16_t>(bone[0])];
      const NuiJoint& b = s.joints[static_cast<uint16_t>(bone[1])];
      DrawBone(px, a.position, b.position, focal_px, cx, cy, player_index);
    }
  }
}

void SyntheticBackend::FillSkeleton(NuiSkeleton* skeleton, uint32_t index,
                                    uint64_t t_us) const {
  const float t = static_cast<float>(t_us) / 1000000.0f;

  // Slow sway, plus a faster bob. Integer index offsets phase so multiple
  // skeletons are visibly out of sync rather than clones.
  const float phase = static_cast<float>(index) * (kPi / 2.0f);
  const float sway = std::sin(t * 0.6f + phase) * 0.05f;
  const float bob = std::sin(t * 1.7f + phase) * 0.02f;
  const float breathe = std::sin(t * 0.9f + phase) * 0.008f;

  // Nudge each skeleton apart in X so a multi-person scene is distinguishable
  // rather than a stack of overlapping clones.
  const float offset_x = static_cast<float>(index) * 0.6f;

  skeleton->tracking_id = index + 1;
  skeleton->seated = 0;
  skeleton->up_status = kUpStatusFloor;
  skeleton->flags = 0;
  skeleton->reserved0 = 0;
  skeleton->reserved1 = 0;
  skeleton->reserved2 = 0;

  skeleton->position[0] = offset_x + sway;
  skeleton->position[1] = 0.9f + bob + breathe;
  skeleton->position[2] = 0.0f;

  // Identity quaternion - we are not modelling rotation yet, and an identity is
  // honest about that rather than a wrong value.
  skeleton->orientation[0] = 0.0f;
  skeleton->orientation[1] = 0.0f;
  skeleton->orientation[2] = 0.0f;
  skeleton->orientation[3] = 1.0f;

  skeleton->up_vector[0] = 0.0f;
  skeleton->up_vector[1] = 1.0f;
  skeleton->up_vector[2] = 0.0f;
  skeleton->up_confidence = 1.0f;

  // Dead-centre on screen; the synthetic source has no camera to project from.
  skeleton->screen_position[0] = 0.5f;
  skeleton->screen_position[1] = 0.5f;
  skeleton->screen_rect[0] = 0.35f;
  skeleton->screen_rect[1] = 0.15f;
  skeleton->screen_rect[2] = 0.65f;
  skeleton->screen_rect[3] = 0.9f;

  // Rest pose per joint, as {x, y, z} with y as a fraction of height.
  struct Rest {
    Joint joint;
    float x;
    float y;
    float z;
  };
  static constexpr Rest kRest[] = {
      {Joint::kHead, 0.00f, 1.00f, 0.00f},   {Joint::kNeck, 0.00f, 0.87f, 0.00f},
      {Joint::kSpineChest, 0.00f, 0.80f, 0.00f}, {Joint::kPelvis, 0.00f, 0.57f, 0.00f},
      {Joint::kShoulderLeft, -0.18f, 0.83f, 0.00f},
      {Joint::kShoulderRight, 0.18f, 0.83f, 0.00f},
      {Joint::kElbowLeft, -0.22f, 0.66f, 0.02f},
      {Joint::kElbowRight, 0.22f, 0.66f, 0.02f},
      {Joint::kWristLeft, -0.25f, 0.50f, 0.04f},
      {Joint::kWristRight, 0.25f, 0.50f, 0.04f},
      {Joint::kHandLeft, -0.26f, 0.43f, 0.05f},
      {Joint::kHandRight, 0.26f, 0.43f, 0.05f},
      {Joint::kHipLeft, -0.10f, 0.55f, 0.00f},
      {Joint::kHipRight, 0.10f, 0.55f, 0.00f},
      {Joint::kKneeLeft, -0.11f, 0.31f, 0.01f},
      {Joint::kKneeRight, 0.11f, 0.31f, 0.01f},
      {Joint::kAnkleLeft, -0.11f, 0.06f, 0.00f},
      {Joint::kAnkleRight, 0.11f, 0.06f, 0.00f},
      {Joint::kFootLeft, -0.11f, 0.01f, -0.08f},
      {Joint::kFootRight, 0.11f, 0.01f, -0.08f},
  };
  static_assert(sizeof(kRest) / sizeof(kRest[0]) == kJointCount,
                "rest pose must cover every joint");

  for (uint16_t j = 0; j < kJointCount; ++j) {
    const Rest& r = kRest[j];
    NuiJoint& joint = skeleton->joints[j];

    // Arms swing; everything else just inherits the body sway/bob.
    const bool is_left = r.x < 0.0f;
    const float swing =
        std::sin(t * 1.1f + phase + (is_left ? 0.0f : kPi)) * 0.04f;
    const bool is_arm = j >= static_cast<uint16_t>(Joint::kShoulderLeft) &&
                        j <= static_cast<uint16_t>(Joint::kHandRight);

    joint.position[0] = offset_x + r.x * kRestHeight + sway +
                        (is_arm ? swing : 0.0f);
    joint.position[1] = r.y * kRestHeight + bob + breathe;
    joint.position[2] = r.z * kRestHeight;

    joint.orientation[0] = 0.0f;
    joint.orientation[1] = 0.0f;
    joint.orientation[2] = 0.0f;
    joint.orientation[3] = 1.0f;

    // Synthetic data is certain by construction. That is a property of the
    // backend, not a claim about a real sensor - the synthetic flag is what
    // tells them apart.
    joint.pos_confidence = 1.0f;
    joint.orient_confidence = 1.0f;
  }
}

}  // namespace nui