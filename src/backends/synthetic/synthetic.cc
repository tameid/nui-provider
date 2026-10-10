#include "backends/synthetic/synthetic.h"

#include <cmath>

#include "nui/encode.h"

namespace nui {
namespace {

// Rest pose, metres, world space (+X right, +Y up, -Z into the screen). A
// standing adult roughly 1.7 m, arms down. Roughly human proportions so a
// title that draws a stick figure looks right.
constexpr float kRestHeight = 1.7f;

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

SyntheticBackend::SyntheticBackend(uint32_t skeleton_count, uint32_t hz)
    : skeleton_count_(skeleton_count ? skeleton_count : 1),
      hz_(hz ? hz : 30) {}

void SyntheticBackend::Reset() {
  frame_id_ = 0;
  capture_us_ = 0;
}

uint32_t SyntheticBackend::NextFrame(NuiFrameHeader* header,
                                     std::vector<NuiSkeleton>* out) {
  const uint64_t step_us = 1000000ull / hz_;

  out->assign(skeleton_count_, NuiSkeleton{});
  for (uint32_t i = 0; i < skeleton_count_; ++i) {
    FillSkeleton(&(*out)[i], i, capture_us_);
  }

  *header = MakeHeader(frame_id_, capture_us_, capture_us_);
  header->flags = kFrameFlagSynthetic;

  ++frame_id_;
  capture_us_ += step_us;
  return skeleton_count_;
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