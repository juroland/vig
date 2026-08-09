#include "led.hpp"

namespace vigo::led {
RgbLed::RgbLed(gpio_num_t red_pin, gpio_num_t green_pin, gpio_num_t blue_pin)
    : r_pin_(red_pin), g_pin_(green_pin), b_pin_(blue_pin) {

  uint64_t pin_mask = (1ULL << r_pin_) | (1ULL << g_pin_) | (1ULL << b_pin_);

  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = pin_mask;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&io_conf);

  off();
}

void RgbLed::setState(bool red_on, bool green_on, bool blue_on) {
  // Common Anode inverted logic: LOW (0) turns it ON, HIGH (1) turns it OFF
  gpio_set_level(r_pin_, red_on ? 0 : 1);
  gpio_set_level(g_pin_, green_on ? 0 : 1);
  gpio_set_level(b_pin_, blue_on ? 0 : 1);
}

void RgbLed::setRed(bool on) { gpio_set_level(r_pin_, on ? 0 : 1); }

void RgbLed::setGreen(bool on) { gpio_set_level(g_pin_, on ? 0 : 1); }

void RgbLed::setBlue(bool on) { gpio_set_level(b_pin_, on ? 0 : 1); }

void RgbLed::off() { setState(false, false, false); }
} // namespace vigo::led
