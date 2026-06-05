#include "vigo_factory.hpp"
#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

static const char *TAG = "VigFactory";

namespace vigo::factory {

static bool s_factory_initialized = false;

esp_err_t init_factory_partition() {
  ESP_LOGI(TAG, "Initializing fct_nvs partition...");
  esp_err_t err = nvs_flash_init_partition("fct_nvs");
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize fct_nvs partition: %s (0x%x)",
             esp_err_to_name(err), err);
    return ESP_ERR_FACTORY_PARTITION_FAIL;
  }
  s_factory_initialized = true;
  ESP_LOGI(TAG, "fct_nvs partition initialized successfully.");
  return ESP_OK;
}

static esp_err_t get_string_param(const char *key, std::string &out_val) {
  if (!s_factory_initialized) {
    return ESP_ERR_FACTORY_NOT_INITIALIZED;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open_from_partition("fct_nvs", "factory", NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open fct_nvs in READONLY mode: %s (0x%x)",
             esp_err_to_name(err), err);
    return err;
  }

  size_t required_size = 0;
  err = nvs_get_str(handle, key, nullptr, &required_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get size for key '%s': %s (0x%x)", key,
             esp_err_to_name(err), err);
    nvs_close(handle);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_FACTORY_KEY_NOT_FOUND : err;
  }

  std::vector<char> buffer(required_size);
  err = nvs_get_str(handle, key, buffer.data(), &required_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read string key '%s': %s (0x%x)", key,
             esp_err_to_name(err), err);
    nvs_close(handle);
    return err;
  }

  out_val.assign(buffer.data(), required_size - 1); // Exclude null terminator
  nvs_close(handle);
  return ESP_OK;
}

esp_err_t get_hardware_id(std::string &out_hardware_id) {
  return get_string_param("hardware_id", out_hardware_id);
}

esp_err_t get_device_token(std::string &out_device_token) {
  return get_string_param("device_token", out_device_token);
}

esp_err_t get_dtls_cert(std::string &out_cert) {
  return get_string_param("dtls_cert", out_cert);
}

esp_err_t get_dtls_key(std::vector<uint8_t> &out_key) {
  if (!s_factory_initialized) {
    return ESP_ERR_FACTORY_NOT_INITIALIZED;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open_from_partition("fct_nvs", "factory", NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open fct_nvs in READONLY mode: %s (0x%x)",
             esp_err_to_name(err), err);
    return err;
  }

  size_t required_size = 0;
  err = nvs_get_blob(handle, "dtls_key", nullptr, &required_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get size for dtls_key blob: %s (0x%x)",
             esp_err_to_name(err), err);
    nvs_close(handle);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_FACTORY_KEY_NOT_FOUND : err;
  }

  out_key.resize(required_size);
  err = nvs_get_blob(handle, "dtls_key", out_key.data(), &required_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read dtls_key blob: %s (0x%x)", esp_err_to_name(err), err);
    nvs_close(handle);
    return err;
  }

  nvs_close(handle);
  return ESP_OK;
}

} // namespace vigo::factory
