#ifndef VIG_COMMON_ERROR_TYPES_HPP
#define VIG_COMMON_ERROR_TYPES_HPP

#include <esp_err.h>
#include <expected>
#include <string_view>

namespace vig {

enum class DeviceError {
  None,
  NetworkInitFailed,
  ConnectionLost,
  HttpPayloadError,
  HttpRequestFailed,
  CameraInitFailed,
  CameraCaptureFailed,
  JsonSerializationFailed,
  EncoderInitFailed,
  EncoderProcessFailed,
  StreamServerInitFailed,
  CameraLdoFailed,
  CameraIspFailed,
  InternalError
};

template <typename T> using Expected = std::expected<T, DeviceError>;

constexpr std::string_view to_string(DeviceError err) {
  switch (err) {
  case DeviceError::None:
    return "None";
  case DeviceError::NetworkInitFailed:
    return "Network Initialization Failed";
  case DeviceError::ConnectionLost:
    return "Connection Lost";
  case DeviceError::HttpPayloadError:
    return "HTTP Payload Error";
  case DeviceError::HttpRequestFailed:
    return "HTTP Request Failed";
  case DeviceError::CameraInitFailed:
    return "Camera Initialization Failed";
  case DeviceError::CameraCaptureFailed:
    return "Camera Capture Failed";
  case DeviceError::JsonSerializationFailed:
    return "JSON Serialization Failed";
  case DeviceError::EncoderInitFailed:
    return "Encoder Initialization Failed";
  case DeviceError::EncoderProcessFailed:
    return "Encoder Process Failed";
  case DeviceError::StreamServerInitFailed:
    return "Stream Server Initialization Failed";
  case DeviceError::CameraLdoFailed:
    return "Camera LDO Initialization Failed";
  case DeviceError::CameraIspFailed:
    return "Camera ISP Initialization Failed";
  case DeviceError::InternalError:
    return "Internal Error";
  default:
    return "Unknown Error";
  }
}

} // namespace vig

#endif // VIG_COMMON_ERROR_TYPES_HPP
