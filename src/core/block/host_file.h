#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sandvm::block {

// A host file with positioned I/O (OVERLAPPED offsets on a synchronous handle: no shared file pointer,
// so concurrent calls need no lock). Shared for reads only, so two writers cannot open one image, while
// several read-only users (a base image under linked clones) can.
class HostFile {
 public:
  // Both throw std::runtime_error with the path and the Win32 error.
  static std::unique_ptr<HostFile> open(const std::wstring& path, bool writable);
  // Fails if the file exists.
  static std::unique_ptr<HostFile> create(const std::wstring& path);
  ~HostFile();
  HostFile(const HostFile&) = delete;
  HostFile& operator=(const HostFile&) = delete;

  const std::wstring& path() const { return path_; }
  bool writable() const { return writable_; }
  // Current size from the file system (it changes as images grow).
  uint64_t size() const;
  // Full transfers only: false on an error or a short read past the end of the file.
  bool read(uint64_t offset, void* data, size_t len) const;
  bool write(uint64_t offset, const void* data, size_t len);
  bool flush();
  bool set_size(uint64_t size);
  // NTFS sparse file: ranges never written take no space and read as zeros.
  bool set_sparse();
  uint64_t last_write_time() const;  // FILETIME as an integer, 0 if unknown

 private:
  HostFile(HANDLE handle, std::wstring path, bool writable)
      : handle_(handle), path_(std::move(path)), writable_(writable) {}

  HANDLE handle_;
  const std::wstring path_;
  const bool writable_;
};

std::string narrow(const std::wstring& s);  // UTF-8, for messages
std::wstring widen_utf8(const std::string& s);
// Directory part including the trailing separator ("" if none).
std::wstring directory_of(const std::wstring& path);
std::wstring file_name_of(const std::wstring& path);
std::wstring absolute_path(const std::wstring& path);
bool is_absolute_path(const std::wstring& path);

}  // namespace sandvm::block
