#pragma once

#include "error_types.hpp"
#include "vig_telemetry.hpp"
#include <memory>
#include <string>

namespace vig::backend {

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
  send_heartbeat(const vig::telemetry::TelemetryData &telemetry);

  Expected<void> send_offline();

private:
  std::string api_base_url_;
  std::string hardware_id_;
  std::string setup_token_;
};

} // namespace vig::backend
