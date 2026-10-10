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
#include <string>
#include <vector>

#include "backends/synthetic/synthetic.h"
#include "ipc/file_writer.h"
#include "nui/frame.h"

namespace {

void PrintUsage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "  --dir <path>       IPC directory to publish frames into (required)\n"
      "  --skeletons <n>    number of skeletons to emit          (default 1)\n"
      "  --hz <n>           frame rate                           (default 30)\n"
      "  --frames <n>       stop after n frames, 0 = run forever  (default 0)\n"
      "  --interval-ms <n>  wall-clock sleep between frames      (default 0,\n"
      "                     i.e. emit as fast as the disk allows)\n"
      "  --help             this text\n"
      "\n"
      "Phase 1 backend is 'synthetic': a deterministic scripted skeleton, no\n"
      "hardware involved. Every frame it writes carries kFrameFlagSynthetic.\n",
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
  uint32_t skeleton_count = 1;
  uint32_t hz = 30;
  uint32_t frame_limit = 0;
  uint32_t interval_ms = 0;

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
    } else if (arg == "--skeletons") {
      if (!ParseU32("--skeletons", next, &skeleton_count)) return 2;
      ++i;
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

  nui::FileWriter writer;
  if (!writer.Open(directory)) {
    std::fprintf(stderr, "failed to open %s: %s\n", directory.c_str(),
                 writer.error().c_str());
    return 1;
  }

  std::printf("nui-provider (synthetic) -> %s\n", directory.c_str());
  std::printf("  %u skeleton(s), %u Hz%s\n", skeleton_count, hz,
              frame_limit ? ", bounded" : ", until killed");
  std::fflush(stdout);

  nui::SyntheticBackend backend(skeleton_count, hz);
  nui::NuiFrameHeader header = {};
  std::vector<nui::NuiSkeleton> skeletons;

  uint32_t emitted = 0;
  while (frame_limit == 0 || emitted < frame_limit) {
    backend.NextFrame(&header, &skeletons);
    if (!writer.Write(header, skeletons.data(),
                      static_cast<uint32_t>(skeletons.size()))) {
      std::fprintf(stderr, "write failed: %s\n", writer.error().c_str());
      return 1;
    }
    ++emitted;

    if (emitted % 300 == 0) {
      std::printf("  %u frames published\n", emitted);
      std::fflush(stdout);
    }

    if (interval_ms) {
      SleepMs(interval_ms);
    } else {
      // No pacing requested: emit as fast as the disk takes it, which is what
      // a burst/replay use wants.
      SleepMs(0);
    }
  }

  std::printf("done: %u frames\n", writer.frames_written());
  return 0;
}