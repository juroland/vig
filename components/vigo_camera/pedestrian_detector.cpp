#include "pedestrian_detector.hpp"
#include "esp_log.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cassert>

// PPA and Cache Headers
#include "driver/ppa.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Include the official Espressif pedestrian detect
#include "pedestrian_detect.hpp"

static const char *TAG = "PedestrianDetector";

static bool ppa_pedestrian_done_cb(ppa_client_handle_t ppa_client,
                                   ppa_event_data_t *event_data, void *user_data) {
  BaseType_t high_task_woken = pdFALSE;
  SemaphoreHandle_t sem = static_cast<SemaphoreHandle_t>(user_data);
  if (sem) {
    xSemaphoreGiveFromISR(sem, &high_task_woken);
  }
  return high_task_woken == pdTRUE;
}

namespace vigo::detection {

PedestrianDetector::PedestrianDetector(int frame_width, int frame_height,
                                       float confidence_threshold, int downscale_factor,
                                       float max_area_proportion,
                                       float min_aspect_ratio, float max_aspect_ratio)
    : confidence_threshold_{confidence_threshold}, downscale_factor_{downscale_factor},
      max_area_proportion_{max_area_proportion}, min_aspect_ratio_{min_aspect_ratio},
      max_aspect_ratio_{max_aspect_ratio},
      input_frame_height_{static_cast<size_t>(frame_height)},
      input_frame_width_{static_cast<size_t>(frame_width)},
      inference_frame_height_{static_cast<size_t>(frame_height / downscale_factor)},
      inference_frame_width_{static_cast<size_t>(frame_width / downscale_factor)},
      inference_frame_bytes_{(inference_frame_width_ * inference_frame_height_ * 3) /
                             2},
      inference_frame_(inference_frame_bytes_), ppa_client_(nullptr),
      ppa_sem_(nullptr) {
  assert(downscale_factor == 2 || downscale_factor == 4);

  // Register PPA client for SRM operation
  ppa_client_config_t config = {};
  config.oper_type = PPA_OPERATION_SRM;
  config.max_pending_trans_num = 1;
  config.data_burst_length = PPA_DATA_BURST_LENGTH_128;

  esp_err_t err = ppa_register_client(
      &config, reinterpret_cast<ppa_client_handle_t *>(&ppa_client_));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register PPA client: %d", err);
  } else {
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = ppa_pedestrian_done_cb;
    if (ppa_client_register_event_callbacks(
            static_cast<ppa_client_handle_t>(ppa_client_), &cbs) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register PPA client callbacks");
    }
  }

  ppa_sem_ = xSemaphoreCreateBinary();

  // For downscale factors > 2, we need an intermediate buffer for two-stage
  // conversion: OUYY_EVYY → YUV420 at 2x, then YUV420 → YUV420 at remaining factor.
  if (downscale_factor_ > 2) {
    size_t half_w = input_frame_width_ / 2;
    size_t half_h = input_frame_height_ / 2;
    intermediate_frame_.resize((half_w * half_h * 3) / 2);
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
  if (ppa_client_) {
    ppa_unregister_client(static_cast<ppa_client_handle_t>(ppa_client_));
    ppa_client_ = nullptr;
  }
  if (ppa_sem_) {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(ppa_sem_));
    ppa_sem_ = nullptr;
  }
}

