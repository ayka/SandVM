#include "sbie/launcher.h"

#include <windows.h>

#include <stdexcept>
#include <string>

namespace sandvm::sbie {
namespace {

bool file_exists(const std::wstring& path) {
  DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring env(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring value(n, L'\0');
  value.resize(GetEnvironmentVariableW(name, value.data(), n));
  return value;
}

std::wstring self_path() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) throw std::runtime_error("GetModuleFileNameW failed");
    if (n < path.size()) {
      path.resize(n);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

// Quotes one argument following the CommandLineToArgvW rules.
std::wstring quote_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
    else out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

}  // namespace

bool running_in_sandbox() { return GetModuleHandleW(L"SbieDll.dll") != nullptr; }

std::optional<std::wstring> find_start_exe() {
  std::vector<std::wstring> dirs;
  if (auto dir = env(L"SANDVM_SBIE_DIR"); !dir.empty()) dirs.push_back(dir);
  if (auto pf = env(L"ProgramFiles"); !pf.empty()) {
    dirs.push_back(pf + L"\\Sandboxie-Plus");
    dirs.push_back(pf + L"\\Sandboxie");
  }
  for (const auto& dir : dirs) {
    std::wstring candidate = dir + L"\\Start.exe";
    if (file_exists(candidate)) return candidate;
  }
  return std::nullopt;
}

bool is_valid_box_name(const std::wstring& box) {
  if (box.empty() || box.size() > 32) return false;
  for (wchar_t c : box) {
    bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_';
    if (!ok) return false;
  }
  return true;
}

int relaunch_in_box(const std::wstring& box, const std::vector<std::wstring>& args) {
  if (!is_valid_box_name(box)) throw std::runtime_error("invalid Sandboxie box name");
  auto start = find_start_exe();
  if (!start) throw std::runtime_error("Sandboxie Start.exe not found (install Sandboxie-Plus or set SANDVM_SBIE_DIR)");

  std::wstring cmdline = quote_arg(*start) + L" /box:" + box + L" /wait " + quote_arg(self_path());
  for (const auto& a : args) cmdline += L" " + quote_arg(a);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(start->c_str(), cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    throw std::runtime_error("CreateProcessW(Start.exe) failed");
  }
  CloseHandle(pi.hThread);
  DWORD code = 0;
  const bool ok = WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code);
  const DWORD error = GetLastError();
  CloseHandle(pi.hProcess);
  if (!ok) throw std::runtime_error("waiting for Start.exe failed (error " + std::to_string(error) + ")");
  return static_cast<int>(code);
}

}  // namespace sandvm::sbie
