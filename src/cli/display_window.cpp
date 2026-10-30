#include "cli/display_window.h"

#include <commdlg.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/devices/i8042.h"
#include "core/log.h"

namespace sandvm::cli {
namespace {
constexpr wchar_t kClassName[] = L"SandVMDisplay";
constexpr UINT kFrameMs = 16;
constexpr unsigned kScreenshotTicks = 60;  // about once a second
constexpr int kTabletMax = 0x7FFF;

enum MenuId : UINT {
  kMenuCtrlAltDel = 100,
  kMenuPause,
  kMenuReset,
  kMenuPowerButton,
  kMenuPowerOff,
  kMenuInsertCd,
  kMenuEjectCd,
  kMenuFullscreen,
  kMenuScreenshot,
};

ATOM register_class(WNDPROC proc) {
  static ATOM atom = [proc] {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc);
  }();
  return atom;
}
}  // namespace

DisplayWindow::DisplayWindow(devices::Vga& vga, Options options, KeySink on_key, MouseSink on_mouse,
                             CloseCallback on_close)
    : vga_(vga), options_(std::move(options)), on_key_(std::move(on_key)), on_mouse_(std::move(on_mouse)),
      on_close_(std::move(on_close)) {
  ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ready_) throw std::runtime_error("CreateEvent failed");
  thread_ = std::thread([this] { thread_main(); });
  WaitForSingleObject(ready_, INFINITE);
}

DisplayWindow::~DisplayWindow() {
  if (thread_.joinable()) {
    PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    thread_.join();
  }
  CloseHandle(ready_);
}

void DisplayWindow::thread_main() {
  MSG msg;
  // Creates this thread's message queue before the constructor can return and the destructor post to it.
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  thread_id_ = GetCurrentThreadId();
  if (options_.show_window) {
    build_menu();
    if (register_class(&DisplayWindow::window_proc)) {
      hwnd_ = CreateWindowExW(0, kClassName, options_.title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              640 + 16, 480 + 59, nullptr, menu_, GetModuleHandleW(nullptr), this);
    }
    if (hwnd_) {
      // Raw mouse input (usage page 1, usage 2) gives unaccelerated relative motion, what a PS/2 mouse
      // reports, and keeps coming while the cursor is pinned at the edge of the clip rectangle.
      RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd_};
      raw_mouse_ = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
      if (!raw_mouse_) SVM_WARN("display: RegisterRawInputDevices failed (error %lu), no mouse", GetLastError());
      ShowWindow(hwnd_, SW_SHOWNORMAL);
    } else {
      SVM_WARN("display: cannot create the window (error %lu)", GetLastError());
    }
  }
  // A thread timer (no window), so the screenshot-only mode runs the same loop.
  const UINT_PTR timer = SetTimer(nullptr, 0, kFrameMs, nullptr);
  if (!timer) SVM_WARN("display: SetTimer failed (error %lu), the picture will not update", GetLastError());
  SetEvent(ready_);

  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
      refresh();
      continue;
    }
    // No TranslateMessage: the guest gets scancodes, not characters.
    DispatchMessageW(&msg);
  }
  if (timer) KillTimer(nullptr, timer);
  if (!options_.screenshot_path.empty()) save_screenshot();
  if (hwnd_) {
    release_mouse();
    DestroyWindow(hwnd_);
  }
  hwnd_ = nullptr;
}

LRESULT CALLBACK DisplayWindow::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<DisplayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<DisplayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || !self->hwnd_) return DefWindowProcW(hwnd, msg, wparam, lparam);
  return self->handle(msg, wparam, lparam);
}

