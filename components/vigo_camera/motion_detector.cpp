#include "motion_detector.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <cmath>
#include <cstdlib>

static const char *TAG = "VigMotion";

// Implement the get_moving_point_number algorithm matching the Espressif esp-dl
// signature, but with global illumination compensation
namespace dl::image {
uint32_t get_moving_point_number(const uint8_t *img1, const uint8_t *img2, int width,
                                 int height, int stride, uint8_t threshold) {
  // First pass: compute average luminance change to compensate for global illumination
  // changes/auto-exposure
  int64_t diff_sum = 0;
  uint32_t n_points = 0;
  for (int i = 0; i < height; i += stride) {
    for (int j = 0; j < width; j += stride) {
      int idx = i * width + j;
      diff_sum += static_cast<int>(img2[idx]) - static_cast<int>(img1[idx]);
      n_points++;
    }
  }

  int avg_diff = 0;
  if (n_points > 0) {
    avg_diff = static_cast<int>(diff_sum / n_points);
  }

  // Second pass: count moving points, compensating for the average difference
  uint32_t n_moving_pts = 0;
  for (int i = 0; i < height; i += stride) {
    for (int j = 0; j < width; j += stride) {
      int idx = i * width + j;
      int diff = (static_cast<int>(img2[idx]) - static_cast<int>(img1[idx])) - avg_diff;
      if (std::abs(diff) > threshold) {
        n_moving_pts++;
      }
    }
  }
  return n_moving_pts;
}
} // namespace dl::image

namespace vigo::motion {

// Static helper to convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to grayscale,
// applying 2x2 software binning (downscaling).
static void convert_ouyy_evyy_to_gray_binned(const uint8_t *src, uint8_t *dst,
                                             int original_width, int original_height) {
  int src_stride = original_width * 3 / 2; // 1.5 bytes per pixel
  int out_width = original_width / 2;
  int out_height = original_height / 2;

  for (int y = 0; y < out_height; y++) {
    const uint8_t *src_row1 = src + (y * 2) * src_stride;
    uint8_t *dst_row = dst + y * out_width;

    for (int x = 0; x < out_width; x += 2) {
      int in_x = x * 2;

      // Extract the Y (luminance) value for the first 2x2 block (at index + 1)
      int src_idx1 = (in_x / 2) * 3;
      uint8_t y00 = src_row1[src_idx1 + 1];

      // Extract the Y (luminance) value for the second 2x2 block (at index + 1)
      int src_idx2 = ((in_x + 2) / 2) * 3;
      uint8_t y02 = src_row1[src_idx2 + 1];

      dst_row[x] = y00;
      dst_row[x + 1] = y02;
    }
  }
}

MotionDetector::MotionDetector(int stride, uint8_t threshold, float min_change_ratio,
                               uint32_t cooldown_ms)
    : stride_(stride), threshold_(threshold), min_change_ratio_(min_change_ratio),
      cooldown_ms_(cooldown_ms) {}

bool MotionDetector::detect_motion(const vigo::camera::CameraFrame &frame) {
  int out_width = frame.width / 2;
  int out_height = frame.height / 2;

  // Initialize or resize the binned grayscale buffer
  std::vector<uint8_t> current_gray(out_width * out_height);
  convert_ouyy_evyy_to_gray_binned(frame.data.data(), current_gray.data(), frame.width,
                                   frame.height);

  // If we don't have a previous frame (e.g. first frame captured), initialize it and
  // skip
  if (prev_gray_buf_.empty() || prev_width_ != out_width ||
      prev_height_ != out_height) {
    prev_gray_buf_ = std::move(current_gray);
    prev_width_ = out_width;
    prev_height_ = out_height;
    return false;
  }

  // Calculate total number of detection points based on stride
  int total_points_x = (out_width + stride_ - 1) / stride_;
  int total_points_y = (out_height + stride_ - 1) / stride_;
  int total_points = total_points_x * total_points_y;

  if (total_points <= 0) {
    return false;
  }

  // Call the Espressif-matching get_moving_point_number algorithm
  uint32_t moving_points =
      dl::image::get_moving_point_number(prev_gray_buf_.data(), current_gray.data(),
                                         out_width, out_height, stride_, threshold_);

  float change_ratio = static_cast<float>(moving_points) / total_points;

  // Update previous frame buffer
  prev_gray_buf_ = std::move(current_gray);

  bool is_motion = (change_ratio >= min_change_ratio_);

  if (is_motion) {
    int64_t now_us = esp_timer_get_time();
    int64_t cooldown_us = static_cast<int64_t>(cooldown_ms_) * 1000;

    if (last_trigger_time_us_ == 0 || (now_us - last_trigger_time_us_) >= cooldown_us) {
      ESP_LOGI(TAG, "Motion Detected! Points changed: %lu / %d (%.2f%%)", moving_points,
               total_points, change_ratio * 100.0f);
      last_trigger_time_us_ = now_us;
      return true;
    } else {
      ESP_LOGD(
          TAG,
          "Motion detected but throttled by cooldown (time since last trigger: %.2f s)",
          static_cast<double>(now_us - last_trigger_time_us_) / 1000000.0);
    }
  }

  return false;
}

} // namespace vigo::motion
