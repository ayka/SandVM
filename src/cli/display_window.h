#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>

#include "core/devices/vga.h"

namespace sandvm::cli {

// Shows the guest's VGA output in a Win32 window and feeds its keyboard and mouse to the guest, on a
// thread of its own (the window, its message loop and the 60 Hz redraw timer all live there).
//
// Keys go out as scancode set 1 with the 0xE0 prefix for extended keys, which is what Windows reports
// in WM_KEYDOWN's lParam and what the i8042 delivers with translation on. Closing the window calls
// on_close (stop the VM); the window itself stays until the object is destroyed.
//
// Mouse, two ways:
//  - pointer integration: while the guest drives the USB tablet (tablet_active), the host cursor's
//    position over the picture goes to on_tablet as an absolute position, with no capture at all;
//  - otherwise a click captures the mouse (cursor hidden and confined to the window), after which raw
//    input deltas, buttons and the wheel go to on_mouse; Ctrl+Alt or losing focus releases it. The
//    capturing click itself is not passed on.
//
// The menu bar drives the machine through Actions (any of them may be empty, its item is then left
// out). Host keys, as in VMware: Ctrl+Alt+Enter full screen, Ctrl+Alt+Insert sends Ctrl+Alt+Del.
// The picture keeps its aspect ratio (black bars) when the window or screen shape differs.
//
// With show_window = false no window is created and only the screenshot (if any) is written, for
// unattended runs. The screenshot is a 32-bit BMP of the current frame, rewritten about once a second.
class DisplayWindow {
 public:
  using KeySink = std::function<void(std::span<const uint8_t>)>;
  // dx right, dy down, dz wheel notches (positive = scroll down), buttons as devices::I8042::push_mouse.
  using MouseSink = std::function<void(int dx, int dy, int dz, unsigned buttons)>;
  // x, y in 0..32767 across the guest picture, buttons bit 0 left, 1 right, 2 middle, dz as above.
  using TabletSink = std::function<void(int x, int y, unsigned buttons, int dz)>;
  using CloseCallback = std::function<void()>;

  struct Actions {
    std::function<void()> ctrl_alt_del;
    std::function<void(bool)> set_paused;
    std::function<void()> reset;
    std::function<void()> power_button;  // ACPI shutdown request
    std::function<void()> power_off;     // immediate stop
    std::function<void(const std::wstring&)> insert_cd;
    std::function<void()> eject_cd;
  };

  struct Options {
    bool show_window = true;
    std::wstring screenshot_path;  // empty = none
    std::wstring title = L"SandVM";
    TabletSink on_tablet;                // empty = no pointer integration
    std::function<bool()> tablet_active;
    Actions actions;
  };

  DisplayWindow(devices::Vga& vga, Options options, KeySink on_key, MouseSink on_mouse, CloseCallback on_close);
  ~DisplayWindow();
  DisplayWindow(const DisplayWindow&) = delete;
  DisplayWindow& operator=(const DisplayWindow&) = delete;

 private:
  static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam);
  void thread_main();
  void build_menu();
  void command(UINT id);
  void refresh();
  void key(UINT msg, WPARAM wparam, LPARAM lparam);
  bool host_key(UINT msg, WPARAM wparam);
  void raw_input(HRAWINPUT handle);
  void pointer(UINT msg, WPARAM wparam, LPARAM lparam);
  bool integrated();
  void capture_mouse();
  void release_mouse();
  bool clip_cursor();
  void update_title();
  void toggle_fullscreen();
  RECT picture_rect();  // where the frame is drawn in the client area
  void save_screenshot();
  bool write_bmp(const std::wstring& path);

  devices::Vga& vga_;
  const Options options_;
  const KeySink on_key_;
  const MouseSink on_mouse_;
  const CloseCallback on_close_;

  HWND hwnd_ = nullptr;
  HMENU menu_ = nullptr;
  HANDLE ready_ = nullptr;  // set once the thread has created its window (or failed to)
  std::atomic<DWORD> thread_id_{0};
  devices::Vga::Frame frame_;
  uint32_t shown_width_ = 0;
  uint32_t shown_height_ = 0;
  unsigned ticks_ = 0;
  bool raw_mouse_ = false;  // raw mouse input registered; capturing is pointless without it
  bool captured_ = false;
  unsigned buttons_ = 0;  // guest-visible button state while captured
  int wheel_ = 0;         // WHEEL_DELTA remainder
  // Pointer integration.
  bool was_integrated_ = false;
  unsigned tablet_buttons_ = 0;
  int tablet_x_ = 0, tablet_y_ = 0;
  // Full screen: the windowed placement to go back to.
  bool fullscreen_ = false;
  WINDOWPLACEMENT windowed_{};
  bool paused_ = false;
  std::thread thread_;
};

}  // namespace sandvm::cli
