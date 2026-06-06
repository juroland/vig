#ifndef VIGO_SURVEILLANCE_PIPELINE_HPP
#define VIGO_SURVEILLANCE_PIPELINE_HPP

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
                       int pedestrian_downscale_factor = 4,
                       float pedestrian_max_area_proportion = 0.70f,
                       float pedestrian_min_aspect_ratio = 0.10f,
                       float pedestrian_max_aspect_ratio = 1.00f);
  ~SurveillancePipeline() = default;

  void process(const vigo::camera::CameraFrame &frame);

  using PipelineCallback = std::function<void(const SurveillancePipelineResult &result,
                                              const vigo::camera::CameraFrame &frame)>;
  void set_callback(PipelineCallback cb);

  void set_pedestrian_threshold(float threshold) {
    pedestrian_detector_.set_threshold(threshold);
  }
  float get_pedestrian_threshold() const {
    return pedestrian_detector_.get_threshold();
  }

  void set_pedestrian_max_area_proportion(float prop) {
    pedestrian_detector_.set_max_area_proportion(prop);
  }
  float get_pedestrian_max_area_proportion() const {
    return pedestrian_detector_.get_max_area_proportion();
  }

  void set_pedestrian_min_aspect_ratio(float ratio) {
    pedestrian_detector_.set_min_aspect_ratio(ratio);
  }
  float get_pedestrian_min_aspect_ratio() const {
    return pedestrian_detector_.get_min_aspect_ratio();
  }

  void set_pedestrian_max_aspect_ratio(float ratio) {
    pedestrian_detector_.set_max_aspect_ratio(ratio);
  }
  float get_pedestrian_max_aspect_ratio() const {
    return pedestrian_detector_.get_max_aspect_ratio();
  }

private:
  vigo::motion::MotionDetector motion_detector_;
  vigo::detection::PedestrianDetector pedestrian_detector_;
  PipelineCallback pipeline_callback_;
  bool async_mode_;
};

} // namespace vigo::pipeline

#endif // VIGO_SURVEILLANCE_PIPELINE_HPP
