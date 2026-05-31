#ifndef VIGO_H264_ENCODER_HPP
#define VIGO_H264_ENCODER_HPP

#include "aligned_allocator.hpp"
#include "error_types.hpp"
#include "esp_h264_enc_single_hw.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace vigo::camera {

struct EncodedFrame {
  std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>> data;
  size_t pts;
  bool is_keyframe;
};

class H264Encoder {
public:
  H264Encoder();
  ~H264Encoder();

  // Delete copy to ensure RAII
  H264Encoder(const H264Encoder &) = delete;
  H264Encoder &operator=(const H264Encoder &) = delete;

  Expected<void> init(int width, int height, int fps, int bitrate_kbps, int gop);
  Expected<EncodedFrame> encode(const uint8_t *yuv_data, size_t size, uint64_t pts);

private:
  using OutBuffer = std::vector<uint8_t, vigo::memory::AlignedPsramAllocator<uint8_t>>;

  esp_h264_enc_handle_t handle_{nullptr};
  bool is_initialized_{false};
  OutBuffer out_buffer_{};
  size_t out_buffer_size_{0};
};

} // namespace vigo::camera

#endif // VIGO_H264_ENCODER_HPP