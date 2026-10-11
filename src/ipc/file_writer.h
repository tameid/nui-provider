// File IPC: the double-buffered frame handoff.
//
// Protocol (docs/kinect_support.md 4, "kinect plugin"):
//
//   <dir>/frame.idx      one line: the slot the guest should read next
//   <dir>/frame.0.bin    encoded NuiFrameHeader + NuiSkeletons
//   <dir>/frame.1.bin
//
// The writer never mutates the slot the guest is reading. It fills the other
// one, flushes it to disk, and only then replaces frame.idx. frame.idx is
// replaced by writing a temp file and renaming it, because a rename is atomic
// within a volume - a plain rewrite can be observed half-written, and the
// guest would then read a torn index.
//
// Latest-frame-wins: if the guest falls behind, stale frames are overwritten
// rather than queued. A Kinect consumer that gets a queue is a consumer with
// growing latency, which is worse than one that skips frames.

#ifndef NUI_IPC_FILE_WRITER_H_
#define NUI_IPC_FILE_WRITER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "backend.h"
#include "nui/frame.h"

namespace nui {

class FileWriter {
 public:
  // `directory` is created if missing. Returns false and sets error() on
  // failure; there is no exception, because a provider that dies because a
  // path is read-only is worse than one that reports it.
  bool Open(const std::string& directory);

  // Encodes and publishes one frame. `header`, `skeletons` and `images` are only
  // read during the call. Returns false on I/O failure.
  //
  // The overload without images is the skeleton-only case and is kept because
  // that is still the common one; it produces a frame with all three image
  // counts zero.
  bool Write(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
             uint32_t skeleton_count);

  bool Write(const NuiFrameHeader& header, const NuiSkeleton* skeletons,
             uint32_t skeleton_count, const BackendImage* images,
             uint32_t image_count);

  // Where the guest should look, for docs and error messages.
  const std::string& directory() const { return directory_; }

  uint32_t frames_written() const { return frames_written_; }
  uint32_t frames_dropped() const { return frames_dropped_; }
  const std::string& error() const { return error_; }

 private:
  bool WriteSlot(uint32_t slot, const uint8_t* data, uint32_t size);
  bool PublishIndex(uint32_t slot);

  std::string directory_;
  // Slot the guest is currently expected to be reading.
  uint32_t read_slot_ = 0;
  uint32_t frames_written_ = 0;
  uint32_t frames_dropped_ = 0;
  std::string error_;
  std::vector<uint8_t> scratch_;
};

}  // namespace nui

#endif  // NUI_IPC_FILE_WRITER_H_