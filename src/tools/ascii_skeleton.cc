#include "tools/ascii_skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nui {
namespace {

// Confidence shading. Deliberately not a gradient: three levels is all a
// terminal can carry, and more would imply a precision it does not have.
char ShadeChar(float confidence) {
  if (confidence >= 0.75f) return '#';
  if (confidence >= 0.35f) return '+';
  return '.';
}

bool Finite3(const float v[3]) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

const std::vector<std::pair<uint16_t, uint16_t>>& SkeletonBones() {
  static const std::vector<std::pair<uint16_t, uint16_t>> bones = {
      {static_cast<uint16_t>(Joint::kHead), static_cast<uint16_t>(Joint::kNeck)},
      {static_cast<uint16_t>(Joint::kNeck), static_cast<uint16_t>(Joint::kSpineChest)},
      {static_cast<uint16_t>(Joint::kSpineChest),
       static_cast<uint16_t>(Joint::kPelvis)},
      {static_cast<uint16_t>(Joint::kSpineChest),
       static_cast<uint16_t>(Joint::kShoulderLeft)},
      {static_cast<uint16_t>(Joint::kSpineChest),
       static_cast<uint16_t>(Joint::kShoulderRight)},
      {static_cast<uint16_t>(Joint::kShoulderLeft),
       static_cast<uint16_t>(Joint::kElbowLeft)},
      {static_cast<uint16_t>(Joint::kElbowLeft),
       static_cast<uint16_t>(Joint::kWristLeft)},
      {static_cast<uint16_t>(Joint::kWristLeft),
       static_cast<uint16_t>(Joint::kHandLeft)},
      {static_cast<uint16_t>(Joint::kShoulderRight),
       static_cast<uint16_t>(Joint::kElbowRight)},
      {static_cast<uint16_t>(Joint::kElbowRight),
       static_cast<uint16_t>(Joint::kWristRight)},
      {static_cast<uint16_t>(Joint::kWristRight),
       static_cast<uint16_t>(Joint::kHandRight)},
      {static_cast<uint16_t>(Joint::kPelvis), static_cast<uint16_t>(Joint::kHipLeft)},
      {static_cast<uint16_t>(Joint::kHipLeft), static_cast<uint16_t>(Joint::kKneeLeft)},
      {static_cast<uint16_t>(Joint::kKneeLeft),
       static_cast<uint16_t>(Joint::kAnkleLeft)},
      {static_cast<uint16_t>(Joint::kAnkleLeft),
       static_cast<uint16_t>(Joint::kFootLeft)},
      {static_cast<uint16_t>(Joint::kPelvis), static_cast<uint16_t>(Joint::kHipRight)},
      {static_cast<uint16_t>(Joint::kHipRight),
       static_cast<uint16_t>(Joint::kKneeRight)},
      {static_cast<uint16_t>(Joint::kKneeRight),
       static_cast<uint16_t>(Joint::kAnkleRight)},
      {static_cast<uint16_t>(Joint::kAnkleRight),
       static_cast<uint16_t>(Joint::kFootRight)},
  };
  return bones;
}

std::vector<std::string> RenderSkeleton(const NuiSkeleton& skeleton, int width,
                                        int height, float min_confidence) {
  std::vector<std::string> grid(static_cast<size_t>(height),
                                std::string(static_cast<size_t>(width), ' '));
  if (width < 4 || height < 4) {
    return grid;
  }

  // Work out the bounds first so the figure fills the grid rather than
  // occupying a few pixels in the middle.
  float min_x = 1e30f, max_x = -1e30f, min_y = 1e30f, max_y = -1e30f;
  bool any = false;
  for (uint16_t i = 0; i < kJointCount; ++i) {
    const NuiJoint& j = skeleton.joints[i];
    if (!Finite3(j.position)) {
      continue;
    }
    any = true;
    min_x = std::min(min_x, j.position[0]);
    max_x = std::max(max_x, j.position[0]);
    min_y = std::min(min_y, j.position[1]);
    max_y = std::max(max_y, j.position[1]);
  }
  if (!any) {
    return grid;
  }

  // Pad so joints on the extreme edge are not clipped by the border.
  const float span_x = std::max(max_x - min_x, 0.05f);
  const float span_y = std::max(max_y - min_y, 0.05f);
  min_x -= span_x * 0.1f;
  max_x += span_x * 0.1f;
  min_y -= span_y * 0.1f;
  max_y += span_y * 0.1f;

  const float range_x = std::max(max_x - min_x, 1e-6f);
  const float range_y = std::max(max_y - min_y, 1e-6f);
  const int inset = 1;
  const int span_w = std::max(width - inset * 2, 1);
  const int span_h = std::max(height - inset * 2, 1);

  // Project once per joint; -1 marks a joint we must not draw.
  std::vector<int> px(kJointCount, -1), py(kJointCount, -1);
  for (uint16_t i = 0; i < kJointCount; ++i) {
    const NuiJoint& j = skeleton.joints[i];
    if (!Finite3(j.position)) {
      continue;
    }
    px[i] = inset + static_cast<int>((j.position[0] - min_x) / range_x * (span_w - 1));
    // Y is up on screen, so invert.
    py[i] = inset + static_cast<int>((1.0f - (j.position[1] - min_y) / range_y) * (span_h - 1));
    px[i] = std::clamp(px[i], 0, width - 1);
    py[i] = std::clamp(py[i], 0, height - 1);
  }

  auto plot = [&](int x, int y, char c, bool overwrite) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return;
    }
    if (overwrite || grid[static_cast<size_t>(y)][static_cast<size_t>(x)] == ' ') {
      grid[static_cast<size_t>(y)][static_cast<size_t>(x)] = c;
    }
  };

  // Bones first, joints on top so a joint is never hidden by a line.
  for (const auto& bone : SkeletonBones()) {
    const uint16_t a = bone.first, b = bone.second;
    if (px[a] < 0 || px[b] < 0) {
      continue;
    }
    const float conf =
        std::min(skeleton.joints[a].pos_confidence, skeleton.joints[b].pos_confidence);
    // A bone the guest should not trust is drawn dotted even if both ends
    // happen to look confident enough individually.
    const char c = conf >= min_confidence ? '-' : '.';
    const int x0 = px[a], y0 = py[a];
    const int x1 = px[b], y1 = py[b];
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    for (int guard = 0; guard < 4096; ++guard) {
      plot(x, y, c, false);
      if (x == x1 && y == y1) {
        break;
      }
      const int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x += sx; }
      if (e2 <= dx) { err += dx; y += sy; }
    }
  }

  for (uint16_t i = 0; i < kJointCount; ++i) {
    if (px[i] < 0) {
      continue;
    }
    plot(px[i], py[i], ShadeChar(skeleton.joints[i].pos_confidence), true);
  }

  return grid;
}

