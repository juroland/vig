#include "stream_server.hpp"
#include "esp_log.h"
#include "stream_page.h"
#include <cstring>
#include <sys/socket.h>

static const char *TAG = "StreamServer";

namespace vig::net {

StreamServer::StreamServer() { clients_mutex_ = xSemaphoreCreateMutex(); }

StreamServer::~StreamServer() {
  stop();
  if (clients_mutex_) {
    vSemaphoreDelete(clients_mutex_);
  }
}

Expected<void> StreamServer::start(int port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.ctrl_port = port + 1;
  config.max_open_sockets = 10;
  config.lru_purge_enable = true;
  config.stack_size = 8192;

  ESP_LOGI(TAG, "Starting Stream Server on port %d", port);

  if (httpd_start(&server_handle_, &config) != ESP_OK) {
    return std::unexpected(DeviceError::StreamServerInitFailed);
  }

  // Root URI (HTML page)
  httpd_uri_t index_uri = {};
  index_uri.uri = "/";
  index_uri.method = HTTP_GET;
  index_uri.handler = [](httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, stream_page_html, HTTPD_RESP_USE_STRLEN);
  };
  index_uri.user_ctx = nullptr;
  if (httpd_register_uri_handler(server_handle_, &index_uri) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register index URI handler");
  }

  // Favicon URI to silence the 404 warning
  httpd_uri_t favicon_uri = {};
  favicon_uri.uri = "/favicon.ico";
  favicon_uri.method = HTTP_GET;
  favicon_uri.handler = [](httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, nullptr, 0);
  };
  favicon_uri.user_ctx = nullptr;
  if (httpd_register_uri_handler(server_handle_, &favicon_uri) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register favicon URI handler");
  }

  // WebSocket URI
  httpd_uri_t ws_uri = {};
  ws_uri.uri = "/stream";
  ws_uri.method = HTTP_GET;
  ws_uri.handler = ws_handler;
  ws_uri.user_ctx = this;
  ws_uri.is_websocket = true;
  if (httpd_register_uri_handler(server_handle_, &ws_uri) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register WebSocket URI handler");
    return std::unexpected(DeviceError::StreamServerInitFailed);
  }

  return {};
}

void StreamServer::stop() {
  if (server_handle_) {
    httpd_stop(server_handle_);
    server_handle_ = nullptr;
  }
}

esp_err_t StreamServer::ws_handler(httpd_req_t *req) {
  auto *self = static_cast<StreamServer *>(req->user_ctx);
  int fd = httpd_req_to_sockfd(req);
  ESP_LOGI("StreamServer", "ws_handler: FD = %d, method = %d", fd, (int)req->method);

  // Safely add client to the active list if not already registered
  xSemaphoreTake(self->clients_mutex_, portMAX_DELAY);
  bool found = false;
  for (const auto &client : self->clients_) {
    if (client.fd == fd) {
      found = true;
      break;
    }
  }
  if (!found) {
    self->clients_.push_back({fd, false});
    ESP_LOGI("StreamServer",
             "Client FD %d successfully registered in clients_ list (Total: %u)", fd,
             (unsigned int)self->clients_.size());
  }
  xSemaphoreGive(self->clients_mutex_);

  // Handle actual WebSocket data/control frames (method is not HTTP_GET)
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  // First call to httpd_ws_recv_frame with max_len = 0 to get the frame length and type
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    if (ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "Failed to read WebSocket frame header from FD %d: %d", fd, ret);
    }
    return ret;
  }

  if (ws_pkt.len > 0) {
    uint8_t buf[128] = {0};
    if (ws_pkt.len < sizeof(buf)) {
      ws_pkt.payload = buf;
      ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Received WS frame from FD %d: len = %d, payload = %s", fd,
                 ws_pkt.len, (char *)ws_pkt.payload);
      } else {
        ESP_LOGW(TAG, "Failed to read WebSocket frame payload from FD %d: %d", fd, ret);
      }
    } else {
      ESP_LOGW(TAG, "WS frame payload too large (%d bytes), dropping", ws_pkt.len);
    }
  }

  return ESP_OK;
}

void StreamServer::push_frame(const camera::EncodedFrame &frame) {
  if (!clients_mutex_ || !server_handle_)
    return;

  // Wrap the frame in a shared_ptr once to avoid multiple copies
  auto shared_frame = std::make_shared<camera::EncodedFrame>(frame);

  xSemaphoreTake(clients_mutex_, portMAX_DELAY);

  if (!clients_.empty()) {
    static uint32_t push_log_count = 0;
    if (push_log_count++ % 50 == 0) {
      ESP_LOGI(TAG, "push_frame active: %u connected client(s)", clients_.size());
    }
  }

  auto it = clients_.begin();
  while (it != clients_.end()) {
    // Robustly check if the client socket is still active
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(it->fd, (struct sockaddr *)&addr, &addr_len) != 0) {
      ESP_LOGI(TAG, "Client socket FD %d closed/dead, removing", it->fd);
      it = clients_.erase(it);
      continue;
    }

    // Promote waiting clients if we have a keyframe
    if (frame.is_keyframe && !it->ready) {
      it->ready = true;
      ESP_LOGI(TAG, "Client (FD: %d) promoted to ready (Keyframe received)", it->fd);
    }

    if (!it->ready) {
      ++it;
      continue;
    }

    AsyncSendArg *arg = new (std::nothrow)
        AsyncSendArg{.hd = server_handle_, .fd = it->fd, .frame = shared_frame};

    if (arg == nullptr) {
      ESP_LOGE(TAG, "OOM: failed to allocate send arg");
      break;
    }

    esp_err_t err = httpd_queue_work(server_handle_, async_send_callback, arg);
    if (err != ESP_OK) {
      // Drop frame on queue full or temporary failure, do NOT violently disconnect
      ESP_LOGD(TAG, "Queue work failed for client FD: %d (error: %d), dropping frame",
               it->fd, err);
      delete arg;
    }

    ++it;
  }

  xSemaphoreGive(clients_mutex_);
}

void StreamServer::async_send_callback(void *arg) {
  auto *send_arg = static_cast<AsyncSendArg *>(arg);

  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.payload = send_arg->frame->data.data();
  ws_pkt.len = send_arg->frame->data.size();
  ws_pkt.type = HTTPD_WS_TYPE_BINARY;

  esp_err_t ret = httpd_ws_send_frame_async(send_arg->hd, send_arg->fd, &ws_pkt);

  if (ret != ESP_OK) {
    ESP_LOGD(TAG, "Async send failed: %d", ret);
  }

  delete send_arg;
}

} // namespace vig::net
