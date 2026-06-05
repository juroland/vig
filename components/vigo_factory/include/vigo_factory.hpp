#ifndef VIG_FACTORY_HPP
#define VIG_FACTORY_HPP

#include <esp_err.h>
#include <string>
#include <vector>

namespace vigo::factory {

// Custom error codes for factory retrieval
constexpr esp_err_t ESP_ERR_FACTORY_NOT_INITIALIZED = 0x501;
constexpr esp_err_t ESP_ERR_FACTORY_KEY_NOT_FOUND = 0x502;
constexpr esp_err_t ESP_ERR_FACTORY_PARTITION_FAIL = 0x503;

/**
 * @brief Initialize the fct_nvs partition using nvs_flash_init_partition.
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t init_factory_partition();

/**
 * @brief Retrieve unique device hardware ID (string) from fct_nvs.
 * @param out_hardware_id Output string to store hardware_id.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t get_hardware_id(std::string &out_hardware_id);

/**
 * @brief Retrieve setup/device token (string) from fct_nvs.
 * @param out_device_token Output string to store device_token.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t get_device_token(std::string &out_device_token);

/**
 * @brief Retrieve DTLS private key (blob/binary) from fct_nvs.
 * @param out_key Output vector to store the key blob.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t get_dtls_key(std::vector<uint8_t> &out_key);

/**
 * @brief Retrieve DTLS certificate (string) from fct_nvs.
 * @param out_cert Output string to store the cert.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t get_dtls_cert(std::string &out_cert);

} // namespace vigo::factory

#endif // VIG_FACTORY_HPP
