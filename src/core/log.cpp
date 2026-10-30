#include "core/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace sandvm {
namespace {
std::atomic<LogLevel> g_level{LogLevel::Info};
std::mutex g_mutex;

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
  }
  return "?";
}
}  // namespace

void set_log_level(LogLevel level) { g_level = level; }
LogLevel log_level() { return g_level.load(); }

void log(LogLevel level, const char* fmt, ...) {
  if (level < g_level.load()) return;
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "[sandvm:%s] ", level_name(level));
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
}

}  // namespace sandvm
