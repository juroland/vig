#ifndef VIG_NET_NET_HPP
#define VIG_NET_NET_HPP

#include "esp_eth.h"
#include "esp_http_client.h"
#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "error_types.hpp"

namespace vig::net {

class HttpClient {
public:
  explicit HttpClient(const std::string &url, const std::string &device_token);
  ~HttpClient();

  // Delete copy to ensure RAII
  HttpClient(const HttpClient &) = delete;
  HttpClient &operator=(const HttpClient &) = delete;

  Expected<int> post_json(const std::string &payload);
  Expected<std::string> post_json_with_response(const std::string &payload);

private:
  esp_http_client_handle_t client_handle_{nullptr};
  std::string auth_header_;
};

class NetworkManager {
public:
  static NetworkManager &instance();

  Expected<void> init_ethernet();
  Expected<void> init_wifi(const std::string &ssid, const std::string &password);
  bool is_connected() const { return connected_; }

private:
  NetworkManager() = default;
  bool connected_{false};
  esp_eth_handle_t eth_handle_{nullptr};

  static void eth_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
  static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
};

} // namespace vig::net

#endif // VIG_NET_NET_HPP
