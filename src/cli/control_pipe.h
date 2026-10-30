#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <thread>

namespace sandvm::cli {

// Text command channel for a GUI or a script: the named pipe \\.\pipe\sandvm.<name>, local clients
// only, one at a time. Each line sent ("pause\n", "cdrom C:\x.iso\n") is passed to the handler on the
// pipe's own thread, and the handler's reply goes back as one line ("ok", "error: ..."). A client may
// send several commands per connection or reconnect for each one.
// stop() (or the destructor) joins the thread; call it before whatever the handler uses goes away.
class ControlPipe {
 public:
  using Handler = std::function<std::string(const std::string& line)>;

  ControlPipe(const std::wstring& name, Handler handler);
  ~ControlPipe();
  ControlPipe(const ControlPipe&) = delete;
  ControlPipe& operator=(const ControlPipe&) = delete;

  void stop();

 private:
  void loop();
  // Waits for an overlapped operation on pipe_; false if quit_ was set first or the operation failed.
  bool wait_io(OVERLAPPED* ov, DWORD* transferred);

  const std::wstring path_;
  const Handler handler_;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE quit_ = nullptr;
  HANDLE io_event_ = nullptr;
  std::thread thread_;
};

// Client side: sends one command line to the machine's pipe and returns its reply line. False if no
// machine with that name is listening (or it did not answer within a few seconds).
bool send_control(const std::wstring& name, const std::string& line, std::string* reply);

}  // namespace sandvm::cli
