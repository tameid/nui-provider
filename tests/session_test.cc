// Recording and replay tests.
//
// The property that makes a reproduction worth anything is that replay emits
// the same bytes that were recorded. Most of this file exists to hold that
// line: if a recording cannot be replayed byte-identically, a bug reproduced
// from it might be a different bug.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "backends/replay/replay.h"
#include "backends/synthetic/synthetic.h"
#include "nui/decode.h"
#include "nui/encode.h"
#include "nui/session.h"
#include "recorder/session_writer.h"

namespace {

std::string TempSession(const std::string& name) {
  return "./_test_session_" + name + ".nui";
}

void RemoveTemp(const std::string& path) { std::remove(path.c_str()); }

std::vector<uint8_t> ReadAllBytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

nui::NuiSkeleton MakeSkeleton(uint32_t tracking_id, float x) {
  nui::NuiSkeleton skeleton = {};
  skeleton.tracking_id = tracking_id;
  skeleton.position[0] = x;
  skeleton.position[1] = 0.9f;
  skeleton.up_vector[1] = 1.0f;
  skeleton.up_confidence = 1.0f;
  skeleton.joints[0].pos_confidence = 1.0f;
  skeleton.joints[1].position[0] = x + 0.1f;
  return skeleton;
}

}  // namespace

TEST_CASE("SessionHeader is 60 bytes", "[session][format]") {
  REQUIRE(sizeof(nui::SessionHeader) == 60);
}

TEST_CASE("session frame size matches the frozen frame encoding", "[session][format]") {
  REQUIRE(nui::SessionFrameSize(2) == nui::FrameSize(2));
}

TEST_CASE("record then replay reproduces frames byte for byte", "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("roundtrip");
  RemoveTemp(path);

  constexpr uint32_t kFrames = 12;
  constexpr uint32_t kSkeletons = 2;

  // Produce, record.
  nui::SyntheticBackend source(2, 30);
  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));

  std::vector<nui::NuiFrameHeader> written_headers;
  std::vector<std::vector<nui::NuiSkeleton>> written_skeletons;
  std::vector<nui::NuiSkeleton> skeletons;
  nui::NuiFrameHeader header = {};
  for (uint32_t i = 0; i < kFrames; ++i) {
    source.NextFrame(&header, &skeletons, &images);
    REQUIRE(skeletons.size() == kSkeletons);
    REQUIRE(recorder.Append(header, skeletons.data(),
                            static_cast<uint32_t>(skeletons.size())));
    written_headers.push_back(header);
    // Copy: `skeletons` is refilled by the next iteration.
    written_skeletons.push_back(skeletons);
  }
  REQUIRE(recorder.Close());
  REQUIRE(recorder.frames_recorded() == kFrames);

  // Replay and compare.
  nui::ReplayBackend replay;
  REQUIRE(replay.Open(path, false));
  REQUIRE(replay.frame_count() == kFrames);
  REQUIRE(replay.hz() == 30);

  std::vector<nui::NuiSkeleton> replayed;
  nui::NuiFrameHeader replay_header = {};
  for (uint32_t i = 0; i < kFrames; ++i) {
    const uint32_t count = replay.NextFrame(&replay_header, &replayed, &images);
    REQUIRE(count == kSkeletons);
    REQUIRE(replayed.size() == kSkeletons);

    // Timestamps and ids must survive untouched - that is the whole point.
    REQUIRE(replay_header.frame_id == written_headers[i].frame_id);
    REQUIRE(replay_header.capture_timestamp_us ==
            written_headers[i].capture_timestamp_us);
    REQUIRE(replay_header.host_timestamp_us == written_headers[i].host_timestamp_us);
    REQUIRE(replay_header.flags == written_headers[i].flags);

    REQUIRE(std::memcmp(replayed.data(), written_skeletons[i].data(),
                        kSkeletons * sizeof(nui::NuiSkeleton)) == 0);
  }

  RemoveTemp(path);
}

TEST_CASE("replay reports exhaustion instead of looping", "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("exhaust");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(1);
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  for (uint32_t i = 0; i < 3; ++i) {
    header.frame_id = i;
    REQUIRE(recorder.Append(header, skeletons.data(), 1));
  }
  REQUIRE(recorder.Close());

  nui::ReplayBackend replay;
  REQUIRE(replay.Open(path, false));
  std::vector<nui::NuiSkeleton> out;
  nui::NuiFrameHeader h = {};
  REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  // Fourth call: no more frames, and not an error.
  REQUIRE(replay.NextFrame(&h, &out, &images) == 0);
  REQUIRE(replay.error().empty());

  RemoveTemp(path);
}

TEST_CASE("replay loops when asked", "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("loop");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(1);
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  for (uint32_t i = 0; i < 2; ++i) {
    header.frame_id = i;
    REQUIRE(recorder.Append(header, skeletons.data(), 1));
  }
  REQUIRE(recorder.Close());

  nui::ReplayBackend replay;
  REQUIRE(replay.Open(path, true));
  std::vector<nui::NuiSkeleton> out;
  nui::NuiFrameHeader h = {};
  for (uint32_t i = 0; i < 6; ++i) {
    REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  }
  REQUIRE(replay.loops() == 2);
  REQUIRE(replay.error().empty());

  RemoveTemp(path);
}

