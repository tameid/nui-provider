#include "recorder/session_writer.h"

#include <chrono>

#include "backend.h"

namespace nui {
namespace {

uint64_t NowUnixUs() {
  const auto since = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(since).count());
}

}  // namespace

SessionWriter::~SessionWriter() {
  if (file_) {
    // Destructor path: recording was abandoned. Close anyway so the bytes
    // written so far are readable, even though frame_count is stale and the
    // file will not replay past what it claims.
    Close();
  }
}

bool SessionWriter::Open(const std::string& path, const char* backend, uint32_t hz) {
  path_ = path;
  error_.clear();
  frames_recorded_ = 0;
  max_skeletons_ = 0;
  started_unix_us_ = NowUnixUs();

  if (LooksLikeSession(path_)) {
    error_ = path_ + " already exists and looks like a session; refusing to overwrite";
    return false;
  }

  file_ = std::fopen(path_.c_str(), "wb+");
  if (!file_) {
    error_ = "cannot open " + path_ + " for writing";
    return false;
  }

  SessionHeader header = {};
  header.magic = kSessionMagic;
  header.version = kSessionVersion;
  header.flags = kSessionFlagRealtime;
  header.frame_count = 0;  // patched by Close
  header.max_skeletons = 0;
  header.hz = hz;
  header.created_unix_us = started_unix_us_;
  std::snprintf(header.backend, kBackendNameLength, "%s", backend ? backend : "unknown");

  if (std::fwrite(&header, sizeof(header), 1, file_) != 1) {
    error_ = "short write of session header to " + path_;
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return true;
}

bool SessionWriter::Append(const NuiFrameHeader& header,
                           const NuiSkeleton* skeletons, uint32_t skeleton_count) {
  if (!file_) {
    error_ = "not open";
    return false;
  }
  // Stamp the count rather than trusting the caller's header: a backend that
  // forgot to set it would otherwise write a file that will not decode.
  NuiFrameHeader stamped = header;
  stamped.magic = kFrameMagic;
  stamped.version = kVersion1;
  stamped.num_skeletons = skeleton_count;

  if (std::fwrite(&stamped, sizeof(stamped), 1, file_) != 1) {
    error_ = "short write of frame header";
    return false;
  }
  if (skeleton_count && skeletons) {
    const size_t bytes = static_cast<size_t>(skeleton_count) * sizeof(NuiSkeleton);
    if (std::fwrite(skeletons, 1, bytes, file_) != bytes) {
      error_ = "short write of frame payload";
      return false;
    }
  }

  if (skeleton_count > max_skeletons_) {
    max_skeletons_ = skeleton_count;
  }
  ++frames_recorded_;
  return true;
}

bool SessionWriter::Close() {
  if (!file_) {
    return true;
  }

  bool ok = true;

  // Patch frame_count and max_skeletons into the header we already wrote.
  SessionHeader header = {};
  if (std::fseek(file_, 0, SEEK_SET) == 0) {
    if (std::fread(&header, sizeof(header), 1, file_) != 1) {
      ok = false;
    } else {
      header.frame_count = frames_recorded_;
      header.max_skeletons = max_skeletons_;
      if (std::fseek(file_, 0, SEEK_SET) != 0 ||
          std::fwrite(&header, sizeof(header), 1, file_) != 1) {
        error_ = "cannot patch session header in " + path_;
        ok = false;
      }
    }
  }

  if (std::fclose(file_) != 0) {
    error_ = "cannot close " + path_;
    ok = false;
  }
  file_ = nullptr;
  return ok;
}

bool LooksLikeSession(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return false;
  }
  SessionHeader header = {};
  const bool read = std::fread(&header, sizeof(header), 1, file) == 1;
  std::fclose(file);
  return read && SessionHeaderLooksValid(header);
}

}  // namespace nui