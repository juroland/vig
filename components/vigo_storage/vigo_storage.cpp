#include <cstdlib>

#include "vigo_storage.hpp"

#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

namespace vigo::storage {

std::expected<SdCard, DeviceError> SdCard::init() {
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

  return SdCard(card, pwr_ctrl_handle, host, slot_config);
}

std::expected<void, DeviceError> SdCard::mount(const char *mount_point) {
  mount_point_ = mount_point;
  esp_vfs_fat_mount_config_t mount_config = {
      .format_if_mount_failed = true,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024,
      .disk_status_check_enable = true,
      .use_one_fat = false,
  };

  auto ret = esp_vfs_fat_sdmmc_mount(mount_point, &host_, &slot_config_, &mount_config,
                                     &card_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to init SD card (%s)", esp_err_to_name(ret));
    return std::unexpected(DeviceError::SDCardMountFailed);
  }

  ESP_LOGI(TAG, "SD card mounted successfully at %s", mount_point);
  sdmmc_card_print_info(stdout, card_);

  is_mounted_ = true;
  return {};
}

void SdCard::unmount() {
  if (is_mounted_ && mount_point_) {
    esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_point_, card_);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "SD card unmounted safely from %s", mount_point_);
    } else {
      ESP_LOGE(TAG, "Error unmounting SD card: %s", esp_err_to_name(err));
    }

    is_mounted_ = false;
  }
}

SdCard::~SdCard() {
  unmount();
  if (pwr_ctrl_handle_) {
    sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle_);
    pwr_ctrl_handle_ = nullptr;
  }
  card_ = nullptr;
  mount_point_ = nullptr;
}

void SdCard::enable_tinyUSB() {
  ESP_LOGI(TAG, "Button pressed: Entering USB Mass Storage mode...");

  // The card is only allocated during mount(); in storage mode the filesystem
  // is never mounted, so initialize the card for raw block access here.
  if (!card_) {
    card_ = static_cast<sdmmc_card_t *>(malloc(sizeof(sdmmc_card_t)));
    if (!card_) {
      ESP_LOGE(TAG, "Failed to allocate memory for sdmmc_card_t");
      abort();
    }

    if (sdmmc_host_init() != ESP_OK ||
        sdmmc_host_init_slot(host_.slot, &slot_config_) != ESP_OK ||
        sdmmc_card_init(&host_, card_) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize SD card for USB Mass Storage mode");
      abort();
    }
  }

  // MSC storage backend for SD card
  tinyusb_msc_storage_config_t config_sdmmc{};
  config_sdmmc.medium.card = card_;
  tinyusb_msc_storage_handle_t storage_hdl;
  ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&config_sdmmc, &storage_hdl));

  // Install TinyUSB driver stack.
  // TINYUSB_DEFAULT_CONFIG() selects the OTG High-Speed port and performs the
  // PHY setup, which is required on ESP32-P4: usb_new_phy() enables the OTG2.0
  // peripheral bus clock. Skipping it leaves the controller clock-gated and
  // makes the TinyUSB task crash with a load access fault.
  tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
  ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

  ESP_LOGI(TAG, "USB Mass Storage is active.");
}

} // namespace vigo::storage
