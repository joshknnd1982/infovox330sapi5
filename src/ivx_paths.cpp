#include "ivx_paths.hpp"

#include "ivx_log.hpp"

#include <mutex>
#include <vector>

namespace ivx {
namespace {

std::mutex g_root_mutex;
std::wstring g_root;
bool g_root_resolved = false;

[[nodiscard]] std::wstring strip_trailing_slash(std::wstring s)
{
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

[[nodiscard]] std::wstring parent_of(const std::wstring& dir)
{
    const std::size_t pos = dir.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos < 2) {
        return {};
    }
    return dir.substr(0, pos);
}

[[nodiscard]] bool looks_like_data_root(const std::wstring& dir)
{
    if (dir.empty()) {
        return false;
    }
    return file_exists(dir + L"\\Ivx330\\Ivx330nt.dll") &&
           file_exists(dir + L"\\Voices Ivx330\\VoiceDescriptions.txt");
}

[[nodiscard]] std::wstring env_value(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        return {};
    }
    return strip_trailing_slash(std::wstring(buf, n));
}

void resolve_root()
{
    const std::wstring from_env = env_value(L"INFOVOX330_DATA_DIR");
    if (!from_env.empty()) {
        g_root = from_env;
        IVX_LOG_I("data root from INFOVOX330_DATA_DIR: %s", log_narrow(g_root.c_str()).c_str());
        if (!looks_like_data_root(g_root)) {
            IVX_LOG_W("INFOVOX330_DATA_DIR does not contain Ivx330\\Ivx330nt.dll");
        }
        return;
    }

    const std::wstring here = own_module_directory();
    std::wstring candidate = here;
    for (int depth = 0; depth < 3 && !candidate.empty(); ++depth) {
        if (looks_like_data_root(candidate)) {
            g_root = candidate;
            IVX_LOG_I("data root resolved to %s", log_narrow(g_root.c_str()).c_str());
            return;
        }
        candidate = parent_of(candidate);
    }

    IVX_LOG_E("could not find Ivx330\\Ivx330nt.dll near %s", log_narrow(here.c_str()).c_str());
    g_root.clear();
}

}  // namespace

bool file_exists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring module_directory(HMODULE module)
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size() - 1) {
            break;
        }
        buf.resize(buf.size() * 2);
    }
    std::wstring path(buf.data());
    const std::size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos) {
        return {};
    }
    return path.substr(0, pos);
}

std::wstring own_module_directory()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&own_module_directory), &self)) {
        self = nullptr;
    }
    return module_directory(self);
}

const std::wstring& data_root()
{
    std::lock_guard<std::mutex> lock(g_root_mutex);
    if (!g_root_resolved) {
        resolve_root();
        g_root_resolved = true;
    }
    return g_root;
}

void set_data_root(const std::wstring& root)
{
    {
        std::lock_guard<std::mutex> lock(g_root_mutex);
        g_root = strip_trailing_slash(root);
        g_root_resolved = true;
    }
    IVX_LOG_I("data root overridden to %s", log_narrow(g_root.c_str()).c_str());
}

std::wstring engine_dll_path()
{
    const std::wstring& root = data_root();
    return root.empty() ? std::wstring() : root + L"\\Ivx330\\Ivx330nt.dll";
}

std::wstring voices_dir()
{
    const std::wstring& root = data_root();
    return root.empty() ? std::wstring() : root + L"\\Voices Ivx330";
}

std::wstring host_dll_path()
{
    // The shim sits beside whichever 32-bit binary is loading it; fall back to the data root
    // so that a renderer run out of a build tree still finds the shipped copy.
    const std::wstring here = own_module_directory();
    if (!here.empty() && file_exists(here + L"\\infovox_host.dll")) {
        return here + L"\\infovox_host.dll";
    }
    const std::wstring& root = data_root();
    if (!root.empty() && file_exists(root + L"\\infovox_host.dll")) {
        return root + L"\\infovox_host.dll";
    }
    return here.empty() ? std::wstring(L"infovox_host.dll") : here + L"\\infovox_host.dll";
}

std::wstring server_exe_path()
{
    const std::wstring here = own_module_directory();
    if (!here.empty() && file_exists(here + L"\\Infovox330Server.exe")) {
        return here + L"\\Infovox330Server.exe";
    }
    const std::wstring parent = parent_of(here);
    if (!parent.empty() && file_exists(parent + L"\\Infovox330Server.exe")) {
        return parent + L"\\Infovox330Server.exe";
    }
    const std::wstring& root = data_root();
    if (!root.empty()) {
        return root + L"\\Infovox330Server.exe";
    }
    return {};
}

}  // namespace ivx
