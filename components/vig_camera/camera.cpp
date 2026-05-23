#include "camera.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>

// ESP Hardware Drivers
extern "C" {
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_ldo_regulator.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_timer.h"
#include "ov5647.h"
}

static const char *TAG = "Camera";

namespace vig::camera {

Expected<void> MockCamera::init() {
  ESP_LOGI(TAG, "Initializing Mock Camera...");
  return {};
}

Expected<CameraFrame> MockCamera::capture() {
  CameraFrame frame;
  frame.width = this->getWidth();
  frame.height = this->getHeight();

  size_t line_bytes = frame.width + (frame.width / 2);
  frame.data.resize(line_bytes * frame.height);

  static uint8_t offset = 0;
  offset += 2;

  for (int y = 0; y < frame.height; ++y) {
    uint8_t *line_ptr = &frame.data[y * line_bytes];
    uint8_t chroma_val = 128;

    for (int x = 0; x < frame.width / 2; ++x) {
      line_ptr[x * 3 + 0] = chroma_val;
      line_ptr[x * 3 + 1] = static_cast<uint8_t>((x * 2 + y + offset) % 256);
      line_ptr[x * 3 + 2] = static_cast<uint8_t>((x * 2 + 1 + y + offset) % 256);
    }
  }

  return frame;
}

struct HardwareCamera::Impl {
  esp_cam_sensor_xclk_handle_t xclk_handle{nullptr};
  esp_cam_ctlr_handle_t csi_handle{nullptr};
  isp_proc_handle_t isp_handle{nullptr};
  esp_ldo_channel_handle_t ldo_handle{nullptr};
  i2c_master_bus_handle_t i2c_bus{nullptr};
  esp_sccb_io_handle_t sccb_handle{nullptr};
  esp_cam_sensor_device_t *camera_handle{nullptr};

  static constexpr int N_BUFFERS{3};
  void *csi_buffers[N_BUFFERS]{nullptr};
  size_t csi_buffer_len{0};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t stride{0};

  QueueHandle_t frame_ready_que{nullptr};
  bool has_last_transaction{false};
  esp_cam_ctlr_trans_t last_trans{};

