#include "cli/control_pipe.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace sandvm::cli {
namespace {
constexpr size_t kMaxLine = 4096;
}  // namespace

ControlPipe::ControlPipe(const std::wstring& name, Handler handler)
    : path_(L"\\\\.\\pipe\\sandvm." + name), handler_(std::move(handler)) {
  quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  io_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  pipe_ = CreateNamedPipeW(path_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                           PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096,
                           4096, 0, nullptr);
  if (!quit_ || !io_event_ || pipe_ == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    stop();
    throw std::runtime_error("control: cannot create the pipe (error " + std::to_string(error) + ")");
  }
  thread_ = std::thread([this] { loop(); });
}

ControlPipe::~ControlPipe() { stop(); }

void ControlPipe::stop() {
  if (quit_) SetEvent(quit_);
  if (thread_.joinable()) thread_.join();
  if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
  pipe_ = INVALID_HANDLE_VALUE;
  for (HANDLE* h : {&quit_, &io_event_}) {
    if (*h) CloseHandle(*h);
    *h = nullptr;
  }
}

bool ControlPipe::wait_io(OVERLAPPED* ov, DWORD* transferred) {
  const HANDLE handles[] = {quit_, io_event_};
  const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
  if (r != WAIT_OBJECT_0 + 1) {
    CancelIoEx(pipe_, ov);
    GetOverlappedResult(pipe_, ov, transferred, TRUE);  // the buffer must outlive the operation
    return false;
  }
  return GetOverlappedResult(pipe_, ov, transferred, FALSE) != FALSE;
}

void ControlPipe::loop() {
  for (;;) {
    OVERLAPPED ov{};
    ov.hEvent = io_event_;
    ResetEvent(io_event_);
    DWORD n = 0;
    if (!ConnectNamedPipe(pipe_, &ov)) {
      const DWORD error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        if (!wait_io(&ov, &n)) {
          if (WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0) return;
          DisconnectNamedPipe(pipe_);
          continue;
        }
      } else if (error != ERROR_PIPE_CONNECTED) {
        SVM_WARN("control: ConnectNamedPipe failed (%lu)", error);
        if (WaitForSingleObject(quit_, 200) == WAIT_OBJECT_0) return;
        continue;
      }
    }

    std::string pending;
    char buf[512];
    bool connected = true;
    while (connected) {
      OVERLAPPED rov{};
      rov.hEvent = io_event_;
      ResetEvent(io_event_);
      DWORD got = 0;
      if (!ReadFile(pipe_, buf, sizeof(buf), &got, &rov)) {
        if (GetLastError() != ERROR_IO_PENDING || !wait_io(&rov, &got)) {
          if (WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0) return;
          break;
        }
      }
      if (got == 0) break;
      pending.append(buf, got);
      size_t eol;
      while ((eol = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, eol);
        pending.erase(0, eol + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string reply;
        try {
          reply = handler_(line);
        } catch (const std::exception& e) {
          reply = std::string("error: ") + e.what();
        }
        reply += '\n';
        OVERLAPPED wov{};
        wov.hEvent = io_event_;
        ResetEvent(io_event_);
        DWORD written = 0;
        if (!WriteFile(pipe_, reply.data(), static_cast<DWORD>(reply.size()), &written, &wov) &&
            (GetLastError() != ERROR_IO_PENDING || !wait_io(&wov, &written))) {
          if (WaitForSingleObject(quit_, 0) == WAIT_OBJECT_0) return;
          connected = false;
          break;
        }
      }
      if (pending.size() > kMaxLine) break;  // not a client that speaks this protocol
    }
    // No FlushFileBuffers: it would block on a client that went away; replies were already read by a
    // client waiting for them.
    DisconnectNamedPipe(pipe_);
  }
}

bool send_control(const std::wstring& name, const std::string& line, std::string* reply) {
  constexpr DWORD kTimeoutMs = 5000;
  const std::wstring path = L"\\\\.\\pipe\\sandvm." + name;
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 50; ++attempt) {
    h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                    nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    if (GetLastError() != ERROR_PIPE_BUSY) return false;  // no such machine
    WaitNamedPipeW(path.c_str(), 100);                     // another client is connected
  }
  if (h == INVALID_HANDLE_VALUE) return false;
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  // One overlapped transfer with a deadline; a server that stops answering cannot hang the client.
  auto transfer = [&](bool write, void* buf, DWORD len, DWORD* done) {
    OVERLAPPED ov{};
    ov.hEvent = event;
    ResetEvent(event);
    const BOOL ok = write ? WriteFile(h, buf, len, done, &ov) : ReadFile(h, buf, len, done, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(event, kTimeoutMs) != WAIT_OBJECT_0) {
      CancelIoEx(h, &ov);
      GetOverlappedResult(h, &ov, done, TRUE);
      return false;
    }
    return GetOverlappedResult(h, &ov, done, FALSE) != FALSE;
  };
  std::string out = line + "\n";
  reply->clear();
  bool ok = event != nullptr;
  for (size_t sent = 0; ok && sent < out.size();) {
    DWORD n = 0;
    ok = transfer(true, out.data() + sent, static_cast<DWORD>(out.size() - sent), &n) && n > 0;
    sent += n;
  }
  bool complete = false;
  while (ok && !complete && reply->size() < 4096) {
    char buf[256];
    DWORD n = 0;
    ok = transfer(false, buf, sizeof(buf), &n) && n > 0;
    for (DWORD i = 0; ok && i < n && !complete; ++i) {
      if (buf[i] == '\n') complete = true;
      else reply->push_back(buf[i]);
    }
  }
  if (event) CloseHandle(event);
  CloseHandle(h);
  return ok && complete;
}

}  // namespace sandvm::cli
