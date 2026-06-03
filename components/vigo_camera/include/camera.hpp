#ifndef VIGO_CAMERA_CAMERA_HPP
#define VIGO_CAMERA_CAMERA_HPP

#include "error_types.hpp"
#include "memory.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace vigo::camera {

/**
 * @class CameraFrameBuffer
 * @brief A zero-allocation, high-performance container for camera frame data.
 *
 * CameraFrameBuffer is designed specifically for resource-constrained systems
 * to handle large chunks of real-time YUV420 frame data without triggering
 * heap fragmentation or CPU cycles on dynamic allocations.
 *
 * It supports two modes of operation:
 * 1. **Internal Pool Mode (Owning)**: Reuses a single, static, globally shared
 * pool pre-allocated in PSRAM (using 64-byte alignment to support P4 cache sync
 * / DMA transfers). This mode is used when generating mock frames or performing
 *    software-based data manipulation.
 * 2. **External Buffer Mode (Non-Owning)**: Acts as a zero-copy wrapper around
 *    memory owned externally (e.g., DMA frame buffers directly populated by
 *    MIPI-CSI/ISP drivers).
 *
 * Provides a standard STL-compatible container interface.
 */
class CameraFrameBuffer {
public:
  /// Maximum supported resolution for the internal PSRAM pool (Full HD).
  static constexpr size_t MAX_RESOLUTION_WIDTH = 1280;
  static constexpr size_t MAX_RESOLUTION_HEIGHT = 960;

  /// YUV420 format chroma subsampling numerator (exactly 1.5 bytes per pixel).
  static constexpr size_t YUV420_NUMERATOR = 3;
  /// YUV420 format chroma subsampling denominator.
  static constexpr size_t YUV420_DENOMINATOR = 2;

  /// Safety padding to accommodate ISP metadata headers and other constraints.
  static constexpr size_t SAFETY_MARGIN_BYTES = 1024;

  /// Compile-time constant representing the required size of the global buffer.
  static constexpr size_t kGlobalPoolSize =
      ((MAX_RESOLUTION_WIDTH * MAX_RESOLUTION_HEIGHT * YUV420_NUMERATOR) /
       YUV420_DENOMINATOR) +
      SAFETY_MARGIN_BYTES;

  /**
   * @brief Constructs an empty CameraFrameBuffer.
   * Initializes pointers to nullptr and flags to non-owning.
   */
  CameraFrameBuffer() {
    ptr_ = nullptr;
    size_ = 0;
    capacity_ = 0;
    is_external_ = false;
  }

  /**
   * @brief Default destructor.
   * @note Memory is not deallocated here. External buffers are owned by the
   * hardware driver, * and the internal pool is a static global vector
   * persistent for the life of the application.
   */
  ~CameraFrameBuffer() = default;

  /**
   * @brief Ensures that the globally shared static PSRAM pool is initialized.
   * Lazily allocates the static buffer using the 64-byte aligned PSRAM
   * allocator on its very first invocation.
   */
  void ensure_internal_pool() {
    if (!ptr_ && !is_external_) {
      static std::vector<uint8_t, memory::AlignedPsramAllocator<uint8_t>>
          global_psram_pool(kGlobalPoolSize);
      ptr_ = global_psram_pool.data();
      capacity_ = kGlobalPoolSize;
    }
  }

  /**
   * @brief Adjusts the logical size of the buffer.
   * @param new_size The new size in bytes.
   * @note If in internal pool mode, asserts that `new_size`does not exceed the
   * maximum pool capacity.
   */
  void resize(size_t new_size) {
    if (!is_external_) {
      ensure_internal_pool();
      assert(new_size <= capacity_);
    }
    size_ = new_size;
  }

  /**
   * @brief Copies data from a contiguous memory range into this buffer.
   * @param first Iterator/pointer to the start of the source data.
   * @param last Iterator/pointer to the end of the source data.
   * @note In internal pool mode, asserts that the copied range does not exceed
   * the maximum pool capacity.
   */
  void assign(const uint8_t *first, const uint8_t *last) {
    size_t len = last - first;
    if (is_external_) {
      std::memcpy(const_cast<uint8_t *>(ptr_), first, len);
    } else {
      ensure_internal_pool();
      assert(len <= capacity_);
      std::memcpy(ptr_, first, len);
    }
    size_ = len;
  }

  /**
   * @brief Maps this buffer as a zero-copy view of an external memory block.
   * Sets the buffer to non-owning (external) mode.
   * @param ptr Raw pointer to the external contiguous block.
   * @param size The size of the external block in bytes.
   */
  void set_external_buffer(const uint8_t *ptr, size_t size) {
    ptr_ = const_cast<uint8_t *>(ptr);
    size_ = size;
    capacity_ = size;
    is_external_ = true;
  }

  /**
   * @brief Returns a direct pointer to the underlying buffer memory.
   * @return Raw pointer to uint8_t array.
   */
  uint8_t *data() { return ptr_; }

  /**
   * @brief Returns a direct const pointer to the underlying buffer memory.
   * @return Const raw pointer to uint8_t array.
   */
  const uint8_t *data() const { return ptr_; }

  /**
   * @brief Gets the current logical size of the frame data inside the buffer.
   * @return Size in bytes.
   */
  size_t size() const { return size_; }

  /**
   * @brief Subscript operator to access individual bytes.
   * @param idx Index of the element to access.
   */
  uint8_t &operator[](size_t idx) { return ptr_[idx]; }

  /**
   * @brief Const subscript operator to access individual bytes.
   * @param idx Index of the element to access.
   */
  const uint8_t &operator[](size_t idx) const { return ptr_[idx]; }

  /**
   * @brief Returns an iterator to the beginning of the frame data.
   */
  uint8_t *begin() { return ptr_; }

  /**
   * @brief Returns a const iterator to the beginning of the frame data.
   */
  const uint8_t *begin() const { return ptr_; }

  /**
   * @brief Returns an iterator to the end of the frame data.
   */
  uint8_t *end() { return ptr_ + size_; }

  /**
   * @brief Returns a const iterator to the end of the frame data.
   */
  const uint8_t *end() const { return ptr_ + size_; }

private:
  uint8_t *ptr_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
  bool is_external_{false};
};

struct CameraFrame {
  CameraFrame() = default;
  CameraFrameBuffer data;
  size_t width{0};
  size_t height{0};
};

class CameraManager {
public:
  virtual ~CameraManager() = default;
  virtual Expected<void> init() = 0;
  virtual Expected<CameraFrame> capture() = 0;
  virtual uint32_t getWidth() const = 0;
  virtual uint32_t getHeight() const = 0;
};

class MockCamera : public CameraManager {
public:
  Expected<void> init() override;
  Expected<CameraFrame> capture() override;
  uint32_t getWidth() const override { return 800; }
  uint32_t getHeight() const override { return 640; }
};

class HardwareCamera : public CameraManager {
public:
  HardwareCamera();
  ~HardwareCamera() override;

  Expected<void> init() override;
  Expected<CameraFrame> capture() override;
  uint32_t getWidth() const override;
  uint32_t getHeight() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace vigo::camera

#endif // VIGO_CAMERA_CAMERA_HPP