  ~Impl() {
    if (csi_handle) {
      esp_cam_ctlr_stop(csi_handle);
      esp_cam_ctlr_del(csi_handle);
    }
    if (isp_handle) {
      esp_isp_disable(isp_handle);
      esp_isp_del_processor(isp_handle);
    }
    if (frame_ready_que) {
      vQueueDelete(frame_ready_que);
    }
    if (xclk_handle) {
      esp_cam_sensor_xclk_stop(xclk_handle);
      esp_cam_sensor_xclk_free(xclk_handle);
    }
    if (camera_handle) {
      esp_cam_sensor_del_dev(camera_handle);
    }
    if (ldo_handle) {
      esp_ldo_release_channel(ldo_handle);
    }
    if (i2c_bus) {
      i2c_del_master_bus(i2c_bus);
    }
  }
};

// IRAM-safe callback for camera transactions
IRAM_ATTR static bool camera_trans_finished_cb(esp_cam_ctlr_handle_t handle,
                                               esp_cam_ctlr_trans_t *trans,
                                               void *user_data) {
  BaseType_t high_task_woken = pdFALSE;
  QueueHandle_t q_handle = static_cast<QueueHandle_t>(user_data);
  if (xQueueSendFromISR(q_handle, trans, &high_task_woken) != pdTRUE) {
    // Queue full
    // TODO : monitor dropped frames
  }
  return high_task_woken == pdTRUE;
}

HardwareCamera::HardwareCamera() {
  ESP_LOGI(TAG, "HardwareCamera constructor...");
  impl_ = std::make_unique<Impl>();
  ESP_LOGI(TAG, "HardwareCamera constructor done.");
}
HardwareCamera::~HardwareCamera() = default;

Expected<void> HardwareCamera::init() {
  printf("HardwareCamera::init() starting...\n");
  ESP_LOGI(TAG, "Initializing Hardware Camera (OV5647 via MIPI-CSI)...");

  // Power on Camera Rail (2.7V) and MIPI PHY (1.1V)
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = 3;
  ldo_cfg.voltage_mv = 2700;
  if (esp_ldo_acquire_channel(&ldo_cfg, &impl_->ldo_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to acquire LDO channel for camera");
    return std::unexpected(DeviceError::CameraLdoFailed);
  }

  esp_ldo_channel_config_t ldo_phy_cfg = {};
  ldo_phy_cfg.chan_id = 1; // MIPI PHY on LDO 1
  ldo_phy_cfg.voltage_mv = 1100;
  esp_ldo_channel_handle_t phy_ldo_handle;
  if (esp_ldo_acquire_channel(&ldo_phy_cfg, &phy_ldo_handle) != ESP_OK) {
    ESP_LOGW(TAG,
             "Failed to acquire LDO channel for MIPI PHY (might be already active)");
  }

  // Provide External Clock (XCLK)
  if (esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER,
                                   &impl_->xclk_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to allocate XCLK");
    return std::unexpected(DeviceError::CameraInitFailed);
  }
  esp_cam_sensor_xclk_config_t xclk_cfg = {};
  xclk_cfg.esp_clock_router_cfg.xclk_pin = GPIO_NUM_13;
  xclk_cfg.esp_clock_router_cfg.xclk_freq_hz = 24000000;
  if (esp_cam_sensor_xclk_start(impl_->xclk_handle, &xclk_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start XCLK");
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  // Init I2C for SCCB
  i2c_master_bus_config_t i2c_bus_cfg = {};
  i2c_bus_cfg.i2c_port = I2C_NUM_0;
  i2c_bus_cfg.sda_io_num = GPIO_NUM_7;
  i2c_bus_cfg.scl_io_num = GPIO_NUM_8;
  i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_bus_cfg.glitch_ignore_cnt = 7;
  if (i2c_new_master_bus(&i2c_bus_cfg, &impl_->i2c_bus) != ESP_OK) {
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  sccb_i2c_config_t sccb_cfg = {};
  sccb_cfg.device_address = OV5647_SCCB_ADDR;
  sccb_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  sccb_cfg.scl_speed_hz = 100000;
  sccb_cfg.addr_bits_width = 16;
  sccb_cfg.val_bits_width = 8;

  if (sccb_new_i2c_io(impl_->i2c_bus, &sccb_cfg, &impl_->sccb_handle) != ESP_OK) {
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  // Detect Camera Sensor
  esp_cam_sensor_config_t sensor_cfg = {};
  sensor_cfg.sccb_handle = impl_->sccb_handle;
  sensor_cfg.reset_pin = GPIO_NUM_NC;
  sensor_cfg.pwdn_pin = GPIO_NUM_NC;
  sensor_cfg.sensor_port = ESP_CAM_SENSOR_MIPI_CSI;

  impl_->camera_handle = ov5647_detect(&sensor_cfg);
  if (!impl_->camera_handle) {
    ESP_LOGE(TAG, "OV5647 sensor not detected");
    return std::unexpected(DeviceError::CameraInitFailed);
  }
  ESP_LOGI(TAG, "Detected sensor: %s", esp_cam_sensor_get_name(impl_->camera_handle));

  // Set camera format to default
  if (esp_cam_sensor_set_format(impl_->camera_handle, NULL) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set camera format");
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  esp_cam_sensor_format_t sensor_fmt = {};
  esp_cam_sensor_get_format(impl_->camera_handle, &sensor_fmt);
  ESP_LOGI(TAG, "Sensor format set to: %s (%dx%d)", sensor_fmt.name,
           (int)sensor_fmt.width, (int)sensor_fmt.height);

  impl_->width = sensor_fmt.width;
  impl_->height = sensor_fmt.height;
  impl_->stride = impl_->width;

  // Setup ISP Processor
  color_raw_element_order_t bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR;
  if (sensor_fmt.isp_info) {
    switch (sensor_fmt.isp_info->isp_v1_info.bayer_type) {
    case ESP_CAM_SENSOR_BAYER_RGGB:
      bayer_order = COLOR_RAW_ELEMENT_ORDER_RGGB;
      break;
    case ESP_CAM_SENSOR_BAYER_GRBG:
      bayer_order = COLOR_RAW_ELEMENT_ORDER_GRBG;
      break;
    case ESP_CAM_SENSOR_BAYER_GBRG:
      bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG;
      break;
    case ESP_CAM_SENSOR_BAYER_BGGR:
      bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR;
      break;
    default:
      break;
    }
  }

  esp_isp_processor_cfg_t isp_cfg = {};
  isp_cfg.clk_hz = 120000000;
  isp_cfg.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
  isp_cfg.input_data_color_type = (sensor_fmt.format == ESP_CAM_SENSOR_PIXFORMAT_RAW10)
                                      ? ISP_COLOR_RAW10
                                      : ISP_COLOR_RAW8;
  isp_cfg.output_data_color_type = ISP_COLOR_YUV420;
  isp_cfg.h_res = impl_->width;
  isp_cfg.v_res = impl_->height;
  isp_cfg.bayer_order = bayer_order;

  if (esp_isp_new_processor(&isp_cfg, &impl_->isp_handle) != ESP_OK) {
    return std::unexpected(DeviceError::CameraIspFailed);
  }

  // Setup CSI Controller (Using sensor native resolution)
  esp_cam_ctlr_csi_config_t csi_config = {};
  csi_config.ctlr_id = 0;
  csi_config.h_res = impl_->width;
  csi_config.v_res = impl_->height;
  csi_config.data_lane_num = 2;
  csi_config.lane_bit_rate_mbps = 400;

  csi_config.input_data_color_type = CAM_CTLR_COLOR_YUV420;
  csi_config.output_data_color_type = CAM_CTLR_COLOR_YUV420;
  csi_config.queue_items = 5;

  if (esp_cam_new_csi_ctlr(&csi_config, &impl_->csi_handle) != ESP_OK) {
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  impl_->frame_ready_que = xQueueCreate(Impl::N_BUFFERS, sizeof(esp_cam_ctlr_trans_t));

  esp_cam_ctlr_evt_cbs_t cbs = {.on_get_new_trans = nullptr,
                                .on_trans_finished = camera_trans_finished_cb};
  if (esp_cam_ctlr_register_event_callbacks(impl_->csi_handle, &cbs,
                                            impl_->frame_ready_que) != ESP_OK) {
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  // Calculate buffer length (RAW8 = 1 byte per pixel)
  impl_->csi_buffer_len = impl_->stride * impl_->height * 3 / 2;
  for (int i = 0; i < Impl::N_BUFFERS; ++i) {
    impl_->csi_buffers[i] = esp_cam_ctlr_alloc_buffer(
        impl_->csi_handle, impl_->csi_buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!impl_->csi_buffers[i]) {
      return std::unexpected(DeviceError::CameraInitFailed);
    }
  }

  if (esp_isp_enable(impl_->isp_handle) != ESP_OK) {
    return std::unexpected(DeviceError::CameraIspFailed);
  }

  if (esp_cam_ctlr_enable(impl_->csi_handle) != ESP_OK ||
      esp_cam_ctlr_start(impl_->csi_handle) != ESP_OK) {
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  // Queue Initial Buffers
  for (int i = 0; i < Impl::N_BUFFERS; ++i) {
    esp_cam_ctlr_trans_t trans = {};
    trans.buffer = impl_->csi_buffers[i];
    trans.buflen = impl_->csi_buffer_len;
    // Pre-sync cache: Invalidate before DMA receives data
    esp_cache_msync(trans.buffer, trans.buflen, ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cam_ctlr_receive(impl_->csi_handle, &trans, 0);
  }

  int enable = 1;
  if (esp_cam_sensor_ioctl(impl_->camera_handle, ESP_CAM_SENSOR_IOC_S_STREAM,
                           &enable) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start sensor stream");
    return std::unexpected(DeviceError::CameraInitFailed);
  }

  ESP_LOGI(TAG, "Hardware Camera started successfully. (CSI+ISP+Sensor)");
  return {};
}

Expected<CameraFrame> HardwareCamera::capture() {
  // Recycle the last transaction buffer (from the previous frame capture) back
  // to the CSI controller. This allows the camera/CSI/ISP to overwrite this
  // buffer only after the host has finished encoding it!
  if (impl_->has_last_transaction) {
    esp_cache_msync(impl_->last_trans.buffer, impl_->last_trans.buflen,
                    ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    esp_cam_ctlr_receive(impl_->csi_handle, &impl_->last_trans, 0);
    impl_->has_last_transaction = false;
  }

  // Fetch the newly captured frame transaction from the frame queue
  esp_cam_ctlr_trans_t trans = {};
  if (xQueueReceive(impl_->frame_ready_que, &trans, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return std::unexpected(DeviceError::CameraCaptureFailed);
  }

  // Invalidate cache to ensure CPU reads fresh data directly from physical
  // PSRAM DMA buffer
  esp_cache_msync(trans.buffer, trans.buflen, ESP_CACHE_MSYNC_FLAG_INVALIDATE);

  CameraFrame frame;
  frame.width = impl_->width;
  frame.height = impl_->height;

  // Diagnostics: Print average values to detect if buffer is all zeroes
  static uint32_t capture_count = 0;
  capture_count++;
  if (capture_count % 150 == 0) {
    uint64_t y_sum = 0, u_sum = 0, v_sum = 0;
    uint32_t y_cnt = 0, u_cnt = 0, v_cnt = 0;
    const uint8_t *p = static_cast<const uint8_t *>(trans.buffer);
    size_t total_bytes = trans.received_size;
    size_t line_bytes = frame.width * 3 / 2;

    for (size_t y = 0; y < frame.height && (y * line_bytes + 1000) < total_bytes; ++y) {
      const uint8_t *line = p + (y * line_bytes);
      bool is_odd = (y % 2 == 0);
      for (size_t x = 0; x < 200; ++x) {
        if (is_odd) {
          u_sum += line[x * 3 + 0];
          u_cnt++;
        } else {
          v_sum += line[x * 3 + 0];
          v_cnt++;
        }
        y_sum += line[x * 3 + 1] + line[x * 3 + 2];
        y_cnt += 2;
      }
    }
    ESP_LOGI("CameraManager", "Frame %lu: rec_sz = %u, Avg Y=%d, U=%d, V=%d",
             (unsigned long)capture_count, (unsigned int)trans.received_size,
             (int)(y_sum / y_cnt), (int)(u_sum / u_cnt), (int)(v_sum / v_cnt));
  }

  // Pure Zero-Copy: Wrap the DMA buffer address in our CameraFrameBuffer
  // without copy!
  frame.data.set_external_buffer(static_cast<const uint8_t *>(trans.buffer),
                                 trans.received_size);

  // Store current transaction as last_trans to recycle it on the NEXT call to
  // capture()
  impl_->last_trans = trans;
  impl_->has_last_transaction = true;

  return frame;
}

uint32_t HardwareCamera::getWidth() const { return impl_->width; }
uint32_t HardwareCamera::getHeight() const { return impl_->height; }

} // namespace vig::camera
