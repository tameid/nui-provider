// The interface every backend implements.
//
// Deliberately tiny: a backend produces frames and nothing else. Recording is
// handled by the provider around this, not by wrapping backends, so adding a
// backend cannot accidentally change how recording behaves.

#ifndef NUI_BACKEND_H_
#define NUI_BACKEND_H_

#include <cstdint>
#include <string>
#include <vector>

#include "nui/frame.h"

namespace nui {

class Backend {
 public:
  virtual ~Backend() = default;

  // Produces the next frame. `out` is resized to the skeleton count and fully
  // overwritten. Returns the number of skeletons written.
  //
  // Frame ids and timestamps are monotonic within a backend. A backend that
  // replays a recording preserves the recorded values rather than restamping,
  // because a title that reacts to frame timing must see the original timing.
  virtual uint32_t NextFrame(NuiFrameHeader* header,
                             std::vector<NuiSkeleton>* out) = 0;

  // Short identifier recorded in the session header, e.g. "synthetic".
  virtual const char* name() const = 0;

  // Frames produced so far, for the session header.
  virtual uint32_t frames_produced() const = 0;

  // Set by the provider. Backends that pace themselves (a real camera does; a
  // recording does not) can consult it, though Phase 1 backends emit as fast
  // as asked and the provider does the sleeping.
  virtual void SetRealtime(bool realtime) { realtime_ = realtime; }
  bool realtime() const { return realtime_; }

 protected:
  bool realtime_ = false;
};

// True if `path` opens and its header identifies it as a session recording.
// Used so `--record` refuses to silently overwrite a recording that was
// probably meant to be replayed.
bool LooksLikeSession(const std::string& path);

}  // namespace nui

#endif  // NUI_BACKEND_H_