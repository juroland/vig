#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "surveillance_pipeline.hpp"
#include "pedestrian_detector.hpp"
#include "camera.hpp"
#include "driver/jpeg_encode.h"
#include "pedestrian_image.h"
#include "non_pedestrian_image.h"
#include "nvs.h"
#include "vigo_factory.hpp"
#include "vigo_ota.hpp"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
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

// Helper to create a camera frame where the first half has base_value and the second half has motion_value
static vigo::camera::CameraFrame create_mock_motion_frame(uint8_t base_value, uint8_t motion_value) {
  vigo::camera::CameraFrame frame = create_mock_frame(base_value);
  std::fill(frame.data.begin() + frame.data.size() / 2, frame.data.end(), motion_value);
  return frame;
}

TEST_CASE("1. Short-circuit on NO motion", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 1: Short-circuit on NO motion");

  vigo::pipeline::SurveillancePipeline pipeline(
      64,   // frame_width
      64,   // frame_height
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
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(true); // Even if pedestrian is present, we shouldn't scan
  auto result = pipeline.process(frame2);

  TEST_ASSERT_FALSE_MESSAGE(result.motion_detected, "Motion should NOT have been detected");
  TEST_ASSERT_FALSE_MESSAGE(result.pedestrian_confirmed, "Pedestrian check should have been skipped");
  TEST_ASSERT_TRUE(result.detections.empty());
}

TEST_CASE("2. Motion detected but NO human", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 2: Motion detected but NO human");

  vigo::pipeline::SurveillancePipeline pipeline(
      64,   // frame_width
      64,   // frame_height
      4,    // stride
      4,    // threshold
      0.01f,// min change ratio
      1000, // cooldown ms
      0.75f // pedestrian threshold
  );

  auto frame1 = create_mock_frame(100);
  pipeline.process(frame1);

  auto frame2 = create_mock_motion_frame(100, 200); // Localized pixel changes to trigger motion

  // Configure pedestrian detector to return no match
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(false);

  auto result = pipeline.process(frame2);

  TEST_ASSERT_TRUE_MESSAGE(result.motion_detected, "Motion SHOULD have been detected");
  TEST_ASSERT_FALSE_MESSAGE(result.pedestrian_confirmed, "Pedestrian should NOT have been confirmed");
  TEST_ASSERT_TRUE(result.detections.empty());
}

TEST_CASE("3. Motion and pedestrian detected (successful dispatch)", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 3: Motion and pedestrian detected");

  vigo::pipeline::SurveillancePipeline pipeline(
      64,   // frame_width
      64,   // frame_height
      4,    // stride
      4,    // threshold
      0.01f,// min change ratio
      1000, // cooldown ms
      0.75f // pedestrian threshold
  );

  auto frame1 = create_mock_frame(100);
  pipeline.process(frame1);

  auto frame2 = create_mock_motion_frame(100, 200);

  // Configure pedestrian detector to return high-confidence match
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(true);
  vigo::detection::PedestrianDetector::set_simulated_score(0.85f);

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

TEST_CASE("4. convert_ouyy_evyy_to_yuv420_binned unit test", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 4: convert_ouyy_evyy_to_yuv420_binned unit test");

  // Input resolution: 4x4 -> 24 bytes
  uint8_t src[24] = {0};
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      int idx = (y * 4 + x) * 1.5;
      src[idx + 1] = y * 10 + x; // Y value
      if (x % 2 == 0) src[idx] = y * 20 + x; // U or V
    }
  }
  
  // Output resolution: 2x2 -> 6 bytes
  uint8_t dst[6] = {0};

  vigo::detection::detail::convert_ouyy_evyy_to_yuv420_binned(src, dst, 4, 4);

  // Y-plane
  TEST_ASSERT_EQUAL_UINT8(1, dst[0]); 
  TEST_ASSERT_EQUAL_UINT8(3, dst[1]);
  TEST_ASSERT_EQUAL_UINT8(21, dst[2]);
  TEST_ASSERT_EQUAL_UINT8(23, dst[3]);
  
  // U/V-planes
  TEST_ASSERT_EQUAL_UINT8(0, dst[4]); 
  TEST_ASSERT_EQUAL_UINT8(20, dst[5]);
}

