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

// An image a backend produced: the wire header plus a pointer to pixels the
// backend owns and keeps alive until the next call to NextFrame.
//
// The lifetime is the whole reason this is a separate struct rather than just
// NuiImage. A depth frame is 320x240x2 = 153600 bytes; copying that per frame
// would be the dominant cost of the program, and the backend is already holding
// it.
struct BackendImage {
  NuiImage header;
  const uint8_t* data = nullptr;
};

class Backend {
 public:
  virtual ~Backend() = default;

  // Produces the next frame. `skeletons` is resized to the skeleton count and
  // fully overwritten; `images` is cleared and refilled. Returns the number of
  // skeletons written.
  //
  // A backend may return 0 skeletons and still supply images. That is not a
  // degenerate case: depth is the primitive and skeleton is derived from it, so
  // a frame with depth but no tracked player is an ordinary thing to see.
  //
  // Frame ids and timestamps are monotonic within a backend. A backend that
  // replays a recording preserves the recorded values rather than restamping,
  // because a title that reacts to frame timing must see the original timing.
  virtual uint32_t NextFrame(NuiFrameHeader* header,
                             std::vector<NuiSkeleton>* skeletons,
                             std::vector<BackendImage>* images) = 0;

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