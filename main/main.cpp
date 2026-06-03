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
#include "net.hpp"
#include "stream_server.hpp"
#include "surveillance_pipeline.hpp"
#include "vigo_backend.hpp"
#include "vigo_telemetry.hpp"
#include "vigo_whip.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

static const char *TAG = "VigoDevice";

namespace vigo {

struct MotionEventPayload {
  std::string base64_jpeg;
  std::vector<backend::DetectionResult> detections;
};

static std::string extract_host(std::string_view url) {
  size_t scheme_pos = url.find("://");
  size_t start = (scheme_pos == std::string_view::npos) ? 0 : scheme_pos + 3;
  size_t end = url.find_first_of(":/", start);
  if (end == std::string_view::npos) {
    return std::string(url.substr(start));
  }
  return std::string(url.substr(start, end - start));
}

static std::string sanitize_whip_url(std::string whip_url,
                                     const std::string &api_host) {
  for (const char *local_name : {"localhost", "127.0.0.1"}) {
    size_t pos = whip_url.find(local_name);
    if (pos != std::string::npos) {
      whip_url.replace(pos, std::string(local_name).length(), api_host);
      break;
    }
  }
  return whip_url;
}

// Returns the WHIP URL stripped of the ?token=... query parameter.
// Used to detect endpoint changes without being sensitive to token rotation.
static std::string strip_token_from_url(const std::string &url) {
  size_t q = url.find('?');
  if (q == std::string::npos)
    return url;
  // Reconstruct, dropping only the 'token' param
  std::string base = url.substr(0, q);
  return base;
}

class Device {
public:
  Device() {
    if constexpr (config::USE_MOCK_CAMERA) {
      camera_ = std::make_unique<camera::MockCamera>();
    } else {
      camera_ = std::make_unique<camera::HardwareCamera>();
    }
  }