TEST_CASE("replay exposes the recorded inter-frame interval", "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("interval");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(1);
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  for (uint32_t i = 0; i < 3; ++i) {
    header.capture_timestamp_us = i * 33333;
    REQUIRE(recorder.Append(header, skeletons.data(), 1));
  }
  REQUIRE(recorder.Close());

  nui::ReplayBackend replay;
  REQUIRE(replay.Open(path, false));
  std::vector<nui::NuiSkeleton> out;
  nui::NuiFrameHeader h = {};
  REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  REQUIRE(replay.current_interval_us() == 0);  // nothing precedes the first
  REQUIRE(replay.NextFrame(&h, &out, &images) == 1);
  REQUIRE(replay.current_interval_us() == 33333);

  RemoveTemp(path);
}

TEST_CASE("replay rejects a file that is not a session", "[session]") {
  std::vector<nui::BackendImage> images;
  nui::ReplayBackend replay;
  REQUIRE_FALSE(replay.Open("./CMakeLists.txt", false));
  REQUIRE_FALSE(replay.error().empty());

  REQUIRE_FALSE(replay.Open("./_does_not_exist.nui", false));
  REQUIRE_FALSE(replay.error().empty());
}

TEST_CASE("a recording with no frames is an error, not an empty replay",
          "[session]") {
  std::vector<nui::BackendImage> images;
  // What a recording killed before Close looks like. Replaying it silently
  // would look like a title that ignored every frame.
  const std::string path = TempSession("empty");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  REQUIRE(recorder.Close());  // zero frames

  nui::ReplayBackend replay;
  REQUIRE_FALSE(replay.Open(path, false));
  REQUIRE(replay.error().find("no frames") != std::string::npos);

  RemoveTemp(path);
}

TEST_CASE("a truncated session is detected rather than half-replayed",
          "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("truncated");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(2);
  nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);
  for (uint32_t i = 0; i < 4; ++i) {
    header.frame_id = i;
    REQUIRE(recorder.Append(header, skeletons.data(), 2));
  }
  REQUIRE(recorder.Close());

  // Chop the payload off the end, leaving the header claiming more frames.
  auto bytes = ReadAllBytes(path);
  REQUIRE(bytes.size() > sizeof(nui::SessionHeader) + nui::FrameSize(2));
  const uint32_t truncated = static_cast<uint32_t>(bytes.size() - nui::FrameSize(2) - 10);
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), truncated);
  }

  nui::ReplayBackend replay;
  REQUIRE(replay.Open(path, false));
  std::vector<nui::NuiSkeleton> out;
  nui::NuiFrameHeader h = {};
  uint32_t emitted = 0;
  while (replay.NextFrame(&h, &out, &images) != 0) {
    ++emitted;
    REQUIRE(emitted < 16);  // must not spin
  }
  REQUIRE(emitted < 4);           // did not honour the claimed count
  REQUIRE_FALSE(replay.error().empty());

  RemoveTemp(path);
}

TEST_CASE("recording refuses to clobber an existing session", "[session]") {
  std::vector<nui::BackendImage> images;
  const std::string path = TempSession("clobber");
  RemoveTemp(path);

  nui::SessionWriter first;
  REQUIRE(first.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(1);
  REQUIRE(first.Append(nui::MakeHeader(0, 0, 0), skeletons.data(), 1));
  REQUIRE(first.Close());

  // A second recording to the same path would destroy the only copy of a
  // reproduction.
  nui::SessionWriter second;
  REQUIRE_FALSE(second.Open(path, "synthetic", 30));
  REQUIRE_FALSE(second.error().empty());

  RemoveTemp(path);
}

TEST_CASE("recorded frames decode through the frozen codec", "[session]") {
  std::vector<nui::BackendImage> images;
  // Recording must not invent a second encoding. If a session frame and an IPC
  // frame are the same bytes, a recorded reproduction is faithful.
  const std::string path = TempSession("decode");
  RemoveTemp(path);

  nui::SessionWriter recorder;
  REQUIRE(recorder.Open(path, "synthetic", 30));
  std::vector<nui::NuiSkeleton> skeletons(1, MakeSkeleton(3, 1.25f));
  const nui::NuiFrameHeader header = nui::MakeHeader(9, 111, 222);
  REQUIRE(recorder.Append(header, skeletons.data(), 1));
  REQUIRE(recorder.Close());

  const auto bytes = ReadAllBytes(path);
  const size_t payload_offset = sizeof(nui::SessionHeader);
  const nui::DecodedFrame decoded =
      nui::DecodeFrame(bytes.data() + payload_offset,
                       static_cast<uint32_t>(bytes.size() - payload_offset));
  REQUIRE(decoded.valid);
  REQUIRE(decoded.header.frame_id == 9);
  REQUIRE(decoded.header.capture_timestamp_us == 111);
  REQUIRE(decoded.skeleton_count == 1);
  REQUIRE(decoded.skeletons[0].tracking_id == 3);
  REQUIRE(decoded.skeletons[0].position[0] == Catch::Approx(1.25f));

  RemoveTemp(path);
}
