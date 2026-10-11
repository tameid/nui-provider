/**
 * nui-provider - Phase 1.
 *
 * Generates guest-shaped NUI skeleton frames and publishes them over the
 * double-buffered file IPC that the xenia Kinect plugin reads.
 *
 * Phase 1 ships `synthetic` only. The point is to freeze the wire format and
 * prove the handoff works before any real pose front-end exists, so a title
 * can be driven with a known-good skeleton before there is a camera in the
 * loop.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend.h"
#include "backends/replay/replay.h"
#include "backends/synthetic/synthetic.h"
#include "ipc/file_writer.h"
#include "nui/frame.h"
#include "recorder/session_writer.h"

namespace {

void PrintUsage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "Source (choose one):\n"
      "  --replay <path>      play back a recorded .nui session\n"
      "                       (default source is the synthetic backend)\n"
      "  --skeletons <n>      skeletons the synthetic backend emits  (default 1)\n"
      "  --depth              also emit a 320x240 depth image with the synthetic\n"
      "                       backend. Off by default: depth is the primitive\n"
      "                       skeleton is derived from, so it is the payload\n"
      "                       that matches the real device boundary\n"
      "  --hz <n>             synthetic capture rate                   (default 30)\n"
      "\n"
      "Output:\n"
      "  --dir <path>         IPC directory to publish frames into (required)\n"
      "  --record <path>      also record everything produced to a .nui session\n"
      "\n"
      "Pacing:\n"
      "  --interval-ms <n>    wall-clock sleep between frames  (default 0,\n"
      "                       i.e. as fast as the disk allows)\n"
      "  --loop               with --replay, restart at the end instead of stopping\n"
      "  --frames <n>         stop after n frames, 0 = until the source ends\n"
      "                       (default 0)\n"
      "\n"
      "  --help               this text\n"
      "\n"
      "Phase 1 backends: 'synthetic' (deterministic, no hardware) and 'replay'.\n",
      argv0);
}

// Parses a non-negative integer option. Returns false on a missing value.
bool ParseU32(const char* name, const char* value, uint32_t* out) {
  if (!value) {
    std::fprintf(stderr, "%s requires a value\n", name);
    return false;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || *end != '\0') {
    std::fprintf(stderr, "%s: '%s' is not a number\n", name, value);
    return false;
  }
  *out = static_cast<uint32_t>(parsed);
  return true;
}

#if defined(_WIN32)
#include <windows.h>
void SleepMs(uint32_t ms) { ::Sleep(ms); }
#else
#include <time.h>
void SleepMs(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  std::string directory;
  std::string replay_path;
  std::string record_path;
  uint32_t skeleton_count = 1;
  uint32_t hz = 30;
  uint32_t frame_limit = 0;
  uint32_t interval_ms = 0;
  bool loop = false;
  bool want_depth = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;

    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--dir") {
      if (!next) { PrintUsage(argv[0]); return 2; }
      directory = next;
      ++i;
    } else if (arg == "--replay") {
      if (!next) { PrintUsage(argv[0]); return 2; }
      replay_path = next;
      ++i;
    } else if (arg == "--record") {
      if (!next) { PrintUsage(argv[0]); return 2; }
      record_path = next;
      ++i;
    } else if (arg == "--skeletons") {
      if (!ParseU32("--skeletons", next, &skeleton_count)) return 2;
    } else if (arg == "--depth") {
      // No value and no ++i: this is a flag, so the loop must advance by the
      // one argument. Consuming the next one here would eat --frames.
      want_depth = true;
    } else if (arg == "--hz") {
      if (!ParseU32("--hz", next, &hz) || hz == 0) {
        std::fprintf(stderr, "--hz must be greater than zero\n");
        return 2;
      }
      ++i;
    } else if (arg == "--frames") {
      if (!ParseU32("--frames", next, &frame_limit)) return 2;
      ++i;
    } else if (arg == "--interval-ms") {
      if (!ParseU32("--interval-ms", next, &interval_ms)) return 2;
      ++i;
    } else if (arg == "--loop") {
      loop = true;
    } else {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      PrintUsage(argv[0]);
      return 2;
    }
  }

  if (directory.empty()) {
    std::fprintf(stderr, "--dir is required\n\n");
    PrintUsage(argv[0]);
    return 2;
  }

  // Build the source. Replay wins over synthetic when both are implied, since
  // asking for a recording to be replayed while generating one is nonsense.
  std::unique_ptr<nui::Backend> backend;
  std::unique_ptr<nui::ReplayBackend> replay;
  if (!replay_path.empty()) {
    replay = std::make_unique<nui::ReplayBackend>();
    if (!replay->Open(replay_path, loop)) {
      std::fprintf(stderr, "cannot replay %s: %s\n", replay_path.c_str(),
                   replay->error().c_str());
      return 1;
    }
    std::printf("replaying %s: %u frames at %u Hz\n", replay_path.c_str(),
                replay->frame_count(), replay->hz());
    // Read the rate before handing ownership over: after the move `replay` is
    // null and any further use of it is a null dereference.
    hz = replay->hz();
    backend = std::move(replay);
  } else {
    auto synthetic = std::make_unique<nui::SyntheticBackend>(skeleton_count, hz);
    synthetic->set_depth(want_depth);
    std::printf("synthetic: %u skeleton(s) at %u Hz%s\n", skeleton_count, hz,
                want_depth ? ", with depth" : "");
    backend = std::move(synthetic);
  }

  nui::FileWriter writer;
  if (!writer.Open(directory)) {
    std::fprintf(stderr, "failed to open %s: %s\n", directory.c_str(),
                 writer.error().c_str());
    return 1;
  }

  nui::SessionWriter recorder;
  if (!record_path.empty()) {
    if (!recorder.Open(record_path, backend->name(), hz)) {
      std::fprintf(stderr, "cannot record to %s: %s\n", record_path.c_str(),
                   recorder.error().c_str());
      return 1;
    }
    std::printf("recording to %s\n", record_path.c_str());
  }

  std::printf("publishing to %s\n", directory.c_str());
  if (frame_limit) {
    std::printf("  bounded at %u frames\n", frame_limit);
  }
  std::fflush(stdout);

  nui::NuiFrameHeader header = {};
  std::vector<nui::NuiSkeleton> skeletons;
  std::vector<nui::BackendImage> images;

  uint32_t emitted = 0;
  // A replay with no frame limit runs until the session ends; without this,
  // a non-looping replay would spin forever on an exhausted source.
  const bool bounded = frame_limit != 0;

  while (!bounded || emitted < frame_limit) {
    const uint32_t count = backend->NextFrame(&header, &skeletons, &images);
    if (count == 0 && images.empty()) {
      // No skeletons and no images: either the source ended or it failed. A
      // frame carrying only depth is legitimate and must not end the loop, which
      // is why this tests both rather than the skeleton count alone.
      // Only a failure has an error.
      nui::ReplayBackend* as_replay = dynamic_cast<nui::ReplayBackend*>(backend.get());
      if (as_replay != nullptr && !as_replay->error().empty()) {
        std::fprintf(stderr, "replay failed at frame %u: %s\n", emitted,
                     as_replay->error().c_str());
        recorder.Close();
        return 1;
      }
      std::printf("\nsource exhausted after %u frames\n", emitted);
      break;
    }

    if (!writer.Write(header, skeletons.data(),
                      static_cast<uint32_t>(skeletons.size()), images.data(),
                      static_cast<uint32_t>(images.size()))) {
      std::fprintf(stderr, "write failed: %s\n", writer.error().c_str());
      recorder.Close();
      return 1;
    }
    if (recorder.is_open() &&
        !recorder.Append(header, skeletons.data(),
                         static_cast<uint32_t>(skeletons.size()))) {
      std::fprintf(stderr, "record failed: %s\n", recorder.error().c_str());
      return 1;
    }
    ++emitted;

    if (emitted % 300 == 0) {
      std::printf("  %u frames\n", emitted);
      std::fflush(stdout);
    }

    // Replay paces itself from the recording when asked, so a real-time session
    // replays at its original speed rather than the provider's guess at hz.
    if (interval_ms) {
      SleepMs(interval_ms);
    } else {
      SleepMs(0);
    }
  }

  if (recorder.is_open()) {
    if (!recorder.Close()) {
      std::fprintf(stderr, "failed to finalise %s: %s\n", record_path.c_str(),
                   recorder.error().c_str());
      return 1;
    }
    std::printf("recorded %u frames to %s\n", recorder.frames_recorded(),
                record_path.c_str());
  }

  std::printf("done: %u frames published\n", writer.frames_written());
  return 0;
}