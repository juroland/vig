#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// Internal libs
#include "camera.hpp"
#include "device_config.hpp"
#include "error_types.hpp"
#include "h264_encoder.hpp"
#include "net.hpp"
#include "stream_server.hpp"

#include <memory>
#include <string>

static const char *TAG = "VigDevice";

namespace vig {

class Device {
public:
  Device() {
    if constexpr (config::USE_MOCK_CAMERA) {
      camera_ = std::make_unique<camera::MockCamera>();
    } else {
      camera_ = std::make_unique<camera::HardwareCamera>();
    }
  }

  Expected<void> start() {
    ESP_LOGI(TAG, "Starting device initialization...");
    ESP_LOGI(TAG, "Initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing Ethernet...");
    auto net_res = net::NetworkManager::instance().init_ethernet();
    if (!net_res) {
      ESP_LOGE(TAG, "Ethernet init failed");
      return net_res;
    }

    ESP_LOGI(TAG, "Initializing Camera...");
    auto cam_res = camera_->init();
    if (!cam_res) {
      ESP_LOGE(TAG, "Camera init failed");
      return cam_res;
    }

    auto enc_res = encoder_.init(camera_->getWidth(), camera_->getHeight(),
                                 config::CAMERA_TARGET_FPS,
                                 config::ENCODER_BITRATE_KBPS, config::ENCODER_GOP);
    if (!enc_res)
      return enc_res;

    auto stream_res = stream_server_.start(config::STREAM_PORT);
    if (!stream_res)
      return stream_res;

    ESP_LOGI(TAG, "Device initialized successfully.");
    return {};
  }

  void run() {
    // Start camera task
    xTaskCreatePinnedToCore(
        [](void *arg) { static_cast<Device *>(arg)->camera_task(); }, "camera_task",
        16384, this, 5, nullptr, 1);

    while (true) {
      vTaskDelay(pdMS_TO_TICKS(config::HEARTBEAT_INTERVAL_MS));
    }
  }

private:
  std::unique_ptr<camera::CameraManager> camera_;
  camera::H264Encoder encoder_;
  net::StreamServer stream_server_;

  void camera_task() {
    ESP_LOGI(TAG, "Camera task started on core %d", xPortGetCoreID());
    esp_task_wdt_add(nullptr);

    uint32_t frame_idx = 0;
    while (true) {
      esp_task_wdt_reset();
      auto frame_res = camera_->capture();
      if (!frame_res) {
        ESP_LOGE(TAG, "Capture failed: %.*s", (int)to_string(frame_res.error()).size(),
                 to_string(frame_res.error()).data());
        vTaskDelay(pdMS_TO_TICKS(100)); // Cool down on error
        continue;
      }

      uint64_t current_pts = esp_timer_get_time() / 1000;
      auto encoded_res =
          encoder_.encode(frame_res->data.data(), frame_res->data.size(), current_pts);

      if (!encoded_res) {
        ESP_LOGE(TAG, "Encode failed: %.*s", (int)to_string(encoded_res.error()).size(),
                 to_string(encoded_res.error()).data());
      } else {
        stream_server_.push_frame(encoded_res.value());
        if (++frame_idx % 100 == 0) {
          ESP_LOGI(TAG, "Processed 100 frames (Current PTS: %llu)", current_pts);
        }
      }
    }
  }
};

} // namespace vig

extern "C" void app_main() {

  xTaskCreatePinnedToCore(
      [](void *arg) {
        ESP_LOGI("Main", "Starting Main App Task...");
        static vig::Device device;

        auto start_res = device.start();
        if (!start_res) {
          ESP_LOGE("Main", "Failed to start device: %.*s",
                   (int)vig::to_string(start_res.error()).size(),
                   vig::to_string(start_res.error()).data());
          vTaskDelete(nullptr);
          return;
        }

        device.run();
      },
      "main_app_task", 24576, nullptr, 5, nullptr, 0);
}
