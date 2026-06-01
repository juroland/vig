#include "vigo_backend.hpp"
#include "cJSON.h"
#include "esp_log.h"
#include "net.hpp"

static const char *TAG = "VigBackend";

namespace vigo::backend {

BackendClient::BackendClient(const std::string &api_base_url,
                             const std::string &hardware_id,
                             const std::string &setup_token)
    : api_base_url_(api_base_url), hardware_id_(hardware_id),
      setup_token_(setup_token) {}

Expected<HeartbeatResponse>
BackendClient::send_heartbeat(const vigo::telemetry::TelemetryData &telemetry) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);

  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());
  cJSON_AddStringToObject(root, "firmware_version", "1.0.0"); // Hardcoded

  cJSON *tele_obj = cJSON_AddObjectToObject(root, "telemetry");
  cJSON_AddNumberToObject(tele_obj, "free_heap", telemetry.free_heap);
  cJSON_AddNumberToObject(tele_obj, "uptime", telemetry.uptime);
  cJSON_AddNumberToObject(tele_obj, "cpu_temp", telemetry.cpu_temp);

  if (!telemetry.snapshot.empty()) {
    cJSON_AddStringToObject(root, "snapshot", telemetry.snapshot.c_str());
  }

  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str)
    return std::unexpected(DeviceError::InternalError);
  std::string payload(json_str);
  cJSON_free(json_str);

  vigo::net::HttpClient client(api_base_url_ + "/api/devices/heartbeat", setup_token_);
  auto resp_res = client.post_json_with_response(payload);
  if (!resp_res.has_value()) {
    ESP_LOGE(TAG, "Heartbeat request failed");
    return std::unexpected(resp_res.error());
  }

  cJSON *resp_json = cJSON_Parse(resp_res.value().c_str());
  if (!resp_json) {
    ESP_LOGE(TAG, "cJSON_Parse failed on payload: '%s'", resp_res.value().c_str());
    return std::unexpected(DeviceError::HttpPayloadError);
  }
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> resp_ptr(resp_json, cJSON_Delete);

  HeartbeatResponse out;
  cJSON *ack = cJSON_GetObjectItem(resp_json, "ack");
  if (ack && cJSON_IsBool(ack)) {
    out.ack = cJSON_IsTrue(ack);
  }

  cJSON *stream_token = cJSON_GetObjectItem(resp_json, "stream_token");
  if (stream_token && cJSON_IsString(stream_token)) {
    out.stream_token = stream_token->valuestring;
  }

  cJSON *whip_url = cJSON_GetObjectItem(resp_json, "whip_url");
  if (whip_url && cJSON_IsString(whip_url)) {
    out.whip_url = whip_url->valuestring;
  }

  return out;
}

Expected<void> BackendClient::send_offline() {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);
  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());

  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str)
    return std::unexpected(DeviceError::InternalError);
  std::string payload(json_str);
  cJSON_free(json_str);

  vigo::net::HttpClient client(api_base_url_ + "/api/devices/offline", setup_token_);
  auto resp_res = client.post_json(payload);
  if (!resp_res.has_value()) {
    return std::unexpected(resp_res.error());
  }

  return {};
}

Expected<void> BackendClient::send_motion_event(const std::string &base64_jpeg) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);
  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());
  cJSON_AddNullToObject(root, "timestamp");
  cJSON_AddStringToObject(root, "capture", base64_jpeg.c_str());

  char *json_str = cJSON_PrintUnformatted(root);
  if (!json_str)
    return std::unexpected(DeviceError::InternalError);
  std::string payload(json_str);
  cJSON_free(json_str);

  vigo::net::HttpClient client(api_base_url_ + "/api/devices/motion", setup_token_);
  auto resp_res = client.post_json(payload);
  if (!resp_res.has_value()) {
    return std::unexpected(resp_res.error());
  }

  return {};
}

} // namespace vigo::backend