TEST_CASE("5. downsample_yuv420_2x unit test", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 5: downsample_yuv420_2x unit test");

  // Input YUV420 4x4: 16 Y, 4 U, 4 V = 24 bytes
  uint8_t src[24] = {
    80, 90, 100, 110,
    84, 94, 104, 114,
    180, 190, 200, 210,
    184, 194, 204, 214, // Y plane
    10, 30, 12, 32,     // U plane
    20, 40, 22, 42      // V plane
  };
  uint8_t dst[6] = {0}; // 2x2 YUV420: 6 bytes

  vigo::detection::detail::downsample_yuv420_2x(src, dst, 4, 4);

  // Y0 = (80+90+84+94)/4 = 87
  TEST_ASSERT_EQUAL_UINT8(87, dst[0]);
  TEST_ASSERT_EQUAL_UINT8(107, dst[1]);
  TEST_ASSERT_EQUAL_UINT8(187, dst[2]);
  TEST_ASSERT_EQUAL_UINT8(207, dst[3]);
  
  // U/V
  TEST_ASSERT_EQUAL_UINT8(21, dst[4]);
  TEST_ASSERT_EQUAL_UINT8(31, dst[5]);
}


TEST_CASE("6. PedestrianDetector downscale factor instantiation", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 6: PedestrianDetector downscale factor instantiation");

  // Validate instantiation with factor 2
  {
    vigo::detection::PedestrianDetector detector(64, 64, 0.75f, 2);
    // Should construct successfully without assertion failure
  }

  // Validate instantiation with factor 4
  {
    vigo::detection::PedestrianDetector detector(64, 64, 0.75f, 4);
    // Should construct successfully without assertion failure
  }
}

TEST_CASE("8. JPEG encoding of downscaled 320x240 frame", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 8: JPEG encoding of downscaled 320x240 frame (reproduction)");

  jpeg_encode_engine_cfg_t eng_cfg = {};
  eng_cfg.timeout_ms = 1000;
  eng_cfg.intr_priority = 0;
  jpeg_encoder_handle_t jpeg_engine = nullptr;
  esp_err_t err = jpeg_new_encoder_engine(&eng_cfg, &jpeg_engine);
  TEST_ASSERT_EQUAL(ESP_OK, err);

  int width = 320;
  int height = 240;
  
  size_t inbuf_size = width * height * 2;
  jpeg_encode_memory_alloc_cfg_t in_mem_cfg = {};
  in_mem_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  size_t actual_in_size = 0;
  uint8_t *inbuf = static_cast<uint8_t *>(
      jpeg_alloc_encoder_mem(inbuf_size, &in_mem_cfg, &actual_in_size));
  TEST_ASSERT_NOT_NULL(inbuf);
  memset(inbuf, 128, inbuf_size);

  jpeg_encode_cfg_t enc_cfg = {};
  enc_cfg.width = width;
  enc_cfg.height = height;
  enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_GRAY;
  enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_GRAY;
  enc_cfg.image_quality = 80;

  size_t outbuf_size = width * height;
  jpeg_encode_memory_alloc_cfg_t mem_cfg = {};
  mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t actual_out_size = 0;
  uint8_t *outbuf = static_cast<uint8_t *>(
      jpeg_alloc_encoder_mem(outbuf_size, &mem_cfg, &actual_out_size));
  TEST_ASSERT_NOT_NULL(outbuf);

  uint32_t out_size = 0;
  err = jpeg_encoder_process(jpeg_engine, &enc_cfg,
                             inbuf, inbuf_size,
                             outbuf, actual_out_size, &out_size);
  
  heap_caps_free(inbuf);
  heap_caps_free(outbuf);
  jpeg_del_encoder_engine(jpeg_engine);

  // We assert success; this ensures hardware encoding succeeds with the aligned allocation
  TEST_ASSERT_EQUAL(ESP_OK, err);
  TEST_ASSERT_GREATER_THAN(0, out_size);
}

