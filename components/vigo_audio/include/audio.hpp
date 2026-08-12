extern "C" {
#include "driver/i2s_std.h"
}

#include <expected>
#include <fstream>
#include <string>
#include <vector>

namespace vigo::audio {

const int SAMPLE_RATE = 16000;
const int READ_BUF_SIZE = 2048;

enum class AudioError { FileOpenFailed, WriteError };

class AudioCapturer {
public:
  AudioCapturer();
  void capture(const std::string &output_dir);

private:
  i2s_chan_handle_t rx_chan_;
};

const auto N_BYTES_PER_SAMPLE = 3;
const auto WAV_WRITE_BUFFER_SIZE = 4096;

class WavFile {
public:
  static std::expected<WavFile, AudioError> create(const std::string &path);
  ~WavFile();

  WavFile(const WavFile &) = delete;
  WavFile &operator=(const WavFile &) = delete;

  WavFile(WavFile &&) noexcept = default;
  WavFile &operator=(WavFile &&) noexcept = default;

  void write(const char sample[3]);

private:
  WavFile(std::ofstream stream, std::vector<char> stream_buffer)
      : stream_(std::move(stream)), stream_buffer_(std::move(stream_buffer)) {}

  std::ofstream stream_;
  std::size_t n_samples_{0};
  std::vector<char> stream_buffer_;

  struct __attribute__((packed)) WavFileHeader {
    char chunk_id[4]{'R', 'I', 'F', 'F'};
    std::int32_t chunk_size{0};
    char format[4]{'W', 'A', 'V', 'E'};
    char subchunk_id[4]{'f', 'm', 't', ' '};
    // PCM subchunk size
    std::int32_t subchunk_size{16};
    // Linear PCM (16-bit field in spec)
    std::int16_t audio_format{1};
    // Mono (16-bit field in spec)
    std::int16_t num_channels{1};
    // Sample rate (e.g., 16kHz)
    std::int32_t sample_rate{16000};
    // Byte rate: SampleRate * NumChannels * (BitsPerSample / 8)
    std::int32_t byte_rate{48000};
    // Block align: NumChannels * (BitsPerSample / 8)
    std::int16_t block_align{3};
    // Bits per sample
    std::int16_t bits_per_sample{24};
    // Data chunk ID
    char subchunk2_id[4]{'d', 'a', 't', 'a'};
    // Data chunk size (total payload size in bytes)
    std::int32_t subchunk2_size{0};
  };
};
}; // namespace vigo::audio