namespace detail {

// Convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to standard YUV420
// format, applying 2x2 software binning (downscaling).
void convert_ouyy_evyy_to_yuv420_binned(const uint8_t *src, uint8_t *dst,
                                        int original_width, int original_height) {
  int src_stride = original_width * 3 / 2;
  int out_width = original_width / 2;
  int out_height = original_height / 2;
  uint8_t *dst_y = dst;
  uint8_t *dst_u = dst + out_width * out_height;
  uint8_t *dst_v = dst_u + (out_width * out_height) / 4;

  for (int y = 0; y < out_height; y++) {
    const uint8_t *src_row1 = src + (y * 2) * src_stride;
    const uint8_t *src_row2 = src + (y * 2 + 1) * src_stride;
    for (int x = 0; x < out_width; x++) {
      int in_x = x * 2;
      int src_idx = (in_x / 2) * 3;
      dst_y[y * out_width + x] = src_row1[src_idx + 1]; // Y
      if (x % 2 == 0 && y % 2 == 0) {
        dst_u[(y / 2) * (out_width / 2) + (x / 2)] = src_row1[src_idx + 0]; // U
        dst_v[(y / 2) * (out_width / 2) + (x / 2)] = src_row2[src_idx + 0]; // V
      }
    }
  }
}

// Downsample YUV420 frame by 2x in both dimensions.
void downsample_yuv420_2x(const uint8_t *src, uint8_t *dst, int src_width,
                          int src_height) {
  int dst_width = src_width / 2;
  int dst_height = src_height / 2;

  const uint8_t *s_y = src;
  const uint8_t *s_u = src + src_width * src_height;
  const uint8_t *s_v = s_u + (src_width * src_height) / 4;

  uint8_t *d_y = dst;
  uint8_t *d_u = dst + dst_width * dst_height;
  uint8_t *d_v = d_u + (dst_width * dst_height) / 4;

  for (int y = 0; y < dst_height; y++) {
    for (int x = 0; x < dst_width; x++) {
      d_y[y * dst_width + x] =
          (s_y[(y * 2) * src_width + (x * 2)] + s_y[(y * 2) * src_width + (x * 2 + 1)] +
           s_y[(y * 2 + 1) * src_width + (x * 2)] +
           s_y[(y * 2 + 1) * src_width + (x * 2 + 1)]) /
          4;
    }
  }
  int u_w = src_width / 2;
  for (int y = 0; y < dst_height / 2; y++) {
    for (int x = 0; x < dst_width / 2; x++) {
      d_u[y * (dst_width / 2) + x] =
          (s_u[(y * 2) * u_w + (x * 2)] + s_u[(y * 2) * u_w + (x * 2 + 1)] +
           s_u[(y * 2 + 1) * u_w + (x * 2)] + s_u[(y * 2 + 1) * u_w + (x * 2 + 1)]) /
          4;
      d_v[y * (dst_width / 2) + x] =
          (s_v[(y * 2) * u_w + (x * 2)] + s_v[(y * 2) * u_w + (x * 2 + 1)] +
           s_v[(y * 2 + 1) * u_w + (x * 2)] + s_v[(y * 2 + 1) * u_w + (x * 2 + 1)]) /
          4;
    }
  }
}

} // namespace detail

void PedestrianDetector::update_debug_frame(const uint8_t *yuv420, int width,
                                            int height, float probability,
                                            bool is_uyvy) {
  std::lock_guard<std::mutex> lock(debug_mutex_);
  if (!yuv420 || width <= 0 || height <= 0) {
    debug_frame_.has_frame = false;
    debug_frame_.probability = 0.0f;
    return;
  }
  debug_frame_.yuyv_data.assign(yuv420, yuv420 + (width * height * 3 / 2));
  debug_frame_.width = width;
  debug_frame_.height = height;
  debug_frame_.probability = probability;
  debug_frame_.is_uyvy = is_uyvy;
  debug_frame_.has_frame = true;
}

bool PedestrianDetector::get_debug_frame(std::vector<uint8_t> &yuyv_out, int &width_out,
                                         int &height_out, float &probability_out,
                                         bool *is_uyvy_out) {
  std::lock_guard<std::mutex> lock(debug_mutex_);
  if (!debug_frame_.has_frame) {
    return false;
  }
  yuyv_out = debug_frame_.yuyv_data;
  width_out = debug_frame_.width;
  height_out = debug_frame_.height;
  probability_out = debug_frame_.probability;
  if (is_uyvy_out) {
    *is_uyvy_out = debug_frame_.is_uyvy;
  }
  return true;
}

