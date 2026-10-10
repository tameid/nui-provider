/**
 * Frame inspector.
 *
 * Reads what nui-provider produced - either a live IPC directory or a recorded
 * .nui session - and makes it legible. There is no GUI here on purpose: the
 * common case is "a title is misbehaving and I need to see what the provider
 * actually sent", and the fastest way to answer that is text in a terminal,
 * including over a log.
 *
 * The ASCII skeleton view is the reason this tool earns its place. A skeleton
 * that is subtly wrong - an elbow in the wrong place, a flipped axis, a joint
 * at zero confidence - is obvious the moment it is drawn and invisible in a
 * table of floats.
 */

#ifndef NUI_TOOLS_FRAME_SOURCE_H_
#define NUI_TOOLS_FRAME_SOURCE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "nui/frame.h"
#include "nui/session.h"

namespace nui {

// A frame plus where it came from, so the caller can label output and tell a
// replayed frame from a freshly published one.
struct SourceFrame {
  NuiFrameHeader header = {};
  std::vector<NuiSkeleton> skeletons;
  uint64_t ordinal = 0;  // 0-based position in the source
};

// Reads either a .nui session or a live IPC directory.
//
// The two are unified deliberately: they contain the same bytes, so anything
// that can inspect a recording should be able to inspect what is being published
// right now, and vice versa. If it cannot, the recording is not a faithful
// reproduction.
class FrameSource {
 public:
  // `path` is either a file (session) or a directory (IPC).
  bool Open(const std::string& path);

  // Returns false at end of stream, or on error. Check error() to tell which.
  bool Next(SourceFrame* out);

  uint32_t total_frames() const { return total_frames_; }
  bool is_session() const { return is_session_; }
  const std::string& path() const { return path_; }
  const std::string& error() const { return error_; }

  // For a live IPC directory: re-read frame.idx and reload if the writer has
  // published a newer slot. A no-op for sessions.
  void Refresh();

 private:
  bool ReadSessionFrame(SourceFrame* out);
  bool ReadIpcFrame(SourceFrame* out);
  bool LoadIpcSlot(uint32_t slot);

  std::string path_;
  bool is_session_ = false;
  FILE* file_ = nullptr;

  SessionHeader session_ = {};
  uint32_t cursor_ = 0;
  uint32_t total_frames_ = 0;

  // IPC state
  uint32_t ipc_slot_ = 0xFFFFFFFFu;
  std::vector<uint8_t> ipc_buffer_;
  NuiFrameHeader ipc_header_ = {};
  std::vector<NuiSkeleton> ipc_skeletons_;

  std::string error_;
};

}  // namespace nui

#endif  // NUI_TOOLS_FRAME_SOURCE_H_