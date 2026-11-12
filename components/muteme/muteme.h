#pragma once

#include <atomic>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace esphome {
namespace muteme {

// LED byte = colour | mode
enum MuteMeColor : uint8_t {
  COLOR_OFF = 0x00,
  COLOR_RED = 0x01,
  COLOR_GREEN = 0x02,
  COLOR_YELLOW = 0x03,
  COLOR_BLUE = 0x04,
  COLOR_PURPLE = 0x05,
  COLOR_CYAN = 0x06,
  COLOR_WHITE = 0x07,
};

enum MuteMeMode : uint8_t {
  MODE_SOLID = 0x00,
  MODE_DIM = 0x10,
  MODE_FAST_PULSE = 0x20,
  MODE_SLOW_PULSE = 0x30,
};

// Last byte of the button's input report
enum MuteMeTouch : uint8_t {
  TOUCH_NONE = 0x00,
  TOUCH_HOLDING = 0x01,
  TOUCH_END = 0x02,
  TOUCH_START = 0x04,
};

struct RawReport {
  uint8_t len;
  uint8_t data[16];
};

class MuteMeComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Thread-safe: queues the LED value for the USB task to send.
  void set_led(uint8_t color, uint8_t mode);
  void set_led(const std::string &color, const std::string &mode);

  void set_feature_report(bool feature) { this->use_feature_report_ = feature; }
  void set_log_raw(bool log_raw) { this->log_raw_ = log_raw; }

  bool is_connected() const { return this->connected_.load(); }
  bool is_touching() const { return this->touching_; }

  void add_on_touch_start_callback(std::function<void()> &&cb) { this->touch_start_cb_.add(std::move(cb)); }
  void add_on_touch_end_callback(std::function<void()> &&cb) { this->touch_end_cb_.add(std::move(cb)); }
  void add_on_connected_callback(std::function<void()> &&cb) { this->connected_cb_.add(std::move(cb)); }
  void add_on_disconnected_callback(std::function<void()> &&cb) { this->disconnected_cb_.add(std::move(cb)); }

 protected:
  static void host_task_(void *arg);
  static void client_task_(void *arg);
  static void client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg);
  static void in_transfer_cb_(usb_transfer_t *transfer);
  static void ctrl_transfer_cb_(usb_transfer_t *transfer);

  // Run on the USB client task only.
  void open_device_(uint8_t address);
  void close_device_();
  void send_led_(uint8_t value);

  void handle_touch_byte_(uint8_t value);
  void start_touch_();
  void end_touch_();

  // Config
  bool use_feature_report_{true};
  bool log_raw_{true};

  // USB state (client task)
  usb_host_client_handle_t client_{nullptr};
  usb_device_handle_t dev_{nullptr};
  usb_transfer_t *in_xfer_{nullptr};
  usb_transfer_t *ctrl_xfer_{nullptr};
  int iface_{-1};
  uint8_t ep_in_{0};
  uint16_t ep_in_mps_{0};
  uint16_t vid_{0};
  uint16_t pid_{0};
  std::atomic<bool> in_busy_{false};
  std::atomic<bool> ctrl_busy_{false};
  std::atomic<int> pending_open_addr_{-1};
  std::atomic<bool> closing_{false};

  // Shared between tasks
  std::atomic<bool> connected_{false};
  std::atomic<uint16_t> pending_led_{0xFFFF};  // 0xFFFF = nothing queued
  std::atomic<uint8_t> current_led_{0x00};
  QueueHandle_t report_queue_{nullptr};

  // Main loop state
  bool last_connected_{false};
  bool touching_{false};
  uint32_t last_touch_ms_{0};
  uint16_t last_vid_{0};
  uint16_t last_pid_{0};

  CallbackManager<void()> touch_start_cb_;
  CallbackManager<void()> touch_end_cb_;
  CallbackManager<void()> connected_cb_;
  CallbackManager<void()> disconnected_cb_;
};

class TouchStartTrigger : public Trigger<> {
 public:
  explicit TouchStartTrigger(MuteMeComponent *parent) {
    parent->add_on_touch_start_callback([this]() { this->trigger(); });
  }
};

class TouchEndTrigger : public Trigger<> {
 public:
  explicit TouchEndTrigger(MuteMeComponent *parent) {
    parent->add_on_touch_end_callback([this]() { this->trigger(); });
  }
};

class ConnectedTrigger : public Trigger<> {
 public:
  explicit ConnectedTrigger(MuteMeComponent *parent) {
    parent->add_on_connected_callback([this]() { this->trigger(); });
  }
};

class DisconnectedTrigger : public Trigger<> {
 public:
  explicit DisconnectedTrigger(MuteMeComponent *parent) {
    parent->add_on_disconnected_callback([this]() { this->trigger(); });
  }
};

}  // namespace muteme
}  // namespace esphome