std::vector<vigo::backend::DetectionResult>
PedestrianDetector::detect(const vigo::camera::CameraFrame &frame,
                           bool *has_discarded) {
  if (has_discarded) {
    *has_discarded = false;
  }
  std::vector<vigo::backend::DetectionResult> results;

  bool conversion_success = false;
  bool using_uyvy = false;

  if (ppa_client_ && ppa_sem_) {
    ppa_in_pic_blk_config_t in_cfg = {};
    in_cfg.buffer = frame.data.data();
    in_cfg.pic_w = static_cast<uint32_t>(input_frame_width_);
    in_cfg.pic_h = static_cast<uint32_t>(input_frame_height_);
    in_cfg.block_w = static_cast<uint32_t>(input_frame_width_);
    in_cfg.block_h = static_cast<uint32_t>(input_frame_height_);
    in_cfg.block_offset_x = 0;
    in_cfg.block_offset_y = 0;
    in_cfg.srm_cm = PPA_SRM_COLOR_MODE_YUV420;

    ppa_out_pic_blk_config_t out_cfg = {};
    out_cfg.buffer = inference_frame_.data();
    out_cfg.buffer_size = static_cast<uint32_t>(inference_frame_.size());
    out_cfg.pic_w = static_cast<uint32_t>(inference_frame_width_);
    out_cfg.pic_h = static_cast<uint32_t>(inference_frame_height_);
    out_cfg.block_offset_x = 0;
    out_cfg.block_offset_y = 0;
    out_cfg.srm_cm = PPA_SRM_COLOR_MODE_YUV420;

    float scale_factor = 1.0f / downscale_factor_;

    ppa_srm_oper_config_t oper_cfg = {};
    oper_cfg.in = in_cfg;
    oper_cfg.out = out_cfg;
    oper_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    oper_cfg.scale_x = scale_factor;
    oper_cfg.scale_y = scale_factor;
    oper_cfg.mirror_x = false;
    oper_cfg.mirror_y = false;
    oper_cfg.rgb_swap = false;
    oper_cfg.byte_swap = false;
    oper_cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    oper_cfg.mode = PPA_TRANS_MODE_NON_BLOCKING;
    oper_cfg.user_data = ppa_sem_;

    esp_err_t err = ppa_do_scale_rotate_mirror(
        static_cast<ppa_client_handle_t>(ppa_client_), &oper_cfg);
    if (err == ESP_OK) {
      if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(ppa_sem_),
                         pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_cache_msync(inference_frame_.data(), inference_frame_.size(),
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        conversion_success = true;
        using_uyvy = true;
      } else {
        ESP_LOGE(TAG, "PPA SRM conversion timed out, falling back to CPU");
      }
    } else {
      ESP_LOGE(TAG, "Failed to start PPA SRM transaction: %d, falling back to CPU",
               err);
    }
  }

  if (!conversion_success) {
    if (downscale_factor_ <= 2) {
      detail::convert_ouyy_evyy_to_yuv420_binned(
          frame.data.data(), inference_frame_.data(), input_frame_width_,
          input_frame_height_);
    } else {
      detail::convert_ouyy_evyy_to_yuv420_binned(
          frame.data.data(), intermediate_frame_.data(), input_frame_width_,
          input_frame_height_);
      int half_w = input_frame_width_ / 2;
      int half_h = input_frame_height_ / 2;
      detail::downsample_yuv420_2x(intermediate_frame_.data(), inference_frame_.data(),
                                   half_w, half_h);
    }
  }

  // Under mock/testing settings
  if (sim_pedestrian_present_) {
    float max_score = 0.0f;
    if (sim_score_ >= confidence_threshold_) {
      float x_min = sim_box_[0];
      float y_min = sim_box_[1];
      float x_max = sim_box_[2];
      float y_max = sim_box_[3];
      if (should_keep_detection(x_min, y_min, x_max, y_max, sim_score_)) {
        vigo::backend::DetectionResult det;
        det.box = {x_min, y_min, x_max, y_max};
        det.score = sim_score_;
        det.label = "pedestrian";
        results.push_back(std::move(det));
        max_score = sim_score_;
        ESP_LOGI(
            TAG,
            "Simulated Pedestrian Detected! BBox=[%.2f, %.2f, %.2f, %.2f], Score=%.2f",
            x_min, y_min, x_max, y_max, sim_score_);
      } else {
        ESP_LOGD(TAG, "Simulated detection discarded by filters");
        if (has_discarded) {
          *has_discarded = true;
        }
      }
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

      auto draw_pixel = [&](int px, int py) {
        size_t y_idx = py * inference_frame_width_ + px;
        inference_frame_[y_idx] = Y_val;

        size_t uv_base = inference_frame_width_ * inference_frame_height_;
        size_t uv_offset = (py / 2) * (inference_frame_width_ / 2) + (px / 2);
        inference_frame_[uv_base + uv_offset] = U_val;
        inference_frame_[uv_base +
                         (inference_frame_width_ * inference_frame_height_ / 4) +
                         uv_offset] = V_val;
      };

      // Draw horizontal lines
      for (int y : {y1, y2}) {
        if (y >= 0 && y < static_cast<int>(inference_frame_height_)) {
          int start_x = std::clamp(std::min(x1, x2), 0,
                                   static_cast<int>(inference_frame_width_) - 1);
          int end_x = std::clamp(std::max(x1, x2), 0,
                                 static_cast<int>(inference_frame_width_) - 1);
          for (int x = start_x; x <= end_x; ++x) {
            draw_pixel(x, y);
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
            draw_pixel(x, y);
          }
        }
      }
    }

    update_debug_frame(inference_frame_.data(), inference_frame_width_,
                       inference_frame_height_, max_score, using_uyvy);
    return results;
  }

  if (!impl_) {
    ESP_LOGW(TAG, "Hardware Pedestrian Detect is not initialized or running in Mock "
                  "camera mode.");
    update_debug_frame(inference_frame_.data(), inference_frame_width_,
                       inference_frame_height_, 0.0f, using_uyvy);
    return results;
  }

  ESP_LOGI(TAG, "Running HW Pedestrian Detect (pico_s8_v1) on %zu x %zu frame...",
           frame.width, frame.height);

  // Set threshold in implementation
  impl_->set_score_thr(confidence_threshold_);

  // The ESP-DL model requires 3-channel RGB input, but we only have the Y-plane
  // (Grayscale). We must quickly duplicate the Y-plane into a temporary RGB888 buffer.
  std::vector<uint8_t> rgb_inference_frame(inference_frame_width_ *
                                           inference_frame_height_ * 3);
  for (size_t i = 0; i < inference_frame_width_ * inference_frame_height_; ++i) {
    uint8_t y;
    if (using_uyvy) {
      size_t y_coord = i / inference_frame_width_;
      size_t x_coord = i % inference_frame_width_;
      size_t row_start = y_coord * (inference_frame_width_ * 3 / 2);
      y = inference_frame_[row_start + (x_coord / 2) * 3 + 1 + (x_coord % 2)];
    } else {
      y = inference_frame_[i];
    }
    rgb_inference_frame[i * 3 + 0] = y; // R
    rgb_inference_frame[i * 3 + 1] = y; // G
    rgb_inference_frame[i * 3 + 2] = y; // B
  }

  // Prepare input image descriptor for ESP-DL
  dl::image::img_t src_img = {.data = rgb_inference_frame.data(),
                              .width = static_cast<uint16_t>(inference_frame_width_),
                              .height = static_cast<uint16_t>(inference_frame_height_),
                              .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888};

  // Run the ESP-DL inference pipeline
  std::list<dl::detect::result_t> &results_list = impl_->run(src_img);

  float max_score = 0.0f;
  for (const auto &res : results_list) {
    if (res.score >= confidence_threshold_) {
      float x_min = static_cast<float>(res.box[0]) / inference_frame_width_;
      float y_min = static_cast<float>(res.box[1]) / inference_frame_height_;
      float x_max = static_cast<float>(res.box[2]) / inference_frame_width_;
      float y_max = static_cast<float>(res.box[3]) / inference_frame_height_;

      if (should_keep_detection(x_min, y_min, x_max, y_max, res.score)) {
        vigo::backend::DetectionResult det;
        det.box = {x_min, y_min, x_max, y_max};
        det.score = res.score;
        det.label = "pedestrian";
        results.push_back(std::move(det));

        if (res.score > max_score) {
          max_score = res.score;
        }

        ESP_LOGI(TAG, "Pedestrian Detected! BBox=[%.2f, %.2f, %.2f, %.2f], Score=%.2f",
                 x_min, y_min, x_max, y_max, res.score);
      } else {
        if (has_discarded) {
          *has_discarded = true;
        }
      }
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

    auto draw_pixel = [&](int px, int py) {
      if (using_uyvy) {
        size_t row_start = py * (inference_frame_width_ * 3 / 2);
        size_t y_offset = (px / 2) * 3 + 1 + (px % 2);
        inference_frame_[row_start + y_offset] = Y_val;

        size_t chroma_offset = (px / 2) * 3;
        if (py % 2 == 0) {
          inference_frame_[row_start + chroma_offset] = U_val;
        } else {
          inference_frame_[row_start + chroma_offset] = V_val;
        }
      } else {
        size_t y_idx = py * inference_frame_width_ + px;
        inference_frame_[y_idx] = Y_val;

        size_t uv_base = inference_frame_width_ * inference_frame_height_;
        size_t uv_offset = (py / 2) * (inference_frame_width_ / 2) + (px / 2);
        inference_frame_[uv_base + uv_offset] = U_val;
        inference_frame_[uv_base +
                         (inference_frame_width_ * inference_frame_height_ / 4) +
                         uv_offset] = V_val;
      }
    };

    for (int y : {y1, y2}) {
      if (y >= 0 && y < static_cast<int>(inference_frame_height_)) {
        int start_x = std::clamp(std::min(x1, x2), 0,
                                 static_cast<int>(inference_frame_width_) - 1);
        int end_x = std::clamp(std::max(x1, x2), 0,
                               static_cast<int>(inference_frame_width_) - 1);
        for (int x = start_x; x <= end_x; ++x) {
          draw_pixel(x, y);
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
          draw_pixel(x, y);
        }
      }
    }
  }

  update_debug_frame(inference_frame_.data(), inference_frame_width_,
                     inference_frame_height_, max_score, using_uyvy);

  return results;
}

bool PedestrianDetector::should_keep_detection(float x_min, float y_min, float x_max,
                                               float y_max, float score) const {
  float w = x_max - x_min;
  float h = y_max - y_min;
  if (w <= 0.0f || h <= 0.0f) {
    return false;
  }
  float area = w * h;
  float aspect_ratio = w / h;

  if (area > max_area_proportion_) {
    ESP_LOGI(TAG,
             "Discarded pedestrian detection (score=%.2f) due to area: %.3f > %.3f",
             score, area, max_area_proportion_);
    return false;
  }

  if (min_aspect_ratio_ > 0.0f && aspect_ratio < min_aspect_ratio_) {
    ESP_LOGI(TAG,
             "Discarded pedestrian detection (score=%.2f) due to aspect ratio (too "
             "narrow): %.3f < %.3f",
             score, aspect_ratio, min_aspect_ratio_);
    return false;
  }

  if (max_aspect_ratio_ > 0.0f && aspect_ratio > max_aspect_ratio_) {
    ESP_LOGI(TAG,
             "Discarded pedestrian detection (score=%.2f) due to aspect ratio (too "
             "wide): %.3f > %.3f",
             score, aspect_ratio, max_aspect_ratio_);
    return false;
  }

  return true;
}

} // namespace vigo::detection
