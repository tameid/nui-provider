// Inspector tests.
//
// The ASCII renderer is not decoration, so it gets tested like the rest: a
// mis-projected skeleton is exactly the kind of bug that looks fine on screen
// and quietly misleads whoever is debugging a title.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ipc/file_writer.h"
#include "nui/encode.h"
#include "nui/frame.h"
#include "recorder/session_writer.h"
#include "tools/ascii_skeleton.h"
#include "tools/frame_source.h"

namespace {

nui::NuiSkeleton StandingSkeleton() {
  nui::NuiSkeleton skeleton = {};
  skeleton.tracking_id = 1;
  skeleton.position[1] = 0.9f;
  skeleton.up_vector[1] = 1.0f;
  skeleton.up_confidence = 1.0f;

  struct Rest {
    nui::Joint joint;
    float x, y;
  };
  static const Rest rest[] = {
      {nui::Joint::kHead, 0.00f, 1.70f},
      {nui::Joint::kNeck, 0.00f, 1.48f},
      {nui::Joint::kSpineChest, 0.00f, 1.36f},
      {nui::Joint::kPelvis, 0.00f, 0.97f},
      {nui::Joint::kShoulderLeft, -0.20f, 1.41f},
      {nui::Joint::kShoulderRight, 0.20f, 1.41f},
      {nui::Joint::kElbowLeft, -0.25f, 1.12f},
      {nui::Joint::kElbowRight, 0.25f, 1.12f},
      {nui::Joint::kWristLeft, -0.28f, 0.85f},
      {nui::Joint::kWristRight, 0.28f, 0.85f},
      {nui::Joint::kHandLeft, -0.29f, 0.73f},
      {nui::Joint::kHandRight, 0.29f, 0.73f},
      {nui::Joint::kHipLeft, -0.11f, 0.93f},
      {nui::Joint::kHipRight, 0.11f, 0.93f},
      {nui::Joint::kKneeLeft, -0.12f, 0.52f},
      {nui::Joint::kKneeRight, 0.12f, 0.52f},
      {nui::Joint::kAnkleLeft, -0.12f, 0.10f},
      {nui::Joint::kAnkleRight, 0.12f, 0.10f},
      {nui::Joint::kFootLeft, -0.12f, 0.02f},
      {nui::Joint::kFootRight, 0.12f, 0.02f},
  };
  for (const Rest& r : rest) {
    nui::NuiJoint& j = skeleton.joints[static_cast<uint16_t>(r.joint)];
    j.position[0] = r.x;
    j.position[1] = r.y;
    j.position[2] = 0.0f;
    j.pos_confidence = 1.0f;
    j.orient_confidence = 1.0f;
  }
  return skeleton;
}

std::string TempSession(const std::string& name) {
  return "./_test_inspect_" + name + ".nui";
}

}  // namespace

TEST_CASE("bones only reference real joints", "[inspect]") {
  for (const auto& bone : nui::SkeletonBones()) {
    REQUIRE(bone.first < nui::kJointCount);
    REQUIRE(bone.second < nui::kJointCount);
    REQUIRE(bone.first != bone.second);
  }
}

TEST_CASE("the skeleton is drawn, upright and mostly connected", "[inspect]") {
  const auto grid = nui::RenderSkeleton(StandingSkeleton(), 48, 22, 0.5f);
  REQUIRE(grid.size() == 22);
  for (const std::string& row : grid) {
    REQUIRE(row.size() == 48);
  }

  // The head is the highest joint and the feet the lowest, so the drawing must
  // reach near the top and near the bottom. Checked as a band rather than an
  // exact row: the renderer pads its bounds, so an exact row is an accident of
  // the padding factor, not a property worth pinning.
  auto topmost_drawn = [&](int within) {
    for (int y = 0; y < within && y < static_cast<int>(grid.size()); ++y) {
      for (char c : grid[static_cast<size_t>(y)]) {
        if (c != ' ') {
          return true;
        }
      }
    }
    return false;
  };
  auto bottommost_drawn = [&](int within) {
    for (int y = static_cast<int>(grid.size()) - 1; y >= static_cast<int>(grid.size()) - within; --y) {
      for (char c : grid[static_cast<size_t>(y)]) {
        if (c != ' ') {
          return true;
        }
      }
    }
    return false;
  };

  REQUIRE(topmost_drawn(4));
  REQUIRE(bottommost_drawn(4));

  // Every joint is confident in this pose, so every joint should land as '#'.
  // Counting them is a stronger check than "enough pixels were drawn": it
  // catches a projection that clips or collapses a specific joint.
  size_t joints_drawn = 0, total_drawn = 0;
  for (const std::string& row : grid) {
    for (char c : row) {
      if (c == '#') {
        ++joints_drawn;
      }
      if (c != ' ') {
        ++total_drawn;
      }
    }
  }
  REQUIRE(joints_drawn == nui::kJointCount);
  // Plus the 19 bones between them; a figure with no bone pixels would mean the
  // lines were not drawn at all even though the joints were.
  REQUIRE(total_drawn > joints_drawn);
}

