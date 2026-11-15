#include "muteme.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace muteme {

static const char *const TAG = "muteme";

static const uint32_t TOUCH_TIMEOUT_MS = 1500;  // no "holding" report for this long = released
static const uint8_t HID_GET_REPORT = 0x01;
static const uint8_t HID_SET_REPORT = 0x09;
static const uint8_t USB_GET_DESCRIPTOR = 0x06;
static const uint8_t HID_DESC_TYPE_HID = 0x21;
static const uint8_t HID_DESC_TYPE_REPORT = 0x22;
static const uint16_t MAX_REPORT_DESC = 448;  // fits the control buffer with room for rounding
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

  // HID probe results from the USB task
  if (this->desc_ready_.exchange(false))
    this->log_descriptor_();
  if (this->result_ready_.exchange(false))
    this->log_probe_result_();

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

    if (self->closing_.load() && !self->in_busy_.load() && !self->ctrl_busy_.load() && !self->out_busy_.load())
      self->close_device_();

    if (self->connected_.load() && !self->closing_.load()) {
      bool probe_is_write = self->probe_pending_.load() && self->probe_cmd_.kind == PROBE_WRITE;
      if (probe_is_write && !self->out_busy_.load()) {
        self->send_probe_();
      } else if (!self->ctrl_busy_.load()) {
        if (self->desc_pending_.exchange(false)) {
          self->request_descriptor_();
        } else if (self->probe_pending_.load() && !probe_is_write) {
          self->send_probe_();
        } else {
          uint16_t led = self->pending_led_.exchange(0xFFFF);
          if (led != 0xFFFF)
            self->send_led_(static_cast<uint8_t>(led));
        }
      }
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
  this->mps0_ = dev_desc->bMaxPacketSize0 == 0 ? 8 : dev_desc->bMaxPacketSize0;

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
  uint8_t ep_out = 0;
  uint16_t out_mps = 0;
  uint16_t report_desc_len = 0;
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
    } else if (type == HID_DESC_TYPE_HID && in_hid && len >= 9 && i + 8 < total) {
      // HID descriptor: the report descriptor's length is at bytes 7-8.
      if (p[i + 6] == HID_DESC_TYPE_REPORT)
        report_desc_len = p[i + 7] | (p[i + 8] << 8);
    } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_hid) {
      auto *ed = reinterpret_cast<const usb_ep_desc_t *>(p + i);
      if ((ed->bmAttributes & 0x03) == USB_BM_ATTRIBUTES_XFER_INT) {
        if ((ed->bEndpointAddress & 0x80) && ep_in == 0) {
          ep_in = ed->bEndpointAddress;
          mps = ed->wMaxPacketSize & 0x07FF;
        } else if (!(ed->bEndpointAddress & 0x80) && ep_out == 0) {
          ep_out = ed->bEndpointAddress;
          out_mps = ed->wMaxPacketSize & 0x07FF;
        }
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

  this->ep_out_ = ep_out;
  this->ep_out_mps_ = out_mps == 0 ? 8 : out_mps;
  this->hid_desc_len_ = report_desc_len;
  if (ep_out != 0) {
    usb_host_transfer_alloc(64, 0, &this->out_xfer_);
    this->out_xfer_->device_handle = this->dev_;
    this->out_xfer_->bEndpointAddress = this->ep_out_;
    this->out_xfer_->callback = MuteMeComponent::out_transfer_cb_;
    this->out_xfer_->context = this;
    this->out_xfer_->timeout_ms = 1000;
  }

  usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 512, 0, &this->ctrl_xfer_);
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

  // Read the HID report descriptor first, then restore the last LED state after a (re)connect.
  this->desc_pending_.store(true);
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
  if (this->out_xfer_ != nullptr) {
    usb_host_transfer_free(this->out_xfer_);
    this->out_xfer_ = nullptr;
  }
  this->dev_ = nullptr;
  this->iface_ = -1;
  this->ep_in_ = 0;
  this->ep_out_ = 0;
  this->desc_pending_.store(false);
  this->probe_pending_.store(false);
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

  this->ctrl_kind_.store(CTRL_LED);
  this->ctrl_busy_.store(true);
  esp_err_t err = usb_host_transfer_submit_control(this->client_, this->ctrl_xfer_);
  if (err != ESP_OK) {
    this->ctrl_busy_.store(false);
    ESP_LOGW(TAG, "LED write failed to start: %s", esp_err_to_name(err));
  }
}

// ---------------------------------------------------------------- HID probe (USB task side)

