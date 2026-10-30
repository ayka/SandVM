#include "cli/console_input.h"

#include <iterator>

namespace sandvm::cli {

ConsoleInput::ConsoleInput(Sink sink) : sink_(std::move(sink)) {
  in_ = GetStdHandle(STD_INPUT_HANDLE);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (GetConsoleMode(out, &saved_out_mode_)) {
    out_mode_saved_ = true;
    SetConsoleMode(out, saved_out_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }

  if (in_ == INVALID_HANDLE_VALUE || in_ == nullptr || !stop_event_) return;
  is_console_ = GetConsoleMode(in_, &saved_in_mode_) != 0;
  try {
    if (is_console_) {
      // Keep ENABLE_PROCESSED_INPUT so Ctrl+C still reaches the host handler that stops the VM.
      SetConsoleMode(in_, (saved_in_mode_ & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) | ENABLE_PROCESSED_INPUT);
      thread_ = std::thread([this] { console_loop(); });
    } else if (GetFileType(in_) != FILE_TYPE_UNKNOWN) {
      thread_ = std::thread([this] { pipe_loop(); });
    }
  } catch (...) {
    // The destructor will not run for a half-built object: undo the console changes here.
    stopping_ = true;
    if (is_console_) SetConsoleMode(in_, saved_in_mode_);
    if (out_mode_saved_) SetConsoleMode(out, saved_out_mode_);
    CloseHandle(stop_event_);
    throw;
  }
}

ConsoleInput::~ConsoleInput() {
  stop();
  if (stop_event_) CloseHandle(stop_event_);
}

void ConsoleInput::stop() {
  if (stopping_.exchange(true)) return;
  if (stop_event_) SetEvent(stop_event_);
  // A pipe read cannot wait on the stop event; cancel the blocking ReadFile instead. Repeat until the
  // thread is out, since a cancel issued before it enters ReadFile has nothing to cancel.
  if (thread_.joinable() && !is_console_) {
    while (!done_.load()) {
      CancelSynchronousIo(thread_.native_handle());
      Sleep(1);
    }
  }
  if (thread_.joinable()) thread_.join();
  if (is_console_) SetConsoleMode(in_, saved_in_mode_);
  if (out_mode_saved_) SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved_out_mode_);
}

void ConsoleInput::send(const char* s) {
  for (; *s; ++s) sink_(static_cast<uint8_t>(*s));
}

void ConsoleInput::console_loop() {
  const HANDLE handles[] = {stop_event_, in_};
  while (!stopping_.load()) {
    DWORD r = WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0 + 1) return;
    INPUT_RECORD records[32];
    DWORD count = 0;
    if (!ReadConsoleInputW(in_, records, static_cast<DWORD>(std::size(records)), &count)) return;
    for (DWORD i = 0; i < count; ++i) {
      if (records[i].EventType != KEY_EVENT) continue;
      const KEY_EVENT_RECORD& key = records[i].Event.KeyEvent;
      if (!key.bKeyDown) continue;
      for (WORD rep = 0; rep < key.wRepeatCount; ++rep) {
        switch (key.wVirtualKeyCode) {
          case VK_UP: send("\x1b[A"); continue;
          case VK_DOWN: send("\x1b[B"); continue;
          case VK_RIGHT: send("\x1b[C"); continue;
          case VK_LEFT: send("\x1b[D"); continue;
          case VK_HOME: send("\x1b[H"); continue;
          case VK_END: send("\x1b[F"); continue;
          case VK_DELETE: send("\x1b[3~"); continue;
          case VK_PRIOR: send("\x1b[5~"); continue;
          case VK_NEXT: send("\x1b[6~"); continue;
          default: break;
        }
        const wchar_t ch = key.uChar.UnicodeChar;
        if (ch == 0) continue;  // modifier or other key without a character
        if (ch < 0x80) {
          sink_(static_cast<uint8_t>(ch));
        } else {
          char utf8[4];
          const int n = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8, sizeof(utf8), nullptr, nullptr);
          for (int k = 0; k < n; ++k) sink_(static_cast<uint8_t>(utf8[k]));
        }
      }
    }
  }
}

void ConsoleInput::pipe_loop() {
  struct Done {
    std::atomic<bool>& flag;
    ~Done() { flag = true; }
  } done{done_};
  uint8_t buf[256];
  while (!stopping_.load()) {
    DWORD got = 0;
    if (!ReadFile(in_, buf, sizeof(buf), &got, nullptr) || got == 0) return;
    for (DWORD i = 0; i < got; ++i) sink_(buf[i]);
  }
}

}  // namespace sandvm::cli
