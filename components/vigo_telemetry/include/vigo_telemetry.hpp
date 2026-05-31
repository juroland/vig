#pragma once

#include "camera.hpp"
#include <cstdint>
#include <driver/jpeg_encode.h>
#include <string>

namespace vigo::telemetry {

struct TelemetryData {
  uint32_t free_heap;
  uint32_t uptime; // in seconds
  float cpu_temp;
  std::string snapshot;          // base64 encoded jpeg
  std::vector<uint8_t> raw_jpeg; // raw jpeg bytes
};

class TelemetryCollector {
public:
  TelemetryCollector(vigo::camera::CameraManager &camera);

  // Delete copy semantics
  TelemetryCollector(const TelemetryCollector &) = delete;
  TelemetryCollector &operator=(const TelemetryCollector &) = delete;

  TelemetryData collect(const vigo::camera::CameraFrame *frame = nullptr);

private:
  vigo::camera::CameraManager &camera_;
  jpeg_encoder_handle_t jpeg_engine_{nullptr};
};

} // namespace vigo::telemetry
