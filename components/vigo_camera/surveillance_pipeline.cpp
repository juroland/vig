#include "surveillance_pipeline.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SurveillancePipeline";

namespace vigo::pipeline {

SurveillancePipeline::SurveillancePipeline(int frame_width, int frame_height,
                                           int motion_stride, uint8_t motion_threshold,
                                           float motion_min_change_ratio,
                                           uint32_t motion_cooldown_ms,
                                           float pedestrian_confidence_threshold)
    : motion_detector_(motion_stride, motion_threshold, motion_min_change_ratio,
                       motion_cooldown_ms),
      pedestrian_detector_(frame_width, frame_height, pedestrian_confidence_threshold) {
}

SurveillancePipelineResult
SurveillancePipeline::process(const vigo::camera::CameraFrame &frame) {
  SurveillancePipelineResult result;

  // Step 1: Run the fast Motion Detector first (The Guard/Filter Pattern)
  if (!motion_detector_.detect_motion(frame)) {
    // If NO motion is detected, immediately skip pedestrian check and return.
    // The main loop can then drop back into a brief sleep or passive state.
    return result;
  }

  result.motion_detected = true;
  ESP_LOGD(TAG, "Motion detected! Initiating high-confidence pedestrian scan...");

  // Step 2: High-Confidence Multi-stage Check
  result.detections = pedestrian_detector_.detect(frame);
  if (!result.detections.empty()) {
    result.pedestrian_confirmed = true;
    ESP_LOGI(TAG,
             "Pedestrian check confirmed high-confidence match! Detections count: %zu",
             result.detections.size());
  } else {
    ESP_LOGD(TAG,
             "Pedestrian check completed: No high-confidence human shape matched.");
  }

  return result;
}

} // namespace vigo::pipeline
