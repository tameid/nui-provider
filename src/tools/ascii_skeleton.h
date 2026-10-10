// ASCII skeleton rendering.
//
// Drawn from a front view (XZ collapsed, Y up) because that is the view that
// makes the common failures obvious: a flipped axis, a joint in the wrong
// place, a collapsed or NaN position. Confidence is shown, not hidden - a
// well-drawn skeleton made of confident-but-wrong joints is exactly the failure
// this tool exists to catch.

#ifndef NUI_TOOLS_ASCII_SKELETON_H_
#define NUI_TOOLS_ASCII_SKELETON_H_

#include <string>
#include <vector>

#include "nui/frame.h"

namespace nui {

// Bones of the Xbox 360 NUI skeleton. Held here rather than in frame.h because
// this is a consumer's concern, not part of the wire contract.
const std::vector<std::pair<uint16_t, uint16_t>>& SkeletonBones();

// Draws `skeleton` into a `width` x `height` character grid.
//
// `min_confidence` marks joints the guest should not trust. Pass 0 to disable.
// Confidence is drawn by shading: '#' full, '+' partial, '.' negligible.
std::vector<std::string> RenderSkeleton(const NuiSkeleton& skeleton, int width,
                                        int height, float min_confidence);

// One-line summary, e.g. "id=1 pelvis=(0.00,0.90,0.00) joints 20/20 conf>=0.5".
std::string DescribeSkeleton(const NuiSkeleton& skeleton, float min_confidence);

// Header rendered as a single line.
std::string DescribeHeader(const NuiFrameHeader& header);

}  // namespace nui

#endif  // NUI_TOOLS_ASCII_SKELETON_H_