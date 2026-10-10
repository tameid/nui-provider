/**
 * nui-inspect - look at what nui-provider produced.
 *
 * Reads a live IPC directory or a recorded .nui session and prints what is in
 * it. Exists because the common failure is "a title is doing something wrong
 * and I need to know what the provider actually sent", and the fastest answer
 * to that is text in a terminal.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "nui/frame.h"
#include "tools/ascii_skeleton.h"
#include "tools/frame_source.h"

namespace {

void PrintUsage(const char* argv0) {
  std::printf(
      "usage: %s <path> [options]\n"
      "\n"
      "  <path>   a .nui session file, or a live IPC directory\n"
      "           (one containing frame.idx and frame.N.bin)\n"
      "\n"
      "  --frames <n>     stop after n frames            (default 1)\n"
      "  --all            process every frame in the source\n"
      "  --skeleton <n>   also draw skeleton n, 0-based   (default: all)\n"
      "  --ascii          draw skeletons as ASCII art\n"
      "  --joints         dump every joint position and confidence\n"
      "  --confidence <f> threshold for 'trustworthy'    (default 0.50)\n"
      "  --width <n>      ASCII width                     (default 48)\n"
      "  --height <n>     ASCII height                    (default 22)\n"
      "  --follow         keep polling a live directory until interrupted\n"
      "  --help           this text\n"
      "\n"
      "Legend: '#' confident  '+' partial  '.' negligible\n"
      "        '-' bone    '.' bone below the confidence threshold\n",
      argv0);
}

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

bool ParseFloat(const char* name, const char* value, float* out) {
  if (!value) {
    std::fprintf(stderr, "%s requires a value\n", name);
    return false;
  }
  char* end = nullptr;
  const double parsed = std::strtod(value, &end);
  if (end == value || *end != '\0') {
    std::fprintf(stderr, "%s: '%s' is not a number\n", name, value);
    return false;
  }
  *out = static_cast<float>(parsed);
  return true;
}

const char* JointName(uint16_t index) {
  static const char* names[] = {
      "Head", "Neck", "SpineChest", "Pelvis", "ShoulderL", "ShoulderR",
      "ElbowL", "ElbowR", "WristL", "WristR", "HandL", "HandR", "HipL", "HipR",
      "KneeL", "KneeR", "AnkleL", "AnkleR", "FootL", "FootR",
  };
  return index < nui::kJointCount ? names[index] : "?";
}

void DumpJoints(const nui::NuiSkeleton& skeleton, float threshold) {
  std::printf("    %-11s %-34s %s\n", "joint", "position (x, y, z)", "conf");
  for (uint16_t i = 0; i < nui::kJointCount; ++i) {
    const nui::NuiJoint& j = skeleton.joints[i];
    std::printf("    %-11s (%+.3f, %+.3f, %+.3f)   pos=%.2f ori=%.2f%s\n",
                JointName(i), j.position[0], j.position[1], j.position[2],
                j.pos_confidence, j.orient_confidence,
                j.pos_confidence < threshold ? "  <-- below threshold" : "");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 2;
  }

  const std::string path = argv[1];
  uint32_t frame_limit = 1;
  bool all_frames = false;
  bool draw = false;
  bool dump_joints = false;
  bool follow = false;
  bool skeleton_set = false;
  uint32_t skeleton_index = 0;
  float threshold = 0.50f;
  int width = 48, height = 22;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const char* next = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (arg == "--help" || arg == "-h") { PrintUsage(argv[0]); return 0; }
    if (arg == "--frames") { if (!ParseU32("--frames", next, &frame_limit)) return 2; ++i; }
    else if (arg == "--all") { all_frames = true; }
    else if (arg == "--ascii") { draw = true; }
    else if (arg == "--joints") { dump_joints = true; }
    else if (arg == "--follow") { follow = true; }
    else if (arg == "--skeleton") {
      if (!ParseU32("--skeleton", next, &skeleton_index)) return 2;
      skeleton_set = true; ++i;
    }
    else if (arg == "--confidence") { if (!ParseFloat("--confidence", next, &threshold)) return 2; ++i; }
    else if (arg == "--width") { uint32_t v; if (!ParseU32("--width", next, &v)) return 2; width = (int)v; ++i; }
    else if (arg == "--height") { uint32_t v; if (!ParseU32("--height", next, &v)) return 2; height = (int)v; ++i; }
    else { std::fprintf(stderr, "unknown option: %s\n", arg.c_str()); PrintUsage(argv[0]); return 2; }
  }

  nui::FrameSource source;
  if (!source.Open(path)) {
    std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(),
                 source.error().c_str());
    return 1;
  }

  std::printf("source: %s (%s%s)\n", path.c_str(),
              source.is_session() ? "session" : "live IPC directory",
              source.is_session() ? "" : ", following until interrupted");
  if (source.is_session()) {
    std::printf("  %u frames\n", source.total_frames());
  }
  if (!draw && !dump_joints) {
    draw = true;  // a bare summary is rarely what anyone wanted
  }
  std::printf("\n");

  uint32_t processed = 0;
  for (;;) {
    nui::SourceFrame frame;
    if (!source.Next(&frame)) {
      if (!source.error().empty()) {
        std::fprintf(stderr, "error: %s\n", source.error().c_str());
        return 1;
      }
      if (follow) {
        // Live directory went quiet; wait for the writer to publish again.
        std::fflush(stdout);
#if defined(_WIN32)
        ::Sleep(20);
#else
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, nullptr);
#endif
        continue;
      }
      break;
    }

    std::printf("#%llu  %s\n", (unsigned long long)frame.ordinal,
                nui::DescribeHeader(frame.header).c_str());

    if (frame.skeletons.empty()) {
      std::printf("    (no skeletons)\n\n");
    }

    for (size_t s = 0; s < frame.skeletons.size(); ++s) {
      if (skeleton_set && s != skeleton_index) {
        continue;
      }
      const nui::NuiSkeleton& skeleton = frame.skeletons[s];
      std::printf("    skeleton %zu: %s\n", s,
                  nui::DescribeSkeleton(skeleton, threshold).c_str());

      if (draw) {
        const auto grid = nui::RenderSkeleton(skeleton, width, height, threshold);
        for (const std::string& row : grid) {
          std::printf("    |%s|\n", row.c_str());
        }
      }
      if (dump_joints) {
        DumpJoints(skeleton, threshold);
      }
    }

    std::printf("\n");
    ++processed;
    if (!all_frames && !follow && processed >= frame_limit) {
      break;
    }
  }

  std::printf("processed %u frame(s)\n", processed);
  return 0;
}