#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "surveillance_pipeline.hpp"
#include "pedestrian_detector.hpp"
#include "camera.hpp"
#include "driver/jpeg_encode.h"
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

  auto frame2 = create_mock_frame(200); // Drastic pixel changes to trigger motion

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

  auto frame2 = create_mock_frame(200);

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

TEST_CASE("4. convert_ouyy_evyy_to_yuyv_binned unit test", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 4: convert_ouyy_evyy_to_yuyv_binned unit test");

  // Input resolution: 4x2
  // Output resolution: 2x1
  uint8_t src[12] = {
    // Row 0 (6 bytes: stride = 4 * 1.5 = 6)
    // [u0, y00, unused, u1, y02, unused]
    10, 100, 0, 20, 110, 0,
    // Row 1 (6 bytes)
    // [v0, unused, unused, v1, unused, unused]
    30, 0,   0, 40, 0,   0
  };
  uint8_t dst[4] = {0}; // 2x1 YUYV is 4 bytes

  vigo::detection::detail::convert_ouyy_evyy_to_yuyv_binned(src, dst, 4, 2);

  // Expected averages:
  // u = (u0 + u1) / 2 = (10 + 20) / 2 = 15
  // v = (v0 + v1) / 2 = (30 + 40) / 2 = 35
  // y00 = 100
  // y02 = 110
  // dst is standard YUYV: [y00, v, y02, u] (swapped per implementation logic)
  TEST_ASSERT_EQUAL_UINT8(100, dst[0]); // Y00
  TEST_ASSERT_EQUAL_UINT8(35, dst[1]);  // V (which is U in standard YUYV)
  TEST_ASSERT_EQUAL_UINT8(110, dst[2]); // Y02
  TEST_ASSERT_EQUAL_UINT8(15, dst[3]);  // U (which is V in standard YUYV)
}

TEST_CASE("5. downsample_yuyv_2x unit test", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 5: downsample_yuyv_2x unit test");

  // Input YUYV 4x2: 16 bytes
  uint8_t src[16] = {
    // Row 0: [Y0 U Y1 V], [Y2 U Y3 V]
    80, 10, 90, 20, 100, 30, 110, 40,
    // Row 1: [Y0 U Y1 V], [Y2 U Y3 V]
    84, 12, 94, 22, 104, 32, 114, 42
  };
  uint8_t dst[4] = {0}; // 2x1 YUYV: 4 bytes

  vigo::detection::detail::downsample_yuyv_2x(src, dst, 4, 2);

  // Expected:
  // y0 = (80 + 90 + 84 + 94) / 4 = 348 / 4 = 87
  // y1 = (100 + 110 + 104 + 114) / 4 = 428 / 4 = 107
  // u = (10 + 30 + 12 + 32) / 4 = 84 / 4 = 21
  // v = (20 + 40 + 22 + 42) / 4 = 124 / 4 = 31
  // dst is: [y0, u, y1, v]
  TEST_ASSERT_EQUAL_UINT8(87, dst[0]);
  TEST_ASSERT_EQUAL_UINT8(21, dst[1]);
  TEST_ASSERT_EQUAL_UINT8(107, dst[2]);
  TEST_ASSERT_EQUAL_UINT8(31, dst[3]);
}

