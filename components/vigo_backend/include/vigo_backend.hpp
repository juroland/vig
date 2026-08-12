#ifndef VIGO_BACKEND_HPP
#define VIGO_BACKEND_HPP

#include "vigo_telemetry.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vigo::backend {

struct DetectionResult {
  std::vector<float> box; // [x_min, y_min, x_max, y_max] normalized coordinates
  float score;
  std::string label;
};

struct HeartbeatResponse {
  bool ack{false};
  bool update_available{false};
  std::string update_version{};
  std::string stream_token{};
  std::string whip_url{};
};

class BackendClient {
public:
  BackendClient(const std::string &api_base_url, const std::string &hardware_id,
                const std::string &setup_token);

  // Delete copy to ensure single ownership / standard semantics
  BackendClient(const BackendClient &) = delete;
  BackendClient &operator=(const BackendClient &) = delete;

  Expected<HeartbeatResponse>
  send_heartbeat(const vigo::telemetry::TelemetryData &telemetry,
                 std::string_view firmware_version,
                 std::string_view ota_status = "IDLE", std::string_view ota_error = "");

  Expected<void> send_offline();

  Expected<void> send_motion_event(const std::string &base64_jpeg,
                                   const std::vector<DetectionResult> &detections = {});

private:
  std::string api_base_url_;
  std::string hardware_id_;
  std::string setup_token_;
};

} // namespace vigo::backend

#endif // VIGO_BACKEND_HPP