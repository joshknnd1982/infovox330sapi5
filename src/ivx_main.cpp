// DLL entry points for the Infovox 330 SAPI 5 engine.
//
// Registration is deliberately minimal: two CLSIDs and one TokenEnums key per speech
// catalogue. The voices themselves are never written to the registry - SAPI asks our
// enumerator for them, and the enumerator reads VoiceDescriptions.txt.

#include <new>

#include <windows.h>
#include <sapi.h>

#include "ivx_com.hpp"
#include "ivx_enum_tokens.hpp"
#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_registry.hpp"
#include "ivx_tts_engine.hpp"
#include "ivx_voices.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
ivx::com::class_object_factory g_class_factory;

// SAPI looks for third-party voice enumerators here. The classic key is what every SAPI 5
// application reads; the OneCore key is what the newer Windows speech stack reads, and
// registering in both costs one extra value.
constexpr const wchar_t* kTokenEnumPaths[] = {
    L"Software\\Microsoft\\Speech\\Voices\\TokenEnums",
    L"Software\\Microsoft\\Speech_OneCore\\Voices\\TokenEnums",
};

constexpr const wchar_t* kEnumeratorName = L"Infovox330";

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid)
{
    wchar_t buffer[64] = {0};
    StringFromGUID2(clsid, buffer, 64);
    return std::wstring(buffer);
}

void register_token_enumerator()
{
    using namespace ivx::sapi;
    using namespace ivx::registry;

    const std::wstring clsid = clsid_to_string(__uuidof(IEnumSpObjectTokensImpl));

    for (const wchar_t* path : kTokenEnumPaths) {
        try {
            key enums(HKEY_LOCAL_MACHINE, path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
            key entry(enums, kEnumeratorName, KEY_SET_VALUE, true);
            entry.set(L"Infovox 330 Voices");
            entry.set(L"CLSID", clsid);
            IVX_LOG_I("registered voice enumerator under %s", ivx::log_narrow(path).c_str());
        }
        catch (const std::exception&) {
            // Speech_OneCore does not exist on every Windows edition. Missing it is not a
            // failure; missing the classic key is, and that one is reported by the caller.
            IVX_LOG_W("could not register the enumerator under %s", ivx::log_narrow(path).c_str());
        }
    }
}

void unregister_token_enumerator() noexcept
{
    using namespace ivx::registry;

    for (const wchar_t* path : kTokenEnumPaths) {
        try {
            key enums(HKEY_LOCAL_MACHINE, path, KEY_ALL_ACCESS);
            enums.delete_subkey(kEnumeratorName);
        }
        catch (...) {
        }
    }
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_dll_handle = instance;
        DisableThreadLibraryCalls(instance);

#ifdef _WIN64
        ivx::log_init(L"sapi5-x64");
#else
        ivx::log_init(L"sapi5-x86");
#endif
        IVX_LOG_I("DLL attached to process");

        try {
            g_class_factory.register_class<ivx::sapi::IEnumSpObjectTokensImpl>();
            g_class_factory.register_class<ivx::sapi::ISpTTSEngineImpl>();
        }
        catch (...) {
            IVX_LOG_E("failed to build the class factory");
            return FALSE;
        }
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    return g_class_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return ivx::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        IVX_LOG_I("DllRegisterServer");
        ivx::com::class_registrar registrar(g_dll_handle);
        registrar.register_class<ivx::sapi::IEnumSpObjectTokensImpl>();
        registrar.register_class<ivx::sapi::ISpTTSEngineImpl>();
        register_token_enumerator();

        const std::size_t voices = ivx::voice_catalogue().size();
        IVX_LOG_I("registration complete; %zu voices are visible from %s", voices,
                  ivx::log_narrow(ivx::data_root().c_str()).c_str());
        if (voices == 0) {
            IVX_LOG_E("registered, but no voices were found - check the data files");
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        IVX_LOG_E("DllRegisterServer failed");
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer()
{
    try {
        IVX_LOG_I("DllUnregisterServer");
        unregister_token_enumerator();
        ivx::com::class_registrar registrar(g_dll_handle);
        registrar.unregister_class<ivx::sapi::IEnumSpObjectTokensImpl>();
        registrar.unregister_class<ivx::sapi::ISpTTSEngineImpl>();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        // Unregistering something that was never registered is not worth failing over.
        return S_OK;
    }
}
