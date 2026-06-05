#ifndef VIGO_MAIN_DEVICE_CONFIG_HPP
#define VIGO_MAIN_DEVICE_CONFIG_HPP

#include "sdkconfig.h"
#include <string_view>

namespace vigo::config {

constexpr std::string_view HARDWARE_ID = CONFIG_VIGO_HARDWARE_ID;
constexpr std::string_view FIRMWARE_VERSION = VIGO_VERSION;
constexpr std::string_view API_BASE_URL = CONFIG_VIGO_API_URL;
constexpr std::string_view DEVICE_TOKEN = CONFIG_VIGO_DEVICE_TOKEN;
constexpr std::string_view DTLS_CERT_PEM = CONFIG_VIGO_DTLS_CERT_PEM;
constexpr std::string_view DTLS_KEY_PEM = CONFIG_VIGO_DTLS_KEY_PEM;

constexpr int HEARTBEAT_INTERVAL_MS = CONFIG_VIGO_HEARTBEAT_INTERVAL_MS;
constexpr int NETWORK_RETRY_DELAY_MS = 5000;

// Camera & Video
#ifdef CONFIG_VIGO_USE_MOCK_CAMERA
constexpr bool USE_MOCK_CAMERA = CONFIG_VIGO_USE_MOCK_CAMERA;
#else
constexpr bool USE_MOCK_CAMERA = false;
#endif
constexpr int CAMERA_WIDTH = CONFIG_VIGO_CAMERA_WIDTH;
constexpr int CAMERA_HEIGHT = CONFIG_VIGO_CAMERA_HEIGHT;
constexpr int CAMERA_TARGET_FPS = 30;
constexpr int ENCODER_BITRATE_KBPS = CONFIG_VIGO_ENCODER_BITRATE_KBPS;
constexpr int ENCODER_GOP = CONFIG_VIGO_ENCODER_GOP;

// Motion & Pedestrian Detection
constexpr int MOTION_STRIDE = CONFIG_VIGO_MOTION_STRIDE;
constexpr int MOTION_THRESHOLD = CONFIG_VIGO_MOTION_THRESHOLD;
constexpr float MOTION_MIN_CHANGE_RATIO = CONFIG_VIGO_MOTION_MIN_CHANGE_RATIO;
constexpr int MOTION_COOLDOWN_MS = CONFIG_VIGO_MOTION_COOLDOWN_MS;
constexpr float PEDESTRIAN_MAX_AREA_PROPORTION =
    CONFIG_VIGO_PEDESTRIAN_MAX_AREA_PROPORTION;
constexpr float PEDESTRIAN_MIN_ASPECT_RATIO = CONFIG_VIGO_PEDESTRIAN_MIN_ASPECT_RATIO;
constexpr float PEDESTRIAN_MAX_ASPECT_RATIO = CONFIG_VIGO_PEDESTRIAN_MAX_ASPECT_RATIO;

// Streaming
constexpr int STREAM_PORT = CONFIG_VIGO_STREAM_PORT;
constexpr int STREAM_MAX_CLIENTS = CONFIG_VIGO_STREAM_MAX_CLIENTS;

// Network interface selection
#ifdef CONFIG_VIGO_USE_WIFI
constexpr bool USE_WIFI = true;
constexpr std::string_view WIFI_SSID = CONFIG_VIGO_WIFI_SSID;
constexpr std::string_view WIFI_PASSWORD = CONFIG_VIGO_WIFI_PASSWORD;
#else
constexpr bool USE_WIFI = false;
constexpr std::string_view WIFI_SSID = "";
constexpr std::string_view WIFI_PASSWORD = "";
#endif

} // namespace vigo::config

#endif // VIGO_MAIN_DEVICE_CONFIG_HPP