LRESULT DisplayWindow::handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      RECT client;
      GetClientRect(hwnd_, &client);
      const RECT rc = picture_rect();
      // Black bars around a picture whose shape differs from the window's.
      HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
      const RECT bars[] = {{0, 0, client.right, rc.top}, {0, rc.bottom, client.right, client.bottom},
                           {0, rc.top, rc.left, rc.bottom}, {rc.right, rc.top, client.right, rc.bottom}};
      for (const RECT& bar : bars) {
        if (bar.right > bar.left && bar.bottom > bar.top) FillRect(dc, &bar, black);
      }
      if (frame_.width && frame_.height) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = static_cast<LONG>(frame_.width);
        bi.bmiHeader.biHeight = -static_cast<LONG>(frame_.height);  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 0, 0, static_cast<int>(frame_.width),
                      static_cast<int>(frame_.height), frame_.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // every paint covers the whole client area
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
      // System keys too, and without DefWindowProc, so Alt and F10 reach the guest instead of the menu.
      key(msg, wparam, lparam);
      return 0;
    case WM_SYSCHAR:
      return 0;
    case WM_COMMAND:
      if (HIWORD(wparam) == 0) command(LOWORD(wparam));
      return 0;
    case WM_LBUTTONDOWN:
      if (integrated()) {
        pointer(msg, wparam, lparam);
      } else if (!captured_) {
        capture_mouse();
      }
      return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
      if (integrated()) pointer(msg, wparam, lparam);
      return 0;
    case WM_INPUT:
      raw_input(reinterpret_cast<HRAWINPUT>(lparam));
      return DefWindowProcW(hwnd_, msg, wparam, lparam);  // lets the system clean up the input
    case WM_SETCURSOR:
      // The guest draws its own pointer: hide ours over the picture while it follows the mouse.
      if ((captured_ || integrated()) && LOWORD(lparam) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
    case WM_KILLFOCUS:
      release_mouse();
      if (tablet_buttons_ && options_.on_tablet) options_.on_tablet(tablet_x_, tablet_y_, 0, 0);
      tablet_buttons_ = 0;
      return 0;
    case WM_SIZE:
    case WM_MOVE:
      if (captured_) clip_cursor();
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
    case WM_CLOSE: {
      // Like closing a VMware window: ask before pulling the plug on a running guest.
      if (options_.actions.power_button) {
        const int answer = MessageBoxW(hwnd_,
                                       L"Sanal makine çalışıyor.\n\nEvet: misafir işletim sistemine kapanma isteği gönder "
                                       L"(ACPI)\nHayır: gücü hemen kes\nİptal: vazgeç",
                                       options_.title.c_str(), MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDYES) options_.actions.power_button();
        else if (answer == IDNO && on_close_) on_close_();
        return 0;
      }
      if (on_close_) on_close_();
      return 0;  // destroyed by the destructor, once the VM has stopped
    }
    default:
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
  }
}

bool DisplayWindow::host_key(UINT msg, WPARAM wparam) {
  if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
  if (!(GetKeyState(VK_CONTROL) & 0x8000) || !(GetKeyState(VK_MENU) & 0x8000)) return false;
  if (wparam == VK_RETURN) {
    toggle_fullscreen();
    return true;
  }
  if (wparam == VK_INSERT && options_.actions.ctrl_alt_del) {
    options_.actions.ctrl_alt_del();
    return true;
  }
  return false;
}

void DisplayWindow::key(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (!on_key_) return;
  if (host_key(msg, wparam)) return;
  const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
  // The key-up of a host key combination is not passed on either (its press was not).
  if (up && (wparam == VK_RETURN || wparam == VK_INSERT) && (GetKeyState(VK_CONTROL) & 0x8000) &&
      (GetKeyState(VK_MENU) & 0x8000)) {
    return;
  }
  if (wparam == VK_PAUSE) {
    // Pause has no break code; the whole sequence goes out on press.
    static constexpr uint8_t kPause[] = {0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5};
    if (!up) on_key_(kPause);
    return;
  }
  const uint8_t scancode = static_cast<uint8_t>((lparam >> 16) & 0xFF);
  if (scancode == 0 || scancode >= 0x80) return;
  // Windows flags Num Lock as extended, but on the keyboard it is plain 0x45 (E0 45 is not a key).
  const bool extended = (lparam & (1 << 24)) && wparam != VK_NUMLOCK;
  uint8_t bytes[2];
  size_t n = 0;
  if (extended) bytes[n++] = 0xE0;
  bytes[n++] = static_cast<uint8_t>(scancode | (up ? 0x80 : 0));
  on_key_(std::span<const uint8_t>(bytes, n));
  // Ctrl+Alt releases the mouse (the guest still sees both keys; their releases follow normally).
  if (captured_ && !up && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000)) release_mouse();
}

