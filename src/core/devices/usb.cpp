#include "core/devices/usb.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"

namespace sandvm::devices {
namespace {
// bRequest values (USB 2.0 table 9-4) and descriptor types (table 9-5).
constexpr uint8_t kGetStatus = 0, kClearFeature = 1, kSetFeature = 3, kSetAddress = 5, kGetDescriptor = 6,
                  kGetConfiguration = 8, kSetConfiguration = 9, kGetInterface = 10, kSetInterface = 11;
constexpr uint8_t kDescDevice = 1, kDescConfig = 2, kDescString = 3;
constexpr uint8_t kDescHid = 0x21, kDescReport = 0x22;
// HID class requests (HID 1.11, 7.2).
constexpr uint8_t kHidGetReport = 1, kHidGetIdle = 2, kHidGetProtocol = 3, kHidSetReport = 9, kHidSetIdle = 10,
                  kHidSetProtocol = 11;
constexpr uint8_t kMaxPacket0 = 8;

// The QEMU tablet's report: 3 buttons + 5 bits padding, X and Y absolute 0..0x7FFF, wheel relative.
constexpr uint8_t kReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00,  // desktop / mouse / app / pointer / phys
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,  // buttons 1..3, 0..1
    0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,  // 3 x 1 bit data, 1 x 5 bit
    0x81, 0x01,                                                  // constant (padding)
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF,  // X, Y, logical 0..
    0x7F, 0x35, 0x00, 0x46, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02,  // 0x7FFF, physical same, 2 x 16 bit
    0x81, 0x02,                                                  // input data, variable, absolute
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x35, 0x00,  // wheel -127..127
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,              // 1 x 8 bit relative
    0xC0, 0xC0,
};

constexpr uint8_t kDeviceDescriptor[] = {
    18, kDescDevice, 0x10, 0x01,  // USB 1.1
    0x00, 0x00, 0x00,             // class per interface
    kMaxPacket0,
    0x09, 0x12,  // idVendor 0x1209 (pid.codes, open source hardware)
    0x01, 0x00,  // idProduct 0x0001 (pid.codes test PID)
    0x00, 0x01,  // bcdDevice 1.00
    1, 2, 3,     // manufacturer, product, serial strings
    1,           // configurations
};

const uint8_t kConfigDescriptor[] = {
    9, kDescConfig, 34, 0, 1, 1, 0, 0xA0, 50,           // 1 interface, bus powered + remote wakeup, 100 mA
    9, 4, 0, 0, 1, 0x03, 0x00, 0x00, 0,                 // interface 0: HID, no boot protocol
    9, kDescHid, 0x11, 0x01, 0, 1, kDescReport,         // HID 1.11, one report descriptor
    sizeof(kReportDescriptor), 0,                       //
    7, 5, 0x81, 0x03, 8, 0, 4,                          // EP1 IN interrupt, 8 bytes, every 4 ms
};

std::vector<uint8_t> string_descriptor(const char* text) {
  std::vector<uint8_t> d = {0, kDescString};
  for (const char* p = text; *p; ++p) {
    d.push_back(static_cast<uint8_t>(*p));
    d.push_back(0);
  }
  d[0] = static_cast<uint8_t>(d.size());
  return d;
}
}  // namespace

void UsbDevice::reset() {
  std::lock_guard lock(mutex_);
  address_ = 0;
  set_address_pending_ = false;
  configuration_ = 0;
  remote_wakeup_ = false;
  stage_ = Stage::Idle;
  buffer_.clear();
  on_reset();
}

uint8_t UsbDevice::address() {
  std::lock_guard lock(mutex_);
  return address_;
}

bool UsbDevice::configured() {
  std::lock_guard lock(mutex_);
  return configuration_ != 0;
}

UsbDevice::Result UsbDevice::packet(Pid pid, uint8_t endpoint, uint8_t* data, size_t max_len, size_t* len) {
  std::lock_guard lock(mutex_);
  if (endpoint == 0) return control(pid, data, max_len, len);
  if (pid == Pid::In) return data_in(endpoint, data, max_len, len);
  if (pid == Pid::Out) return data_out(endpoint, data, *len);
  return Result::Stall;
}

