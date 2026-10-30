#include "cli/control_events.h"

#include <stdexcept>
#include <utility>

namespace sandvm::cli {

ControlEvents::ControlEvents(const std::wstring& name, std::function<void()> on_power, std::function<void()> on_stop)
    : on_power_(std::move(on_power)), on_stop_(std::move(on_stop)) {
  const std::wstring prefix = L"Local\\sandvm." + name;
  power_ = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".power").c_str());
  stop_ = CreateEventW(nullptr, FALSE, FALSE, (prefix + L".stop").c_str());
  quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!power_ || !stop_ || !quit_) {
    stop();
    throw std::runtime_error("control: cannot create the named events");
  }
  // A request sent while the previous machine was being torn down (reboot) is stale.
  ResetEvent(power_);
  ResetEvent(stop_);
  thread_ = std::thread([this] { loop(); });
}

ControlEvents::~ControlEvents() { stop(); }

void ControlEvents::stop() {
  if (quit_) SetEvent(quit_);
  if (thread_.joinable()) thread_.join();
  for (HANDLE* h : {&power_, &stop_, &quit_}) {
    if (*h) CloseHandle(*h);
    *h = nullptr;
  }
}

void ControlEvents::loop() {
  const HANDLE handles[] = {quit_, power_, stop_};
  for (;;) {
    const DWORD r = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 + 1) on_power_();
    else if (r == WAIT_OBJECT_0 + 2) on_stop_();
    else return;  // quit, or a wait failure
  }
}

}  // namespace sandvm::cli
