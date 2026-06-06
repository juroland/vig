#ifndef VIGO_MOTION_DETECTOR_HPP
#define VIGO_MOTION_DETECTOR_HPP

#include "camera.hpp"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "memory.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace vigo::motion {

struct MotionWorkItem {
  bool ppa_success;
  int out_width;
  int out_height;
  vigo::camera::CameraFrame frame;
};

class MotionDetector {
public:
  // stride: check every Nth pixel in X and Y
  // threshold: activation threshold of each pixel difference (0-255)
  // min_change_ratio: fraction of active points required to trigger motion (e.g. 0.015
  // for 1.5%) cooldown_ms: minimum time between sent events (e.g. 10000 for 10 seconds)
  MotionDetector(int stride = 16, uint8_t threshold = 8, float min_change_ratio = 0.05f,
                 uint32_t cooldown_ms = 10000);

  ~MotionDetector();

  // Submit frame for motion processing (asynchronous)
  void process_frame_async(const vigo::camera::CameraFrame &frame);

  using MotionCallback =
      std::function<void(bool motion_detected, const vigo::camera::CameraFrame &frame)>;
  void set_callback(MotionCallback cb) { callback_ = cb; }

  bool is_async_mode() const { return async_mode_; }

private:
  int stride_;
  uint8_t threshold_;
  float min_change_ratio_;
  uint32_t cooldown_ms_;
  bool async_mode_;

  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> prev_gray_buf_;
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> current_gray_buf_;
  int prev_width_{0};
  int prev_height_{0};

  int64_t last_trigger_time_us_{0};

  ppa_client_handle_t ppa_client_{nullptr};
  bool ppa_used_for_current_frame_{false};

  // Async management
  QueueHandle_t queue_handle_{nullptr};
  TaskHandle_t worker_task_handle_{nullptr};
  MotionCallback callback_;
  vigo::camera::CameraFrame async_frame_;

  std::atomic<bool> is_processing_{false};

  static bool ppa_motion_done_cb(ppa_client_t *ppa_client, ppa_event_data_t *event_data,
                                 void *user_data);

  static void worker_task_entry(void *param);
  void worker_loop();
};

} // namespace vigo::motion

#endif // VIGO_MOTION_DETECTOR_HPP
