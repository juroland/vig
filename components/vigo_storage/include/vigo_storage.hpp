#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "error_types.hpp"

#include <expected>

namespace vigo::storage {

const auto PIN_CLK = GPIO_NUM_43;
const auto PIN_CMD = GPIO_NUM_44;
const auto PIN_D0 = GPIO_NUM_39;
const auto PIN_D1 = GPIO_NUM_40;
const auto PIN_D2 = GPIO_NUM_41;
const auto PIN_D3 = GPIO_NUM_42;
const auto SLOT_WIDTH = 4;
const auto SD_PWR_CTRL_LDO_IO_ID = 4;

class SdCard {
private:
  const char *mount_point_{nullptr};
  sdmmc_card_t *card_{nullptr};
  bool is_mounted_{false};
  sd_pwr_ctrl_handle_t pwr_ctrl_handle_{nullptr};
  sdmmc_host_t host_;
  sdmmc_slot_config_t slot_config_;

  SdCard(sdmmc_card_t *card, sd_pwr_ctrl_handle_t pwr_ctrl_handle, sdmmc_host_t host,
         sdmmc_slot_config_t slot_config)
      : card_(card), is_mounted_(false), pwr_ctrl_handle_(pwr_ctrl_handle), host_(host),
        slot_config_(slot_config) {}

  static constexpr const char *TAG = "SdCard";

public:
  // Delete copy semantics to prevent double unmounting/resource sharing bugs
  SdCard(const SdCard &) = delete;
  SdCard &operator=(const SdCard &) = delete;

  // Enable move semantics for transferring ownership across scopes
  SdCard(SdCard &&other) noexcept
      : mount_point_(other.mount_point_), card_(other.card_),
        is_mounted_(other.is_mounted_), pwr_ctrl_handle_(other.pwr_ctrl_handle_),
        host_(other.host_), slot_config_(other.slot_config_) {
    other.is_mounted_ = false;
    other.card_ = nullptr;
    other.mount_point_ = nullptr;
    other.pwr_ctrl_handle_ = nullptr;
  }

  SdCard &operator=(SdCard &&other) noexcept {
    if (this != &other) {
      unmount();

      mount_point_ = other.mount_point_;
      card_ = other.card_;
      is_mounted_ = other.is_mounted_;
      pwr_ctrl_handle_ = other.pwr_ctrl_handle_;
      host_ = other.host_;
      slot_config_ = other.slot_config_;

      other.is_mounted_ = false;
      other.card_ = nullptr;
      other.mount_point_ = nullptr;
      other.pwr_ctrl_handle_ = nullptr;
    }
    return *this;
  }

  ~SdCard();

  [[nodiscard]] static std::expected<SdCard, DeviceError> init();
  std::expected<void, DeviceError> mount(const char *path = "/sdcard");

  void enable_tinyUSB();

  void unmount();

  [[nodiscard]] bool is_mounted() const { return is_mounted_; }
  [[nodiscard]] const sdmmc_card_t *get_card_info() const { return card_; }
};
}; // namespace vigo::storage