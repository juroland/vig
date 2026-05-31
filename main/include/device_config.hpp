#ifndef VIG_MAIN_DEVICE_CONFIG_HPP
#define VIG_MAIN_DEVICE_CONFIG_HPP

#include "sdkconfig.h"
#include <string_view>

namespace vig::config {

constexpr std::string_view HARDWARE_ID = CONFIG_VIG_HARDWARE_ID;
constexpr std::string_view FIRMWARE_VERSION = VIG_VERSION;
constexpr std::string_view API_BASE_URL = CONFIG_VIG_API_URL;
constexpr std::string_view DEVICE_TOKEN = CONFIG_VIG_DEVICE_TOKEN;
constexpr std::string_view WHIP_URL = CONFIG_VIG_WHIP_URL;

constexpr int HEARTBEAT_INTERVAL_MS = CONFIG_VIG_HEARTBEAT_INTERVAL_MS;
constexpr int NETWORK_RETRY_DELAY_MS = 5000;

// Camera & Video
#ifdef CONFIG_VIG_USE_MOCK_CAMERA
constexpr bool USE_MOCK_CAMERA = CONFIG_VIG_USE_MOCK_CAMERA;
#else
constexpr bool USE_MOCK_CAMERA = false;
#endif
constexpr int CAMERA_WIDTH = CONFIG_VIG_CAMERA_WIDTH;
constexpr int CAMERA_HEIGHT = CONFIG_VIG_CAMERA_HEIGHT;
constexpr int CAMERA_TARGET_FPS = 30;
constexpr int ENCODER_BITRATE_KBPS = CONFIG_VIG_ENCODER_BITRATE_KBPS;
constexpr int ENCODER_GOP = CONFIG_VIG_ENCODER_GOP;

// Streaming
constexpr int STREAM_PORT = CONFIG_VIG_STREAM_PORT;
constexpr int STREAM_MAX_CLIENTS = CONFIG_VIG_STREAM_MAX_CLIENTS;

// Network interface selection
#ifdef CONFIG_VIG_USE_WIFI
constexpr bool USE_WIFI = true;
constexpr std::string_view WIFI_SSID = CONFIG_VIG_WIFI_SSID;
constexpr std::string_view WIFI_PASSWORD = CONFIG_VIG_WIFI_PASSWORD;
#else
constexpr bool USE_WIFI = false;
constexpr std::string_view WIFI_SSID = "";
constexpr std::string_view WIFI_PASSWORD = "";
#endif

} // namespace vig::config

#endif // VIG_MAIN_DEVICE_CONFIG_HPP
