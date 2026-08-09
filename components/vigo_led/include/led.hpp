extern "C" {
#include "driver/gpio.h"
}

namespace vigo::led {

#include "driver/gpio.h"

class RgbLed {
public:
  // Constructor accepting individual GPIO pins for Red, Green, and Blue
  RgbLed(gpio_num_t red_pin, gpio_num_t green_pin, gpio_num_t blue_pin);

  // Set individual states for each color channel
  void setState(bool red_on, bool green_on, bool blue_on);

  // Convenience methods for individual channels
  void setRed(bool on);
  void setGreen(bool on);
  void setBlue(bool on);

  // Turn off all channels
  void off();

private:
  gpio_num_t r_pin_;
  gpio_num_t g_pin_;
  gpio_num_t b_pin_;
};

}; // namespace vigo::led