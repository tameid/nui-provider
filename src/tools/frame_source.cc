#include "tools/frame_source.h"

#include <cstdio>
#include <cstring>

#include "nui/decode.h"

namespace nui {
namespace {

std::string SlotPath(const std::string& dir, uint32_t slot) {
  return dir + "/frame." + std::to_string(slot) + ".bin";
}

std::string IndexPath(const std::string& dir) { return dir + "/frame.idx"; }

bool IsDirectory(const std::string& path) {
  // Avoid <filesystem> here: this is on the hot path of --follow and the
  // stat-based check is a single call.
  FILE* probe = std::fopen(IndexPath(path).c_str(), "rb");
  if (probe) {
    std::fclose(probe);
    return true;
  }
  return false;
}

bool ReadIndexSlot(const std::string& dir, uint32_t* slot) {
  FILE* file = std::fopen(IndexPath(dir).c_str(), "rb");
  if (!file) {
    return false;
  }
  unsigned long value = 0;
  const int matched = std::fscanf(file, "%lu", &value);
  std::fclose(file);
  if (matched != 1) {
    return false;
  }
  *slot = static_cast<uint32_t>(value);
  return true;
}

}  // namespace

bool FrameSource::Open(const std::string& path) {
  path_ = path;
  error_.clear();

  if (IsDirectory(path)) {
    is_session_ = false;
    total_frames_ = 0;  // live: no end
    return true;
  }

  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) {
    error_ = "cannot open " + path;
    return false;
  }
  if (std::fread(&session_, sizeof(session_), 1, file_) != 1) {
    error_ = path + " is too small to be a session";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  if (!SessionHeaderLooksValid(session_)) {
    error_ = path + " is not an NUI session (bad magic or version)";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  is_session_ = true;
  total_frames_ = session_.frame_count;
  return true;
}

void FrameSource::Refresh() {
  // Only sessions hold a FILE*. A live directory is re-read through frame.idx,
  // so testing file_ here would skip every directory source.
  if (is_session_) {
    return;
  }
  uint32_t slot = 0xFFFFFFFFu;
  if (!ReadIndexSlot(path_, &slot)) {
    return;
  }
  if (slot != ipc_slot_) {
    LoadIpcSlot(slot);
  }
}

bool FrameSource::LoadIpcSlot(uint32_t slot) {
  const std::string path = SlotPath(path_, slot);
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    // The writer may be mid-publish; not an error worth surfacing.
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long size = std::ftell(file);
  if (size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }

  ipc_buffer_.resize(static_cast<size_t>(size));
  const size_t got = std::fread(ipc_buffer_.data(), 1, ipc_buffer_.size(), file);
  std::fclose(file);
  if (got != ipc_buffer_.size()) {
    return false;
  }

  const DecodedFrame decoded =
      DecodeFrame(ipc_buffer_.data(), static_cast<uint32_t>(ipc_buffer_.size()));
  if (!decoded.valid) {
    return false;
  }

  ipc_header_ = decoded.header;
  ipc_skeletons_.assign(decoded.skeletons, decoded.skeletons + decoded.skeleton_count);
  ipc_slot_ = slot;
  return true;
}

bool FrameSource::ReadSessionFrame(SourceFrame* out) {
  NuiFrameHeader header = {};
  if (std::fread(&header, sizeof(header), 1, file_) != 1) {
    return false;  // clean end of stream
  }
  if (!HeaderLooksValid(header)) {
    error_ = path_ + " frame " + std::to_string(cursor_) + " has a bad header";
    return false;
  }

  const size_t bytes = static_cast<size_t>(header.num_skeletons) * sizeof(NuiSkeleton);
  std::vector<uint8_t> payload(sizeof(header) + bytes);
  std::memcpy(payload.data(), &header, sizeof(header));
  if (bytes && std::fread(payload.data() + sizeof(header), 1, bytes, file_) != bytes) {
    error_ = path_ + " frame " + std::to_string(cursor_) + " is truncated";
    return false;
  }

  const DecodedFrame decoded =
      DecodeFrame(payload.data(), static_cast<uint32_t>(payload.size()));
  if (!decoded.valid) {
    error_ = path_ + " frame " + std::to_string(cursor_) + " did not decode";
    return false;
  }

  out->header = decoded.header;
  out->skeletons.assign(decoded.skeletons,
                         decoded.skeletons + decoded.skeleton_count);
  out->ordinal = cursor_++;
  return true;
}

bool FrameSource::ReadIpcFrame(SourceFrame* out) {
  Refresh();
  if (ipc_slot_ == 0xFFFFFFFFu) {
    return false;
  }
  out->header = ipc_header_;
  out->skeletons = ipc_skeletons_;
  out->ordinal = total_frames_++;
  return true;
}

bool FrameSource::Next(SourceFrame* out) {
  if (!error_.empty()) {
    return false;
  }
  return is_session_ ? ReadSessionFrame(out) : ReadIpcFrame(out);
}

}  // namespace nui