#include "h264_encoder.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "H264Encoder";

namespace vig::camera
{

    H264Encoder::H264Encoder() = default;

    H264Encoder::~H264Encoder()
    {
        if (handle_)
        {
            esp_h264_enc_close(handle_);
            esp_h264_enc_del(handle_);
        }
    }

    Expected<void> H264Encoder::init(int width, int height, int fps, int bitrate_kbps, int gop)
    {
        if (is_initialized_)
            return {};

        ESP_LOGI(TAG, "Initializing Hardware H.264 Encoder (%dx%d, %d fps, %d kbps, GOP %d)",
                 width, height, fps, bitrate_kbps, gop);

        esp_h264_enc_cfg_hw_t cfg = {
            .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
            .gop = static_cast<uint8_t>(gop),
            .fps = static_cast<uint8_t>(fps),
            .res = {
                .width = static_cast<uint16_t>(width),
                .height = static_cast<uint16_t>(height),
            },
            .rc = {
                .bitrate = static_cast<uint32_t>(bitrate_kbps * 1000),
                .qp_min = 20,
                .qp_max = 40,
            },
        };

        esp_h264_err_t err = esp_h264_enc_hw_new(&cfg, &handle_);
        if (err != ESP_H264_ERR_OK)
        {
            ESP_LOGE(TAG, "Failed to create hardware encoder: %d", err);
            return std::unexpected(DeviceError::EncoderInitFailed);
        }

        if (esp_h264_enc_open(handle_) != ESP_H264_ERR_OK)
        {
            ESP_LOGE(TAG, "Failed to open hardware encoder");
            return std::unexpected(DeviceError::EncoderInitFailed);
        }

        // Allocate output buffer (2 bytes per pixel on average)
        out_buffer_size_ = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
        out_buffer_.resize(out_buffer_size_);
        if (out_buffer_.empty())
        {
            ESP_LOGE(TAG, "Failed to allocate aligned output buffer (%zu bytes)", out_buffer_size_);
            esp_h264_enc_close(handle_);
            esp_h264_enc_del(handle_);
            handle_ = nullptr;
            return std::unexpected(DeviceError::EncoderInitFailed);
        }

        is_initialized_ = true;
        return {};
    }

    Expected<EncodedFrame> H264Encoder::encode(const uint8_t *yuv_data, size_t size, uint64_t pts)
    {
        if (!is_initialized_)
            return std::unexpected(DeviceError::EncoderInitFailed);

        esp_h264_enc_in_frame_t in_frame = {};
        in_frame.raw_data.buffer = const_cast<uint8_t *>(yuv_data);
        in_frame.raw_data.len = (uint32_t)size;
        in_frame.pts = (uint32_t)pts;

        esp_h264_enc_out_frame_t out_frame = {};
        out_frame.raw_data.buffer = out_buffer_.data();
        out_frame.raw_data.len = out_buffer_size_;

        esp_h264_err_t err = esp_h264_enc_process(handle_, &in_frame, &out_frame);
        if (err != ESP_H264_ERR_OK)
        {
            ESP_LOGE(TAG, "Encoding failed: %d", err);
            return std::unexpected(DeviceError::EncoderProcessFailed);
        }

        EncodedFrame frame;
        frame.data.assign(out_frame.raw_data.buffer, out_frame.raw_data.buffer + out_frame.length);
        frame.pts = out_frame.pts;
        frame.is_keyframe = (out_frame.frame_type == ESP_H264_FRAME_TYPE_IDR || out_frame.frame_type == ESP_H264_FRAME_TYPE_I);

        static uint32_t frame_count = 0;
        frame_count++;
        if (frame_count % 250 == 0)
        {
            ESP_LOGI("H264Encoder", "Encoded frame %d: type = %d, keyframe = %d, len = %d",
                     (int)frame_count, (int)out_frame.frame_type, (int)frame.is_keyframe, (int)out_frame.length);
        }

        return frame;
    }

} // namespace vig::camera