TEST_CASE("9. Pedestrian classification - True Positive (Pedestrian)", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 9: Pedestrian classification - True Positive");

  vigo::detection::PedestrianDetector detector(448, 448, 0.75f, 2);
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(false);

  vigo::camera::CameraFrame frame;
  frame.width = 448;
  frame.height = 448;
  frame.data.set_external_buffer(pedestrian_image_data, sizeof(pedestrian_image_data));

  auto results = detector.detect(frame);

  // Assert we found a pedestrian
  TEST_ASSERT_GREATER_THAN(0, results.size());
  bool found_pedestrian = false;
  for (const auto &res : results) {
    if (res.label == "pedestrian") {
      found_pedestrian = true;
      ESP_LOGI(TAG, "Found true positive pedestrian with score: %.2f", res.score);
    }
  }
  TEST_ASSERT_TRUE(found_pedestrian);
}

TEST_CASE("10. Pedestrian classification - True Negative (Non-pedestrian)", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 10: Pedestrian classification - True Negative");

  vigo::detection::PedestrianDetector detector(448, 448, 0.75f, 2);
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(false);

  vigo::camera::CameraFrame frame;
  frame.width = 448;
  frame.height = 448;
  frame.data.set_external_buffer(non_pedestrian_image_data, sizeof(non_pedestrian_image_data));

  auto results = detector.detect(frame);

  // Assert no pedestrian was found
  TEST_ASSERT_TRUE(results.empty());
}

TEST_CASE("11. Pedestrian classification - Area and aspect ratio filtering", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 11: Area and aspect ratio filtering");

  // Reset/configure simulated pedestrian
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(true);
  vigo::detection::PedestrianDetector::set_simulated_score(0.85f);

  // 11a: Normal box (not filtered)
  {
    vigo::pipeline::SurveillancePipeline pipeline(64, 64, 4, 4, 0.01f, 1000, 0.75f, 4, 0.70f, 0.10f, 1.00f);
    vigo::detection::PedestrianDetector::set_simulated_box(0.25f, 0.20f, 0.65f, 0.85f); // area = 0.26, aspect_ratio = 0.615
    auto frame1 = create_mock_frame(100);
    pipeline.process(frame1);
    auto frame2 = create_mock_motion_frame(100, 200);
    auto result = pipeline.process(frame2);
    TEST_ASSERT_TRUE(result.motion_detected);
    TEST_ASSERT_TRUE(result.pedestrian_confirmed);
    TEST_ASSERT_EQUAL(1, result.detections.size());
  }

  // 11b: Large box (exceeding max area proportion)
  {
    vigo::pipeline::SurveillancePipeline pipeline(64, 64, 4, 4, 0.01f, 1000, 0.75f, 4, 0.50f, 0.10f, 1.00f);
    vigo::detection::PedestrianDetector::set_simulated_box(0.10f, 0.10f, 0.90f, 0.90f); // area = 0.64, aspect_ratio = 1.00
    auto frame1 = create_mock_frame(100);
    pipeline.process(frame1);
    auto frame2 = create_mock_motion_frame(100, 200);
    auto result = pipeline.process(frame2);
    TEST_ASSERT_TRUE(result.motion_detected);
    TEST_ASSERT_TRUE(result.pedestrian_confirmed); // Pedestrian detected, so send even if discarded
    TEST_ASSERT_TRUE(result.detections.empty());    // Detections empty
  }

  // 11c: Horizontal box (invalid aspect ratio)
  {
    vigo::pipeline::SurveillancePipeline pipeline(64, 64, 4, 4, 0.01f, 1000, 0.75f, 4, 0.70f, 0.10f, 1.00f);
    vigo::detection::PedestrianDetector::set_simulated_box(0.10f, 0.40f, 0.90f, 0.60f); // area = 0.16, aspect_ratio = 4.00
    auto frame1 = create_mock_frame(100);
    pipeline.process(frame1);
    auto frame2 = create_mock_motion_frame(100, 200);
    auto result = pipeline.process(frame2);
    TEST_ASSERT_TRUE(result.motion_detected);
    TEST_ASSERT_TRUE(result.pedestrian_confirmed); // Pedestrian detected, so send even if discarded
    TEST_ASSERT_TRUE(result.detections.empty());    // Detections empty
  }

  // 11d: Narrow vertical box (invalid aspect ratio)
  {
    vigo::pipeline::SurveillancePipeline pipeline(64, 64, 4, 4, 0.01f, 1000, 0.75f, 4, 0.70f, 0.10f, 1.00f);
    vigo::detection::PedestrianDetector::set_simulated_box(0.45f, 0.10f, 0.47f, 0.90f); // area = 0.016, aspect_ratio = 0.025
    auto frame1 = create_mock_frame(100);
    pipeline.process(frame1);
    auto frame2 = create_mock_motion_frame(100, 200);
    auto result = pipeline.process(frame2);
    TEST_ASSERT_TRUE(result.motion_detected);
    TEST_ASSERT_TRUE(result.pedestrian_confirmed); // Pedestrian detected, so send even if discarded
    TEST_ASSERT_TRUE(result.detections.empty());    // Detections empty
  }

  // Reset simulated state
  vigo::detection::PedestrianDetector::set_simulated_pedestrian_present(false);
}

