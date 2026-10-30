#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace sandvm::devices {

// A full-speed USB function as a host controller sees it: token packets addressed to its endpoints.
// UsbDevice implements the default control pipe (endpoint 0: SETUP / DATA / STATUS stages, the
// standard requests from USB 2.0 chapter 9); subclasses supply descriptors, class requests and their
// other endpoints. Thread-safe (one lock per device; host input may arrive from any thread).
class UsbDevice {
 public:
  enum class Pid : uint8_t { Setup = 0x2D, In = 0x69, Out = 0xE1 };
  enum class Result { Ok, Nak, Stall };

  virtual ~UsbDevice() = default;

  // Bus reset (port reset): default address 0, not configured.
  void reset();
  uint8_t address();
  bool configured();
  // One transaction. IN: fills at most `max_len` bytes into `data` and sets *len. SETUP / OUT: `data`
  // holds *len bytes from the host.
  Result packet(Pid pid, uint8_t endpoint, uint8_t* data, size_t max_len, size_t* len);

 protected:
  struct Setup {
    uint8_t request_type, request;
    uint16_t value, index, length;
  };

  // Device, configuration and string descriptors; `index` / `lang` as in GET_DESCRIPTOR.
  virtual bool descriptor(uint8_t type, uint8_t index, uint16_t lang, std::vector<uint8_t>* out) = 0;
  // Class or vendor requests and standard interface requests the base does not handle. For a
  // device-to-host request, fill *in; for host-to-device, `out` holds the data stage. False = STALL.
  virtual bool other_request(const Setup& setup, std::span<const uint8_t> out, std::vector<uint8_t>* in) = 0;
  // Non-control endpoints; called with the device lock held.
  virtual Result data_in(uint8_t endpoint, uint8_t* data, size_t max_len, size_t* len) = 0;
  virtual Result data_out(uint8_t endpoint, const uint8_t* data, size_t len) {
    (void)endpoint, (void)data, (void)len;
    return Result::Stall;
  }
  virtual void on_reset() {}
  virtual void on_configured(uint8_t /*config*/) {}

  std::mutex mutex_;

 private:
  enum class Stage { Idle, DataIn, DataOut, StatusIn, StatusOut };
  Result control(Pid pid, uint8_t* data, size_t max_len, size_t* len);
  bool standard_request(const Setup& setup, std::vector<uint8_t>* in);
  void finish_status();

  uint8_t address_ = 0;
  uint8_t pending_address_ = 0;  // SET_ADDRESS takes effect after its status stage
  bool set_address_pending_ = false;
  uint8_t configuration_ = 0;
  bool remote_wakeup_ = false;
  Stage stage_ = Stage::Idle;
  Setup setup_{};
  std::vector<uint8_t> buffer_;  // data stage
  size_t buffer_pos_ = 0;
};

// HID tablet: a pointer with absolute X/Y (0..32767 each), three buttons and a wheel, on interrupt
// endpoint 1. Guests drive it with their generic HID mouse drivers (Linux usbhid/hid-generic, Windows
// mouhid since XP), so the pointer follows the host cursor without capturing it.
class UsbTablet : public UsbDevice {
 public:
  static constexpr int kMax = 0x7FFF;
  // x, y in 0..kMax; buttons bit 0 left, 1 right, 2 middle; dz wheel notches (positive = down).
  void update(int x, int y, unsigned buttons, int dz);
  // True while the guest polls the report endpoint, i.e. a driver is using the tablet.
  bool in_use();

 protected:
  bool descriptor(uint8_t type, uint8_t index, uint16_t lang, std::vector<uint8_t>* out) override;
  bool other_request(const Setup& setup, std::span<const uint8_t> out, std::vector<uint8_t>* in) override;
  Result data_in(uint8_t endpoint, uint8_t* data, size_t max_len, size_t* len) override;
  void on_reset() override;

 private:
  void report(uint8_t* out);  // 6 bytes

  int x_ = 0, y_ = 0;
  unsigned buttons_ = 0;
  int wheel_ = 0;        // notches not yet reported
  bool changed_ = false;
  uint8_t idle_ = 0;     // SET_IDLE duration (4 ms units), 0 = report only on change
  uint64_t last_poll_ = 0;  // GetTickCount64 of the last report endpoint poll
};

}  // namespace sandvm::devices
