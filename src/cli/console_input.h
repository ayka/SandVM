#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace sandvm::cli {

// Forwards host stdin to the guest's serial console, byte by byte, on its own thread.
// A console stdin is switched to raw mode (no line editing, no local echo: the guest echoes) and
// arrow/navigation keys become the VT100 sequences a Linux tty expects. Ctrl+C keeps its host meaning
// (stop the VM). A redirected stdin (pipe/file) is forwarded as is until EOF.
// Also turns on VT processing for stdout so the guest's escape sequences render.
// stop() (or the destructor) joins the thread; call it before the device it feeds is destroyed.
class ConsoleInput {
 public:
  using Sink = std::function<void(uint8_t)>;

  explicit ConsoleInput(Sink sink);
  ~ConsoleInput();
  ConsoleInput(const ConsoleInput&) = delete;
  ConsoleInput& operator=(const ConsoleInput&) = delete;

  void stop();

 private:
  void console_loop();
  void pipe_loop();
  void send(const char* s);

  Sink sink_;
  HANDLE in_ = INVALID_HANDLE_VALUE;
  HANDLE stop_event_ = nullptr;
  bool is_console_ = false;
  DWORD saved_in_mode_ = 0;
  DWORD saved_out_mode_ = 0;
  bool out_mode_saved_ = false;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> done_{false};  // the reader thread has left its loop
  std::thread thread_;
};

}  // namespace sandvm::cli
