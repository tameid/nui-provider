// Synthetic backend: a deterministic, scripted skeleton.
//
// Purpose is CI and the frame inspector - it must produce the same bytes on
// every machine and every run, so a test failure means a real change. It also
// happens to be the easiest way to see what a title does with a single known
// pose.

#ifndef NUI_BACKEND_SYNTHETIC_H_
#define NUI_BACKEND_SYNTHETIC_H_

#include <cstdint>
#include <vector>

#include "nui/frame.h"

namespace nui {

// A pose generator. Emits one or more skeletons with plausible-but-obviously
// synthetic motion: joints sway around a rest pose at a fixed rate.
//
// Every amplitude is small and the motion is smooth, because a real backend's
// whole difficulty is the opposite and a synthetic source that jitters would
// teach us the wrong things about what a title tolerates.
class SyntheticBackend {
 public:
  // `skeleton_count` skeletons, `hz` frames per second of internal clock.
  SyntheticBackend(uint32_t skeleton_count, uint32_t hz);

  // Advances internal time by one tick and fills `out`. Returns the number of
  // skeletons written. frame_id is updated and stamped on the header.
  uint32_t NextFrame(NuiFrameHeader* header, std::vector<NuiSkeleton>* out);

  void Reset();

  uint32_t frame_id() const { return frame_id_; }
  uint64_t capture_us() const { return capture_us_; }

 private:
  void FillSkeleton(NuiSkeleton* skeleton, uint32_t index, uint64_t t_us) const;

  uint32_t skeleton_count_;
  uint32_t hz_;
  uint32_t frame_id_ = 0;
  uint64_t capture_us_ = 0;
};

}  // namespace nui

#endif  // NUI_BACKEND_SYNTHETIC_H_