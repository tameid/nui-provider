// Replay backend: plays back a recorded ".nui" session.
//
// Exists for A/B testing and bug reproduction. Two properties matter more than
// anything else here:
//
//   1. Bytes out equal bytes in. Replay re-emits the recorded frames verbatim,
//      including their original frame ids and capture timestamps, rather than
//      restamping them. A title that reacts to timing - and they all do - must
//      see the timing that caused the original problem, or the reproduction is
//      fiction.
//
//   2. It cannot silently truncate. Every read is bounds-checked against the
//      frame count in the session header, and a short or corrupt file is an
//      error rather than a shorter replay.

#ifndef NUI_BACKEND_REPLAY_H_
#define NUI_BACKEND_REPLAY_H_

#include <cstdint>
#include <string>
#include <vector>

#include "backend.h"
#include "nui/decode.h"
#include "nui/session.h"

namespace nui {

class ReplayBackend : public Backend {
 public:
  // `loop` restarts from the first frame at the end instead of stopping.
  // Returns false and sets error() if the file is not a readable session.
  bool Open(const std::string& path, bool loop);

  uint32_t NextFrame(NuiFrameHeader* header, std::vector<NuiSkeleton>* out) override;

  const char* name() const override { return "replay"; }
  uint32_t frames_produced() const override { return emitted_; }

  void SetRealtime(bool realtime) override;

  uint32_t frame_count() const { return header_.frame_count; }
  uint32_t hz() const { return header_.hz; }
  uint32_t loops() const { return loops_; }

  // Interval between the current frame and the previous one, in microseconds,
  // taken from the recording. 0 for the first frame. Useful for pacing a
  // replay that was recorded as fast as possible.
  uint64_t current_interval_us() const { return current_interval_us_; }

  const std::string& error() const { return error_; }

 private:
  bool ReadNextRaw(std::vector<uint8_t>* payload, uint32_t* skeletons);
  bool Restart();

  std::string path_;
  FILE* file_ = nullptr;
  SessionHeader header_ = {};
  uint32_t cursor_ = 0;
  uint32_t emitted_ = 0;
  uint32_t loops_ = 0;
  uint64_t previous_capture_us_ = 0;
  uint64_t current_interval_us_ = 0;
  bool loop_ = false;
  bool realtime_ = false;
  std::string error_;
};

}  // namespace nui

#endif  // NUI_BACKEND_REPLAY_H_