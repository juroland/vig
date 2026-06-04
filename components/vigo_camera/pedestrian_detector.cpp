#include "pedestrian_detector.hpp"
#include "esp_log.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cassert>

// Include the official Espressif pedestrian detect
#include "pedestrian_detect.hpp"

static const char *TAG = "PedestrianDetector";

namespace vigo::detection {

PedestrianDetector::PedestrianDetector(int frame_width, int frame_height,
                                       float confidence_threshold, int downscale_factor)
    : confidence_threshold_{confidence_threshold}, downscale_factor_{downscale_factor},
      input_frame_height_{static_cast<size_t>(frame_height)},
      input_frame_width_{static_cast<size_t>(frame_width)},
      inference_frame_height_{static_cast<size_t>(frame_height / downscale_factor)},
      inference_frame_width_{static_cast<size_t>(frame_width / downscale_factor)},
      inference_frame_bytes_{inference_frame_width_ * inference_frame_height_ * 2},
      inference_frame_(inference_frame_bytes_) {
  assert(downscale_factor == 2 || downscale_factor == 4);

  // For downscale factors > 2, we need an intermediate buffer for two-stage
  // conversion: OUYY_EVYY → YUYV at 2x, then YUYV → YUYV at remaining factor.
  if (downscale_factor_ > 2) {
    size_t half_w = input_frame_width_ / 2;
    size_t half_h = input_frame_height_ / 2;
    intermediate_frame_.resize(half_w * half_h * 2);
  }

#ifndef CONFIG_VIGO_USE_MOCK_CAMERA
  ESP_LOGI(TAG, "Initializing hardware PedestrianDetect pico_s8_v1 model...");
  impl_ = new ::PedestrianDetect(::PedestrianDetect::PICO_S8_V1,
                                 true); // lazy_load = true
#endif
  ESP_LOGI(TAG, "Inference resolution: %zu x %zu (downscale factor: %d)",
           inference_frame_width_, inference_frame_height_, downscale_factor_);
}

PedestrianDetector::~PedestrianDetector() {
  if (impl_) {
    delete impl_;
    impl_ = nullptr;
  }
}

namespace detail {

// Convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to YUV422
// interleaved (YUYV) format, applying 2x2 software binning (downscaling).
void convert_ouyy_evyy_to_yuyv_binned(const uint8_t *src, uint8_t *dst,
                                      int original_width, int original_height) {
  int src_stride = original_width * 3 / 2; // 1.5 bytes per pixel
  int out_width = original_width / 2;
  int out_height = original_height / 2;

  for (int y = 0; y < out_height; y++) {
    const uint8_t *src_row1 = src + (y * 2) * src_stride;
    const uint8_t *src_row2 = src + (y * 2 + 1) * src_stride;
    uint8_t *dst_row = dst + y * out_width * 2;

    for (int x = 0; x < out_width; x += 2) {
      int in_x = x * 2;

      // First 2x2 block
      int src_idx1 = (in_x / 2) * 3;
      uint8_t u0 = src_row1[src_idx1 + 0];
      uint8_t y00 = src_row1[src_idx1 + 1];
      uint8_t v0 = src_row2[src_idx1 + 0];

      // Second 2x2 block
      int src_idx2 = ((in_x + 2) / 2) * 3;
      uint8_t u1 = src_row1[src_idx2 + 0];
      uint8_t y02 = src_row1[src_idx2 + 1];
      uint8_t v1 = src_row2[src_idx2 + 0];

      uint8_t u = (u0 + u1) / 2;
      uint8_t v = (v0 + v1) / 2;

      // Swapped indices to convert raw V/U rows to standard YUYV order (U at idx 1, V
      // at idx 3)
      dst_row[x * 2 + 0] = y00;
      dst_row[x * 2 + 1] = v; // Since v is actually U
      dst_row[x * 2 + 2] = y02;
      dst_row[x * 2 + 3] = u; // Since u is actually V
    }
  }
}

// Downsample YUYV interleaved frame by 2x in both dimensions.
// Averages 2x2 pixel blocks, producing (width/2, height/2) output.
void downsample_yuyv_2x(const uint8_t *src, uint8_t *dst, int src_width,
                        int src_height) {
  int dst_width = src_width / 2;
  int dst_height = src_height / 2;

  for (int y = 0; y < dst_height; y++) {
    const uint8_t *row0 = src + (y * 2) * src_width * 2;
    const uint8_t *row1 = src + (y * 2 + 1) * src_width * 2;
    uint8_t *dst_row = dst + y * dst_width * 2;

    for (int x = 0; x < dst_width; x += 2) {
      int sx = x * 2;
      // YUYV macro-pixel: [Y0 U Y1 V]
      // Average Y from 2x2 block of source pixels
      uint8_t y0 = (row0[sx * 2 + 0] + row0[(sx + 1) * 2 + 0] + row1[sx * 2 + 0] +
                    row1[(sx + 1) * 2 + 0]) /
                   4;
      uint8_t y1 = (row0[(sx + 2) * 2 + 0] + row0[(sx + 3) * 2 + 0] +
                    row1[(sx + 2) * 2 + 0] + row1[(sx + 3) * 2 + 0]) /
                   4;
      // Average U and V from neighboring macro-pixels
      uint8_t u = (row0[sx * 2 + 1] + row0[(sx + 2) * 2 + 1] + row1[sx * 2 + 1] +
                   row1[(sx + 2) * 2 + 1]) /
                  4;
      uint8_t v = (row0[sx * 2 + 3] + row0[(sx + 2) * 2 + 3] + row1[sx * 2 + 3] +
                   row1[(sx + 2) * 2 + 3]) /
                  4;

      dst_row[x * 2 + 0] = y0;
      dst_row[x * 2 + 1] = u;
      dst_row[x * 2 + 2] = y1;
      dst_row[x * 2 + 3] = v;
    }
  }
}

} // namespace detail

void PedestrianDetector::update_debug_frame(const uint8_t *yuyv, int width, int height,
                                            float probability) {
  std::lock_guard<std::mutex> lock(debug_mutex_);
  if (!yuyv || width <= 0 || height <= 0) {
    debug_frame_.has_frame = false;
    debug_frame_.probability = 0.0f;
    return;
  }
  debug_frame_.yuyv_data.assign(yuyv, yuyv + (width * height * 2));
  debug_frame_.width = width;
  debug_frame_.height = height;
  debug_frame_.probability = probability;
  debug_frame_.has_frame = true;
}

bool PedestrianDetector::get_debug_frame(std::vector<uint8_t> &yuyv_out, int &width_out,
                                         int &height_out, float &probability_out) {
  std::lock_guard<std::mutex> lock(debug_mutex_);
  if (!debug_frame_.has_frame) {
    return false;
  }
  yuyv_out = debug_frame_.yuyv_data;
  width_out = debug_frame_.width;
  height_out = debug_frame_.height;
  probability_out = debug_frame_.probability;
  return true;
}

std::vector<vigo::backend::DetectionResult>
PedestrianDetector::detect(const vigo::camera::CameraFrame &frame) {
  std::vector<vigo::backend::DetectionResult> results;

  // Stage 1: Convert OUYY_EVYY → YUYV with inherent 2x2 binning (always half-res)
  if (downscale_factor_ <= 2) {
    // Single-stage: output directly to inference buffer
    detail::convert_ouyy_evyy_to_yuyv_binned(frame.data.data(), inference_frame_.data(),
                                             input_frame_width_, input_frame_height_);
  } else {
    // Two-stage: convert to intermediate half-res, then downsample further
    detail::convert_ouyy_evyy_to_yuyv_binned(frame.data.data(),
                                             intermediate_frame_.data(),
                                             input_frame_width_, input_frame_height_);
    // Stage 2: Downsample YUYV by remaining factor (2x for total 4x)
    int half_w = input_frame_width_ / 2;
    int half_h = input_frame_height_ / 2;
    detail::downsample_yuyv_2x(intermediate_frame_.data(), inference_frame_.data(),
                               half_w, half_h);
  }

  // Under mock/testing settings
  if (sim_pedestrian_present_) {
    float max_score = 0.0f;
    if (sim_score_ >= confidence_threshold_) {
      vigo::backend::DetectionResult det;
      det.box = {0.25f, 0.20f, 0.65f,
                 0.85f}; // [x_min, y_min, x_max, y_max] normalized coordinates
      det.score = sim_score_;
      det.label = "pedestrian";
      results.push_back(std::move(det));
      max_score = sim_score_;
      ESP_LOGI(
          TAG,
          "Simulated Pedestrian Detected! BBox=[0.25, 0.20, 0.65, 0.85], Score=%.2f",
          sim_score_);
    } else {
      ESP_LOGD(TAG, "Inference completed: No pedestrian detected (Simulated)");
    }

    // Draw mock bounding boxes on inference_frame_
    for (const auto &res : results) {
      int x1 = static_cast<int>(res.box[0] * inference_frame_width_);
      int y1 = static_cast<int>(res.box[1] * inference_frame_height_);
      int x2 = static_cast<int>(res.box[2] * inference_frame_width_);
      int y2 = static_cast<int>(res.box[3] * inference_frame_height_);

      uint8_t Y_val = 149;
      uint8_t U_val = 44;
      uint8_t V_val = 21;

      // Draw horizontal lines
      for (int y : {y1, y2}) {
        if (y >= 0 && y < static_cast<int>(inference_frame_height_)) {
          int start_x = std::clamp(std::min(x1, x2), 0,
                                   static_cast<int>(inference_frame_width_) - 1);
          int end_x = std::clamp(std::max(x1, x2), 0,
                                 static_cast<int>(inference_frame_width_) - 1);
          for (int x = start_x; x <= end_x; ++x) {
            inference_frame_[y * inference_frame_width_ * 2 + x * 2] = Y_val;
            int chroma_idx = (x / 2) * 4;
            inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 1] = U_val;
            inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 3] = V_val;
          }
        }
      }
      // Draw vertical lines
      for (int x : {x1, x2}) {
        if (x >= 0 && x < static_cast<int>(inference_frame_width_)) {
          int start_y = std::clamp(std::min(y1, y2), 0,
                                   static_cast<int>(inference_frame_height_) - 1);
          int end_y = std::clamp(std::max(y1, y2), 0,
                                 static_cast<int>(inference_frame_height_) - 1);
          for (int y = start_y; y <= end_y; ++y) {
            inference_frame_[y * inference_frame_width_ * 2 + x * 2] = Y_val;
            int chroma_idx = (x / 2) * 4;
            inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 1] = U_val;
            inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 3] = V_val;
          }
        }
      }
    }

    update_debug_frame(inference_frame_.data(), inference_frame_width_,
                       inference_frame_height_, max_score);
    return results;
  }

  if (!impl_) {
    ESP_LOGW(TAG, "Hardware Pedestrian Detect is not initialized or running in Mock "
                  "camera mode.");
    update_debug_frame(inference_frame_.data(), inference_frame_width_,
                       inference_frame_height_, 0.0f);
    return results;
  }

  ESP_LOGI(TAG, "Running HW Pedestrian Detect (pico_s8_v1) on %zu x %zu frame...",
           frame.width, frame.height);

  // Set threshold in implementation
  impl_->set_score_thr(confidence_threshold_);

  // Prepare input image descriptor for ESP-DL
  dl::image::img_t src_img = {.data = inference_frame_.data(),
                              .width = static_cast<uint16_t>(inference_frame_width_),
                              .height = static_cast<uint16_t>(inference_frame_height_),
                              .pix_type = dl::image::DL_IMAGE_PIX_TYPE_YUYV};

  // Run the ESP-DL inference pipeline
  std::list<dl::detect::result_t> &results_list = impl_->run(src_img);

  float max_score = 0.0f;
  for (const auto &res : results_list) {
    if (res.score >= confidence_threshold_) {
      vigo::backend::DetectionResult det;
      float x_min = static_cast<float>(res.box[0]) / inference_frame_width_;
      float y_min = static_cast<float>(res.box[1]) / inference_frame_height_;
      float x_max = static_cast<float>(res.box[2]) / inference_frame_width_;
      float y_max = static_cast<float>(res.box[3]) / inference_frame_height_;

      det.box = {x_min, y_min, x_max, y_max};
      det.score = res.score;
      det.label = "pedestrian";
      results.push_back(std::move(det));

      if (res.score > max_score) {
        max_score = res.score;
      }

      ESP_LOGI(TAG, "Pedestrian Detected! BBox=[%.2f, %.2f, %.2f, %.2f], Score=%.2f",
               x_min, y_min, x_max, y_max, res.score);
    }
  }

  // Draw bounding boxes on inference_frame_ for the debug frame
  for (const auto &res : results) {
    int x1 = static_cast<int>(res.box[0] * inference_frame_width_);
    int y1 = static_cast<int>(res.box[1] * inference_frame_height_);
    int x2 = static_cast<int>(res.box[2] * inference_frame_width_);
    int y2 = static_cast<int>(res.box[3] * inference_frame_height_);

    uint8_t Y_val = 149;
    uint8_t U_val = 44;
    uint8_t V_val = 21;

    for (int y : {y1, y2}) {
      if (y >= 0 && y < static_cast<int>(inference_frame_height_)) {
        int start_x = std::clamp(std::min(x1, x2), 0,
                                 static_cast<int>(inference_frame_width_) - 1);
        int end_x = std::clamp(std::max(x1, x2), 0,
                               static_cast<int>(inference_frame_width_) - 1);
        for (int x = start_x; x <= end_x; ++x) {
          inference_frame_[y * inference_frame_width_ * 2 + x * 2] = Y_val;
          int chroma_idx = (x / 2) * 4;
          inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 1] = U_val;
          inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 3] = V_val;
        }
      }
    }
    for (int x : {x1, x2}) {
      if (x >= 0 && x < static_cast<int>(inference_frame_width_)) {
        int start_y = std::clamp(std::min(y1, y2), 0,
                                 static_cast<int>(inference_frame_height_) - 1);
        int end_y = std::clamp(std::max(y1, y2), 0,
                               static_cast<int>(inference_frame_height_) - 1);
        for (int y = start_y; y <= end_y; ++y) {
          inference_frame_[y * inference_frame_width_ * 2 + x * 2] = Y_val;
          int chroma_idx = (x / 2) * 4;
          inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 1] = U_val;
          inference_frame_[y * inference_frame_width_ * 2 + chroma_idx + 3] = V_val;
        }
      }
    }
  }

  update_debug_frame(inference_frame_.data(), inference_frame_width_,
                     inference_frame_height_, max_score);

  return results;
}

} // namespace vigo::detection
