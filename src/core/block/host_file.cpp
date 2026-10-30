#include "core/block/host_file.h"

#include <winioctl.h>

#include <stdexcept>
#include <vector>

namespace sandvm::block {
namespace {
constexpr DWORD kMaxChunk = 1u << 30;

std::runtime_error file_error(const char* what, const std::wstring& path) {
  return std::runtime_error(std::string(what) + " " + narrow(path) + " (error " + std::to_string(GetLastError()) + ")");
}
}  // namespace

std::unique_ptr<HostFile> HostFile::open(const std::wstring& path, bool writable) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw file_error("cannot open", path);
  return std::unique_ptr<HostFile>(new HostFile(h, path, writable));
}

std::unique_ptr<HostFile> HostFile::create(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) throw file_error("cannot create", path);
  return std::unique_ptr<HostFile>(new HostFile(h, path, true));
}

HostFile::~HostFile() { CloseHandle(handle_); }

uint64_t HostFile::size() const {
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle_, &size)) return 0;
  return static_cast<uint64_t>(size.QuadPart);
}

bool HostFile::read(uint64_t offset, void* data, size_t len) const {
  auto* p = static_cast<uint8_t*>(data);
  while (len > 0) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    const DWORD chunk = static_cast<DWORD>(len > kMaxChunk ? kMaxChunk : len);
    DWORD done = 0;
    if (!ReadFile(handle_, p, chunk, &done, &ov) || done == 0) return false;
    p += done;
    offset += done;
    len -= done;
  }
  return true;
}

bool HostFile::write(uint64_t offset, const void* data, size_t len) {
  if (!writable_) return false;
  auto* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(offset);
    ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    const DWORD chunk = static_cast<DWORD>(len > kMaxChunk ? kMaxChunk : len);
    DWORD done = 0;
    if (!WriteFile(handle_, p, chunk, &done, &ov) || done == 0) return false;
    p += done;
    offset += done;
    len -= done;
  }
  return true;
}

bool HostFile::flush() { return !writable_ || FlushFileBuffers(handle_); }

bool HostFile::set_size(uint64_t size) {
  FILE_END_OF_FILE_INFO info{};
  info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
  return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof(info));
}

bool HostFile::set_sparse() {
  DWORD returned = 0;
  return DeviceIoControl(handle_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
}

uint64_t HostFile::last_write_time() const {
  FILETIME t{};
  if (!GetFileTime(handle_, nullptr, nullptr, &t)) return 0;
  return (uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime;
}

std::string narrow(const std::wstring& s) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n : 0, '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring widen_utf8(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n > 0 ? n : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::wstring directory_of(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::wstring file_name_of(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring absolute_path(const std::wstring& path) {
  const DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (n == 0) return path;
  std::wstring out(n, L'\0');
  const DWORD written = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
  if (written == 0 || written >= n) return path;
  out.resize(written);
  return out;
}

bool is_absolute_path(const std::wstring& path) {
  return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
         (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'));
}

}  // namespace sandvm::block
