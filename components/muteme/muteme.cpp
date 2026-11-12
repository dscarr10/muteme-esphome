#include "muteme.h"

#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace muteme {

static const char *const TAG = "muteme";

static const uint32_t TOUCH_TIMEOUT_MS = 1500;  // no "holding" report for this long = released
static const uint8_t HID_SET_REPORT = 0x09;
static const uint8_t HID_REPORT_TYPE_OUTPUT = 0x02;
static const uint8_t HID_REPORT_TYPE_FEATURE = 0x03;

// ---------------------------------------------------------------- setup / loop

void MuteMeComponent::setup() {
  this->report_queue_ = xQueueCreate(16, sizeof(RawReport));

  usb_host_config_t host_config = {};
  host_config.skip_phy_setup = false;
  host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  xTaskCreatePinnedToCore(MuteMeComponent::host_task_, "usb_host", 4096, this, 5, nullptr, 0);
  xTaskCreatePinnedToCore(MuteMeComponent::client_task_, "muteme_usb", 4096, this, 5, nullptr, 0);
}

void MuteMeComponent::loop() {
  // Connection changes
  bool connected = this->connected_.load();
  if (connected != this->last_connected_) {
    this->last_connected_ = connected;
    if (connected) {
      ESP_LOGI(TAG, "MuteMe connected (VID 0x%04X, PID 0x%04X)", this->vid_, this->pid_);
      this->connected_cb_.call();
    } else {
      ESP_LOGW(TAG, "MuteMe disconnected");
      if (this->touching_)
        this->end_touch_();
      this->disconnected_cb_.call();
    }
  }

  // Input reports from the USB task
  RawReport rep;
  while (this->report_queue_ != nullptr && xQueueReceive(this->report_queue_, &rep, 0) == pdTRUE) {
    if (rep.len == 0)
      continue;
    if (this->log_raw_) {
      ESP_LOGD(TAG, "Report (%u bytes): %s", rep.len, format_hex_pretty(rep.data, rep.len).c_str());
    }
    uint8_t value = rep.len >= 4 ? rep.data[3] : rep.data[rep.len - 1];
    this->handle_touch_byte_(value);
  }

  // Safety net in case a release report is missed
  if (this->touching_ && millis() - this->last_touch_ms_ > TOUCH_TIMEOUT_MS) {
    ESP_LOGD(TAG, "Touch timed out, treating as released");
    this->end_touch_();
  }
}

void MuteMeComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "MuteMe (USB host):");
  ESP_LOGCONFIG(TAG, "  LED report type: %s", this->use_feature_report_ ? "feature" : "output");
  ESP_LOGCONFIG(TAG, "  Log raw reports: %s", YESNO(this->log_raw_));
  ESP_LOGCONFIG(TAG, "  Connected: %s", YESNO(this->connected_.load()));
}

// ---------------------------------------------------------------- touch handling

void MuteMeComponent::handle_touch_byte_(uint8_t value) {
  switch (value) {
    case TOUCH_START:
    case TOUCH_HOLDING:
      this->last_touch_ms_ = millis();
      if (!this->touching_)
        this->start_touch_();
      break;
    case TOUCH_END:
      if (this->touching_)
        this->end_touch_();
      break;
    case TOUCH_NONE:
      break;
    default:
      ESP_LOGW(TAG, "Unknown touch value 0x%02X (turn on log_raw_reports and send me the log)", value);
      break;
  }
}

void MuteMeComponent::start_touch_() {
  this->touching_ = true;
  ESP_LOGD(TAG, "Touch start");
  this->touch_start_cb_.call();
}

void MuteMeComponent::end_touch_() {
  this->touching_ = false;
  ESP_LOGD(TAG, "Touch end");
  this->touch_end_cb_.call();
}

// ---------------------------------------------------------------- LED

void MuteMeComponent::set_led(uint8_t color, uint8_t mode) {
  uint8_t value = (color & 0x0F) | (mode & 0xF0);
  if (color == COLOR_OFF)
    value = 0x00;
  this->current_led_.store(value);
  this->pending_led_.store(value);
}

void MuteMeComponent::set_led(const std::string &color, const std::string &mode) {
  std::string c = str_lower_case(color);
  std::string m = str_lower_case(mode);

  uint8_t cv = COLOR_OFF;
  if (c == "red")
    cv = COLOR_RED;
  else if (c == "green")
    cv = COLOR_GREEN;
  else if (c == "yellow")
    cv = COLOR_YELLOW;
  else if (c == "blue")
    cv = COLOR_BLUE;
  else if (c == "purple" || c == "magenta")
    cv = COLOR_PURPLE;
  else if (c == "cyan")
    cv = COLOR_CYAN;
  else if (c == "white")
    cv = COLOR_WHITE;
  else if (c != "off")
    ESP_LOGW(TAG, "Unknown colour '%s', using off", color.c_str());

  uint8_t mv = MODE_SOLID;
  if (m == "dim")
    mv = MODE_DIM;
  else if (m == "fast_pulse" || m == "fast pulse")
    mv = MODE_FAST_PULSE;
  else if (m == "slow_pulse" || m == "slow pulse")
    mv = MODE_SLOW_PULSE;
  else if (m != "solid" && !m.empty())
    ESP_LOGW(TAG, "Unknown mode '%s', using solid", mode.c_str());

  ESP_LOGD(TAG, "Set LED: %s / %s", c.c_str(), m.c_str());
  this->set_led(cv, mv);
}

