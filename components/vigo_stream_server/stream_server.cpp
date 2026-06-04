#include "stream_server.hpp"
#include "esp_log.h"
#include "pedestrian_detector.hpp"
#include "stream_page.h"
#include <cstring>
#include <netinet/tcp.h>
#include <sys/socket.h>

static const char *TAG = "StreamServer";

namespace vigo::net {

StreamServer::StreamServer() { clients_mutex_ = xSemaphoreCreateMutex(); }

StreamServer::~StreamServer() {
  stop();
  if (clients_mutex_) {
    vSemaphoreDelete(clients_mutex_);
  }
}

Expected<void> StreamServer::start(int port, SnapshotCallback snapshot_cb) {
  snapshot_cb_ = snapshot_cb;

  // Initialize persistent JPEG encoder engine
  jpeg_encode_engine_cfg_t eng_cfg = {};
  eng_cfg.timeout_ms = 1000;
  eng_cfg.intr_priority = 0;
  if (jpeg_new_encoder_engine(&eng_cfg, &jpeg_engine_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create persistent JPEG encoder engine for StreamServer");
  }

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

  // Snapshot URI
  httpd_uri_t snap_uri = {};
  snap_uri.uri = "/snapshot";
  snap_uri.method = HTTP_GET;
  snap_uri.handler = [](httpd_req_t *req) {
    auto *self = static_cast<StreamServer *>(req->user_ctx);
    if (self->snapshot_cb_) {
      auto jpeg_data = self->snapshot_cb_();
      if (!jpeg_data.empty()) {
        httpd_resp_set_type(req, "image/jpeg");
        return httpd_resp_send(req, (const char *)jpeg_data.data(), jpeg_data.size());
      }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
  };
  snap_uri.user_ctx = this;
  if (httpd_register_uri_handler(server_handle_, &snap_uri) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register snapshot URI handler");
  }

  // Debug info URI
  httpd_uri_t debug_info_uri = {};
  debug_info_uri.uri = "/debug/info";
  debug_info_uri.method = HTTP_GET;
  debug_info_uri.handler = [](httpd_req_t *req) {
    std::vector<uint8_t> yuyv_data;
    int width = 0;
    int height = 0;
    float probability = 0.0f;
    vigo::detection::PedestrianDetector::get_debug_frame(yuyv_data, width, height,
                                                         probability);

    char json[64];
    snprintf(json, sizeof(json), "{\"probability\": %.4f}", probability);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  };
  debug_info_uri.user_ctx = nullptr;
  if (httpd_register_uri_handler(server_handle_, &debug_info_uri) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register debug info URI handler");
  }

  // Debug image URI
  httpd_uri_t debug_img_uri = {};
  debug_img_uri.uri = "/debug/image";
  debug_img_uri.method = HTTP_GET;
  debug_img_uri.handler = [](httpd_req_t *req) {
    auto *self = static_cast<StreamServer *>(req->user_ctx);
    std::vector<uint8_t> yuyv_data;
    int width = 0;
    int height = 0;
    float probability = 0.0f;
    if (vigo::detection::PedestrianDetector::get_debug_frame(yuyv_data, width, height,
                                                             probability)) {
      if (self->jpeg_engine_) {
        jpeg_encode_cfg_t enc_cfg = {};
        enc_cfg.width = width;
        enc_cfg.height = height;
        enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_YUV422;
        enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
        enc_cfg.image_quality = 80;
        enc_cfg.pixel_reverse = true;

        size_t inbuf_size = yuyv_data.size();
        jpeg_encode_memory_alloc_cfg_t in_mem_cfg = {};
        in_mem_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
        size_t actual_in_size = 0;
        uint8_t *inbuf = static_cast<uint8_t *>(
            jpeg_alloc_encoder_mem(inbuf_size, &in_mem_cfg, &actual_in_size));

        size_t outbuf_size = width * height;
        jpeg_encode_memory_alloc_cfg_t mem_cfg = {};
        mem_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
        size_t actual_out_size = 0;
        uint8_t *outbuf = static_cast<uint8_t *>(
            jpeg_alloc_encoder_mem(outbuf_size, &mem_cfg, &actual_out_size));

        if (inbuf && outbuf) {
          memcpy(inbuf, yuyv_data.data(), inbuf_size);
          uint32_t out_size = 0;
          esp_err_t err =
              jpeg_encoder_process(self->jpeg_engine_, &enc_cfg, inbuf, inbuf_size,
                                   outbuf, actual_out_size, &out_size);
          if (err == ESP_OK && out_size > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_set_hdr(req, "Cache-Control",
                               "no-cache, no-store, must-revalidate");
            esp_err_t ret = httpd_resp_send(req, (const char *)outbuf, out_size);
            heap_caps_free(inbuf);
            heap_caps_free(outbuf);
            return ret;
          }
        }
        if (inbuf) {
          heap_caps_free(inbuf);
        }
        if (outbuf) {
          heap_caps_free(outbuf);
        }
      }
    }
    httpd_resp_send_404(req);
    return ESP_FAIL;
  };
  debug_img_uri.user_ctx = this;
  if (httpd_register_uri_handler(server_handle_, &debug_img_uri) != ESP_OK) {
    ESP_LOGW(TAG, "Failed to register debug image URI handler");
  }

  return {};
}

void StreamServer::stop() {
  if (server_handle_) {
    httpd_stop(server_handle_);
    server_handle_ = nullptr;
  }
  if (jpeg_engine_) {
    jpeg_del_encoder_engine(jpeg_engine_);
    jpeg_engine_ = nullptr;
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
    // Optimize TCP socket for low-latency video streaming
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flag, sizeof(flag));

    // Increase send buffer size to handle bursty I-frames
    int sndbuf = 128 * 1024; // 128 KB
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (void *)&sndbuf, sizeof(sndbuf));

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

void StreamServer::push_frame(const std::shared_ptr<camera::EncodedFrame> &frame) {
  if (!clients_mutex_ || !server_handle_)
    return;

  xSemaphoreTake(clients_mutex_, portMAX_DELAY);

  auto it = clients_.begin();
  while (it != clients_.end()) {
    // Promote waiting clients if we have a keyframe
    if (frame->is_keyframe && !it->ready) {
      it->ready = true;
      ESP_LOGI(TAG, "Client (FD: %d) promoted to ready (Keyframe received)", it->fd);
    }

    if (!it->ready) {
      ++it;
      continue;
    }

    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.payload = const_cast<uint8_t *>(frame->data.data());
    ws_pkt.len = frame->data.size();
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;
    ws_pkt.final = true;

    esp_err_t err = httpd_ws_send_data(server_handle_, it->fd, &ws_pkt);
    if (err != ESP_OK) {
      if (err == ESP_ERR_INVALID_ARG || err == ESP_FAIL) {
        ESP_LOGI(TAG, "Client FD %d disconnected, removing", it->fd);
        it = clients_.erase(it);
        continue;
      } else {
        ESP_LOGD(TAG, "Send failed for client FD: %d (error: %d), dropping frame",
                 it->fd, err);
      }
    }

    ++it;
  }

  xSemaphoreGive(clients_mutex_);
}

} // namespace vigo::net
