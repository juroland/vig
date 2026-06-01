#pragma once

#include "camera.hpp"
#include <cstdint>
#include <vector>

namespace vigo::motion {

class MotionDetector {
public:
  // stride: check every Nth pixel in X and Y
  // threshold: activation threshold of each pixel difference (0-255)
  // min_change_ratio: fraction of active points required to trigger motion (e.g. 0.015
  // for 1.5%) cooldown_ms: minimum time between sent events (e.g. 10000 for 10 seconds)
  MotionDetector(int stride = 16, uint8_t threshold = 8, float min_change_ratio = 0.05f,
                 uint32_t cooldown_ms = 10000);

  ~MotionDetector() = default;

  // Returns true if motion was detected and the cooldown period has expired
  bool detect_motion(const vigo::camera::CameraFrame &frame);

private:
  int stride_;
  uint8_t threshold_;
  float min_change_ratio_;
  uint32_t cooldown_ms_;

  std::vector<uint8_t> prev_gray_buf_;
  int prev_width_{0};
  int prev_height_{0};

  int64_t last_trigger_time_us_{0};
};

} // namespace vigo::motion
