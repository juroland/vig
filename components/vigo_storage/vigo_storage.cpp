#include "vigo_storage.hpp"

#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace vigo::storage {

std::expected<SdCard, DeviceError> SdCard::mount(const char *mount_point) {
  esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = true,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = true,
      .use_one_fat = false,
  };

  sdmmc_card_t *card = nullptr;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  sd_pwr_ctrl_ldo_config_t ldo_config = {.ldo_chan_id = SD_PWR_CTRL_LDO_IO_ID};
  sd_pwr_ctrl_handle_t pwr_ctrl_handle = nullptr;

  auto ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
    sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
    return std::unexpected(DeviceError::SDCardPowerControlFailed);
  }
  host.pwr_ctrl_handle = pwr_ctrl_handle;
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = SLOT_WIDTH;
  slot_config.clk = PIN_CLK;
  slot_config.cmd = PIN_CMD;
  slot_config.d0 = PIN_D0;
  slot_config.d1 = PIN_D1;
  slot_config.d2 = PIN_D2;
  slot_config.d3 = PIN_D3;
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SD card (%s)", esp_err_to_name(ret));
    return std::unexpected(DeviceError::SDCardMountFailed);
  }

  ESP_LOGI(TAG, "SD card mounted successfully at %s", mount_point);
  sdmmc_card_print_info(stdout, card);

  return SdCard(mount_point, card, pwr_ctrl_handle);
}

void SdCard::unmount() {
  if (is_mounted_ && mount_point_) {
    esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_point_, card_);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "SD card unmounted safely from %s", mount_point_);
    } else {
      ESP_LOGE(TAG, "Error unmounting SD card: %s", esp_err_to_name(err));
    }

    if (pwr_ctrl_handle_) {
      sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle_);
      pwr_ctrl_handle_ = nullptr;
    }

    is_mounted_ = false;
    card_ = nullptr;
    mount_point_ = nullptr;
  }
}
} // namespace vigo::storage