  ~Device() {
    if (telemetry_task_handle_) {
      vTaskDelete(telemetry_task_handle_);
    }
    if (motion_upload_task_handle_) {
      vTaskDelete(motion_upload_task_handle_);
    }
    if (detection_task_handle_) {
      vTaskDelete(detection_task_handle_);
    }
    if (detection_idle_sem_) {
      vSemaphoreDelete(detection_idle_sem_);
    }
    if (detection_start_sem_) {
      vSemaphoreDelete(detection_start_sem_);
    }
    if (motion_queue_) {
      MotionEventPayload *payload = nullptr;
      while (xQueueReceive(motion_queue_, &payload, 0) == pdTRUE) {
        delete payload;
      }
      vQueueDelete(motion_queue_);
    }
    if (whip_publisher_) {
      whip_publisher_->stop();
    }
    if (backend_client_) {
      backend_client_->send_offline();
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

    ESP_LOGI(TAG, "Initializing Network...");
    Expected<void> net_res;
    if constexpr (config::USE_WIFI) {
      ESP_LOGI(TAG, "Using WiFi (SSID: %s)", std::string(config::WIFI_SSID).c_str());
      net_res = net::NetworkManager::instance().init_wifi(
          std::string(config::WIFI_SSID), std::string(config::WIFI_PASSWORD));
    } else {
      ESP_LOGI(TAG, "Using Ethernet");
      net_res = net::NetworkManager::instance().init_ethernet();
    }
    if (!net_res) {
      ESP_LOGE(TAG, "Network init failed");
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

    auto snapshot_cb = [this]() -> std::vector<uint8_t> {
      std::lock_guard<std::mutex> lock(latest_snapshot_mutex_);
      return latest_snapshot_;
    };

    auto stream_res = stream_server_.start(config::STREAM_PORT, snapshot_cb);
    if (!stream_res)
      return stream_res;

    ESP_LOGI(TAG, "Initializing Backend Client...");
    backend_client_ = std::make_unique<backend::BackendClient>(
        std::string(config::API_BASE_URL), std::string(config::HARDWARE_ID),
        std::string(config::DEVICE_TOKEN));

    telemetry_collector_ = std::make_unique<telemetry::TelemetryCollector>(*camera_);

    ESP_LOGI(TAG, "Initializing Motion Queue...");
    motion_queue_ = xQueueCreate(5, sizeof(std::string *));
    if (!motion_queue_) {
      ESP_LOGE(TAG, "Failed to create motion upload queue");
      return std::unexpected(DeviceError::InternalError);
    }

    ESP_LOGI(TAG, "Initializing Detection Semaphores and Buffers...");
    detection_idle_sem_ = xSemaphoreCreateBinary();
    detection_start_sem_ = xSemaphoreCreateBinary();
    if (!detection_idle_sem_ || !detection_start_sem_) {
      ESP_LOGE(TAG, "Failed to create detection semaphores");
      return std::unexpected(DeviceError::InternalError);
    }
    xSemaphoreGive(detection_idle_sem_);

    size_t frame_bytes = config::CAMERA_WIDTH * config::CAMERA_HEIGHT * 3 / 2;
    detection_buffer_.resize(frame_bytes);
    detection_frame_.data.set_external_buffer(detection_buffer_.data(), detection_buffer_.size());

    telemetry_buffer_.resize(frame_bytes);
    latest_frame_.data.set_external_buffer(telemetry_buffer_.data(), telemetry_buffer_.size());

    ESP_LOGI(TAG, "Device initialized successfully.");
    return {};
  }

  void run() {
    // Start camera task
    xTaskCreatePinnedToCore(
        [](void *arg) { static_cast<Device *>(arg)->camera_task(); }, "camera_task",
        16384, this, 5, nullptr, 1);

    // Start telemetry heartbeat task
    xTaskCreatePinnedToCore(
        [](void *arg) { static_cast<Device *>(arg)->telemetry_task(); },
        "telemetry_task", 8192, this, 3, &telemetry_task_handle_, 0);

    // Start background motion upload task on Core 0
    xTaskCreatePinnedToCore(
        [](void *arg) { static_cast<Device *>(arg)->motion_upload_task(); },
        "motion_upload", 8192, this, 2, &motion_upload_task_handle_, 0);

    // Start background detection task on Core 0
    xTaskCreatePinnedToCore(
        [](void *arg) { static_cast<Device *>(arg)->detection_task(); },
        "detection_task", 16384, this, 4, &detection_task_handle_, 0);

    while (true) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

private:
  std::unique_ptr<camera::CameraManager> camera_;
  camera::H264Encoder encoder_;
  net::StreamServer stream_server_;

  // Connectivity
  std::unique_ptr<backend::BackendClient> backend_client_;
  std::unique_ptr<telemetry::TelemetryCollector> telemetry_collector_;

  std::mutex whip_mutex_;
  std::unique_ptr<whip::WhipPublisher> whip_publisher_;
  std::string active_whip_url_;
  std::string active_stream_token_;

  TaskHandle_t telemetry_task_handle_ = nullptr;

  // Cascading Inference Pipeline
  pipeline::SurveillancePipeline surveillance_pipeline_{config::CAMERA_WIDTH,
                                                        config::CAMERA_HEIGHT,
                                                        config::MOTION_STRIDE,
                                                        config::MOTION_THRESHOLD,
                                                        config::MOTION_MIN_CHANGE_RATIO,
                                                        config::MOTION_COOLDOWN_MS,
                                                        0.75f};
  QueueHandle_t motion_queue_ = nullptr;
  TaskHandle_t motion_upload_task_handle_ = nullptr;

  // Asynchronous Detection members
  SemaphoreHandle_t detection_idle_sem_ = nullptr;
  SemaphoreHandle_t detection_start_sem_ = nullptr;
  TaskHandle_t detection_task_handle_ = nullptr;
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> detection_buffer_;
  camera::CameraFrame detection_frame_;
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> telemetry_buffer_;

  // Thread-safe shared frame for telemetry collector
  std::mutex latest_frame_mutex_;
  camera::CameraFrame latest_frame_;
  bool has_latest_frame_{false};
  std::atomic<bool> request_telemetry_snapshot_{false};

  std::mutex latest_snapshot_mutex_;
  std::vector<uint8_t> latest_snapshot_;

  void telemetry_task() {
    ESP_LOGI(TAG, "Telemetry & Heartbeat task started");

    // Wait for Network Connection
    while (!net::NetworkManager::instance().is_connected()) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (true) {
      ESP_LOGI(TAG, "Collecting telemetry data...");

      // Signal camera_task to capture the next frame on-demand
      request_telemetry_snapshot_ = true;

      // Wait a short time for camera_task to perform the copy (up to 200ms)
      int wait_cycles = 0;
      while (request_telemetry_snapshot_ && wait_cycles < 20) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_cycles++;
      }

      std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> local_telemetry_buf;
      camera::CameraFrame frame_copy;
      bool got_frame = false;
      {
        std::lock_guard<std::mutex> lock(latest_frame_mutex_);
        if (has_latest_frame_) {
          frame_copy.width = latest_frame_.width;
          frame_copy.height = latest_frame_.height;
          local_telemetry_buf.resize(latest_frame_.data.size());
          std::memcpy(local_telemetry_buf.data(), latest_frame_.data.data(), latest_frame_.data.size());
          frame_copy.data.set_external_buffer(local_telemetry_buf.data(), local_telemetry_buf.size());
          has_latest_frame_ = false; // Reset flag for next demand cycle
          got_frame = true;
        }
      }

      auto telemetry_data =
          telemetry_collector_->collect(got_frame ? &frame_copy : nullptr);

      {
        std::lock_guard<std::mutex> snap_lock(latest_snapshot_mutex_);
        latest_snapshot_ = telemetry_data.raw_jpeg;
      }

      ESP_LOGI(TAG, "Posting heartbeat to backend...");
      auto hb_res = backend_client_->send_heartbeat(telemetry_data);
      if (!hb_res) {
        ESP_LOGE(TAG, "Heartbeat request failed: %.*s",
                 (int)to_string(hb_res.error()).size(),
                 to_string(hb_res.error()).data());
      } else {
        auto hb = hb_res.value();
        ESP_LOGI(TAG, "Heartbeat ACK received. Stream Token size: %zu",
                 hb.stream_token.length());

        std::string target_whip_url;
        std::string api_host = extract_host(config::API_BASE_URL);
        target_whip_url = sanitize_whip_url(hb.whip_url, api_host);

        // Append token as a query parameter for robust compatibility with MediaMTX auth
        // hooks
        if (!hb.stream_token.empty()) {
          if (target_whip_url.find('?') == std::string::npos) {
            target_whip_url += "?token=" + hb.stream_token;
          } else {
            target_whip_url += "&token=" + hb.stream_token;
          }
        }

        std::lock_guard<std::mutex> lock(whip_mutex_);
        if (!target_whip_url.empty() && !hb.stream_token.empty()) {
          std::string new_endpoint = strip_token_from_url(target_whip_url);
          std::string active_endpoint = strip_token_from_url(active_whip_url_);

          if (!whip_publisher_ || whip_publisher_->has_error() ||
              new_endpoint != active_endpoint) {
            // Endpoint changed, errored out, or not started yet: full restart
            ESP_LOGI(TAG, "Starting/restarting WHIP stream to %s (original: %s)",
                     target_whip_url.c_str(), hb.whip_url.c_str());
            if (whip_publisher_) {
              whip_publisher_->stop();
              whip_publisher_.reset();
            }

            whip_publisher_ = std::make_unique<whip::WhipPublisher>(
                target_whip_url, hb.stream_token, std::string(config::DTLS_CERT_PEM),
                std::string(config::DTLS_KEY_PEM));
            auto start_res = whip_publisher_->start();
            if (!start_res) {
              ESP_LOGE(TAG, "Failed to start WHIP publisher: %.*s",
                       (int)to_string(start_res.error()).size(),
                       to_string(start_res.error()).data());
              whip_publisher_.reset();
            } else {
              active_whip_url_ = target_whip_url;
              active_stream_token_ = hb.stream_token;
            }
          } else {
            // Same endpoint, just rotate the token reference
            ESP_LOGD(TAG, "Token rotated — keeping active WHIP connection");
            active_stream_token_ = hb.stream_token;
          }
        } else {
          if (whip_publisher_) {
            ESP_LOGI(TAG, "Stopping active WHIP publisher as requested by backend.");
            whip_publisher_->stop();
            whip_publisher_.reset();
            active_whip_url_.clear();
            active_stream_token_.clear();
          }
        }
      }

      vTaskDelay(pdMS_TO_TICKS(config::HEARTBEAT_INTERVAL_MS));
    }
  }

  void detection_task() {
    ESP_LOGI(TAG, "Detection task started on core %d", xPortGetCoreID());
    esp_task_wdt_add(nullptr);

    while (true) {
      esp_task_wdt_reset();
      if (xSemaphoreTake(detection_start_sem_, portMAX_DELAY) == pdTRUE) {
        esp_task_wdt_reset();

        auto pipeline_res = surveillance_pipeline_.process(detection_frame_);
        if (pipeline_res.pedestrian_confirmed) {
          auto telemetry_data = telemetry_collector_->collect(&detection_frame_);
          if (!telemetry_data.snapshot.empty()) {
            auto *payload = new MotionEventPayload{std::move(telemetry_data.snapshot),
                                                   std::move(pipeline_res.detections)};
            if (xQueueSend(motion_queue_, &payload, 0) != pdTRUE) {
              ESP_LOGW(TAG, "Motion upload queue full, dropping event");
              delete payload;
            }
          }
        } else if (pipeline_res.motion_detected) {
          ESP_LOGI(
              TAG,
              "Motion detected but no pedestrian confirmed; skipping backend upload");
        }

        xSemaphoreGive(detection_idle_sem_);
      }
    }
  }

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

      // Non-blocking handoff to the detection task if it is idle
      if (xSemaphoreTake(detection_idle_sem_, 0) == pdTRUE) {
        detection_frame_.width = frame_res->width;
        detection_frame_.height = frame_res->height;
        std::memcpy(detection_buffer_.data(), frame_res->data.data(), frame_res->data.size());
        xSemaphoreGive(detection_start_sem_);
      }

      uint64_t current_pts = esp_timer_get_time() / 1000;
      auto encoded_res =
          encoder_.encode(frame_res->data.data(), frame_res->data.size(), current_pts);

      if (!encoded_res) {
        ESP_LOGE(TAG, "Encode failed: %.*s", (int)to_string(encoded_res.error()).size(),
                 to_string(encoded_res.error()).data());
      } else {
        // Save the latest raw frame for telemetry snapshots on-demand
        if (request_telemetry_snapshot_) {
          std::lock_guard<std::mutex> lock(latest_frame_mutex_);
          latest_frame_.width = frame_res->width;
          latest_frame_.height = frame_res->height;
          std::memcpy(telemetry_buffer_.data(), frame_res->data.data(), frame_res->data.size());
          has_latest_frame_ = true;
          request_telemetry_snapshot_ = false; // Done copying, reset request flag
        }

        // Create shared_ptr once (move the encoded data - zero copy after this)
        auto shared_frame =
            std::make_shared<camera::EncodedFrame>(std::move(encoded_res.value()));

        // Stream to the WHIP endpoint first (UDP, fast fire-and-forget)
        if (whip_mutex_.try_lock()) {
          if (whip_publisher_) {
            whip_publisher_->push_frame(*shared_frame);
          }
          whip_mutex_.unlock();
        }

        stream_server_.push_frame(shared_frame);

        if (++frame_idx % 100 == 0) {
          ESP_LOGI(TAG, "Processed 100 frames (Current PTS: %llu)", current_pts);
        }
      }
    }
  }