UsbDevice::Result UsbDevice::control(Pid pid, uint8_t* data, size_t max_len, size_t* len) {
  switch (pid) {
    case Pid::Setup: {
      // A SETUP always starts a new control transfer, whatever state the last one was left in.
      if (*len != 8) return Result::Stall;
      setup_ = {data[0], data[1], static_cast<uint16_t>(data[2] | (data[3] << 8)),
                static_cast<uint16_t>(data[4] | (data[5] << 8)), static_cast<uint16_t>(data[6] | (data[7] << 8))};
      buffer_.clear();
      buffer_pos_ = 0;
      if (setup_.request_type & 0x80) {
        std::vector<uint8_t> in;
        if (!standard_request(setup_, &in)) {
          stage_ = Stage::Idle;
          setup_.request = 0xFF;  // marks the transfer as failed: its next stage stalls
          return Result::Ok;
        }
        if (in.size() > setup_.length) in.resize(setup_.length);
        buffer_ = std::move(in);
        stage_ = Stage::DataIn;
      } else if (setup_.length) {
        stage_ = Stage::DataOut;
      } else {
        if (!standard_request(setup_, nullptr)) {
          stage_ = Stage::Idle;
          setup_.request = 0xFF;
          return Result::Ok;
        }
        stage_ = Stage::StatusIn;
      }
      return Result::Ok;
    }
    case Pid::In:
      if (stage_ == Stage::DataIn) {
        const size_t n = std::min(max_len, buffer_.size() - buffer_pos_);
        std::memcpy(data, buffer_.data() + buffer_pos_, n);
        buffer_pos_ += n;
        *len = n;
        return Result::Ok;
      }
      if (stage_ == Stage::DataOut) {
        // Status stage of a host-to-device request: now the data is complete and can be acted on.
        if (buffer_.size() < setup_.length || !standard_request(setup_, nullptr)) {
          stage_ = Stage::Idle;
          return Result::Stall;
        }
        *len = 0;
        stage_ = Stage::Idle;
        return Result::Ok;
      }
      if (stage_ == Stage::StatusIn) {
        *len = 0;
        finish_status();
        return Result::Ok;
      }
      return Result::Stall;
    case Pid::Out:
      if (stage_ == Stage::DataOut) {
        const size_t room = setup_.length - buffer_.size();
        buffer_.insert(buffer_.end(), data, data + std::min(*len, room));
        return Result::Ok;
      }
      if (stage_ == Stage::DataIn) {  // status stage of a device-to-host request
        stage_ = Stage::Idle;
        return Result::Ok;
      }
      return Result::Stall;
  }
  return Result::Stall;
}

void UsbDevice::finish_status() {
  stage_ = Stage::Idle;
  if (set_address_pending_) {
    address_ = pending_address_;
    set_address_pending_ = false;
  }
}

bool UsbDevice::standard_request(const Setup& s, std::vector<uint8_t>* in) {
  if (s.request == 0xFF) return false;
  const std::span<const uint8_t> out(buffer_.data(), buffer_.size());
  if ((s.request_type & 0x60) != 0) return other_request(s, out, in);  // class / vendor
  const unsigned recipient = s.request_type & 0x1F;
  if (recipient == 0) {  // device
    switch (s.request) {
      case kGetStatus:
        if (!in) return false;
        *in = {static_cast<uint8_t>(remote_wakeup_ ? 0x02 : 0x00), 0};
        return true;
      case kClearFeature:
      case kSetFeature:
        if (s.value == 1) remote_wakeup_ = s.request == kSetFeature;
        return true;
      case kSetAddress:
        if (s.value > 127) return false;
        pending_address_ = static_cast<uint8_t>(s.value);
        set_address_pending_ = true;
        return true;
      case kGetDescriptor: {
        if (!in) return false;
        return descriptor(static_cast<uint8_t>(s.value >> 8), static_cast<uint8_t>(s.value), s.index, in);
      }
      case kGetConfiguration:
        if (!in) return false;
        *in = {configuration_};
        return true;
      case kSetConfiguration:
        if (s.value > 1) return false;
        configuration_ = static_cast<uint8_t>(s.value);
        on_configured(configuration_);
        return true;
      default:
        return false;
    }
  }
  if (recipient == 1) {  // interface
    switch (s.request) {
      case kGetStatus:
        if (!in) return false;
        *in = {0, 0};
        return true;
      case kGetInterface:
        if (!in) return false;
        *in = {0};
        return true;
      case kSetInterface:
        return s.value == 0;
      default:
        return other_request(s, out, in);  // e.g. GET_DESCRIPTOR for HID class descriptors
    }
  }
  if (recipient == 2) {  // endpoint: no halt state is kept, so the feature requests just succeed
    if (s.request == kGetStatus) {
      if (!in) return false;
      *in = {0, 0};
      return true;
    }
    return s.request == kClearFeature || s.request == kSetFeature;
  }
  return false;
}

