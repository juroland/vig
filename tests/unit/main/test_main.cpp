#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "surveillance_pipeline.hpp"
#include "pedestrian_detector.hpp"
#include "camera.hpp"
#include <string>

static const char *TAG = "TestPipelineUnit";

// Helper to create a camera frame with specified pixel value (OUYY_EVYY YUV420 format)
static vigo::camera::CameraFrame create_mock_frame(uint8_t pixel_value) {
  vigo::camera::CameraFrame frame;
  frame.width = 64;
  frame.height = 64;
  frame.data.resize(64 * 64 * 3 / 2);
  std::fill(frame.data.begin(), frame.data.end(), pixel_value);
  return frame;
}

TEST_CASE("1. Short-circuit on NO motion", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 1: Short-circuit on NO motion");

  vigo::pipeline::SurveillancePipeline pipeline(
      4,    // stride
      4,    // threshold
      0.01f,// min change ratio
      1000, // cooldown ms
      0.75f // pedestrian threshold
  );

  // Feeding two identical frames results in no motion detected
  auto frame1 = create_mock_frame(128);
  auto frame2 = create_mock_frame(128);

  // Initialize background frame
  pipeline.process(frame1);

  // Process identical frame: should return false for motion and bail early
  vigo::detection::PedestrianDetect::set_simulated_pedestrian_present(true); // Even if pedestrian is present, we shouldn't scan
  auto result = pipeline.process(frame2);

  TEST_ASSERT_FALSE_MESSAGE(result.motion_detected, "Motion should NOT have been detected");
  TEST_ASSERT_FALSE_MESSAGE(result.pedestrian_confirmed, "Pedestrian check should have been skipped");
  TEST_ASSERT_TRUE(result.detections.empty());
}

TEST_CASE("2. Motion detected but NO human", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 2: Motion detected but NO human");

  vigo::pipeline::SurveillancePipeline pipeline(
      4,    // stride
      4,    // threshold
      0.01f,// min change ratio
      1000, // cooldown ms
      0.75f // pedestrian threshold
  );

  auto frame1 = create_mock_frame(100);
  auto frame2 = create_mock_frame(200); // Drastic pixel changes to trigger motion

  pipeline.process(frame1);

  // Configure pedestrian detector to return no match
  vigo::detection::PedestrianDetect::set_simulated_pedestrian_present(false);

  auto result = pipeline.process(frame2);

  TEST_ASSERT_TRUE_MESSAGE(result.motion_detected, "Motion SHOULD have been detected");
  TEST_ASSERT_FALSE_MESSAGE(result.pedestrian_confirmed, "Pedestrian should NOT have been confirmed");
  TEST_ASSERT_TRUE(result.detections.empty());
}

TEST_CASE("3. Motion and pedestrian detected (successful dispatch)", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 3: Motion and pedestrian detected");

  vigo::pipeline::SurveillancePipeline pipeline(
      4,    // stride
      4,    // threshold
      0.01f,// min change ratio
      1000, // cooldown ms
      0.75f // pedestrian threshold
  );

  auto frame1 = create_mock_frame(100);
  auto frame2 = create_mock_frame(200);

  pipeline.process(frame1);

  // Configure pedestrian detector to return high-confidence match
  vigo::detection::PedestrianDetect::set_simulated_pedestrian_present(true);
  vigo::detection::PedestrianDetect::set_simulated_score(0.85f);

  auto result = pipeline.process(frame2);

  TEST_ASSERT_TRUE_MESSAGE(result.motion_detected, "Motion SHOULD have been detected");
  TEST_ASSERT_TRUE_MESSAGE(result.pedestrian_confirmed, "Pedestrian SHOULD have been confirmed");
  TEST_ASSERT_EQUAL_INT(1, result.detections.size());
  
  auto &det = result.detections[0];
  TEST_ASSERT_EQUAL_STRING("pedestrian", det.label.c_str());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.85f, det.score);
  
  // Verify normalized coordinates are present
  TEST_ASSERT_EQUAL_INT(4, det.box.size());
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, det.box[0]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.20f, det.box[1]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.65f, det.box[2]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.85f, det.box[3]);
}

extern "C" void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  UNITY_BEGIN();
  unity_run_test_by_name("1. Short-circuit on NO motion");
  unity_run_test_by_name("2. Motion detected but NO human");
  unity_run_test_by_name("3. Motion and pedestrian detected (successful dispatch)");
  UNITY_END();
}