  void motion_upload_task() {
    ESP_LOGI(TAG, "Motion upload task started on core %d", xPortGetCoreID());
    MotionEventPayload *payload = nullptr;
    while (true) {
      if (xQueueReceive(motion_queue_, &payload, portMAX_DELAY) == pdTRUE &&
          payload != nullptr) {
        ESP_LOGI(TAG, "Uploading motion event capture with %zu detections...",
                 payload->detections.size());
        auto upload_res = backend_client_->send_motion_event(payload->base64_jpeg,
                                                             payload->detections);
        if (!upload_res) {
          ESP_LOGE(TAG, "Failed to upload motion event: %.*s",
                   (int)to_string(upload_res.error()).size(),
                   to_string(upload_res.error()).data());
        } else {
          ESP_LOGI(TAG, "Motion event successfully uploaded to backend!");
        }
        delete payload;
        payload = nullptr;
      }
    }
  }
};

} // namespace vigo

extern "C" void app_main() {

  xTaskCreatePinnedToCore(
      [](void *arg) {
        ESP_LOGI("Main", "Starting Main App Task...");
        static vigo::Device device;

        auto start_res = device.start();
        if (!start_res) {
          ESP_LOGE("Main", "Failed to start device: %.*s",
                   (int)vigo::to_string(start_res.error()).size(),
                   vigo::to_string(start_res.error()).data());
          vTaskDelete(nullptr);
          return;
        }

        device.run();
      },
      "main_app_task", 24576, nullptr, 5, nullptr, 0);
}