void UsbTablet::update(int x, int y, unsigned buttons, int dz) {
  std::lock_guard lock(mutex_);
  x = std::clamp(x, 0, kMax);
  y = std::clamp(y, 0, kMax);
  if (x == x_ && y == y_ && buttons == buttons_ && dz == 0) return;
  x_ = x;
  y_ = y;
  buttons_ = buttons & 7;
  wheel_ = std::clamp(wheel_ + dz, -1024, 1024);
  changed_ = true;
}

bool UsbTablet::in_use() {
  std::lock_guard lock(mutex_);
  return last_poll_ && GetTickCount64() - last_poll_ < 500;
}

void UsbTablet::on_reset() {
  changed_ = true;  // the first report after configuration tells the guest where the pointer is
  idle_ = 0;
  last_poll_ = 0;
}

void UsbTablet::report(uint8_t* r) {
  // HID wheel is positive away from the user; our dz counts notches towards the user.
  const int notches = std::clamp(-wheel_, -127, 127);
  wheel_ += notches;
  r[0] = static_cast<uint8_t>(buttons_);
  r[1] = static_cast<uint8_t>(x_);
  r[2] = static_cast<uint8_t>(x_ >> 8);
  r[3] = static_cast<uint8_t>(y_);
  r[4] = static_cast<uint8_t>(y_ >> 8);
  r[5] = static_cast<uint8_t>(static_cast<int8_t>(notches));
  changed_ = wheel_ != 0;
}

bool UsbTablet::descriptor(uint8_t type, uint8_t index, uint16_t lang, std::vector<uint8_t>* out) {
  (void)lang;
  switch (type) {
    case kDescDevice:
      out->assign(std::begin(kDeviceDescriptor), std::end(kDeviceDescriptor));
      return true;
    case kDescConfig:
      if (index != 0) return false;
      out->assign(std::begin(kConfigDescriptor), std::end(kConfigDescriptor));
      return true;
    case kDescString:
      switch (index) {
        case 0: *out = {4, kDescString, 0x09, 0x04}; return true;  // US English
        case 1: *out = string_descriptor("SandVM"); return true;
        case 2: *out = string_descriptor("SandVM USB Tablet"); return true;
        case 3: *out = string_descriptor("1"); return true;
        default: return false;
      }
    default:
      return false;
  }
}

bool UsbTablet::other_request(const Setup& s, std::span<const uint8_t> /*out*/, std::vector<uint8_t>* in) {
  if ((s.request_type & 0x60) == 0) {
    // Standard GET_DESCRIPTOR to the interface: the HID and report descriptors.
    if (s.request != kGetDescriptor || !in) return false;
    if ((s.value >> 8) == kDescReport) {
      in->assign(std::begin(kReportDescriptor), std::end(kReportDescriptor));
      return true;
    }
    if ((s.value >> 8) == kDescHid) {
      in->assign(kConfigDescriptor + 18, kConfigDescriptor + 27);
      return true;
    }
    return false;
  }
  if ((s.request_type & 0x60) != 0x20) return false;  // vendor
  switch (s.request) {
    case kHidGetReport:
      if (!in) return false;
      in->resize(6);
      report(in->data());
      return true;
    case kHidGetIdle:
      if (!in) return false;
      *in = {idle_};
      return true;
    case kHidGetProtocol:
      if (!in) return false;
      *in = {1};  // report protocol
      return true;
    case kHidSetIdle:
      idle_ = static_cast<uint8_t>(s.value >> 8);
      return true;
    case kHidSetReport:
    case kHidSetProtocol:
      return true;
    default:
      return false;
  }
}

UsbDevice::Result UsbTablet::data_in(uint8_t endpoint, uint8_t* data, size_t max_len, size_t* len) {
  if (endpoint != 1) return Result::Stall;
  last_poll_ = GetTickCount64();
  if (!changed_) return Result::Nak;
  uint8_t r[6];
  report(r);
  *len = std::min<size_t>(max_len, sizeof(r));
  std::memcpy(data, r, *len);
  return Result::Ok;
}

}  // namespace sandvm::devices
