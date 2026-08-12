#include <chrono>
#include <ranges>

#include "audio.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
}

static const char *TAG = "Audio";

namespace vigo::audio {
AudioCapturer::AudioCapturer() {

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_chan_));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = GPIO_NUM_20,
              .ws = GPIO_NUM_21,
              .dout = I2S_GPIO_UNUSED,
              .din = GPIO_NUM_22,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan_, &std_cfg));
}

std::string make_wave_filename(const std::string &output_dir) {
  auto now = std::chrono::system_clock::now();

  auto duration_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());

  return output_dir + "/" + std::to_string(duration_ms.count()) + ".wav";
}

void AudioCapturer::capture(const std::string &output_dir) {
  uint8_t *r_buf = (uint8_t *)calloc(1, READ_BUF_SIZE);
  size_t r_bytes = 0;

  ESP_ERROR_CHECK(i2s_channel_enable(rx_chan_));

  while (true) {
    auto file_res = WavFile::create(make_wave_filename(output_dir));
    if (file_res) {
      auto &file = file_res.value();
      for (int i = 0; i < 200; ++i) {
        if (i2s_channel_read(rx_chan_, r_buf, READ_BUF_SIZE, &r_bytes, 1000) ==
            ESP_OK) {
          ESP_LOGD(TAG, "Read Task: i2s read %zu bytes", r_bytes);
          ESP_LOGD(TAG, "[0] %x [1] %x [2] %x [3] %x [4] %x [5] %x [6] %x [7] %x",
                   r_buf[0], r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5], r_buf[6],
                   r_buf[7]);
          // Parse samples and write them to the wav file.
          for (auto i : std::views::iota(0uz, r_bytes) | std::views::stride(4)) {
            file.write(reinterpret_cast<const char *>(&r_buf[i] + 1));
          }
        } else {
          ESP_LOGE(TAG, "Read Task: i2s read failed");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

std::expected<WavFile, AudioError> WavFile::create(const std::string &path) {
  std::vector<char> stream_buffer(WAV_WRITE_BUFFER_SIZE);
  std::ofstream stream;
  stream.rdbuf()->pubsetbuf(stream_buffer.data(), stream_buffer.size());

  stream.open(path, std::ios::binary);
  if (!stream.is_open()) {
    ESP_LOGE(TAG, "Failed to open file for writing: %s", path.c_str());
    return std::unexpected(AudioError::FileOpenFailed);
  }

  WavFileHeader header;
  ESP_LOGI(TAG, "Create file %s", path.c_str());
  stream.write(reinterpret_cast<const char *>(&header), sizeof(WavFileHeader));

  return WavFile(std::move(stream), std::move(stream_buffer));
};

WavFile::~WavFile() {
  std::uint32_t total_data_bytes = n_samples_ * N_BYTES_PER_SAMPLE;

  auto chunk_size = 36 + total_data_bytes;
  stream_.seekp(4, std::ios::beg);
  stream_.write(reinterpret_cast<const char *>(&chunk_size), sizeof(chunk_size));

  auto subchunk2_size = total_data_bytes;
  stream_.seekp(40, std::ios::beg);
  stream_.write(reinterpret_cast<const char *>(&subchunk2_size),
                sizeof(subchunk2_size));

  ESP_LOGI(TAG, "Close file with size %s", std::to_string(total_data_bytes).c_str());
  stream_.close();
}

void WavFile::write(const char sample[N_BYTES_PER_SAMPLE]) {
  stream_.write(sample, N_BYTES_PER_SAMPLE);
  n_samples_ += 1;
}
} // namespace vigo::audio