std::string DescribeSkeleton(const NuiSkeleton& skeleton, float min_confidence) {
  int trustworthy = 0;
  for (uint16_t i = 0; i < kJointCount; ++i) {
    if (skeleton.joints[i].pos_confidence >= min_confidence) {
      ++trustworthy;
    }
  }

  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "id=%u pelvis=(%+.2f,%+.2f,%+.2f) seated=%u up=%u joints=%u/%u>=%.2f",
                skeleton.tracking_id, skeleton.position[0], skeleton.position[1],
                skeleton.position[2], skeleton.seated, skeleton.up_status,
                trustworthy, static_cast<unsigned>(kJointCount), min_confidence);
  return buf;
}

std::string DescribeHeader(const NuiFrameHeader& header) {
  // Latency is how long after capture the frame was written, so host must be
  // later than capture for there to be any. Clocks can disagree; report zero
  // rather than a negative that would read as a provider bug.
  const double latency_ms =
      header.host_timestamp_us > header.capture_timestamp_us
          ? (header.host_timestamp_us - header.capture_timestamp_us) / 1000.0
          : 0.0;

  // Join with a separator rather than appending a trailing space to each, which
  // would render as "[synthetic ]".
  std::string flags;
  auto add_flag = [&flags](const char* name) {
    if (!flags.empty()) {
      flags += ' ';
    }
    flags += name;
  };
  if (header.flags & kFrameFlagProvisional) add_flag("provisional");
  if (header.flags & kFrameFlagExtrapolated) add_flag("extrapolated");
  if (header.flags & kFrameFlagSynthetic) add_flag("synthetic");

  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "frame=%u skeletons=%u depth=%u ir=%u colour=%u latency=%.1fms",
                header.frame_id, header.num_skeletons, header.num_depth_frames,
                header.num_ir_frames, header.num_color_frames, latency_ms);
  std::string text = buf;
  if (!flags.empty()) {
    text += " [" + flags + "]";
  }
  return text;
}

}  // namespace nui