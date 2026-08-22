// Locating the engine, the voice data and the 32-bit helper, without the registry.
//
// Every path is derived from the location of the calling binary. The layout the installer
// produces is:
//
//   <root>\Infovox330SAPI5.dll      32-bit SAPI 5 engine
//   <root>\Infovox330Server.exe     32-bit helper, used by the 64-bit engine
//   <root>\infovox_host.dll         registry-virtualising shim (32-bit)
//   <root>\x64\Infovox330SAPI5.dll  64-bit SAPI 5 engine
//   <root>\Ivx330\Ivx330nt.dll      the engine proper
//   <root>\Voices Ivx330\           voice data + VoiceDescriptions.txt
//
// so the 64-bit DLL has to look one directory up. INFOVOX330_DATA_DIR overrides the search
// entirely, which is what the sample renderer and the test harness use.

#pragma once

#include <windows.h>
#include <string>

namespace ivx {

// Directory containing the given module, without a trailing backslash. Pass the handle of
// the current DLL, or nullptr for the running executable.
[[nodiscard]] std::wstring module_directory(HMODULE module);

// Directory containing the module that this function was linked into.
[[nodiscard]] std::wstring own_module_directory();

// The directory holding Ivx330\ and Voices Ivx330\. Empty if it could not be found.
// Resolved once and cached.
[[nodiscard]] const std::wstring& data_root();

// Overrides the cached data root. Used by the sample renderer and the tests.
void set_data_root(const std::wstring& root);

[[nodiscard]] std::wstring engine_dll_path();   // <root>\Ivx330\Ivx330nt.dll
[[nodiscard]] std::wstring voices_dir();        // <root>\Voices Ivx330
[[nodiscard]] std::wstring host_dll_path();     // infovox_host.dll (beside the 32-bit binary)
[[nodiscard]] std::wstring server_exe_path();   // <root>\Infovox330Server.exe

[[nodiscard]] bool file_exists(const std::wstring& path);

}  // namespace ivx
