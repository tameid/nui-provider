// IPC tests.
//
// The protocol's whole value is that the guest never sees a torn frame, so
// these tests check the observable consequences of that: frame.idx always names
// a slot that is fully written, and it alternates.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ipc/file_writer.h"
#include "nui/decode.h"
#include "nui/encode.h"
#include "nui/frame.h"

namespace {

std::string ReadAllText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadAllBytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

uint32_t ReadIndex(const std::string& dir) {
  const std::string text = ReadAllText(dir + "/frame.idx");
  if (text.empty()) {
    return 0xFFFFFFFFu;
  }
  return static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

std::string TempDir(const std::string& name) {
  return "./_test_tmp_" + name;
}

void RemoveTemp(const std::string& dir) {
  std::remove((dir + "/frame.idx").c_str());
  std::remove((dir + "/frame.idx.tmp").c_str());
  std::remove((dir + "/frame.0.bin").c_str());
  std::remove((dir + "/frame.1.bin").c_str());
  std::remove(dir.c_str());
}

nui::NuiSkeleton MakeSkeleton(uint32_t tracking_id) {
  nui::NuiSkeleton skeleton = {};
  skeleton.tracking_id = tracking_id;
  skeleton.position[1] = 0.9f;
  skeleton.up_vector[1] = 1.0f;
  skeleton.up_confidence = 1.0f;
  skeleton.joints[0].pos_confidence = 1.0f;
  return skeleton;
}

}  // namespace

TEST_CASE("Open creates the directory and an initial index", "[ipc]") {
  const std::string dir = TempDir("open");
  RemoveTemp(dir);

  nui::FileWriter writer;
  REQUIRE(writer.Open(dir));
  REQUIRE(ReadIndex(dir) == 0);
  REQUIRE(writer.error().empty());

  RemoveTemp(dir);
}

TEST_CASE("a published frame is readable and complete", "[ipc]") {
  const std::string dir = TempDir("publish");
  RemoveTemp(dir);

  nui::FileWriter writer;
  REQUIRE(writer.Open(dir));

  const std::vector<nui::NuiSkeleton> skeletons{MakeSkeleton(11)};
  nui::NuiFrameHeader header = nui::MakeHeader(5, 100, 200);
  REQUIRE(writer.Write(header, skeletons.data(), 1));

  const uint32_t slot = ReadIndex(dir);
  REQUIRE(slot <= 1);

  const auto bytes = ReadAllBytes(dir + "/frame." + std::to_string(slot) + ".bin");
  const nui::DecodedFrame decoded = nui::DecodeFrame(bytes.data(),
                                                    static_cast<uint32_t>(bytes.size()));
  REQUIRE(decoded.valid);
  REQUIRE(decoded.header.frame_id == 5);
  REQUIRE(decoded.skeleton_count == 1);
  REQUIRE(decoded.skeletons[0].tracking_id == 11);

  RemoveTemp(dir);
}

TEST_CASE("the index alternates so the guest is never overwritten", "[ipc]") {
  const std::string dir = TempDir("alternate");
  RemoveTemp(dir);

  nui::FileWriter writer;
  REQUIRE(writer.Open(dir));

  const std::vector<nui::NuiSkeleton> skeletons{MakeSkeleton(1)};
  const nui::NuiFrameHeader header = nui::MakeHeader(0, 0, 0);

  uint32_t previous = ReadIndex(dir);
  for (int i = 0; i < 4; ++i) {
    REQUIRE(writer.Write(header, skeletons.data(), 1));
    const uint32_t current = ReadIndex(dir);
    REQUIRE(current != previous);
    REQUIRE(current <= 1);
    previous = current;
  }
  REQUIRE(writer.frames_written() == 4);

  RemoveTemp(dir);
}

TEST_CASE("no temp index is left behind after a write", "[ipc]") {
  const std::string dir = TempDir("notmp");
  RemoveTemp(dir);

  nui::FileWriter writer;
  REQUIRE(writer.Open(dir));

  const std::vector<nui::NuiSkeleton> skeletons{MakeSkeleton(1)};
  REQUIRE(writer.Write(nui::MakeHeader(1, 0, 0), skeletons.data(), 1));

  // frame.idx.tmp is renamed over frame.idx, so it must not survive.
  REQUIRE(ReadAllText(dir + "/frame.idx.tmp").empty());

  RemoveTemp(dir);
}

TEST_CASE("Open reports a bad path instead of throwing", "[ipc]") {
  // A plain deep path is not a failure - create_directories makes it. The real
  // failure is a path whose parent is a regular file, which no mkdir can fix.
  const std::string blocker = TempDir("blocker");
  RemoveTemp(blocker);
  {
    FILE* file = std::fopen(blocker.c_str(), "wb");
    REQUIRE(file != nullptr);
    std::fclose(file);
  }

  nui::FileWriter writer;
  REQUIRE_FALSE(writer.Open(blocker + "/under"));
  REQUIRE_FALSE(writer.error().empty());

  std::remove(blocker.c_str());
}