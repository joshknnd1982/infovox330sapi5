// Speaks through the registered SAPI 5 stack, out loud.
//
// Unlike ivx_sapitest, which loads the DLL directly, this goes the whole way round: SAPI
// enumerates voices, picks ours out of the registry, loads the engine and plays the result
// on the sound card. It is the post-install check, and the one that answers "can I hear it".
//
//   ivx_speak.exe --list
//   ivx_speak.exe [--voice <name fragment>] [--rate N] [--volume N] [text...]
//
// Only sapi.h is used, not sphelper.h: the helper header drags in ATL and a deprecated
// version check, and everything needed here is two interfaces wide.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring token_description(ISpObjectToken* token)
{
    LPWSTR value = nullptr;
    std::wstring result;
    if (SUCCEEDED(token->GetStringValue(nullptr, &value)) && value) {
        result = value;
        CoTaskMemFree(value);
        return result;
    }
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
        if (SUCCEEDED(attributes->GetStringValue(L"Name", &value)) && value) {
            result = value;
            CoTaskMemFree(value);
        }
        attributes->Release();
    }
    return result;
}

std::wstring token_attribute(ISpObjectToken* token, const wchar_t* name)
{
    std::wstring result;
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
        LPWSTR value = nullptr;
        if (SUCCEEDED(attributes->GetStringValue(name, &value)) && value) {
            result = value;
            CoTaskMemFree(value);
        }
        attributes->Release();
    }
    return result;
}

const wchar_t kEngineClsid[] = L"{61B158B8-3639-4B87-8944-9D55320C6F80}";

bool ours(ISpObjectToken* token)
{
    LPWSTR clsid = nullptr;
    const bool match = SUCCEEDED(token->GetStringValue(L"CLSID", &clsid)) && clsid &&
                       _wcsicmp(clsid, kEngineClsid) == 0;
    CoTaskMemFree(clsid);
    return match;
}

// Larry, then Roger, then Lucy, then whichever voice comes first.
int preference(ISpObjectToken* token)
{
    if (!token_attribute(token, L"InfovoxBaseSpeaker").empty()) {
        return 3;
    }
    const std::wstring speaker = token_attribute(token, L"InfovoxSpeaker");
    return speaker == L"Larry" ? 0 : (speaker == L"Roger" ? 1 : (speaker == L"Lucy" ? 2 : 3));
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    bool list_only = false;
    std::wstring voice_filter;
    std::wstring text;
    long rate = 0;
    int volume = 100;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (arg == L"--list") {
            list_only = true;
        } else if (arg == L"--voice") {
            voice_filter = next();
        } else if (arg == L"--rate") {
            rate = _wtol(next().c_str());
        } else if (arg == L"--volume") {
            volume = _wtoi(next().c_str());
        } else {
            if (!text.empty()) {
                text += L" ";
            }
            text += arg;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    ISpVoice* voice = nullptr;
    hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, __uuidof(ISpVoice),
                          reinterpret_cast<void**>(&voice));
    if (FAILED(hr) || !voice) {
        fwprintf(stderr, L"Could not create a SAPI voice: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
        fwprintf(stderr, L"SAPI 5 itself is unavailable, which is not something this "
                         L"installer controls.\n");
        return 1;
    }

    ISpObjectTokenCategory* category = nullptr;
    hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                          __uuidof(ISpObjectTokenCategory),
                          reinterpret_cast<void**>(&category));
    if (SUCCEEDED(hr) && category) {
        hr = category->SetId(SPCAT_VOICES, FALSE);
    }
    IEnumSpObjectTokens* tokens = nullptr;
    if (SUCCEEDED(hr) && category) {
        hr = category->EnumTokens(nullptr, nullptr, &tokens);
    }
    if (category) {
        category->Release();
    }
    if (FAILED(hr) || !tokens) {
        fwprintf(stderr, L"Could not enumerate voices: 0x%08lX\n", static_cast<unsigned long>(hr));
        voice->Release();
        return 1;
    }

    ULONG count = 0;
    tokens->GetCount(&count);

    ISpObjectToken* chosen = nullptr;
    int chosen_preference = 4;
    int infovox_count = 0;

    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken* token = nullptr;
        if (FAILED(tokens->Item(i, &token)) || !token) {
            continue;
        }
        const std::wstring description = token_description(token);
        const std::wstring language = token_attribute(token, L"Language");
        const std::wstring vendor = token_attribute(token, L"Vendor");
        const bool is_ours = ours(token);
        if (is_ours) {
            ++infovox_count;
        }

        if (list_only) {
            wprintf(L"%2lu %-46s lang=%-5s vendor=%s\n", i, description.c_str(), language.c_str(),
                    vendor.c_str());
        }

        const bool matches =
            is_ours && (voice_filter.empty() ||
                        description.find(voice_filter) != std::wstring::npos ||
                        token_attribute(token, L"InfovoxSpeaker") == voice_filter);
        const int rank = voice_filter.empty() ? preference(token) : 0;
        if (matches && rank < chosen_preference) {
            if (chosen) {
                chosen->Release();
            }
            chosen = token;
            chosen->AddRef();
            chosen_preference = rank;
        }
        token->Release();
    }
    tokens->Release();

    if (list_only) {
        wprintf(L"\n%lu voices installed, %d of them Infovox 330\n", count, infovox_count);
        if (chosen) {
            chosen->Release();
        }
        voice->Release();
        CoUninitialize();
        return infovox_count > 0 ? 0 : 1;
    }

    if (!chosen) {
        fwprintf(stderr, L"No matching Infovox 330 voice is registered.\n");
        fwprintf(stderr, L"Run with --list to see what SAPI can see.\n");
        voice->Release();
        CoUninitialize();
        return 1;
    }

    wprintf(L"Speaking with: %s\n", token_description(chosen).c_str());
    voice->SetVoice(chosen);
    voice->SetRate(rate);
    voice->SetVolume(static_cast<USHORT>(volume < 0 ? 0 : (volume > 100 ? 100 : volume)));

    if (text.empty()) {
        text = L"Infovox three thirty is installed and speaking through S A P I five.";
    }

    hr = voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
    voice->WaitUntilDone(INFINITE);

    if (FAILED(hr)) {
        fwprintf(stderr, L"Speak failed: 0x%08lX\n", static_cast<unsigned long>(hr));
    }

    chosen->Release();
    voice->Release();
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
