#include "ivx_log.hpp"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ivx {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Debug;
std::wstring g_path;
bool g_initialised = false;

[[nodiscard]] std::wstring env_value(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        return {};
    }
    return std::wstring(buf, n);
}

[[nodiscard]] LogLevel parse_level(const std::wstring& s, LogLevel fallback)
{
    if (s.empty()) {
        return fallback;
    }
    if (_wcsicmp(s.c_str(), L"off") == 0) return LogLevel::Off;
    if (_wcsicmp(s.c_str(), L"error") == 0) return LogLevel::Error;
    if (_wcsicmp(s.c_str(), L"warn") == 0) return LogLevel::Warn;
    if (_wcsicmp(s.c_str(), L"info") == 0) return LogLevel::Info;
    if (_wcsicmp(s.c_str(), L"debug") == 0) return LogLevel::Debug;
    if (_wcsicmp(s.c_str(), L"trace") == 0) return LogLevel::Trace;
    return fallback;
}

[[nodiscard]] const char* level_tag(LogLevel level)
{
    switch (level) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Trace: return "TRACE";
        default:              return "?????";
    }
}

// Creates every missing component of an absolute directory path.
void make_directories(const std::wstring& path)
{
    for (std::size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        }
    }
}

[[nodiscard]] std::wstring default_log_dir()
{
    wchar_t* known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        dir = known;
    }
    if (known) {
        CoTaskMemFree(known);
    }
    if (dir.empty()) {
        wchar_t temp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, temp) > 0) {
            dir = temp;
            while (!dir.empty() && dir.back() == L'\\') {
                dir.pop_back();
            }
        }
    }
    if (dir.empty()) {
        dir = L"C:";
    }
    return dir + L"\\Infovox330 SAPI5\\Logs";
}

}  // namespace

void log_init(const wchar_t* component)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialised) {
        return;
    }
    g_initialised = true;

    g_level = parse_level(env_value(L"INFOVOX330_LOG_LEVEL"), LogLevel::Debug);
    if (g_level == LogLevel::Off) {
        return;
    }

    std::wstring dir = env_value(L"INFOVOX330_LOG_DIR");
    if (dir.empty()) {
        dir = default_log_dir();
    }
    while (!dir.empty() && dir.back() == L'\\') {
        dir.pop_back();
    }
    make_directories(dir);

    wchar_t name[MAX_PATH];
    _snwprintf_s(name, _TRUNCATE, L"%s\\infovox330-%s-%lu.log", dir.c_str(),
                 component ? component : L"unknown", GetCurrentProcessId());
    g_path = name;

    if (_wfopen_s(&g_file, g_path.c_str(), L"a, ccs=UTF-8") != 0) {
        g_file = nullptr;
        return;
    }

    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_file,
             L"\n==== Infovox 330 SAPI5 [%s] started %04u-%02u-%02u %02u:%02u:%02u "
             L"pid=%lu host=%s ====\n",
             component ? component : L"unknown", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, GetCurrentProcessId(), exe);
    fflush(g_file);
}

void log_shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fwprintf(g_file, L"==== log closed ====\n");
        fclose(g_file);
        g_file = nullptr;
    }
}

bool log_enabled(LogLevel level)
{
    return static_cast<int>(level) <= static_cast<int>(g_level);
}

LogLevel log_level()
{
    return g_level;
}

const std::wstring& log_file_path()
{
    return g_path;
}

void log_write(LogLevel level, const char* fmt, ...)
{
    if (!log_enabled(level)) {
        return;
    }

    char message[4096];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_file, L"%02u:%02u:%02u.%03u [%5lu] %S %S\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, GetCurrentThreadId(), level_tag(level), message);
    fflush(g_file);
}

std::string hresult_string(HRESULT hr)
{
    char buf[16];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string guid_string(const GUID& guid)
{
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3,
                guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}

std::string log_narrow(const wchar_t* s)
{
    if (!s || !*s) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::vector<char> buf(static_cast<std::size_t>(needed));
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf.data(), needed, nullptr, nullptr);
    return std::string(buf.data());
}

}  // namespace ivx
