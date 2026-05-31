#include "camera.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.hpp"
#include "unity.h"
#include "vig_backend.hpp"
#include "vig_telemetry.hpp"
#include "vig_whip.hpp"
#include <string>

static const char *TAG = "TestBackend";

// MOCK Camera for Telemetry
class DummyCamera : public vig::camera::CameraManager {
public:
  vig::Expected<void> init() override { return {}; }
  vig::Expected<vig::camera::CameraFrame> capture() override {
    vig::camera::CameraFrame frame;
    frame.width = 64;
    frame.height = 64;
    frame.data.resize(64 * 64 * 1.5);
    return frame;
  }
  uint32_t getWidth() const override { return 64; }
  uint32_t getHeight() const override { return 64; }
};

static std::string g_host_ip = "192.168.1.105";

TEST_CASE("Backend integration tests", "[backend]") {
  ESP_LOGI(TAG, "Starting backend integration test with host %s",
           g_host_ip.c_str());

  std::string api_url = "http://" + g_host_ip + ":8081";

  vig::backend::BackendClient client(api_url, "SIM_CAM_001", "test_token_123");

  DummyCamera cam;
  vig::telemetry::TelemetryCollector collector(cam);
  auto telemetry = collector.collect();

  ESP_LOGI(TAG, "Sending heartbeat...");
  auto hb_res = client.send_heartbeat(telemetry);
  TEST_ASSERT_TRUE_MESSAGE(hb_res.has_value(), "Heartbeat request failed");

  auto hb = hb_res.value();
  TEST_ASSERT_TRUE(hb.ack);
  TEST_ASSERT_NOT_EMPTY(hb.stream_token.c_str());
  TEST_ASSERT_NOT_EMPTY(hb.whip_url.c_str());

  ESP_LOGI(TAG, "Testing WHIP streaming...");
  vig::whip::WhipPublisher whip(hb.whip_url, hb.stream_token);
  auto whip_res = whip.start();
  TEST_ASSERT_TRUE_MESSAGE(whip_res.has_value(), "WHIP Start failed");

  ESP_LOGI(TAG, "Sending offline...");
  auto off_res = client.send_offline();
  TEST_ASSERT_TRUE_MESSAGE(off_res.has_value(), "Offline request failed");

  ESP_LOGI(TAG, "Integration test complete.");
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Initializing Ethernet...");
  auto err = vig::net::NetworkManager::instance().init_ethernet();
  if (!err.has_value()) {
    ESP_LOGE(TAG, "Ethernet Init Failed");
  }

  // Wait for DHCP
  ESP_LOGI(TAG, "Waiting for IP...");
  for (int i = 0; i < 10; ++i) {
    if (vig::net::NetworkManager::instance().is_connected()) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  UNITY_BEGIN();
  unity_run_test_by_name("Backend integration tests");
  UNITY_END();
}