void DisplayWindow::raw_input(HRAWINPUT handle) {
  if (!captured_ || !on_mouse_) return;
  RAWINPUT input;
  UINT size = sizeof(input);
  if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) ||
      input.header.dwType != RIM_TYPEMOUSE) {
    return;
  }
  const RAWMOUSE& m = input.data.mouse;
  int dx = 0;
  int dy = 0;
  // Absolute devices (a host inside a remote desktop session or another VM) have no relative motion
  // to pass on; their buttons still work.
  if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
    dx = m.lLastX;
    dy = m.lLastY;
  }
  static constexpr struct {
    USHORT down, up;
    unsigned bit;
  } kButtons[] = {
      {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, devices::I8042::kMouseLeft},
      {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, devices::I8042::kMouseRight},
      {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, devices::I8042::kMouseMiddle},
      {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, devices::I8042::kMouse4},
      {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, devices::I8042::kMouse5},
  };
  for (const auto& b : kButtons) {
    if (m.usButtonFlags & b.down) buttons_ |= b.bit;
    if (m.usButtonFlags & b.up) buttons_ &= ~b.bit;
  }
  int dz = 0;
  if (m.usButtonFlags & RI_MOUSE_WHEEL) {
    // Windows counts away from the user as positive, in WHEEL_DELTA (120) steps or fractions of one.
    wheel_ += static_cast<SHORT>(m.usButtonData);
    dz = -(wheel_ / WHEEL_DELTA);
    wheel_ %= WHEEL_DELTA;
  }
  on_mouse_(dx, dy, dz, buttons_);
}

void DisplayWindow::capture_mouse() {
  // Without raw input nothing would reach the guest: never hide the cursor for nothing.
  if (!on_mouse_ || !raw_mouse_) return;
  captured_ = true;
  buttons_ = 0;
  wheel_ = 0;
  if (!clip_cursor()) {
    captured_ = false;
    return;
  }
  SetCursor(nullptr);
  update_title();
}

void DisplayWindow::release_mouse() {
  if (!captured_) return;
  captured_ = false;
  ClipCursor(nullptr);
  // Buttons still held would otherwise stay pressed in the guest.
  if (buttons_ && on_mouse_) on_mouse_(0, 0, 0, 0);
  buttons_ = 0;
  update_title();
}

bool DisplayWindow::clip_cursor() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
  return ClipCursor(&rc) != FALSE;
}

void DisplayWindow::update_title() {
  const wchar_t* hint = was_integrated_              ? L" - fare entegre"
                        : !on_mouse_ || !raw_mouse_ ? L""
                        : captured_                 ? L" - fareyi bırakmak için Ctrl+Alt"
                                                    : L" - fareyi yakalamak için tıklayın";
  wchar_t title[256];
  std::swprintf(title, 256, L"%ls - %ux%u%ls%ls", options_.title.c_str(), frame_.width, frame_.height, hint,
                paused_ ? L" [duraklatıldı]" : L"");
  SetWindowTextW(hwnd_, title);
}

bool DisplayWindow::integrated() {
  return !captured_ && options_.on_tablet && options_.tablet_active && options_.tablet_active();
}

RECT DisplayWindow::picture_rect() {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  if (!frame_.width || !frame_.height || rc.right <= 0 || rc.bottom <= 0) return rc;
  // Largest rectangle with the frame's aspect ratio, centered.
  const int64_t w = rc.right, h = rc.bottom;
  int64_t pw = w, ph = h;
  if (w * frame_.height > h * frame_.width) pw = h * frame_.width / frame_.height;
  else ph = w * frame_.height / frame_.width;
  const LONG left = static_cast<LONG>((w - pw) / 2), top = static_cast<LONG>((h - ph) / 2);
  return {left, top, left + static_cast<LONG>(pw), top + static_cast<LONG>(ph)};
}