// ---------------------------------------------------------------- USB tasks

void MuteMeComponent::host_task_(void *arg) {
  while (true) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

void MuteMeComponent::client_task_(void *arg) {
  auto *self = static_cast<MuteMeComponent *>(arg);

  usb_host_client_config_t client_config = {};
  client_config.is_synchronous = false;
  client_config.max_num_event_msg = 5;
  client_config.async.client_event_callback = MuteMeComponent::client_event_cb_;
  client_config.async.callback_arg = self;
  esp_err_t err = usb_host_client_register(&client_config, &self->client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    usb_host_client_handle_events(self->client_, pdMS_TO_TICKS(50));

    int addr = self->pending_open_addr_.exchange(-1);
    if (addr >= 0 && self->dev_ == nullptr)
      self->open_device_(static_cast<uint8_t>(addr));

    if (self->closing_.load() && !self->in_busy_.load() && !self->ctrl_busy_.load())
      self->close_device_();

    if (self->connected_.load() && !self->closing_.load() && !self->ctrl_busy_.load()) {
      uint16_t led = self->pending_led_.exchange(0xFFFF);
      if (led != 0xFFFF)
        self->send_led_(static_cast<uint8_t>(led));
    }
  }
}

void MuteMeComponent::client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<MuteMeComponent *>(arg);
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      self->pending_open_addr_.store(msg->new_dev.address);
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      if (self->dev_ != nullptr && msg->dev_gone.dev_hdl == self->dev_) {
        self->connected_.store(false);
        self->closing_.store(true);
      }
      break;
    default:
      break;
  }
}

void MuteMeComponent::open_device_(uint8_t address) {
  esp_err_t err = usb_host_device_open(this->client_, address, &this->dev_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't open USB device: %s", esp_err_to_name(err));
    this->dev_ = nullptr;
    return;
  }

  const usb_device_desc_t *dev_desc;
  usb_host_get_device_descriptor(this->dev_, &dev_desc);
  this->vid_ = dev_desc->idVendor;
  this->pid_ = dev_desc->idProduct;

  const usb_config_desc_t *cfg;
  if (usb_host_get_active_config_descriptor(this->dev_, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't read config descriptor");
    usb_host_device_close(this->client_, this->dev_);
    this->dev_ = nullptr;
    return;
  }

  // Find the first HID interface and its interrupt IN endpoint.
  const uint8_t *p = reinterpret_cast<const uint8_t *>(cfg);
  uint16_t total = cfg->wTotalLength;
  int iface = -1;
  bool in_hid = false;
  uint8_t ep_in = 0;
  uint16_t mps = 0;
  for (uint16_t i = 0; i + 1 < total;) {
    uint8_t len = p[i];
    uint8_t type = p[i + 1];
    if (len == 0)
      break;
    if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      auto *id = reinterpret_cast<const usb_intf_desc_t *>(p + i);
      if (id->bInterfaceClass == USB_CLASS_HID && iface < 0) {
        iface = id->bInterfaceNumber;
        in_hid = true;
      } else {
        in_hid = false;
      }
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hid && ep_in == 0) {
      auto *ed = reinterpret_cast<const usb_ep_desc_t *>(p + i);
      if ((ed->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_INT && (ed->bEndpointAddress & 0x80)) {
        ep_in = ed->bEndpointAddress;
        mps = ed->wMaxPacketSize & 0x07FF;
      }
    }
    i += len;
  }

  if (iface < 0 || ep_in == 0) {
    ESP_LOGW(TAG, "Device 0x%04X:0x%04X has no HID interrupt-IN endpoint; ignoring", this->vid_, this->pid_);
    usb_host_device_close(this->client_, this->dev_);
    this->dev_ = nullptr;
    return;
  }

  err = usb_host_interface_claim(this->client_, this->dev_, iface, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't claim interface %d: %s", iface, esp_err_to_name(err));
    usb_host_device_close(this->client_, this->dev_);
    this->dev_ = nullptr;
    return;
  }

  this->iface_ = iface;
  this->ep_in_ = ep_in;
  this->ep_in_mps_ = mps == 0 ? 8 : mps;

  usb_host_transfer_alloc(this->ep_in_mps_, 0, &this->in_xfer_);
  this->in_xfer_->device_handle = this->dev_;
  this->in_xfer_->bEndpointAddress = this->ep_in_;
  this->in_xfer_->callback = MuteMeComponent::in_transfer_cb_;
  this->in_xfer_->context = this;
  this->in_xfer_->num_bytes = this->ep_in_mps_;

  usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &this->ctrl_xfer_);
  this->ctrl_xfer_->device_handle = this->dev_;
  this->ctrl_xfer_->bEndpointAddress = 0;
  this->ctrl_xfer_->callback = MuteMeComponent::ctrl_transfer_cb_;
  this->ctrl_xfer_->context = this;
  this->ctrl_xfer_->timeout_ms = 1000;

  this->closing_.store(false);
  this->connected_.store(true);

  this->in_busy_.store(true);
  if (usb_host_transfer_submit(this->in_xfer_) != ESP_OK) {
    this->in_busy_.store(false);
    ESP_LOGE(TAG, "Couldn't start reading button reports");
  }

  // Restore the last LED state after a (re)connect.
  this->pending_led_.store(this->current_led_.load());
}

