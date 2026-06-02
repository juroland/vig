#pragma once

#include "camera.hpp"
#include "vigo_backend.hpp"
#include <mutex>
#include <vector>

// Forward declare the ESP-DL PedestrianDetect class to keep compilation fast and
// decoupling high.
class PedestrianDetect;

namespace vigo::detection {

struct DebugFrame {
  std::vector<uint8_t> yuyv_data;
  int width{0};
  int height{0};
  float probability{0.0f};
  bool has_frame{false};
};

class PedestrianDetect {
public:
  explicit PedestrianDetect(float confidence_threshold = 0.75f);
  ~PedestrianDetect();

  // Run the detection inference on the frame buffer.
  std::vector<vigo::backend::DetectionResult>
  detect(const vigo::camera::CameraFrame &frame);

  void set_threshold(float threshold) { confidence_threshold_ = threshold; }
  float get_threshold() const { return confidence_threshold_; }

  static void update_debug_frame(const uint8_t *yuyv, int width, int height,
                                 float probability);
  static bool get_debug_frame(std::vector<uint8_t> &yuyv_out, int &width_out,
                              int &height_out, float &probability_out);

  // Simulation & Mocking Controls for testing and interactive dashboards
  static void set_simulated_pedestrian_present(bool present) {
    sim_pedestrian_present_ = present;
  }
  static void set_simulated_score(float score) { sim_score_ = score; }

private:
  float confidence_threshold_;
  ::PedestrianDetect *impl_{nullptr};

  static inline std::mutex debug_mutex_;
  static inline DebugFrame debug_frame_;

  static inline bool sim_pedestrian_present_{false};
  static inline float sim_score_{0.85f};
};

} // namespace vigo::detection