void DisplayWindow::pointer(UINT msg, WPARAM wparam, LPARAM lparam) {
  POINT pt{static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam))};
  if (msg == WM_MOUSEWHEEL) ScreenToClient(hwnd_, &pt);  // wheel messages carry screen coordinates
  const RECT rc = picture_rect();
  const LONG w = std::max<LONG>(rc.right - rc.left - 1, 1), h = std::max<LONG>(rc.bottom - rc.top - 1, 1);
  tablet_x_ = static_cast<int>(std::clamp<LONG>(pt.x - rc.left, 0, w) * int64_t{kTabletMax} / w);
  tablet_y_ = static_cast<int>(std::clamp<LONG>(pt.y - rc.top, 0, h) * int64_t{kTabletMax} / h);
  const WPARAM keys = msg == WM_MOUSEWHEEL ? GET_KEYSTATE_WPARAM(wparam) : wparam;
  unsigned buttons = 0;
  if (keys & MK_LBUTTON) buttons |= 1;
  if (keys & MK_RBUTTON) buttons |= 2;
  if (keys & MK_MBUTTON) buttons |= 4;
  int dz = 0;
  if (msg == WM_MOUSEWHEEL) {
    wheel_ += GET_WHEEL_DELTA_WPARAM(wparam);
    dz = -(wheel_ / WHEEL_DELTA);
    wheel_ %= WHEEL_DELTA;
  }
  // Keep receiving the mouse while a button is held, so a drag that leaves the window ends properly.
  if (buttons && !tablet_buttons_) SetCapture(hwnd_);
  else if (!buttons && tablet_buttons_) ReleaseCapture();
  tablet_buttons_ = buttons;
  options_.on_tablet(tablet_x_, tablet_y_, buttons, dz);
}

void DisplayWindow::toggle_fullscreen() {
  if (!hwnd_) return;
  if (!fullscreen_) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    windowed_.length = sizeof(windowed_);
    if (!GetWindowPlacement(hwnd_, &windowed_) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
      return;
    }
    fullscreen_ = true;
    SetMenu(hwnd_, nullptr);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    fullscreen_ = false;
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    SetMenu(hwnd_, menu_);
    SetWindowPlacement(hwnd_, &windowed_);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  if (menu_) CheckMenuItem(menu_, kMenuFullscreen, fullscreen_ ? MF_CHECKED : MF_UNCHECKED);
  if (captured_) clip_cursor();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void DisplayWindow::build_menu() {
  const Actions& a = options_.actions;
  menu_ = CreateMenu();
  HMENU machine = CreatePopupMenu();
  if (a.ctrl_alt_del) AppendMenuW(machine, MF_STRING, kMenuCtrlAltDel, L"Ctrl+Alt+Del gönder\tCtrl+Alt+Insert");
  if (a.set_paused) AppendMenuW(machine, MF_STRING, kMenuPause, L"Duraklat");
  if (a.reset) AppendMenuW(machine, MF_STRING, kMenuReset, L"Sıfırla (reset)");
  AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
  if (a.power_button) AppendMenuW(machine, MF_STRING, kMenuPowerButton, L"Kapat (ACPI kapanma isteği)");
  if (a.power_off) AppendMenuW(machine, MF_STRING, kMenuPowerOff, L"Gücü kes");
  AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(machine), L"&Makine");
  if (a.insert_cd || a.eject_cd) {
    HMENU devices = CreatePopupMenu();
    if (a.insert_cd) AppendMenuW(devices, MF_STRING, kMenuInsertCd, L"CD/DVD kalıbı tak...");
    if (a.eject_cd) AppendMenuW(devices, MF_STRING, kMenuEjectCd, L"CD/DVD'yi çıkar");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), L"&Aygıtlar");
  }
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuFullscreen, L"Tam ekran\tCtrl+Alt+Enter");
  AppendMenuW(view, MF_STRING, kMenuScreenshot, L"Ekran görüntüsü kaydet...");
  AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&Görünüm");
}

