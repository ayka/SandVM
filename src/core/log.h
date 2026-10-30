#pragma once

namespace sandvm {

enum class LogLevel { Debug, Info, Warn, Error };

void set_log_level(LogLevel level);
LogLevel log_level();
void log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

}  // namespace sandvm

#define SVM_DEBUG(...) ::sandvm::log(::sandvm::LogLevel::Debug, __VA_ARGS__)
#define SVM_INFO(...) ::sandvm::log(::sandvm::LogLevel::Info, __VA_ARGS__)
#define SVM_WARN(...) ::sandvm::log(::sandvm::LogLevel::Warn, __VA_ARGS__)
#define SVM_ERROR(...) ::sandvm::log(::sandvm::LogLevel::Error, __VA_ARGS__)
