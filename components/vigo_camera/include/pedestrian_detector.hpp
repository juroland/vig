#pragma once

#include "camera.hpp"
#include "memory.hpp"
#include "vigo_backend.hpp"
#include <mutex>
#include <vector>

// ESP pedestrian_detect
class PedestrianDetect;

namespace vigo::detection {

// Internal helpers exposed for unit testing
namespace detail {

// Convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to YUV422
// interleaved (YUYV) format, applying 2x2 software binning (downscaling).
// Output dimensions: (original_width/2, original_height/2)
void convert_ouyy_evyy_to_yuyv_binned(const uint8_t *src, uint8_t *dst,
                                      int original_width, int original_height);

// Downsample YUYV interleaved frame by 2x in both dimensions.
// Averages 2x2 pixel blocks, producing (src_width/2, src_height/2) output.
void downsample_yuyv_2x(const uint8_t *src, uint8_t *dst, int src_width,
                        int src_height);

} // namespace detail

struct DebugFrame {
  std::vector<uint8_t> yuyv_data;
  int width{0};
  int height{0};
  float probability{0.0f};
  bool has_frame{false};
};

class PedestrianDetector {
public:
  explicit PedestrianDetector(int frame_width, int frame_height,
                              float confidence_threshold = 0.75f,
                              int downscale_factor = 4);
  ~PedestrianDetector();

  std::vector<vigo::backend::DetectionResult>
  detect(const vigo::camera::CameraFrame &frame);

  void set_threshold(float threshold) { confidence_threshold_ = threshold; }
  float get_threshold() const { return confidence_threshold_; }

  static void update_debug_frame(const uint8_t *yuyv, int width, int height,
                                 float probability);
  static bool get_debug_frame(std::vector<uint8_t> &yuyv_out, int &width_out,
                              int &height_out, float &probability_out);

  // Simulation & Mocking Controls
  static void set_simulated_pedestrian_present(bool present) {
    sim_pedestrian_present_ = present;
  }
  static void set_simulated_score(float score) { sim_score_ = score; }

private:
  ::PedestrianDetect *impl_{nullptr};

  float confidence_threshold_;
  int downscale_factor_;
  size_t input_frame_height_;
  size_t input_frame_width_;

  // Downscaled input frame for inference
  size_t inference_frame_height_;
  size_t inference_frame_width_;
  size_t inference_frame_bytes_;
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> inference_frame_;

  // Intermediate buffer for two-stage downscaling (used when downscale_factor > 2)
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>>
      intermediate_frame_;

  static inline std::mutex debug_mutex_;
  static inline DebugFrame debug_frame_;

  static inline bool sim_pedestrian_present_{false};
  static inline float sim_score_{0.85f};
};

} // namespace vigo::detection
