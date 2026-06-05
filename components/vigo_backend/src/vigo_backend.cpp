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
BackendClient::send_heartbeat(const vigo::telemetry::TelemetryData &telemetry,
                              std::string_view firmware_version,
                              std::string_view ota_status, std::string_view ota_error) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);

  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());
  cJSON_AddStringToObject(root, "firmware_version",
                          std::string(firmware_version).c_str());

  // OTA status reporting
  cJSON_AddStringToObject(root, "ota_status", std::string(ota_status).c_str());
  if (!ota_error.empty()) {
    cJSON_AddStringToObject(root, "ota_error", std::string(ota_error).c_str());
  } else {
    cJSON_AddNullToObject(root, "ota_error");
  }

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

  cJSON *update_available = cJSON_GetObjectItem(resp_json, "update_available");
  if (update_available && cJSON_IsBool(update_available)) {
    out.update_available = cJSON_IsTrue(update_available);
  }

  cJSON *update_version = cJSON_GetObjectItem(resp_json, "update_version");
  if (update_version && cJSON_IsString(update_version)) {
    out.update_version = update_version->valuestring;
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

Expected<void>
BackendClient::send_motion_event(const std::string &base64_jpeg,
                                 const std::vector<DetectionResult> &detections) {
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return std::unexpected(DeviceError::InternalError);

  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_ptr(root, cJSON_Delete);
  cJSON_AddStringToObject(root, "hardware_id", hardware_id_.c_str());
  cJSON_AddNullToObject(root, "timestamp");
  cJSON_AddStringToObject(root, "capture", base64_jpeg.c_str());

  if (!detections.empty()) {
    cJSON *dets_arr = cJSON_AddArrayToObject(root, "detections");
    for (const auto &det : detections) {
      cJSON *det_obj = cJSON_CreateObject();
      if (det_obj) {
        cJSON *box_arr = cJSON_CreateArray();
        if (box_arr) {
          for (float coord : det.box) {
            cJSON_AddItemToArray(box_arr,
                                 cJSON_CreateNumber(static_cast<double>(coord)));
          }
          cJSON_AddItemToObject(det_obj, "box", box_arr);
        }
        cJSON_AddNumberToObject(det_obj, "score", static_cast<double>(det.score));
        cJSON_AddStringToObject(det_obj, "label", det.label.c_str());
        cJSON_AddItemToArray(dets_arr, det_obj);
      }
    }
  } else {
    cJSON_AddNullToObject(root, "detections");
  }

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
