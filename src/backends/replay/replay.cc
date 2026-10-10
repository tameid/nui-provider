#include "backends/replay/replay.h"

#include <cstdio>
#include <cstring>

namespace nui {

bool ReplayBackend::Open(const std::string& path, bool loop) {
  path_ = path;
  loop_ = loop;
  error_.clear();
  cursor_ = 0;
  emitted_ = 0;
  loops_ = 0;
  previous_capture_us_ = 0;
  current_interval_us_ = 0;

  file_ = std::fopen(path_.c_str(), "rb");
  if (!file_) {
    error_ = "cannot open " + path_;
    return false;
  }

  if (std::fread(&header_, sizeof(header_), 1, file_) != 1) {
    error_ = path_ + " is too small to be a session";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  if (!SessionHeaderLooksValid(header_)) {
    error_ = path_ + " is not an NUI session (bad magic or version)";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  if (header_.frame_count == 0) {
    // Almost always a recording that was killed before Close patched the
    // count. Say so, rather than replaying nothing and looking like a title
    // that ignored us.
    error_ = path_ + " contains no frames (recording may not have finished)";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return true;
}

void ReplayBackend::SetRealtime(bool realtime) {
  realtime_ = realtime;
  Backend::SetRealtime(realtime);
}

bool ReplayBackend::Restart() {
  if (std::fseek(file_, 0, SEEK_END) != 0) {
    error_ = "cannot seek " + path_;
    return false;
  }
  // Skip past the header we already hold, rather than re-reading it: the bytes
  // cannot have changed, and this keeps cursor_ and the file position agreed.
  if (std::fseek(file_, static_cast<long>(sizeof(SessionHeader)), SEEK_SET) != 0) {
    error_ = "cannot rewind " + path_;
    return false;
  }
  cursor_ = 0;
  ++loops_;
  previous_capture_us_ = 0;
  return true;
}

bool ReplayBackend::ReadNextRaw(std::vector<uint8_t>* payload, uint32_t* skeletons) {
  if (cursor_ >= header_.frame_count) {
    return false;
  }

  NuiFrameHeader frame = {};
  if (std::fread(&frame, sizeof(frame), 1, file_) != 1) {
    error_ = path_ + " ended early at frame " + std::to_string(cursor_);
    return false;
  }
  if (!HeaderLooksValid(frame)) {
    error_ = path_ + " frame " + std::to_string(cursor_) + " has a bad header";
    return false;
  }

  const size_t bytes = static_cast<size_t>(frame.num_skeletons) * sizeof(NuiSkeleton);
  payload->assign(sizeof(frame) + bytes, 0);
  std::memcpy(payload->data(), &frame, sizeof(frame));
  if (bytes) {
    if (std::fread(payload->data() + sizeof(frame), 1, bytes, file_) != bytes) {
      error_ = path_ + " frame " + std::to_string(cursor_) + " payload is truncated";
      return false;
    }
  }

  *skeletons = frame.num_skeletons;
  ++cursor_;
  return true;
}

uint32_t ReplayBackend::NextFrame(NuiFrameHeader* header,
                                  std::vector<NuiSkeleton>* out) {
  if (!file_) {
    error_ = "not open";
    return 0;
  }

  std::vector<uint8_t> payload;
  uint32_t skeleton_count = 0;

  if (cursor_ >= header_.frame_count) {
    if (!loop_) {
      // End of session. Signal it by clearing the header, which is how the
      // provider tells "no more frames" from "a frame with zero skeletons".
      *header = NuiFrameHeader{};
      out->clear();
      return 0;
    }
    if (!Restart()) {
      return 0;
    }
  }

  if (!ReadNextRaw(&payload, &skeleton_count)) {
    if (!loop_ && error_.empty()) {
      error_ = "replay ended at frame " + std::to_string(cursor_);
    }
    return 0;
  }

  const DecodedFrame decoded =
      DecodeFrame(payload.data(), static_cast<uint32_t>(payload.size()));
  if (!decoded.valid) {
    error_ = path_ + " frame " + std::to_string(cursor_ - 1) + " did not decode";
    return 0;
  }

  *header = decoded.header;
  current_interval_us_ = 0;
  if (decoded.header.capture_timestamp_us > previous_capture_us_) {
    current_interval_us_ = decoded.header.capture_timestamp_us - previous_capture_us_;
  }
  previous_capture_us_ = decoded.header.capture_timestamp_us;

  out->assign(skeleton_count, NuiSkeleton{});
  if (skeleton_count) {
    std::memcpy(out->data(), decoded.skeletons,
                static_cast<size_t>(skeleton_count) * sizeof(NuiSkeleton));
  }
  ++emitted_;
  return skeleton_count;
}

}  // namespace nui