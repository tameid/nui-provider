#include "ipc/file_writer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "nui/encode.h"

namespace nui {
namespace {

constexpr uint32_t kSlotCount = 2;

std::string SlotPath(const std::string& directory, uint32_t slot) {
  return directory + "/frame." + std::to_string(slot) + ".bin";
}

std::string IndexPath(const std::string& directory) { return directory + "/frame.idx"; }

std::string IndexTempPath(const std::string& directory) {
  return directory + "/frame.idx.tmp";
}

}  // namespace

bool FileWriter::Open(const std::string& directory) {
  directory_ = directory;
  error_.clear();
  frames_written_ = 0;
  frames_dropped_ = 0;

  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec && !std::filesystem::is_directory(directory_)) {
    error_ = "cannot create " + directory_ + ": " + ec.message();
    return false;
  }

  // Start from a known state so a stale frame.idx from a previous run is not
  // mistaken for a live one.
  if (!PublishIndex(0)) {
    return false;
  }
  return true;
}

bool FileWriter::PublishIndex(uint32_t slot) {
  const std::string temp = IndexTempPath(directory_);
  const std::string final_path = IndexPath(directory_);
  const std::string text = std::to_string(slot) + "\n";

  FILE* file = std::fopen(temp.c_str(), "wb");
  if (!file) {
    error_ = "cannot write " + temp + ": " + std::strerror(errno);
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  std::fclose(file);
  if (!ok) {
    error_ = "short write to " + temp;
    return false;
  }

  // Rename-over is the atomic step. Use std::filesystem rather than
  // std::rename: MSVC's std::rename refuses to replace an existing file, and
  // frame.idx always exists by the second write. filesystem::rename is
  // specified to replace, and maps to MoveFileEx(REPLACE_EXISTING) here, which
  // is still atomic within a volume.
  std::error_code ec;
  std::filesystem::rename(temp, final_path, ec);
  if (ec) {
    error_ = "cannot replace " + final_path + ": " + ec.message();
    return false;
  }

  read_slot_ = slot;
  return true;
}

bool FileWriter::WriteSlot(uint32_t slot, const uint8_t* data, uint32_t size) {
  const std::string path = SlotPath(directory_, slot);
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    error_ = "cannot write " + path + ": " + std::strerror(errno);
    return false;
  }
  const bool ok = std::fwrite(data, 1, size, file) == size;
  // Flush before publishing the index: the guest may read the slot the instant
  // frame.idx changes, and a buffered write still in the CRT cache would be a
  // short frame.
  std::fflush(file);
  std::fclose(file);
  if (!ok) {
    error_ = "short write to " + path;
    return false;
  }
  return true;
}

bool FileWriter::Write(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
                       uint32_t skeleton_count) {
  const uint32_t needed = EncodedSize(skeleton_count);
  scratch_.resize(needed);

  const uint32_t written = EncodeFrame(header, skeletons, skeleton_count,
                                       static_cast<uint16_t>(header.flags),
                                       scratch_.data(),
                                       static_cast<uint32_t>(scratch_.size()));
  if (written == 0) {
    error_ = "encode failed for " + std::to_string(skeleton_count) + " skeletons";
    return false;
  }

  // Never write the slot the guest may be reading.
  const uint32_t slot = read_slot_ ^ 1u;
  if (!WriteSlot(slot, scratch_.data(), written)) {
    return false;
  }
  if (!PublishIndex(slot)) {
    return false;
  }

  ++frames_written_;
  return true;
}

}  // namespace nui