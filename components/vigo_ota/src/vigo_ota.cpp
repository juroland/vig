#include "vigo_ota.hpp"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "net.hpp"
#include <vector>

static const char *TAG = "VigOTA";

namespace vigo::ota {

FirmwareUpdater::FirmwareUpdater(std::string_view api_url, std::string_view token,
                                 std::string_view hardware_id,
                                 std::string_view current_version)
    : api_url_(api_url), token_(token), hardware_id_(hardware_id),
      current_version_(current_version) {}

Expected<UpdateInfo> FirmwareUpdater::check_for_update() {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);

  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());
  cJSON_AddStringToObject(root, "current_version", current_version_.c_str());

  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str)
    return std::unexpected(DeviceError::InternalError);
  std::string payload(json_str);
  cJSON_free(json_str);

  vigo::net::HttpClient client(api_url_ + "/api/devices/ota/check", token_);
  auto resp_res = client.post_json_with_response(payload);
  if (!resp_res.has_value()) {
    ESP_LOGE(TAG, "OTA check request failed");
    return std::unexpected(resp_res.error());
  }

  cJSON *resp_json = cJSON_Parse(resp_res.value().c_str());
  if (!resp_json) {
    ESP_LOGE(TAG, "Failed to parse OTA check response: '%s'", resp_res.value().c_str());
    return std::unexpected(DeviceError::HttpPayloadError);
  }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> resp_ptr(resp_json, cJSON_Delete);

  UpdateInfo info;

  cJSON *available = cJSON_GetObjectItem(resp_json, "available");
  if (available && cJSON_IsBool(available)) {
    info.available = cJSON_IsTrue(available);
  }

  if (!info.available) {
    ESP_LOGI(TAG, "Firmware is up to date (%s)", current_version_.c_str());
    return info;
  }

  cJSON *version = cJSON_GetObjectItem(resp_json, "version");
  if (version && cJSON_IsString(version)) {
    info.version = version->valuestring;
  }

  cJSON *url = cJSON_GetObjectItem(resp_json, "url");
  if (url && cJSON_IsString(url)) {
    info.url = url->valuestring;
  }

  cJSON *size_bytes = cJSON_GetObjectItem(resp_json, "size_bytes");
  if (size_bytes && cJSON_IsNumber(size_bytes)) {
    info.size_bytes = static_cast<size_t>(size_bytes->valuedouble);
  }

  cJSON *checksum = cJSON_GetObjectItem(resp_json, "checksum");
  if (checksum && cJSON_IsString(checksum)) {
    info.checksum = checksum->valuestring;
  }

  ESP_LOGI(TAG, "Update available: v%s (%zu bytes)", info.version.c_str(),
           info.size_bytes);
  return info;
}

static std::string s_auth_header;

static esp_err_t ota_http_init_cb(esp_http_client_handle_t client) {
  if (!s_auth_header.empty()) {
    esp_http_client_set_header(client, "Authorization", s_auth_header.c_str());
  }
  return ESP_OK;
}

Expected<void> FirmwareUpdater::apply_update(const UpdateInfo &info) {
  ESP_LOGI(TAG, "Starting OTA update to version %s (%zu bytes) via esp_https_ota",
           info.version.c_str(), info.size_bytes);

  status_ = OtaStatus::Downloading;
  last_error_.clear();

  std::string download_url = api_url_ + info.url;
  s_auth_header = "Bearer " + token_;

  esp_http_client_config_t http_config = {};
  http_config.url = download_url.c_str();
  http_config.timeout_ms = 30000;
  http_config.keep_alive_enable = true;
  http_config.buffer_size = 4096;
  http_config.crt_bundle_attach = esp_crt_bundle_attach;
#ifdef CONFIG_MBEDTLS_TYPE_EXTERNAL_TX_BUFFER
  // Clean security handling: if TLS is active, let ESP-IDF handle certificate
  // verification using system bundle or custom anchors
#endif

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;
  ota_config.http_client_init_cb = ota_http_init_cb;

  esp_https_ota_handle_t ota_handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_https_ota_begin failed: %s (0x%x)", esp_err_to_name(err), err);
    status_ = OtaStatus::Failed;
    last_error_ = std::string("esp_https_ota_begin: ") + esp_err_to_name(err);
    return std::unexpected(DeviceError::InternalError);
  }

  esp_err_t perform_err = ESP_OK;
  while (true) {
    perform_err = esp_https_ota_perform(ota_handle);
    if (perform_err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      break;
    }

    int read_len = esp_https_ota_get_image_len_read(ota_handle);
    int total_len = esp_https_ota_get_image_size(ota_handle);
    if (total_len > 0) {
      ESP_LOGI(TAG, "OTA progress: %d / %d bytes", read_len, total_len);
    } else {
      ESP_LOGI(TAG, "OTA progress: %d bytes read", read_len);
    }
  }

  if (perform_err != ESP_OK) {
    ESP_LOGE(TAG, "esp_https_ota_perform failed: %s (0x%x)",
             esp_err_to_name(perform_err), perform_err);
    esp_https_ota_abort(ota_handle);
    status_ = OtaStatus::Failed;
    last_error_ = std::string("esp_https_ota_perform: ") + esp_err_to_name(perform_err);
    return std::unexpected(DeviceError::InternalError);
  }

  status_ = OtaStatus::Installing;

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    ESP_LOGE(TAG, "Complete data was not received");
    esp_https_ota_abort(ota_handle);
    status_ = OtaStatus::Failed;
    last_error_ = "Incomplete OTA data";
    return std::unexpected(DeviceError::InternalError);
  }

  err = esp_https_ota_finish(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_https_ota_finish failed: %s (0x%x)", esp_err_to_name(err), err);
    status_ = OtaStatus::Failed;
    last_error_ = std::string("esp_https_ota_finish: ") + esp_err_to_name(err);
    return std::unexpected(DeviceError::InternalError);
  }

  status_ = OtaStatus::Success;
  ESP_LOGI(TAG, "OTA update to v%s applied successfully via esp_https_ota.",
           info.version.c_str());
  return {};
}

} // namespace vigo::ota
