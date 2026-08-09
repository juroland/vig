#include "audio.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_err.h"
}

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

void AudioCapturer::capture() {
  uint8_t *r_buf = (uint8_t *)calloc(1, READ_BUF_SIZE);
  assert(r_buf); // Check if r_buf allocation success
  size_t r_bytes = 0;

  /* Enable the RX channel */
  ESP_ERROR_CHECK(i2s_channel_enable(rx_chan_));

  while (1) {
    /* Read i2s data */
    if (i2s_channel_read(rx_chan_, r_buf, READ_BUF_SIZE, &r_bytes, 1000) == ESP_OK) {
      printf("Read Task: i2s read %d bytes\n-----------------------------------\n",
             r_bytes);
      printf("[0] %x [1] %x [2] %x [3] %x\n[4] %x [5] %x [6] %x [7] %x\n\n", r_buf[0],
             r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5], r_buf[6], r_buf[7]);
    } else {
      printf("Read Task: i2s read failed\n");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  free(r_buf);
  vTaskDelete(NULL);
}
} // namespace vigo::audio
