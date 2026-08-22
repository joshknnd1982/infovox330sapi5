// File logging shared by every Infovox 330 SAPI 5 binary.
//
// Every component writes to its own file under one directory so that a bug report is a
// single folder. Writes are flushed immediately: a crash mid-utterance must not lose the
// lines that explain it.
//
// Environment overrides (read once, at log_init):
//   INFOVOX330_LOG_DIR    directory to write into
//   INFOVOX330_LOG_LEVEL  off | error | warn | info | debug | trace

#pragma once

#include <windows.h>
#include <objbase.h>
#include <string>

namespace ivx {

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

// component is used as the log file's base name, e.g. L"sapi5-x86".
void log_init(const wchar_t* component);
void log_shutdown();

[[nodiscard]] bool log_enabled(LogLevel level);
[[nodiscard]] LogLevel log_level();
[[nodiscard]] const std::wstring& log_file_path();

void log_write(LogLevel level, const char* fmt, ...);

// Formats an HRESULT as "0x80045002" for log lines.
[[nodiscard]] std::string hresult_string(HRESULT hr);

// Formats a GUID as "{XXXXXXXX-...}" for log lines.
[[nodiscard]] std::string guid_string(const GUID& guid);

// Narrows a wide string for log lines, replacing anything unrepresentable.
[[nodiscard]] std::string log_narrow(const wchar_t* s);

}  // namespace ivx

#define IVX_LOG_E(...) ::ivx::log_write(::ivx::LogLevel::Error, __VA_ARGS__)
#define IVX_LOG_W(...) ::ivx::log_write(::ivx::LogLevel::Warn, __VA_ARGS__)
#define IVX_LOG_I(...) ::ivx::log_write(::ivx::LogLevel::Info, __VA_ARGS__)
#define IVX_LOG_D(...)                                          \
    do {                                                        \
        if (::ivx::log_enabled(::ivx::LogLevel::Debug)) {        \
            ::ivx::log_write(::ivx::LogLevel::Debug, __VA_ARGS__); \
        }                                                       \
    } while (0)
#define IVX_LOG_T(...)                                          \
    do {                                                        \
        if (::ivx::log_enabled(::ivx::LogLevel::Trace)) {        \
            ::ivx::log_write(::ivx::LogLevel::Trace, __VA_ARGS__); \
        }                                                       \
    } while (0)
