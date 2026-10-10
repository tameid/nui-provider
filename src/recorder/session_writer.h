// Session recording.
//
// Appends frames to a ".nui" file as they are produced. The header is written
// with frame_count zero and patched on Close, because the count is not known
// until recording stops; the file is therefore only valid once Close has run,
// which is why a recording that is killed leaves an unreadable file rather than
// one that silently looks complete.

#ifndef NUI_RECORDER_SESSION_WRITER_H_
#define NUI_RECORDER_SESSION_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <string>

#include "nui/frame.h"
#include "nui/session.h"

namespace nui {

class SessionWriter {
 public:
  ~SessionWriter();

  // `backend` is the producing backend's name, `hz` its capture rate.
  bool Open(const std::string& path, const char* backend, uint32_t hz);
  bool is_open() const { return file_ != nullptr; }

  // Appends one frame exactly as it will be replayed.
  bool Append(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
              uint32_t skeleton_count);

  // Patches the frame count into the header and closes. Safe to call twice.
  bool Close();

  uint32_t frames_recorded() const { return frames_recorded_; }
  uint32_t max_skeletons() const { return max_skeletons_; }
  const std::string& error() const { return error_; }

 private:
  FILE* file_ = nullptr;
  std::string path_;
  uint32_t frames_recorded_ = 0;
  uint32_t max_skeletons_ = 0;
  uint64_t started_unix_us_ = 0;
  std::string error_;
};

}  // namespace nui

#endif  // NUI_RECORDER_SESSION_WRITER_H_