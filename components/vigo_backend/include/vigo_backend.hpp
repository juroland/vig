#pragma once

#include "error_types.hpp"
#include "vigo_telemetry.hpp"
#include <memory>
#include <string>

namespace vigo::backend {

struct HeartbeatResponse {
  bool ack{false};
  bool update_available{false};
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
  send_heartbeat(const vigo::telemetry::TelemetryData &telemetry);

  Expected<void> send_offline();

  Expected<void> send_motion_event(const std::string &base64_jpeg);

private:
  std::string api_base_url_;
  std::string hardware_id_;
  std::string setup_token_;
};

} // namespace vigo::backend
