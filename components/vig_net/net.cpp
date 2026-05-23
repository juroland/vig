#include "net.hpp"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <cstring>

static const char *TAG = "VigNet";

namespace vig::net {

HttpClient::HttpClient(const std::string &url, const std::string &device_token) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  client_handle_ = esp_http_client_init(&config);

  auth_header_ = "Bearer " + device_token;
}

HttpClient::~HttpClient() {
  if (client_handle_) {
    esp_http_client_cleanup(client_handle_);
  }
}

Expected<int> HttpClient::post_json(const std::string &payload) {
  if (!client_handle_)
    return std::unexpected(DeviceError::InternalError);

  esp_http_client_set_header(client_handle_, "Content-Type", "application/json");
  esp_http_client_set_header(client_handle_, "Authorization", auth_header_.c_str());
  esp_http_client_set_post_field(client_handle_, payload.c_str(), payload.length());

  esp_err_t err = esp_http_client_perform(client_handle_);
  if (err != ESP_OK) {
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  return esp_http_client_get_status_code(client_handle_);
}

Expected<std::string> HttpClient::post_json_with_response(const std::string &payload) {
  if (!client_handle_)
    return std::unexpected(DeviceError::InternalError);

  esp_http_client_set_header(client_handle_, "Content-Type", "application/json");
  esp_http_client_set_header(client_handle_, "Authorization", auth_header_.c_str());
  esp_http_client_set_post_field(client_handle_, payload.c_str(), payload.length());

  esp_err_t err = esp_http_client_perform(client_handle_);
  if (err != ESP_OK) {
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  int status = esp_http_client_get_status_code(client_handle_);
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "HTTP error status %d", status);
    return std::unexpected(DeviceError::HttpRequestFailed);
  }

  int content_length = esp_http_client_get_content_length(client_handle_);
  if (content_length <= 0 || content_length > 4096) {
    return std::string{};
  }

  std::string response(static_cast<size_t>(content_length), '\0');
  int read =
      esp_http_client_read_response(client_handle_, response.data(), content_length);
  if (read < 0) {
    return std::unexpected(DeviceError::HttpPayloadError);
  }
  response.resize(static_cast<size_t>(read));

  return response;
}

NetworkManager &NetworkManager::instance() {
  static NetworkManager inst;
  return inst;
}

Expected<void> NetworkManager::init_ethernet() {
  ESP_LOGI(TAG, "esp_netif_init...");
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_LOGI(TAG, "esp_event_loop_create_default...");
  esp_err_t loop_ret = esp_event_loop_create_default();
  if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
    ESP_ERROR_CHECK(loop_ret);
  }

  ESP_LOGI(TAG, "esp_netif_new...");
  esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
  if (eth_netif == nullptr) {
    return std::unexpected(DeviceError::NetworkInitFailed);
  }

  ESP_LOGI(TAG, "esp_eth_mac_new_esp32...");
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_esp32_emac_config_t esp32_emac_config = {};
  esp32_emac_config.smi_gpio.mdc_num = 31;
  esp32_emac_config.smi_gpio.mdio_num = 52;
  esp32_emac_config.interface = EMAC_DATA_INTERFACE_RMII;
  esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
  esp32_emac_config.clock_config.rmii.clock_gpio = 50;
  esp32_emac_config.dma_burst_len = ETH_DMA_BURST_LEN_32;
  esp32_emac_config.intr_priority = 0;
  esp32_emac_config.emac_dataif_gpio.rmii.tx_en_num = 49;
  esp32_emac_config.emac_dataif_gpio.rmii.txd0_num = 34;
  esp32_emac_config.emac_dataif_gpio.rmii.txd1_num = 35;
  esp32_emac_config.emac_dataif_gpio.rmii.crs_dv_num = 28;
  esp32_emac_config.emac_dataif_gpio.rmii.rxd0_num = 29;
  esp32_emac_config.emac_dataif_gpio.rmii.rxd1_num = 30;
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

  ESP_LOGI(TAG, "esp_eth_phy_new_generic...");
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.phy_addr = 1;
  phy_config.reset_gpio_num = 5;
  esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);

  ESP_LOGI(TAG, "esp_eth_driver_install...");
  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  if (esp_eth_driver_install(&config, &eth_handle_) != ESP_OK) {
    return std::unexpected(DeviceError::NetworkInitFailed);
  }

  ESP_LOGI(TAG, "esp_netif_attach...");
  if (esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle_)) != ESP_OK) {
    return std::unexpected(DeviceError::NetworkInitFailed);
  }

  ESP_LOGI(TAG, "esp_event_handler_instance_register...");
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, this, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_event_handler, this, nullptr));

  ESP_LOGI(TAG, "esp_eth_start...");
  if (esp_eth_start(eth_handle_) != ESP_OK) {
    return std::unexpected(DeviceError::NetworkInitFailed);
  }

  ESP_LOGI(TAG, "Ethernet initialized.");
  return {};
}

void NetworkManager::eth_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
  auto *self = static_cast<NetworkManager *>(arg);

  if (event_base == ETH_EVENT) {
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "Ethernet Link Up");
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "Ethernet Link Down");
      self->connected_ = false;
      break;
    default:
      break;
    }
  } else if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_ETH_GOT_IP) {
      ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
      ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
      self->connected_ = true;
    }
  } else {
    ESP_LOGE(TAG, "Unhandled event: ", event_base);
  }
}

} // namespace vig::net
