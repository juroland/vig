#ifndef VIGO_OTA_HPP
#define VIGO_OTA_HPP

#include "error_types.hpp"
#include <cstddef>
#include <string>
#include <string_view>

namespace vigo::ota {

/// OTA update status reported back to the backend via the heartbeat.
enum class OtaStatus {
  Idle,
  Downloading,
  Installing,
  Success,
  Failed,
};

/// Returns the heartbeat-compatible string for a given OTA status.
constexpr std::string_view to_status_string(OtaStatus status) {
  switch (status) {
  case OtaStatus::Idle:
    return "IDLE";
  case OtaStatus::Downloading:
    return "DOWNLOADING";
  case OtaStatus::Installing:
    return "INSTALLING";
  case OtaStatus::Success:
    return "SUCCESS";
  case OtaStatus::Failed:
    return "FAILED";
  default:
    return "IDLE";
  }
}

/// Metadata returned from the OTA check endpoint.
struct UpdateInfo {
  bool available{false};
  std::string version{};
  std::string url{};
  size_t size_bytes{0};
  std::string checksum{};
};

/// Performs the end-to-end OTA firmware update flow:
///   1. Query backend for available updates (POST /api/devices/ota/check)
///   2. Stream the binary from the backend (GET /api/devices/ota/download/{version})
///   3. Flash to the passive OTA partition
///   4. Set the new partition as the boot target
///
/// The caller is responsible for rebooting the device after a successful update.
class FirmwareUpdater {
public:
  explicit FirmwareUpdater(std::string_view api_url, std::string_view token,
                           std::string_view hardware_id,
                           std::string_view current_version);

  // Delete copy to ensure single ownership
  FirmwareUpdater(const FirmwareUpdater &) = delete;
  FirmwareUpdater &operator=(const FirmwareUpdater &) = delete;

  /// Check the backend for a pending firmware update.
  Expected<UpdateInfo> check_for_update();

  /// Download and flash the firmware described by `info`.
  /// On success, the new partition is set as the boot target — caller must reboot.
  Expected<void> apply_update(const UpdateInfo &info);

  /// Returns the current OTA status (for heartbeat reporting).
  OtaStatus status() const { return status_; }

  /// Returns the last error message (empty if none).
  const std::string &last_error() const { return last_error_; }

private:
  std::string api_url_;
  std::string token_;
  std::string hardware_id_;
  std::string current_version_;

  OtaStatus status_{OtaStatus::Idle};
  std::string last_error_{};
};

} // namespace vigo::ota

#endif // VIGO_OTA_HPP
