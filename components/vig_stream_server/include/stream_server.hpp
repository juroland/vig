#pragma once

#include "error_types.hpp"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "h264_encoder.hpp"
#include <functional>
#include <list>
#include <memory>
#include <vector>

namespace vig::net {

class StreamServer {
public:
  StreamServer();
  ~StreamServer();

  // Delete copy to ensure RAII
  StreamServer(const StreamServer &) = delete;
  StreamServer &operator=(const StreamServer &) = delete;

  using SnapshotCallback = std::function<std::vector<uint8_t>()>;

  Expected<void> start(int port, SnapshotCallback snapshot_cb = nullptr);
  void stop();

  void push_frame(const camera::EncodedFrame &frame);

private:
  httpd_handle_t server_handle_{nullptr};
  SemaphoreHandle_t clients_mutex_{nullptr};
  struct Client {
    int fd;
    bool ready;
  };
  std::list<Client> clients_;

  SnapshotCallback snapshot_cb_ = nullptr;

  static esp_err_t ws_handler(httpd_req_t *req);
  static void async_send_callback(void *arg);

  struct AsyncSendArg {
    httpd_handle_t hd;
    int fd;
    std::shared_ptr<camera::EncodedFrame> frame;
  };
};

} // namespace vig::net