void MuteMeComponent::close_device_() {
  if (this->dev_ != nullptr) {
    if (this->iface_ >= 0) {
      usb_host_endpoint_halt(this->dev_, this->ep_in_);
      usb_host_endpoint_flush(this->dev_, this->ep_in_);
      usb_host_endpoint_clear(this->dev_, this->ep_in_);
      usb_host_interface_release(this->client_, this->dev_, this->iface_);
    }
    usb_host_device_close(this->client_, this->dev_);
  }
  if (this->in_xfer_ != nullptr) {
    usb_host_transfer_free(this->in_xfer_);
    this->in_xfer_ = nullptr;
  }
  if (this->ctrl_xfer_ != nullptr) {
    usb_host_transfer_free(this->ctrl_xfer_);
    this->ctrl_xfer_ = nullptr;
  }
  this->dev_ = nullptr;
  this->iface_ = -1;
  this->ep_in_ = 0;
  this->closing_.store(false);
}

void MuteMeComponent::send_led_(uint8_t value) {
  if (this->ctrl_xfer_ == nullptr)
    return;

  // Mirrors what the MuteMe app sends through hidapi: report ID = the LED byte, payload = the LED byte.
  // "Off" is report ID 0 with a single 0x00 payload byte.
  auto *setup = reinterpret_cast<usb_setup_packet_t *>(this->ctrl_xfer_->data_buffer);
  setup->bmRequestType =
      USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup->bRequest = HID_SET_REPORT;
  uint8_t report_type = this->use_feature_report_ ? HID_REPORT_TYPE_FEATURE : HID_REPORT_TYPE_OUTPUT;
  setup->wValue = (static_cast<uint16_t>(report_type) << 8) | value;
  setup->wIndex = static_cast<uint16_t>(this->iface_);
  setup->wLength = 1;
  this->ctrl_xfer_->data_buffer[sizeof(usb_setup_packet_t)] = value;
  this->ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + 1;

  this->ctrl_busy_.store(true);
  esp_err_t err = usb_host_transfer_submit_control(this->client_, this->ctrl_xfer_);
  if (err != ESP_OK) {
    this->ctrl_busy_.store(false);
    ESP_LOGW(TAG, "LED write failed to start: %s", esp_err_to_name(err));
  }
}

void MuteMeComponent::in_transfer_cb_(usb_transfer_t *transfer) {
  auto *self = static_cast<MuteMeComponent *>(transfer->context);

  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
    RawReport rep;
    size_t n = static_cast<size_t>(transfer->actual_num_bytes);
    rep.len = static_cast<uint8_t>(n > sizeof(rep.data) ? sizeof(rep.data) : n);
    memcpy(rep.data, transfer->data_buffer, rep.len);
    xQueueSend(self->report_queue_, &rep, 0);
  }

  bool keep_going = self->connected_.load() && !self->closing_.load() &&
                    transfer->status != USB_TRANSFER_STATUS_NO_DEVICE &&
                    transfer->status != USB_TRANSFER_STATUS_CANCELED;
  if (keep_going && usb_host_transfer_submit(transfer) == ESP_OK)
    return;
  self->in_busy_.store(false);
}

void MuteMeComponent::ctrl_transfer_cb_(usb_transfer_t *transfer) {
  auto *self = static_cast<MuteMeComponent *>(transfer->context);
  if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    ESP_LOGW(TAG, "LED write status %d (if the LED never changes, try report_type: output)",
             static_cast<int>(transfer->status));
  }
  self->ctrl_busy_.store(false);
}

}  // namespace muteme
}  // namespace esphome
