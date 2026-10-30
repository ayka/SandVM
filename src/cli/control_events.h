#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

namespace sandvm::cli {

// Lets another process (the SandVM Manager GUI) drive a running VM without a console: two named
// auto-reset events, Local\sandvm.<name>.power (ACPI power button) and Local\sandvm.<name>.stop
// (hard stop, like Ctrl+C). A GUI parent has no console to send Ctrl+C through, and killing the
// process skips the guest's shutdown.
// stop() (or the destructor) joins the thread; call it before the VM the callbacks use goes away.
class ControlEvents {
 public:
  ControlEvents(const std::wstring& name, std::function<void()> on_power, std::function<void()> on_stop);
  ~ControlEvents();
  ControlEvents(const ControlEvents&) = delete;
  ControlEvents& operator=(const ControlEvents&) = delete;

  void stop();

 private:
  void loop();

  std::function<void()> on_power_;
  std::function<void()> on_stop_;
  HANDLE power_ = nullptr;
  HANDLE stop_ = nullptr;
  HANDLE quit_ = nullptr;
  std::thread thread_;
};

}  // namespace sandvm::cli