void MuteMeComponent::request_descriptor_() {
  if (this->ctrl_xfer_ == nullptr)
    return;
  uint16_t want = this->hid_desc_len_ == 0 ? 256 : this->hid_desc_len_;
  if (want > MAX_REPORT_DESC)
    want = MAX_REPORT_DESC;
  uint16_t rounded = ((want + this->mps0_ - 1) / this->mps0_) * this->mps0_;

  auto *setup = reinterpret_cast<usb_setup_packet_t *>(this->ctrl_xfer_->data_buffer);
  setup->bmRequestType =
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
  setup->bRequest = USB_GET_DESCRIPTOR;
  setup->wValue = static_cast<uint16_t>(HID_DESC_TYPE_REPORT) << 8;
  setup->wIndex = static_cast<uint16_t>(this->iface_);
  setup->wLength = want;
  this->ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + rounded;

  this->ctrl_kind_.store(CTRL_DESC);
  this->ctrl_busy_.store(true);
  if (usb_host_transfer_submit_control(this->client_, this->ctrl_xfer_) != ESP_OK)
    this->ctrl_busy_.store(false);
}

void MuteMeComponent::send_probe_() {
  const ProbeCmd &cmd = this->probe_cmd_;

  if (cmd.kind == PROBE_WRITE) {
    if (this->out_xfer_ == nullptr) {
      this->probe_pending_.store(false);
      return;
    }
    memcpy(this->out_xfer_->data_buffer, cmd.data, cmd.len);
    this->out_xfer_->num_bytes = cmd.len;
    this->out_busy_.store(true);
    this->probe_pending_.store(false);
    if (usb_host_transfer_submit(this->out_xfer_) != ESP_OK) {
      this->out_busy_.store(false);
      this->result_kind_ = PROBE_WRITE;
      this->result_status_ = -1;
      this->result_len_ = 0;
      this->result_ready_.store(true);
    }
    return;
  }

  if (this->ctrl_xfer_ == nullptr) {
    this->probe_pending_.store(false);
    return;
  }
  auto *setup = reinterpret_cast<usb_setup_packet_t *>(this->ctrl_xfer_->data_buffer);
  setup->wValue = (static_cast<uint16_t>(cmd.report_type) << 8) | cmd.report_id;
  setup->wIndex = static_cast<uint16_t>(this->iface_);
  setup->wLength = cmd.len;
  if (cmd.kind == PROBE_SET) {
    setup->bmRequestType =
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = HID_SET_REPORT;
    memcpy(this->ctrl_xfer_->data_buffer + sizeof(usb_setup_packet_t), cmd.data, cmd.len);
    this->ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + cmd.len;
  } else {
    setup->bmRequestType =
        USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = HID_GET_REPORT;
    uint16_t rounded = ((cmd.len + this->mps0_ - 1) / this->mps0_) * this->mps0_;
    this->ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + rounded;
  }

  this->result_kind_ = cmd.kind;
  this->ctrl_kind_.store(CTRL_PROBE);
  this->ctrl_busy_.store(true);
  this->probe_pending_.store(false);
  if (usb_host_transfer_submit_control(this->client_, this->ctrl_xfer_) != ESP_OK) {
    this->ctrl_busy_.store(false);
    this->result_status_ = -1;
    this->result_len_ = 0;
    this->result_ready_.store(true);
  }
}

void MuteMeComponent::out_transfer_cb_(usb_transfer_t *transfer) {
  auto *self = static_cast<MuteMeComponent *>(transfer->context);
  self->result_kind_ = PROBE_WRITE;
  self->result_status_ = static_cast<int>(transfer->status);
  self->result_len_ = 0;
  self->result_ready_.store(true);
  self->out_busy_.store(false);
}

// ---------------------------------------------------------------- HID probe (main loop side)

static bool parse_hex_bytes(const std::vector<std::string> &tok, size_t start, uint8_t *out, uint16_t *len) {
  *len = 0;
  for (size_t i = start; i < tok.size(); i++) {
    if (*len >= 64)
      return false;
    char *end = nullptr;
    unsigned long v = strtoul(tok[i].c_str(), &end, 16);
    if (end == tok[i].c_str() || *end != '\0' || v > 0xFF)
      return false;
    out[(*len)++] = static_cast<uint8_t>(v);
  }
  return *len > 0;
}

static uint8_t parse_report_type(const std::string &s) {
  if (s == "input" || s == "in")
    return 1;
  if (s == "output" || s == "out")
    return 2;
  if (s == "feature")
    return 3;
  return 0;
}

