extern "C" {
#include "driver/i2s_std.h"
}

namespace vigo::audio {

const int SAMPLE_RATE = 16000;
const int READ_BUF_SIZE = 2048;

class AudioCapturer {
public:
  AudioCapturer();
  void capture();

private:
  i2s_chan_handle_t tx_chan_;
  i2s_chan_handle_t rx_chan_;
};
}; // namespace vigo::audio