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

#include "backend.h"
#include "nui/frame.h"

namespace nui {

// A pose generator. Emits one or more skeletons with plausible-but-obviously
// synthetic motion: joints sway around a rest pose at a fixed rate.
//
// Every amplitude is small and the motion is smooth, because a real backend's
// whole difficulty is the opposite and a synthetic source that jitters would
// teach us the wrong things about what a title tolerates.
class SyntheticBackend : public Backend {
 public:
  // `skeleton_count` skeletons, `hz` frames per second of internal clock.
  SyntheticBackend(uint32_t skeleton_count, uint32_t hz);

  uint32_t NextFrame(NuiFrameHeader* header,
                     std::vector<NuiSkeleton>* skeletons,
                     std::vector<BackendImage>* images) override;

  const char* name() const override { return "synthetic"; }
  uint32_t frames_produced() const override { return frame_id_; }

  void Reset();

  uint32_t frame_id() const { return frame_id_; }
  uint64_t capture_us() const { return capture_us_; }

  // Depth is off by default so existing recordings and the skeleton's frame size
  // are unchanged unless asked for. Callers that want the pipeline ordered
  // depth-first turn it on.
  void set_depth(bool enabled) { depth_enabled_ = enabled; }
  bool depth() const { return depth_enabled_; }

 private:
  void FillSkeleton(NuiSkeleton* skeleton, uint32_t index, uint64_t t_us) const;
  // Renders a depth image consistent with `skeletons`. The consistency is the
  // point: a title that cross-checks a joint against the depth image behind it
  // should find them agreeing.
  void RenderDepth(const std::vector<NuiSkeleton>& skeletons);

  uint32_t skeleton_count_;
  uint32_t hz_;
  uint32_t frame_id_ = 0;
  uint64_t capture_us_ = 0;
  bool depth_enabled_ = false;
  // Owned here because the image pointer handed to the caller must stay valid
  // until the next NextFrame. Reused across frames to keep this allocation-free
  // in steady state.
  std::vector<uint8_t> depth_pixels_;
};

}  // namespace nui

#endif  // NUI_BACKEND_SYNTHETIC_H_