#pragma once

#include "camera.hpp"
#include <cstdint>
#include <string>

namespace vig::telemetry {

struct TelemetryData {
  uint32_t free_heap;
  uint32_t uptime; // in seconds
  float cpu_temp;
  std::string snapshot;          // base64 encoded jpeg
  std::vector<uint8_t> raw_jpeg; // raw jpeg bytes
};

class TelemetryCollector {
public:
  TelemetryCollector(vig::camera::CameraManager &camera);

  // Delete copy semantics
  TelemetryCollector(const TelemetryCollector &) = delete;
  TelemetryCollector &operator=(const TelemetryCollector &) = delete;

  TelemetryData collect(const vig::camera::CameraFrame *frame = nullptr);

private:
  vig::camera::CameraManager &camera_;
};

} // namespace vig::telemetry
