#include "backends/replay/replay.h"

#include <cstdio>
#include <cstring>

namespace nui {

bool ReplayBackend::Open(const std::string& path, bool loop) {
  path_ = path;
  loop_ = loop;
  error_.clear();
  cursor_ = 0;
  emitted_ = 0;
  loops_ = 0;
  previous_capture_us_ = 0;
  current_interval_us_ = 0;

  file_ = std::fopen(path_.c_str(), "rb");
  if (!file_) {
    error_ = "cannot open " + path_;
    return false;
  }

  if (std::fread(&header_, sizeof(header_), 1, file_) != 1) {
    error_ = path_ + " is too small to be a session";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  if (!SessionHeaderLooksValid(header_)) {
    error_ = path_ + " is not an NUI session (bad magic or version)";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  if (header_.frame_count == 0) {
    // Almost always a recording that was killed before Close patched the
    // count. Say so, rather than replaying nothing and looking like a title
    // that ignored us.
    error_ = path_ + " contains no frames (recording may not have finished)";
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  return true;
}

void ReplayBackend::SetRealtime(bool realtime) {
  realtime_ = realtime;
  Backend::SetRealtime(realtime);
}

bool ReplayBackend::Restart() {
  if (std::fseek(file_, 0, SEEK_END) != 0) {
    error_ = "cannot seek " + path_;
    return false;
  }
  // Skip past the header we already hold, rather than re-reading it: the bytes
  // cannot have changed, and this keeps cursor_ and the file position agreed.
  if (std::fseek(file_, static_cast<long>(sizeof(SessionHeader)), SEEK_SET) != 0) {
    error_ = "cannot rewind " + path_;
    return false;
  }
  cursor_ = 0;
  ++loops_;
  previous_capture_us_ = 0;
  return true;
}

bool ReplayBackend::ReadNextRaw(std::vector<uint8_t>* payload, uint32_t* skeletons) {
  if (cursor_ >= header_.frame_count) {
    return false;
  }

  NuiFrameHeader frame = {};
  if (std::fread(&frame, sizeof(frame), 1, file_) != 1) {
    error_ = path_ + " ended early at frame " + std::to_string(cursor_);
    return false;
  }
  if (!HeaderLooksValid(frame)) {
    error_ = path_ + " frame " + std::to_string(cursor_) + " has a bad header";
    return false;
  }

  // Skeletons first, then images in the fixed depth/ir/color order. Each image
  // carries its own NuiImage header, so the size is only knowable by walking
  // them - assuming a size from the skeleton count would silently truncate every
  // recorded depth frame.
  const size_t skeleton_bytes =
      static_cast<size_t>(frame.num_skeletons) * sizeof(NuiSkeleton);

  // Read the skeleton block, then walk the images to find the total length.
  std::vector<uint8_t> body;
  body.resize(skeleton_bytes);
  if (skeleton_bytes &&
      std::fread(body.data(), 1, skeleton_bytes, file_) != skeleton_bytes) {
    error_ = path_ + " frame " + std::to_string(cursor_) +
             " skeleton payload is truncated";
    return false;
  }

  const uint32_t image_total = frame.num_depth_frames + frame.num_ir_frames +
                               frame.num_color_frames;
  for (uint32_t i = 0; i < image_total; ++i) {
    NuiImage image = {};
    if (std::fread(&image, sizeof(image), 1, file_) != 1) {
      error_ = path_ + " frame " + std::to_string(cursor_) +
               " image header is truncated";
      return false;
    }
    body.resize(body.size() + sizeof(image) + image.data_size);
    std::memcpy(body.data() + body.size() - sizeof(image) - image.data_size, &image,
                sizeof(image));
    if (image.data_size &&
        std::fread(body.data() + body.size() - image.data_size, 1, image.data_size,
                   file_) != image.data_size) {
      error_ = path_ + " frame " + std::to_string(cursor_) +
               " image data is truncated";
      return false;
    }
  }

  payload->assign(sizeof(frame) + body.size(), 0);
  std::memcpy(payload->data(), &frame, sizeof(frame));
  if (!body.empty()) {
    std::memcpy(payload->data() + sizeof(frame), body.data(), body.size());
  }

  *skeletons = frame.num_skeletons;
  ++cursor_;
  return true;
}

uint32_t ReplayBackend::NextFrame(NuiFrameHeader* header,
                                  std::vector<NuiSkeleton>* out,
                                  std::vector<BackendImage>* images) {
  if (!file_) {
    error_ = "not open";
    return 0;
  }

  std::vector<uint8_t> payload;
  uint32_t skeleton_count = 0;

  if (cursor_ >= header_.frame_count) {
    if (!loop_) {
      // End of session. Signal it by clearing the header, which is how the
      // provider tells "no more frames" from "a frame with zero skeletons".
      *header = NuiFrameHeader{};
      out->clear();
      images->clear();
      return 0;
    }
    if (!Restart()) {
      return 0;
    }
  }

  if (!ReadNextRaw(&payload, &skeleton_count)) {
    if (!loop_ && error_.empty()) {
      error_ = "replay ended at frame " + std::to_string(cursor_);
    }
    return 0;
  }

  const DecodedFrame decoded =
      DecodeFrame(payload.data(), static_cast<uint32_t>(payload.size()));
  if (!decoded.valid) {
    error_ = path_ + " frame " + std::to_string(cursor_ - 1) + " did not decode";
    return 0;
  }

  *header = decoded.header;
  current_interval_us_ = 0;
  if (decoded.header.capture_timestamp_us > previous_capture_us_) {
    current_interval_us_ = decoded.header.capture_timestamp_us - previous_capture_us_;
  }
  previous_capture_us_ = decoded.header.capture_timestamp_us;

  out->assign(skeleton_count, NuiSkeleton{});
  if (skeleton_count) {
    std::memcpy(out->data(), decoded.skeletons,
                static_cast<size_t>(skeleton_count) * sizeof(NuiSkeleton));
  }

  // Replay hands the recorded pixels straight back. They live in `payload`,
  // which is only valid until the next read, so copy them into storage this
  // backend owns for as long as the Backend contract promises the caller.
  images->clear();
  replay_images_.clear();
  replay_pixels_.clear();
  for (uint32_t i = 0; i < decoded.image_count; ++i) {
    // Deep copy: the decoded pointers aim into `payload`, which the next read
    // overwrites, so handing them straight out would break the contract.
    replay_pixels_.insert(replay_pixels_.end(), decoded.images[i].data,
                          decoded.images[i].data +
                              decoded.images[i].header.data_size);
    BackendImage image;
    image.header = decoded.images[i].header;
    image.data = replay_pixels_.data() + (replay_pixels_.size() -
                                          decoded.images[i].header.data_size);
    replay_images_.push_back(image);
  }
  *images = replay_images_;

  ++emitted_;
  return skeleton_count;
}

}  // namespace nui