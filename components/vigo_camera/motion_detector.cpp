#include "motion_detector.hpp"

#include <cmath>
#include <cstdlib>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "VigMotion";

// Implement the get_moving_point_number algorithm matching the Espressif esp-dl
// signature, but with global illumination compensation
namespace dl::image {

uint32_t get_moving_point_number(const uint8_t *img1, const uint8_t *img2, int width,
                                 int height, int stride, uint8_t threshold) {
  int64_t diff_sum = 0;
  uint32_t n_points = 0;
  for (int i = 0; i < height; i += stride) {
    const uint8_t *row1 = img1 + i * width;
    const uint8_t *row2 = img2 + i * width;
    const uint8_t *ptr1 = row1;
    const uint8_t *ptr2 = row2;
    for (int j = 0; j < width; j += stride) {
      diff_sum += static_cast<int>(*ptr2) - static_cast<int>(*ptr1);
      ptr1 += stride;
      ptr2 += stride;
      n_points++;
    }
  }

  int avg_diff = 0;
  if (n_points > 0) {
    avg_diff = static_cast<int>(diff_sum / n_points);
  }

  uint32_t n_moving_pts = 0;
  for (int i = 0; i < height; i += stride) {
    const uint8_t *row1 = img1 + i * width;
    const uint8_t *row2 = img2 + i * width;
    const uint8_t *ptr1 = row1;
    const uint8_t *ptr2 = row2;
    for (int j = 0; j < width; j += stride) {
      int diff = (static_cast<int>(*ptr2) - static_cast<int>(*ptr1)) - avg_diff;
      if (std::abs(diff) > threshold) {
        n_moving_pts++;
      }
      ptr1 += stride;
      ptr2 += stride;
    }
  }
  return n_moving_pts;
}

uint32_t get_moving_point_number_uyvy(const uint8_t *img1, const uint8_t *img2,
                                      int width, int height, int stride,
                                      uint8_t threshold) {
  int64_t diff_sum = 0;
  uint32_t n_points = 0;
  int row_stride = width * 3 / 2;

  if (stride % 2 == 0) {
    int col_step_bytes = (stride / 2) * 3;
    for (int i = 0; i < height; i += stride) {
      const uint8_t *row1 = img1 + i * row_stride;
      const uint8_t *row2 = img2 + i * row_stride;
      const uint8_t *ptr1 = row1 + 1;
      const uint8_t *ptr2 = row2 + 1;
      for (int j = 0; j < width; j += stride) {
        diff_sum += static_cast<int>(*ptr2) - static_cast<int>(*ptr1);
        ptr1 += col_step_bytes;
        ptr2 += col_step_bytes;
        n_points++;
      }
    }

    int avg_diff = 0;
    if (n_points > 0) {
      avg_diff = static_cast<int>(diff_sum / n_points);
    }

    uint32_t n_moving_pts = 0;
    for (int i = 0; i < height; i += stride) {
      const uint8_t *row1 = img1 + i * row_stride;
      const uint8_t *row2 = img2 + i * row_stride;
      const uint8_t *ptr1 = row1 + 1;
      const uint8_t *ptr2 = row2 + 1;
      for (int j = 0; j < width; j += stride) {
        int diff = (static_cast<int>(*ptr2) - static_cast<int>(*ptr1)) - avg_diff;
        if (std::abs(diff) > threshold) {
          n_moving_pts++;
        }
        ptr1 += col_step_bytes;
        ptr2 += col_step_bytes;
      }
    }
    return n_moving_pts;
  } else {
    // Fallback if stride is odd (rare/unexpected)
    for (int i = 0; i < height; i += stride) {
      int row_offset = i * row_stride;
      for (int j = 0; j < width; j += stride) {
        int offset = row_offset + (j / 2) * 3 + 1 + (j % 2);
        diff_sum += static_cast<int>(img2[offset]) - static_cast<int>(img1[offset]);
        n_points++;
      }
    }

    int avg_diff = 0;
    if (n_points > 0) {
      avg_diff = static_cast<int>(diff_sum / n_points);
    }

    uint32_t n_moving_pts = 0;
    for (int i = 0; i < height; i += stride) {
      int row_offset = i * row_stride;
      for (int j = 0; j < width; j += stride) {
        int offset = row_offset + (j / 2) * 3 + 1 + (j % 2);
        int diff = (static_cast<int>(img2[offset]) - static_cast<int>(img1[offset])) -
                   avg_diff;
        if (std::abs(diff) > threshold) {
          n_moving_pts++;
        }
      }
    }
    return n_moving_pts;
  }
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

bool MotionDetector::ppa_motion_done_cb(ppa_client_t *ppa_client,
                                        ppa_event_data_t *event_data, void *user_data) {
  auto detector = static_cast<MotionDetector *>(user_data);

  BaseType_t high_task_woken = pdFALSE;

  // Unblock the worker task
  if (detector->worker_task_handle_ != nullptr) {
    vTaskNotifyGiveFromISR(detector->worker_task_handle_, &high_task_woken);
  }

  // If the worker task has a higher priority than the currently interrupted task,
  // force an immediate context switch so the worker runs right now.
  return high_task_woken == pdTRUE;
}

MotionDetector::MotionDetector(int stride, uint8_t threshold, float min_change_ratio,
                               uint32_t cooldown_ms)
    : stride_(stride), threshold_(threshold), min_change_ratio_(min_change_ratio),
      cooldown_ms_(cooldown_ms), ppa_client_(nullptr) {

  // Register PPA client for SRM operation
  ppa_client_config_t config = {};
  config.oper_type = PPA_OPERATION_SRM;
  config.max_pending_trans_num = 1;
  config.data_burst_length = PPA_DATA_BURST_LENGTH_64;

  esp_err_t err = ppa_register_client(
      &config, reinterpret_cast<ppa_client_handle_t *>(&ppa_client_));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register PPA client: %d", err);
  } else {
    ppa_event_callbacks_t cbs = {};
    cbs.on_trans_done = MotionDetector::ppa_motion_done_cb;
    if (ppa_client_register_event_callbacks(
            static_cast<ppa_client_handle_t>(ppa_client_), &cbs) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register PPA client event callbacks");
    }
  }

  queue_handle_ = xQueueCreate(2, sizeof(MotionWorkItem));
  xTaskCreatePinnedToCore(MotionDetector::worker_task_entry, "motion_worker", 4096,
                          this, 5,
                          reinterpret_cast<TaskHandle_t *>(&worker_task_handle_), 0);
}

MotionDetector::~MotionDetector() {
  if (worker_task_handle_) {
    vTaskDelete(worker_task_handle_);
    worker_task_handle_ = nullptr;
  }
  if (queue_handle_) {
    vQueueDelete(static_cast<QueueHandle_t>(queue_handle_));
    queue_handle_ = nullptr;
  }
  if (ppa_client_) {
    ppa_unregister_client(static_cast<ppa_client_handle_t>(ppa_client_));
    ppa_client_ = nullptr;
  }
}

void MotionDetector::process_frame_async(const vigo::camera::CameraFrame &frame) {
  // Lock the pipeline to prevent tearing
  bool expected = false;
  if (!is_processing_.compare_exchange_strong(expected, true,
                                              std::memory_order_acquire)) {
    ESP_LOGD(TAG, "Worker busy, dropping frame to maintain real-time performance");
    return;
  }

  int out_width = frame.width / 2;
  int out_height = frame.height / 2;
  // YUV420 requires width * height * 1.5 bytes
  size_t required_size = (out_width * out_height * 3) / 2;

  // Initialize or resize the buffer
  if (current_gray_buf_.size() != required_size) {
    current_gray_buf_.resize(required_size);
  }

  async_frame_ = frame;
  prev_width_ = out_width;
  prev_height_ = out_height;

  bool ppa_started = false;

  if (ppa_client_) {
    ppa_in_pic_blk_config_t in_cfg = {};
    in_cfg.buffer = frame.data.data();
    in_cfg.pic_w = static_cast<uint32_t>(frame.width);
    in_cfg.pic_h = static_cast<uint32_t>(frame.height);
    in_cfg.block_w = static_cast<uint32_t>(frame.width);
    in_cfg.block_h = static_cast<uint32_t>(frame.height);
    in_cfg.block_offset_x = 0;
    in_cfg.block_offset_y = 0;
    in_cfg.srm_cm = PPA_SRM_COLOR_MODE_YUV420;

    ppa_out_pic_blk_config_t out_cfg = {};
    out_cfg.buffer = current_gray_buf_.data();
    out_cfg.buffer_size = static_cast<uint32_t>(current_gray_buf_.size());
    out_cfg.pic_w = static_cast<uint32_t>(out_width);
    out_cfg.pic_h = static_cast<uint32_t>(out_height);
    out_cfg.block_offset_x = 0;
    out_cfg.block_offset_y = 0;
    out_cfg.srm_cm = PPA_SRM_COLOR_MODE_YUV420;

    ppa_srm_oper_config_t oper_cfg = {};
    oper_cfg.in = in_cfg;
    oper_cfg.out = out_cfg;
    oper_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    oper_cfg.scale_x = 0.5f;
    oper_cfg.scale_y = 0.5f;
    oper_cfg.mirror_x = false;
    oper_cfg.mirror_y = false;
    oper_cfg.rgb_swap = false;
    oper_cfg.byte_swap = false;
    oper_cfg.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    oper_cfg.mode = PPA_TRANS_MODE_NON_BLOCKING;
    oper_cfg.user_data = this;

    // REFACTOR: function for 64 bytes alignment
    size_t src_sync_size = (frame.data.size() + 63) & ~63;
    esp_cache_msync((void *)frame.data.data(), src_sync_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    esp_err_t err = ppa_do_scale_rotate_mirror(
        static_cast<ppa_client_handle_t>(ppa_client_), &oper_cfg);
    if (err == ESP_OK) {
      ppa_started = true;
    } else {
      ESP_LOGE(TAG, "Failed to start PPA SRM transaction: %d, falling back to CPU",
               err);
    }
  }

  ppa_used_for_current_frame_ = ppa_started;

  // If CPU fallback is required, we trigger the worker task manually
  if (!ppa_started) {
    xTaskNotifyGive(worker_task_handle_);
  }
}

void MotionDetector::worker_task_entry(void *param) {
  static_cast<MotionDetector *>(param)->worker_loop();
}

void MotionDetector::worker_loop() {
  while (true) {
    // Wait indefinitely for either the ISR (Hardware) or process_frame_async (Software)
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    int out_width = prev_width_;
    int out_height = prev_height_;
    bool used_ppa = ppa_used_for_current_frame_;

    if (used_ppa) {
      // REFACTOR: function for 64 bytes alignment
      size_t sync_size = (current_gray_buf_.size() + 63) & ~63;
      esp_cache_msync(current_gray_buf_.data(), sync_size,
                      ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    } else {
      convert_ouyy_evyy_to_gray_binned(async_frame_.data.data(),
                                       current_gray_buf_.data(), async_frame_.width,
                                       async_frame_.height);
    }

    if (prev_gray_buf_.empty()) {
      std::swap(prev_gray_buf_, current_gray_buf_);
      if (callback_) {
        callback_(false, async_frame_);
      }
      is_processing_.store(false, std::memory_order_release); // UNLOCK
      continue;
    }

    int total_points_x = (out_width + stride_ - 1) / stride_;
    int total_points_y = (out_height + stride_ - 1) / stride_;
    int total_points = total_points_x * total_points_y;

    if (total_points <= 0) {
      if (callback_) {
        callback_(false, async_frame_);
      }
      is_processing_.store(false, std::memory_order_release); // UNLOCK
      continue;
    }

    uint32_t moving_points;
    if (used_ppa) {
      moving_points = dl::image::get_moving_point_number_uyvy(
          prev_gray_buf_.data(), current_gray_buf_.data(), out_width, out_height,
          stride_, threshold_);
    } else {
      moving_points = dl::image::get_moving_point_number(
          prev_gray_buf_.data(), current_gray_buf_.data(), out_width, out_height,
          stride_, threshold_);
    }

    float change_ratio = static_cast<float>(moving_points) / total_points;

    // Safe swapping handles pointer update inside worker thread context
    std::swap(prev_gray_buf_, current_gray_buf_);

    bool is_motion = (change_ratio >= min_change_ratio_);
    bool report_motion = false;

    if (is_motion) {
      int64_t now_us = esp_timer_get_time();
      int64_t cooldown_us = static_cast<int64_t>(cooldown_ms_) * 1000;

      if (last_trigger_time_us_ == 0 ||
          (now_us - last_trigger_time_us_) >= cooldown_us) {
        ESP_LOGI(TAG, "Motion Detected! Points changed: %lu / %d (%.2f%%)",
                 moving_points, total_points, change_ratio * 100.0f);
        last_trigger_time_us_ = now_us;
        report_motion = true;
      } else {
        ESP_LOGD(TAG,
                 "Motion detected but throttled by cooldown (time since last trigger: "
                 "%.2f s)",
                 static_cast<double>(now_us - last_trigger_time_us_) / 1000000.0);
      }
    }

    if (callback_) {
      callback_(report_motion, async_frame_);
    }
    is_processing_.store(false, std::memory_order_release); // UNLOCK
  }
}

} // namespace vigo::motion