void MuteMeComponent::run_command(const std::string &command) {
  std::vector<std::string> tok;
  std::string cur;
  for (char c : str_lower_case(command)) {
    if (c == ' ' || c == ',' || c == '\t') {
      if (!cur.empty())
        tok.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    tok.push_back(cur);
  if (tok.empty())
    return;

  const char *usage = "probe: use 'descriptor', 'led <byte>', 'set <feature|output> <bytes>', "
                      "'get <feature|input|output> <id> <length>' or 'write <bytes>'";

  if (tok[0] == "descriptor") {
    this->dump_descriptor();
    return;
  }
  if (!this->connected_.load()) {
    ESP_LOGW(TAG, "probe: MuteMe isn't connected");
    return;
  }
  if (this->probe_pending_.load()) {
    ESP_LOGW(TAG, "probe: previous command still pending, try again");
    return;
  }

  ProbeCmd cmd;
  if (tok[0] == "led" && tok.size() == 2) {
    uint8_t b;
    uint16_t n;
    if (!parse_hex_bytes(tok, 1, &b, &n)) {
      ESP_LOGW(TAG, "%s", usage);
      return;
    }
    ESP_LOGI(TAG, "probe: raw LED byte 0x%02X", b);
    this->current_led_.store(b);
    this->pending_led_.store(b);
    return;
  } else if (tok[0] == "set" && tok.size() >= 3) {
    cmd.kind = PROBE_SET;
    cmd.report_type = parse_report_type(tok[1]);
    if (cmd.report_type < 2 || !parse_hex_bytes(tok, 2, cmd.data, &cmd.len)) {
      ESP_LOGW(TAG, "%s", usage);
      return;
    }
    cmd.report_id = cmd.data[0];
    ESP_LOGI(TAG, "probe: SET_REPORT %s, ID 0x%02X: %s", tok[1].c_str(), cmd.report_id,
             format_hex_pretty(cmd.data, cmd.len).c_str());
  } else if (tok[0] == "get" && tok.size() == 4) {
    cmd.kind = PROBE_GET;
    cmd.report_type = parse_report_type(tok[1]);
    char *end = nullptr;
    unsigned long id = strtoul(tok[2].c_str(), &end, 16);
    unsigned long n = strtoul(tok[3].c_str(), nullptr, 10);
    if (cmd.report_type == 0 || id > 0xFF || n == 0 || n > 64) {
      ESP_LOGW(TAG, "%s (length is decimal, 1-64)", usage);
      return;
    }
    cmd.report_id = static_cast<uint8_t>(id);
    cmd.len = static_cast<uint16_t>(n);
    ESP_LOGI(TAG, "probe: GET_REPORT %s, ID 0x%02X, %u bytes", tok[1].c_str(), cmd.report_id, cmd.len);
  } else if (tok[0] == "write" && tok.size() >= 2) {
    if (this->ep_out_ == 0) {
      ESP_LOGW(TAG, "probe: this MuteMe has no interrupt OUT endpoint, so 'write' can't be used");
      return;
    }
    cmd.kind = PROBE_WRITE;
    if (!parse_hex_bytes(tok, 1, cmd.data, &cmd.len)) {
      ESP_LOGW(TAG, "%s", usage);
      return;
    }
    ESP_LOGI(TAG, "probe: interrupt OUT write: %s", format_hex_pretty(cmd.data, cmd.len).c_str());
  } else {
    ESP_LOGW(TAG, "%s", usage);
    return;
  }

  this->probe_cmd_ = cmd;
  this->probe_pending_.store(true);
}

void MuteMeComponent::dump_descriptor() {
  if (this->desc_len_ == 0) {
    ESP_LOGW(TAG, "HID report descriptor hasn't been read (is the MuteMe connected?)");
    return;
  }
  this->log_descriptor_();
}

void MuteMeComponent::log_descriptor_() {
  const uint8_t *d = this->desc_buf_;
  uint16_t len = this->desc_len_;
  ESP_LOGI(TAG, "HID report descriptor (%u bytes, device says %u); endpoints: IN 0x%02X, OUT %s", len,
           this->hid_desc_len_, this->ep_in_, this->ep_out_ ? "present" : "none");
  if (this->ep_out_)
    ESP_LOGI(TAG, "  OUT endpoint 0x%02X, max packet %u", this->ep_out_, this->ep_out_mps_);
  for (uint16_t i = 0; i < len; i += 24) {
    uint16_t n = len - i < 24 ? len - i : 24;
    ESP_LOGI(TAG, "  %03u: %s", i, format_hex_pretty(d + i, n).c_str());
  }

  // Summarise the reports: total bits per (kind, report ID).
  struct Summary {
    uint8_t kind;  // 8 input, 9 output, 11 feature
    uint8_t id;
    uint32_t bits;
    uint16_t page;
  };
  Summary sums[24];
  uint8_t nsums = 0;
  uint8_t rid = 0;
  uint32_t rsize = 0, rcount = 0;
  uint16_t upage = 0;
  for (uint16_t i = 0; i < len;) {
    uint8_t b = d[i];
    if (b == 0xFE) {  // long item
      if (i + 1 >= len)
        break;
      i += 3 + d[i + 1];
      continue;
    }
    uint8_t sz = b & 0x03;
    if (sz == 3)
      sz = 4;
    uint8_t type = (b >> 2) & 0x03;
    uint8_t tag = b >> 4;
    uint32_t v = 0;
    for (uint8_t k = 0; k < sz && i + 1 + k < len; k++)
      v |= static_cast<uint32_t>(d[i + 1 + k]) << (8 * k);
    i += 1 + sz;

    if (type == 1) {  // global
      if (tag == 0x0)
        upage = v;
      else if (tag == 0x7)
        rsize = v;
      else if (tag == 0x8)
        rid = v;
      else if (tag == 0x9)
        rcount = v;
    } else if (type == 0 && (tag == 0x8 || tag == 0x9 || tag == 0xB)) {  // main: input/output/feature
      int found = -1;
      for (uint8_t s = 0; s < nsums; s++)
        if (sums[s].kind == tag && sums[s].id == rid)
          found = s;
      if (found < 0 && nsums < 24) {
        sums[nsums] = {tag, rid, 0, upage};
        found = nsums++;
      }
      if (found >= 0)
        sums[found].bits += rsize * rcount;
    }
  }
  for (uint8_t s = 0; s < nsums; s++) {
    const char *k = sums[s].kind == 0x8 ? "input  " : sums[s].kind == 0x9 ? "output " : "feature";
    ESP_LOGI(TAG, "  %s report ID 0x%02X: %u data bytes (usage page 0x%04X)", k, sums[s].id,
             (sums[s].bits + 7) / 8, sums[s].page);
  }
}

void MuteMeComponent::log_probe_result_() {
  const char *what = this->result_kind_ == PROBE_SET ? "SET_REPORT" :
                     this->result_kind_ == PROBE_GET ? "GET_REPORT" : "OUT write";
  if (this->result_status_ != 0) {
    // 4 = stall: the button rejected it. -1 = couldn't start.
    ESP_LOGW(TAG, "probe: %s failed, status %d%s", what, this->result_status_,
             this->result_status_ == 4 ? " (stall: the button rejected it)" : "");
    return;
  }
  if (this->result_kind_ == PROBE_GET) {
    ESP_LOGI(TAG, "probe: GET_REPORT reply (%u bytes): %s", this->result_len_,
             format_hex_pretty(this->result_buf_, this->result_len_).c_str());
  } else {
    ESP_LOGI(TAG, "probe: %s accepted", what);
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
  bool ok = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
  int got = transfer->actual_num_bytes - static_cast<int>(sizeof(usb_setup_packet_t));
  if (got < 0)
    got = 0;
  const uint8_t *data = transfer->data_buffer + sizeof(usb_setup_packet_t);

  switch (self->ctrl_kind_.load()) {
    case CTRL_DESC:
      if (ok && got > 0) {
        uint16_t n = got > static_cast<int>(sizeof(self->desc_buf_)) ? sizeof(self->desc_buf_) : got;
        memcpy(self->desc_buf_, data, n);
        self->desc_len_ = n;
        self->desc_ready_.store(true);
      }
      break;
    case CTRL_PROBE: {
      self->result_status_ = ok ? 0 : static_cast<int>(transfer->status);
      uint16_t n = 0;
      if (ok && self->result_kind_ == PROBE_GET) {
        n = got > static_cast<int>(sizeof(self->result_buf_)) ? sizeof(self->result_buf_) : got;
        memcpy(self->result_buf_, data, n);
      }
      self->result_len_ = n;
      self->result_ready_.store(true);
      break;
    }
    default:
      if (!ok) {
        ESP_LOGW(TAG, "LED write status %d (if the LED never changes, try report_type: output)",
                 static_cast<int>(transfer->status));
      }
      break;
  }
  self->ctrl_busy_.store(false);
}

}  // namespace muteme
}  // namespace esphome
