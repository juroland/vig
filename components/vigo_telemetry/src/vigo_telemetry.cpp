#include "vigo_telemetry.hpp"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"

static const char *TAG = "VigTelemetry";

namespace vigo::telemetry {

TelemetryCollector::TelemetryCollector(vigo::camera::CameraManager &camera)
    : camera_(camera) {
  jpeg_encode_engine_cfg_t eng_cfg = {};
  eng_cfg.timeout_ms = 1000;
  eng_cfg.intr_priority = 0;

  if (jpeg_new_encoder_engine(&eng_cfg, &jpeg_engine_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create persistent JPEG encoder engine");
  }
}

// Static helper to convert ESP32 proprietary OUYY_EVYY (Espressif YUV420) to YUV422
// interleaved (YVYU) format, applying 2x2 software binning (downscaling).
static void convert_ouyy_evyy_to_yvyu_binned(const uint8_t *src, uint8_t *dst,
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

      // Swapped U and V indices to correct for hardware pixel_reverse behavior
      dst_row[x * 2 + 0] = y00;
      dst_row[x * 2 + 1] = u; // Was v
      dst_row[x * 2 + 2] = y02;
      dst_row[x * 2 + 3] = v; // Was u
    }
  }
}

TelemetryData TelemetryCollector::collect(const vigo::camera::CameraFrame *frame) {
  TelemetryData data;
  data.free_heap = esp_get_free_heap_size();
  data.uptime = esp_timer_get_time() / 1000000;
  // TODO: Implement CPU temperature if supported by target. For now, 0.0
  data.cpu_temp = 0.0f;

  vigo::camera::CameraFrame local_frame;
  const vigo::camera::CameraFrame *target_frame = frame;

  if (!target_frame) {
    auto frame_res = camera_.capture();
    if (!frame_res.has_value()) {
      ESP_LOGE(TAG, "Failed to capture frame for telemetry snapshot");
      return data;
    }
    local_frame = std::move(frame_res.value());
    target_frame = &local_frame;
  }

  const vigo::camera::CameraFrame &frame_ref = *target_frame;

  ESP_LOGI(TAG, "Frame stats: ptr = %p, size = %zu, width = %zu, height = %zu",
           (void *)frame_ref.data.data(), frame_ref.data.size(), frame_ref.width,
           frame_ref.height);

  // Apply 2x2 software binning and convert raw camera YUV420 frame to YUV422
  int snapshot_width = frame_ref.width / 2;
  int snapshot_height = frame_ref.height / 2;
  size_t yuv422_size = snapshot_width * snapshot_height * 2;
  uint8_t *yuv422_buf =
      static_cast<uint8_t *>(heap_caps_malloc(yuv422_size, MALLOC_CAP_SPIRAM));
  if (!yuv422_buf) {
    ESP_LOGE(TAG, "Failed to allocate YUV422 conversion buffer");
    return data;
  }
  convert_ouyy_evyy_to_yvyu_binned(frame_ref.data.data(), yuv422_buf, frame_ref.width,
                                   frame_ref.height);

  if (!jpeg_engine_) {
    ESP_LOGE(TAG, "JPEG encoder engine not available");
    heap_caps_free(yuv422_buf);
    return data;
  }

  jpeg_encode_cfg_t enc_cfg = {};
  enc_cfg.width = snapshot_width;
  enc_cfg.height = snapshot_height;
  enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
  enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
  enc_cfg.image_quality = 80;
  enc_cfg.pixel_reverse = true;

  // Allocate output buffer for JPEG using the driver's aligned allocator.
  size_t outbuf_size = snapshot_width * snapshot_height;
  jpeg_encode_memory_alloc_cfg_t mem_cfg = {};
  mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t actual_out_size = 0;
  uint8_t *outbuf = static_cast<uint8_t *>(
      jpeg_alloc_encoder_mem(outbuf_size, &mem_cfg, &actual_out_size));

  if (!outbuf) {
    ESP_LOGE(TAG, "Failed to allocate JPEG output buffer");
    heap_caps_free(yuv422_buf);
    return data;
  }

  uint32_t out_size = 0;
  esp_err_t err = jpeg_encoder_process(jpeg_engine_, &enc_cfg, yuv422_buf, yuv422_size,
                                       outbuf, actual_out_size, &out_size);

  if (err == ESP_OK && out_size > 0) {
    // Base64 Encode
    size_t b64_len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64_len, outbuf, out_size);

    std::string b64_str(b64_len, '\0');
    size_t olen = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(b64_str.data()), b64_len,
                          &olen, outbuf, out_size);

    // Remove null terminator if it was included in length
    if (olen > 0 && b64_str[olen - 1] == '\0') {
      b64_str.resize(olen - 1);
    } else {
      b64_str.resize(olen);
    }

    data.snapshot = std::move(b64_str);
    data.raw_jpeg.assign(outbuf, outbuf + out_size);
  } else {
    ESP_LOGE(TAG, "JPEG encoding failed: %d", err);
  }

  heap_caps_free(outbuf);
  heap_caps_free(yuv422_buf);

  return data;
}

} // namespace vigo::telemetry
