#pragma once

#include "camera.hpp"
#include "motion_detector.hpp"
#include "pedestrian_detector.hpp"
#include "vigo_backend.hpp"
#include <vector>

namespace vigo::pipeline {

struct SurveillancePipelineResult {
  bool motion_detected{false};
  bool pedestrian_confirmed{false};
  std::vector<vigo::backend::DetectionResult> detections;
};

class SurveillancePipeline {
public:
  SurveillancePipeline(int frame_width, int frame_height, int motion_stride = 16,
                       uint8_t motion_threshold = 8,
                       float motion_min_change_ratio = 0.05f,
                       uint32_t motion_cooldown_ms = 10000,
                       float pedestrian_confidence_threshold = 0.75f,
                       int pedestrian_downscale_factor = 4);
  ~SurveillancePipeline() = default;

  // Process a captured camera frame through the cascading pipeline.
  SurveillancePipelineResult process(const vigo::camera::CameraFrame &frame);

  void set_pedestrian_threshold(float threshold) {
    pedestrian_detector_.set_threshold(threshold);
  }
  float get_pedestrian_threshold() const {
    return pedestrian_detector_.get_threshold();
  }

private:
  vigo::motion::MotionDetector motion_detector_;
  vigo::detection::PedestrianDetector pedestrian_detector_;
};

} // namespace vigo::pipeline