TEST_CASE("7. downsample_yuyv_2x larger unit test (8x4 -> 4x2)", "[pipeline]") {
  ESP_LOGI(TAG, "Running Test 7: downsample_yuyv_2x larger unit test (8x4 -> 4x2)");

  // Input YUYV 8x4: 64 bytes
  uint8_t src[64];
  for (int y = 0; y < 4; ++y) {
    uint8_t *row = src + y * 16;
    for (int x = 0; x < 4; ++x) {
      // Each macro-pixel (2 pixels): [Y0, U, Y1, V]
      row[x * 4 + 0] = y * 10 + x * 2;       // Y0
      row[x * 4 + 1] = y * 10 + 50 + x;      // U
      row[x * 4 + 2] = y * 10 + x * 2 + 1;   // Y1
      row[x * 4 + 3] = y * 10 + 80 + x;      // V
    }
  }

  uint8_t dst[16] = {0}; // 4x2 YUYV is 16 bytes
  vigo::detection::detail::downsample_yuyv_2x(src, dst, 8, 4);

  // Let's manually calculate expected outputs:
  // dst_width = 4, dst_height = 2
  // For dst y = 0 (averages src rows 0 and 1):
  // - Output macro-pixel 0 (dst x = 0):
  //   y0 = (src_row0[Y0] + src_row0[Y1] + src_row1[Y0] + src_row1[Y1]) / 4
  //      = (0 + 1 + 10 + 11) / 4 = 22 / 4 = 5
  //   y1 = (src_row0[Y2] + src_row0[Y3] + src_row1[Y2] + src_row1[Y3]) / 4
  //      = (2 + 3 + 12 + 13) / 4 = 30 / 4 = 7
  //   u  = (src_row0[U0] + src_row0[U1] + src_row1[U0] + src_row1[U1]) / 4
  //      = (50 + 51 + 60 + 61) / 4 = 222 / 4 = 55
  //   v  = (src_row0[V0] + src_row0[V1] + src_row1[V0] + src_row1[V1]) / 4
  //      = (80 + 81 + 90 + 91) / 4 = 342 / 4 = 85
  //   So dst[0..3] = [5, 55, 7, 85]
  TEST_ASSERT_EQUAL_UINT8(5, dst[0]);
  TEST_ASSERT_EQUAL_UINT8(55, dst[1]);
  TEST_ASSERT_EQUAL_UINT8(7, dst[2]);
  TEST_ASSERT_EQUAL_UINT8(85, dst[3]);

  // - Output macro-pixel 1 (dst x = 2):
  //   y0 = (src_row0[Y4] + src_row0[Y5] + src_row1[Y4] + src_row1[Y5]) / 4
  //      = (4 + 5 + 14 + 15) / 4 = 38 / 4 = 9
  //   y1 = (src_row0[Y6] + src_row0[Y7] + src_row1[Y6] + src_row1[Y7]) / 4
  //      = (6 + 7 + 16 + 17) / 4 = 46 / 4 = 11
  //   u  = (src_row0[U2] + src_row0[U3] + src_row1[U2] + src_row1[U3]) / 4
  //      = (52 + 53 + 62 + 63) / 4 = 230 / 4 = 57
  //   v  = (src_row0[V2] + src_row0[V3] + src_row1[V2] + src_row1[V3]) / 4
  //      = (82 + 83 + 92 + 93) / 4 = 350 / 4 = 87
  //   So dst[4..7] = [9, 57, 11, 87]
  TEST_ASSERT_EQUAL_UINT8(9, dst[4]);
  TEST_ASSERT_EQUAL_UINT8(57, dst[5]);
  TEST_ASSERT_EQUAL_UINT8(11, dst[6]);
  TEST_ASSERT_EQUAL_UINT8(87, dst[7]);

  // For dst y = 1 (averages src rows 2 and 3):
  // - Output macro-pixel 0 (dst x = 0):
  //   y0 = (src_row2[Y0] + src_row2[Y1] + src_row3[Y0] + src_row3[Y1]) / 4
  //      = (20 + 21 + 30 + 31) / 4 = 102 / 4 = 25
  //   y1 = (src_row2[Y2] + src_row2[Y3] + src_row3[Y2] + src_row3[Y3]) / 4
  //      = (22 + 23 + 32 + 33) / 4 = 110 / 4 = 27
  //   u  = (src_row2[U0] + src_row2[U1] + src_row3[U0] + src_row3[U1]) / 4
  //      = (70 + 71 + 80 + 81) / 4 = 302 / 4 = 75
  //   v  = (src_row2[V0] + src_row2[V1] + src_row3[V0] + src_row3[V1]) / 4
  //      = (100 + 101 + 110 + 111) / 4 = 422 / 4 = 105
  //   So dst[8..11] = [25, 75, 27, 105]
  TEST_ASSERT_EQUAL_UINT8(25, dst[8]);
  TEST_ASSERT_EQUAL_UINT8(75, dst[9]);
  TEST_ASSERT_EQUAL_UINT8(27, dst[10]);
  TEST_ASSERT_EQUAL_UINT8(105, dst[11]);

  // - Output macro-pixel 1 (dst x = 2):
  //   y0 = (src_row2[Y4] + src_row2[Y5] + src_row3[Y4] + src_row3[Y5]) / 4
  //      = (24 + 25 + 34 + 35) / 4 = 118 / 4 = 29
  //   y1 = (src_row2[Y6] + src_row2[Y7] + src_row3[Y6] + src_row3[Y7]) / 4
  //      = (26 + 27 + 36 + 37) / 4 = 126 / 4 = 31
  //   u  = (src_row2[U2] + src_row2[U3] + src_row3[U2] + src_row3[U3]) / 4
  //      = (72 + 73 + 82 + 83) / 4 = 310 / 4 = 77
  //   v  = (src_row2[V2] + src_row2[V3] + src_row3[V2] + src_row3[V3]) / 4
  //      = (102 + 103 + 112 + 113) / 4 = 430 / 4 = 107
  //   So dst[12..15] = [29, 77, 31, 107]
  TEST_ASSERT_EQUAL_UINT8(29, dst[12]);
  TEST_ASSERT_EQUAL_UINT8(77, dst[13]);
  TEST_ASSERT_EQUAL_UINT8(31, dst[14]);
  TEST_ASSERT_EQUAL_UINT8(107, dst[15]);
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
  enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
  enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  enc_cfg.image_quality = 80;
  enc_cfg.pixel_reverse = true;

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
  UNITY_END();
}