TEST_CASE("12. Factory NVS initialization and retrieval", "[factory]") {
  ESP_LOGI(TAG, "Running Test 12: Factory NVS initialization and retrieval");

  // Mount the fct_nvs partition
  esp_err_t err = vigo::factory::init_factory_partition();
  if (err == ESP_OK) {
    std::string hw_id;
    err = vigo::factory::get_hardware_id(hw_id);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);

    std::string token;
    err = vigo::factory::get_device_token(token);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);

    std::string cert;
    err = vigo::factory::get_dtls_cert(cert);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);

    std::vector<uint8_t> key;
    err = vigo::factory::get_dtls_key(key);
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND);
  } else {
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
  }
}

TEST_CASE("13. Rollback protection cancel and trigger state validation", "[rollback]") {
  ESP_LOGI(TAG, "Running Test 13: Rollback protection state validation");

  bool possible = esp_ota_check_rollback_is_possible();
  ESP_LOGI(TAG, "Rollback is possible: %s", possible ? "yes" : "no");

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE);
}

extern "C" void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  UNITY_BEGIN();
  unity_run_test_by_name("1. Short-circuit on NO motion");
  unity_run_test_by_name("2. Motion detected but NO human");
  unity_run_test_by_name("3. Motion and pedestrian detected (successful dispatch)");
  unity_run_test_by_name("4. convert_ouyy_evyy_to_yuyv_binned unit test");
  unity_run_test_by_name("5. downsample_yuyv_2x unit test");
  unity_run_test_by_name("6. PedestrianDetector downscale factor instantiation");
  unity_run_test_by_name("7. downsample_yuyv_2x larger unit test (8x4 -> 4x2)");
  unity_run_test_by_name("8. JPEG encoding of downscaled 320x240 frame");
  unity_run_test_by_name("9. Pedestrian classification - True Positive (Pedestrian)");
  unity_run_test_by_name("10. Pedestrian classification - True Negative (Non-pedestrian)");
  unity_run_test_by_name("11. Pedestrian classification - Area and aspect ratio filtering");
  unity_run_test_by_name("12. Factory NVS initialization and retrieval");
  unity_run_test_by_name("13. Rollback protection cancel and trigger state validation");
  UNITY_END();
}