void DisplayWindow::command(UINT id) {
  const Actions& a = options_.actions;
  switch (id) {
    case kMenuCtrlAltDel:
      if (a.ctrl_alt_del) a.ctrl_alt_del();
      break;
    case kMenuPause:
      if (!a.set_paused) break;
      paused_ = !paused_;
      a.set_paused(paused_);
      CheckMenuItem(menu_, kMenuPause, paused_ ? MF_CHECKED : MF_UNCHECKED);
      update_title();
      break;
    case kMenuReset:
      if (a.reset && MessageBoxW(hwnd_, L"Sanal makine sıfırlansın mı? Kaydedilmemiş veriler kaybolur.",
                                 options_.title.c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
        a.reset();
      }
      break;
    case kMenuPowerButton:
      if (a.power_button) a.power_button();
      break;
    case kMenuPowerOff:
      if (a.power_off && MessageBoxW(hwnd_, L"Sanal makinenin gücü kesilsin mi? Kaydedilmemiş veriler kaybolur.",
                                     options_.title.c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
        a.power_off();
      }
      break;
    case kMenuInsertCd: {
      if (!a.insert_cd) break;
      wchar_t file[MAX_PATH] = {};
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hwnd_;
      ofn.lpstrFilter = L"CD/DVD kalıbı (*.iso)\0*.iso\0Tüm dosyalar\0*.*\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (GetOpenFileNameW(&ofn)) a.insert_cd(file);
      break;
    }
    case kMenuEjectCd:
      if (a.eject_cd) a.eject_cd();
      break;
    case kMenuFullscreen:
      toggle_fullscreen();
      break;
    case kMenuScreenshot: {
      wchar_t file[MAX_PATH] = L"sandvm.bmp";
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = hwnd_;
      ofn.lpstrFilter = L"Bitmap (*.bmp)\0*.bmp\0";
      ofn.lpstrDefExt = L"bmp";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      if (GetSaveFileNameW(&ofn) && !write_bmp(file)) {
        MessageBoxW(hwnd_, L"Ekran görüntüsü kaydedilemedi.", options_.title.c_str(), MB_OK | MB_ICONERROR);
      }
      break;
    }
    default:
      break;
  }
}

void DisplayWindow::refresh() {
  vga_.render(&frame_);
  if (hwnd_) {
    // The guest started (or stopped) driving the tablet: switch between integration and capture.
    const bool tablet = options_.on_tablet && options_.tablet_active && options_.tablet_active();
    if (tablet && captured_) release_mouse();
    if (tablet != was_integrated_) {
      was_integrated_ = tablet;
      update_title();
    }
    if (fullscreen_) {
      shown_width_ = frame_.width;
      shown_height_ = frame_.height;
    } else if (frame_.width != shown_width_ || frame_.height != shown_height_) {
      shown_width_ = frame_.width;
      shown_height_ = frame_.height;
      // Small modes (320x200 VBE) get doubled so they stay readable.
      const unsigned scale = frame_.width < 640 && frame_.height < 400 ? 2 : 1;
      RECT rc{0, 0, static_cast<LONG>(frame_.width * scale), static_cast<LONG>(frame_.height * scale)};
      AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, menu_ != nullptr, 0);
      SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      update_title();
      if (captured_) clip_cursor();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
  if (!options_.screenshot_path.empty() && ++ticks_ % kScreenshotTicks == 0) save_screenshot();
}

void DisplayWindow::save_screenshot() {
  // Write a temporary file and rename it over the old one, so a reader never sees half a picture.
  const std::wstring tmp = options_.screenshot_path + L".tmp";
  if (!write_bmp(tmp) || !MoveFileExW(tmp.c_str(), options_.screenshot_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
  }
}

bool DisplayWindow::write_bmp(const std::wstring& path) {
  if (!frame_.width || !frame_.height) return false;
  BITMAPINFOHEADER info{};
  info.biSize = sizeof(info);
  info.biWidth = static_cast<LONG>(frame_.width);
  info.biHeight = -static_cast<LONG>(frame_.height);
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  const DWORD pixel_bytes = frame_.width * frame_.height * 4;
  BITMAPFILEHEADER file{};
  file.bfType = 0x4D42;  // "BM"
  file.bfOffBits = sizeof(file) + sizeof(info);
  file.bfSize = file.bfOffBits + pixel_bytes;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(h, &file, sizeof(file), &written, nullptr) &&
                  WriteFile(h, &info, sizeof(info), &written, nullptr) &&
                  WriteFile(h, frame_.pixels.data(), pixel_bytes, &written, nullptr);
  CloseHandle(h);
  return ok;
}

}  // namespace sandvm::cli