TEST_CASE("left and right joints land on opposite sides", "[inspect]") {
  const auto grid = nui::RenderSkeleton(StandingSkeleton(), 48, 22, 0.5f);
  int leftmost = 48, rightmost = -1;
  for (const std::string& row : grid) {
    for (size_t x = 0; x < row.size(); ++x) {
      if (row[x] != ' ') {
        leftmost = std::min(leftmost, static_cast<int>(x));
        rightmost = std::max(rightmost, static_cast<int>(x));
      }
    }
  }
  REQUIRE(leftmost < rightmost);
  // Roughly symmetric figure, so the extents should be balanced about the
  // middle. Catches a flipped axis, which would put everything on one side.
  const int left_span = leftmost;
  const int right_span = 47 - rightmost;
  REQUIRE(left_span == Catch::Approx(right_span).margin(6));
}

TEST_CASE("a collapsed skeleton does not crash the renderer", "[inspect]") {
  nui::NuiSkeleton flat = {};
  flat.joints[0].position[0] = std::nanf("");
  const auto grid = nui::RenderSkeleton(flat, 32, 16, 0.5f);
  REQUIRE(grid.size() == 16);
}

TEST_CASE("confidence is reflected in the output", "[inspect]") {
  nui::NuiSkeleton skeleton = StandingSkeleton();
  skeleton.joints[static_cast<uint16_t>(nui::Joint::kElbowLeft)].pos_confidence = 0.1f;
  skeleton.joints[static_cast<uint16_t>(nui::Joint::kElbowRight)].pos_confidence = 0.9f;

  const std::string text = nui::DescribeSkeleton(skeleton, 0.5f);
  REQUIRE(text.find("joints=19/20") != std::string::npos);

  const auto grid = nui::RenderSkeleton(skeleton, 48, 22, 0.5f);
  bool any_low_confidence = false;
  for (const std::string& row : grid) {
    for (char c : row) {
      if (c == '+' || c == '.') {
        any_low_confidence = true;
      }
    }
  }
  REQUIRE(any_low_confidence);
}

TEST_CASE("the header line reports counts, latency and flags", "[inspect]") {
  nui::NuiFrameHeader header = nui::MakeHeader(7, 1000, 1500);
  header.num_skeletons = 2;
  header.flags = nui::kFrameFlagSynthetic | nui::kFrameFlagExtrapolated;

  const std::string text = nui::DescribeHeader(header);
  REQUIRE(text.find("frame=7") != std::string::npos);
  REQUIRE(text.find("skeletons=2") != std::string::npos);
  REQUIRE(text.find("latency=0.5ms") != std::string::npos);
  REQUIRE(text.find("synthetic") != std::string::npos);
  REQUIRE(text.find("extrapolated") != std::string::npos);
}

TEST_CASE("a frame with capture after host does not report negative latency",
          "[inspect]") {
  // Clocks can disagree; a negative latency in the output would read as a bug
  // in the provider rather than a clock mismatch.
  nui::NuiFrameHeader header = nui::MakeHeader(1, 9000, 1000);
  const std::string text = nui::DescribeHeader(header);
  REQUIRE(text.find("latency=0.0ms") != std::string::npos);
}

TEST_CASE("the inspector reads a session end to end", "[inspect]") {
  const std::string path = TempSession("session");
  std::remove(path.c_str());

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  const std::vector<nui::NuiSkeleton> skeletons(2, StandingSkeleton());
  for (uint32_t i = 0; i < 5; ++i) {
    nui::NuiFrameHeader header = nui::MakeHeader(i, i * 33333, i * 33333 + 100);
    REQUIRE(recorder.Append(header, skeletons.data(), 2));
  }
  REQUIRE(recorder.Close());

  nui::FrameSource source;
  REQUIRE(source.Open(path));
  REQUIRE(source.is_session());
  REQUIRE(source.total_frames() == 5);

  nui::SourceFrame frame;
  uint32_t seen = 0;
  while (source.Next(&frame)) {
    REQUIRE(frame.skeletons.size() == 2);
    REQUIRE(frame.ordinal == seen);
    REQUIRE(frame.header.frame_id == seen);
    ++seen;
  }
  REQUIRE(seen == 5);
  REQUIRE(source.error().empty());

  std::remove(path.c_str());
}

TEST_CASE("the inspector rejects something that is not a session", "[inspect]") {
  nui::FrameSource source;
  REQUIRE_FALSE(source.Open("./CMakeLists.txt"));
  REQUIRE_FALSE(source.error().empty());
}

TEST_CASE("the inspector reads a live IPC directory", "[inspect]") {
  const std::string dir = "./_test_inspect_ipc";
  std::remove((dir + "/frame.idx").c_str());
  std::remove((dir + "/frame.0.bin").c_str());
  std::remove((dir + "/frame.1.bin").c_str());

  nui::FileWriter writer;
  REQUIRE(writer.Open(dir));
  const std::vector<nui::NuiSkeleton> skeletons(1, StandingSkeleton());
  REQUIRE(writer.Write(nui::MakeHeader(1, 0, 0), skeletons.data(), 1));

  nui::FrameSource source;
  REQUIRE(source.Open(dir));
  REQUIRE_FALSE(source.is_session());

  nui::SourceFrame frame;
  REQUIRE(source.Next(&frame));
  REQUIRE(frame.skeletons.size() == 1);
  REQUIRE(frame.skeletons[0].tracking_id == 1);

  std::remove((dir + "/frame.idx").c_str());
  std::remove((dir + "/frame.0.bin").c_str());
  std::remove((dir + "/frame.1.bin").c_str());
  std::remove(dir.c_str());
}