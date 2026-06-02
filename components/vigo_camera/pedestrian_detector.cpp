#include "pedestrian_detector.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

// Include the official Espressif pedestrian detect component header
#include "pedestrian_detect.hpp"

static const char *TAG = "PedestrianDetect";

namespace vigo::detection {

PedestrianDetect::PedestrianDetect(float confidence_threshold)
    : confidence_threshold_(confidence_threshold) {
#ifndef CONFIG_VIGO_USE_MOCK_CAMERA
  ESP_LOGI(TAG, "Initializing hardware PedestrianDetect pico_s8_v1 model...");
  impl_ =
      new ::PedestrianDetect(::PedestrianDetect::PICO_S8_V1, true); // lazy_load = true
#endif
}

PedestrianDetect::~PedestrianDetect() {
  if (impl_) {
    delete impl_;
    impl_ = nullptr;
  }
}

// Static helper to convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to YUV422
// interleaved (YUYV) format, applying 2x2 software binning (downscaling).
static void convert_ouyy_evyy_to_yuyv_binned(const uint8_t *src, uint8_t *dst,
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

void PedestrianDetect::update_debug_frame(const uint8_t *yuyv, int width, int height,
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

bool PedestrianDetect::get_debug_frame(std::vector<uint8_t> &yuyv_out, int &width_out,
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
PedestrianDetect::detect(const vigo::camera::CameraFrame &frame) {
  std::vector<vigo::backend::DetectionResult> results;

  int snapshot_width = frame.width / 2;
  int snapshot_height = frame.height / 2;
  size_t yuyv_size = snapshot_width * snapshot_height * 2;

  uint8_t *yuyv_buf =
      static_cast<uint8_t *>(heap_caps_malloc(yuyv_size, MALLOC_CAP_SPIRAM));
  if (!yuyv_buf) {
    ESP_LOGE(TAG, "Failed to allocate YUYV conversion buffer for pedestrian detect");
    return results;
  }

  convert_ouyy_evyy_to_yuyv_binned(frame.data.data(), yuyv_buf, frame.width,
                                   frame.height);

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

    // Draw mock bounding boxes on yuyv_buf
    for (const auto &res : results) {
      int x1 = static_cast<int>(res.box[0] * snapshot_width);
      int y1 = static_cast<int>(res.box[1] * snapshot_height);
      int x2 = static_cast<int>(res.box[2] * snapshot_width);
      int y2 = static_cast<int>(res.box[3] * snapshot_height);

      uint8_t Y_val = 149;
      uint8_t U_val = 44;
      uint8_t V_val = 21;

      // Draw horizontal lines
      for (int y : {y1, y2}) {
        if (y >= 0 && y < snapshot_height) {
          int start_x = std::clamp(std::min(x1, x2), 0, snapshot_width - 1);
          int end_x = std::clamp(std::max(x1, x2), 0, snapshot_width - 1);
          for (int x = start_x; x <= end_x; ++x) {
            yuyv_buf[y * snapshot_width * 2 + x * 2] = Y_val;
            int chroma_idx = (x / 2) * 4;
            yuyv_buf[y * snapshot_width * 2 + chroma_idx + 1] = U_val;
            yuyv_buf[y * snapshot_width * 2 + chroma_idx + 3] = V_val;
          }
        }
      }
      // Draw vertical lines
      for (int x : {x1, x2}) {
        if (x >= 0 && x < snapshot_width) {
          int start_y = std::clamp(std::min(y1, y2), 0, snapshot_height - 1);
          int end_y = std::clamp(std::max(y1, y2), 0, snapshot_height - 1);
          for (int y = start_y; y <= end_y; ++y) {
            yuyv_buf[y * snapshot_width * 2 + x * 2] = Y_val;
            int chroma_idx = (x / 2) * 4;
            yuyv_buf[y * snapshot_width * 2 + chroma_idx + 1] = U_val;
            yuyv_buf[y * snapshot_width * 2 + chroma_idx + 3] = V_val;
          }
        }
      }
    }

    update_debug_frame(yuyv_buf, snapshot_width, snapshot_height, max_score);
    heap_caps_free(yuyv_buf);
    return results;
  }

  if (!impl_) {
    ESP_LOGW(TAG, "Hardware Pedestrian Detect is not initialized or running in Mock "
                  "camera mode.");
    update_debug_frame(yuyv_buf, snapshot_width, snapshot_height, 0.0f);
    heap_caps_free(yuyv_buf);
    return results;
  }

  ESP_LOGI(TAG, "Running HW Pedestrian Detect (pico_s8_v1) on %zu x %zu frame...",
           frame.width, frame.height);

  // Set threshold in implementation
  impl_->set_score_thr(confidence_threshold_);

  // Prepare input image descriptor for ESP-DL
  dl::image::img_t src_img = {.data = yuyv_buf,
                              .width = static_cast<uint16_t>(snapshot_width),
                              .height = static_cast<uint16_t>(snapshot_height),
                              .pix_type = dl::image::DL_IMAGE_PIX_TYPE_YUYV};

  // Run the Espressif ESP-DL inference pipeline
  std::list<dl::detect::result_t> &results_list = impl_->run(src_img);

  float max_score = 0.0f;
  for (const auto &res : results_list) {
    if (res.score >= confidence_threshold_) {
      vigo::backend::DetectionResult det;
      float x_min = static_cast<float>(res.box[0]) / snapshot_width;
      float y_min = static_cast<float>(res.box[1]) / snapshot_height;
      float x_max = static_cast<float>(res.box[2]) / snapshot_width;
      float y_max = static_cast<float>(res.box[3]) / snapshot_height;

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

  // Draw bounding boxes on yuyv_buf for the debug frame
  for (const auto &res : results) {
    int x1 = static_cast<int>(res.box[0] * snapshot_width);
    int y1 = static_cast<int>(res.box[1] * snapshot_height);
    int x2 = static_cast<int>(res.box[2] * snapshot_width);
    int y2 = static_cast<int>(res.box[3] * snapshot_height);

    uint8_t Y_val = 149;
    uint8_t U_val = 44;
    uint8_t V_val = 21;

    for (int y : {y1, y2}) {
      if (y >= 0 && y < snapshot_height) {
        int start_x = std::clamp(std::min(x1, x2), 0, snapshot_width - 1);
        int end_x = std::clamp(std::max(x1, x2), 0, snapshot_width - 1);
        for (int x = start_x; x <= end_x; ++x) {
          yuyv_buf[y * snapshot_width * 2 + x * 2] = Y_val;
          int chroma_idx = (x / 2) * 4;
          yuyv_buf[y * snapshot_width * 2 + chroma_idx + 1] = U_val;
          yuyv_buf[y * snapshot_width * 2 + chroma_idx + 3] = V_val;
        }
      }
    }
    for (int x : {x1, x2}) {
      if (x >= 0 && x < snapshot_width) {
        int start_y = std::clamp(std::min(y1, y2), 0, snapshot_height - 1);
        int end_y = std::clamp(std::max(y1, y2), 0, snapshot_height - 1);
        for (int y = start_y; y <= end_y; ++y) {
          yuyv_buf[y * snapshot_width * 2 + x * 2] = Y_val;
          int chroma_idx = (x / 2) * 4;
          yuyv_buf[y * snapshot_width * 2 + chroma_idx + 1] = U_val;
          yuyv_buf[y * snapshot_width * 2 + chroma_idx + 3] = V_val;
        }
      }
    }
  }

  update_debug_frame(yuyv_buf, snapshot_width, snapshot_height, max_score);

  heap_caps_free(yuyv_buf);
  return results;
}

} // namespace vigo::detection
