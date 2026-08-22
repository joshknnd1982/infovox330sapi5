// SAPI 4 interface definitions needed to drive the Infovox 330 engine.
//
// These are declared locally rather than pulled from the Microsoft SAPI 4 SDK so that
// nothing here depends on the SAPI 4 runtime being installed or registered. The layouts
// mirror the ones the NVDA add-on drives the engine with (synthDrivers32/_infovox_sapi4.py),
// which is the authoritative reference for what this particular engine accepts.

#pragma once

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

namespace ivx {
namespace sapi4 {

inline constexpr int kSvfnLen = 262;
inline constexpr int kLangLen = 64;

using QWORD = unsigned __int64;

// TextData / Phoneme character sets.
enum VOICECHARSET : int {
    CHARSET_TEXT = 0,
    CHARSET_IPAPHONETIC = 1,
    CHARSET_ENGINEPHONETIC = 2,
};

// TextData flags.
inline constexpr DWORD TTSDATAFLAG_TAGGED = 1;

// Attribute ranges. The engine clamps these to whatever it actually supports, which is how
// the real minimum and maximum are discovered (set to the extreme, then read back).
inline constexpr WORD  TTSATTR_MINPITCH  = 0;
inline constexpr WORD  TTSATTR_MAXPITCH  = 0xFFFF;
inline constexpr DWORD TTSATTR_MINSPEED  = 0;
inline constexpr DWORD TTSATTR_MAXSPEED  = 0xFFFFFFFF;
inline constexpr DWORD TTSATTR_MINVOLUME = 0;
inline constexpr DWORD TTSATTR_MAXVOLUME = 0xFFFFFFFF;

// TTSMODEINFOW::dwFeatures bits.
inline constexpr DWORD TTSFEATURE_VOLUME     = 2;
inline constexpr DWORD TTSFEATURE_SPEED      = 4;
inline constexpr DWORD TTSFEATURE_PITCH      = 8;
inline constexpr DWORD TTSFEATURE_FIXEDAUDIO = 1024;

// IAudioDestNotifySink::AudioStop reason codes.
inline constexpr WORD IANSRSN_NODATA = 0;

// SAPI 4 audio HRESULTs. Spelled as signed decimals so there is no chance of transcribing
// the hex wrong; these are the exact values the engine tests against.
inline constexpr HRESULT AUDERR_BADDEVICEID              = static_cast<HRESULT>(-2147220735);
inline constexpr HRESULT AUDERR_NEEDWAVEFORMAT           = static_cast<HRESULT>(-2147220734);
inline constexpr HRESULT AUDERR_NOTENOUGHDATA            = static_cast<HRESULT>(-2147220991);
inline constexpr HRESULT AUDERR_NOTPLAYING               = static_cast<HRESULT>(-2147220730);
inline constexpr HRESULT AUDERR_WAVEFORMATNOTSUPPORTED   = static_cast<HRESULT>(-2147220990);
inline constexpr HRESULT AUDERR_WAVEDEVICEBUSY           = static_cast<HRESULT>(-2147220989);
inline constexpr HRESULT AUDERR_INVALIDNOTIFYSINK        = static_cast<HRESULT>(-2147220711);
inline constexpr HRESULT AUDERR_ALREADYCLAIMED           = static_cast<HRESULT>(-2147220707);
inline constexpr HRESULT AUDERR_NOTCLAIMED               = static_cast<HRESULT>(-2147220706);
inline constexpr HRESULT AUDERR_STILLPLAYING             = static_cast<HRESULT>(-2147220705);
inline constexpr HRESULT AUDERR_ALREADYSTARTED           = static_cast<HRESULT>(-2147220704);

#pragma pack(push, 4)

struct LANGUAGEW {
    LANGID LanguageID;
    WCHAR  szDialect[kLangLen];
};

struct TTSMODEINFOW {
    GUID      gEngine;
    WCHAR     szMfgName[kSvfnLen];
    WCHAR     szProductName[kSvfnLen];
    GUID      gModeID;
    WCHAR     szModeName[kSvfnLen];
    LANGUAGEW language;
    WCHAR     szSpeaker[kSvfnLen];
    WCHAR     szStyle[kSvfnLen];
    WORD      wGender;
    WORD      wAge;
    DWORD     dwFeatures;
    DWORD     dwInterfaces;
    DWORD     dwEngineFeatures;
};

struct SDATA {
    void* pData;
    DWORD dwSize;
};

struct TTSMOUTH {
    BYTE bMouthHeight;
    BYTE bMouthWidth;
    BYTE bMouthUpturn;
    BYTE bJawOpen;
    BYTE bTeethUpperVisible;
    BYTE bTeethLowerVisible;
    BYTE bTonguePosn;
    BYTE bLipTension;
};

#pragma pack(pop)

static_assert(sizeof(LANGUAGEW) == 130, "LANGUAGEW layout must match the engine's");
static_assert(sizeof(TTSMODEINFOW) == 2800, "TTSMODEINFOW layout must match the engine's");

struct ITTSCentralW;

struct __declspec(uuid("{6B837B20-4A47-101B-931A-00AA0047BA4F}")) ITTSEnumW : public IUnknown {
    STDMETHOD(Next)(ULONG cModeInfo, TTSMODEINFOW* pModeInfo, ULONG* pcFetched) = 0;
    STDMETHOD(Skip)(ULONG cSkip) = 0;
    STDMETHOD(Reset)() = 0;
    STDMETHOD(Clone)(ITTSEnumW** ppEnum) = 0;
    STDMETHOD(Select)(GUID gModeID, ITTSCentralW** ppCentral, IUnknown* pAudioDest) = 0;
};

struct __declspec(uuid("{28016060-4A47-101B-931A-00AA0047BA4F}")) ITTSCentralW : public IUnknown {
    STDMETHOD(Inject)(LPCWSTR pszString) = 0;
    STDMETHOD(ModeGet)(TTSMODEINFOW* pModeInfo) = 0;
    STDMETHOD(Phoneme)(VOICECHARSET CharSet, DWORD dwFlags, SDATA dPhone, SDATA* pdConvert) = 0;
    STDMETHOD(PosnGet)(QWORD* pqTimeStamp) = 0;
    STDMETHOD(TextData)(VOICECHARSET CharSet, DWORD dwFlags, SDATA dText,
                        IUnknown* pTTSBufNotifySink, GUID iidNotify) = 0;
    STDMETHOD(ToFileTime)(QWORD* pqTimeStamp, FILETIME* pFT) = 0;
    STDMETHOD(AudioPause)() = 0;
    STDMETHOD(AudioResume)() = 0;
    STDMETHOD(AudioReset)() = 0;
    STDMETHOD(Register)(void* pNotifyInterface, GUID iidNotify, DWORD* pdwKey) = 0;
    STDMETHOD(UnRegister)(DWORD dwKey) = 0;
};

struct __declspec(uuid("{1287A280-4A47-101B-931A-00AA0047BA4F}")) ITTSAttributesW : public IUnknown {
    STDMETHOD(PitchGet)(WORD* pwPitch) = 0;
    STDMETHOD(PitchSet)(WORD wPitch) = 0;
    STDMETHOD(RealTimeGet)(DWORD* pdwRealTime) = 0;
    STDMETHOD(RealTimeSet)(DWORD dwRealTime) = 0;
    STDMETHOD(SpeedGet)(DWORD* pdwSpeed) = 0;
    STDMETHOD(SpeedSet)(DWORD dwSpeed) = 0;
    STDMETHOD(VolumeGet)(DWORD* pdwVolume) = 0;
    STDMETHOD(VolumeSet)(DWORD dwVolume) = 0;
};

struct __declspec(uuid("{C0FA8F40-4A46-101B-931A-00AA0047BA4F}")) ITTSNotifySinkW : public IUnknown {
    STDMETHOD(AttribChanged)(DWORD dwAttribute) = 0;
    STDMETHOD(AudioStart)(QWORD qTimeStamp) = 0;
    STDMETHOD(AudioStop)(QWORD qTimeStamp) = 0;
    STDMETHOD(Visual)(QWORD qTimeStamp, WCHAR cIPAPhoneme, WCHAR cEnginePhoneme,
                      DWORD dwHints, TTSMOUTH* pTTSMouth) = 0;
};

struct __declspec(uuid("{E4963D40-C743-11cd-80E5-00AA003E4B50}")) ITTSBufNotifySink : public IUnknown {
    STDMETHOD(TextDataDone)(QWORD qTimeStamp, DWORD dwFlags) = 0;
    STDMETHOD(TextDataStarted)(QWORD qTimeStamp) = 0;
    STDMETHOD(BookMark)(QWORD qTimeStamp, DWORD dwMarkNum) = 0;
    STDMETHOD(WordPosition)(QWORD qTimeStamp, DWORD dwByteOffset) = 0;
};

struct __declspec(uuid("{F546B340-C743-11cd-80E5-00AA003E4B50}")) IAudio : public IUnknown {
    STDMETHOD(Flush)() = 0;
    STDMETHOD(LevelGet)(DWORD* pdwLevel) = 0;
    STDMETHOD(LevelSet)(DWORD dwLevel) = 0;
    STDMETHOD(PassNotify)(void* pNotifyInterface, GUID iidNotify) = 0;
    STDMETHOD(PosnGet)(QWORD* pqTimeStamp) = 0;
    STDMETHOD(Claim)() = 0;
    STDMETHOD(UnClaim)() = 0;
    STDMETHOD(Start)() = 0;
    STDMETHOD(Stop)() = 0;
    STDMETHOD(TotalGet)(QWORD* pqWord) = 0;
    STDMETHOD(ToFileTime)(QWORD* pqWord, FILETIME* pFT) = 0;
    STDMETHOD(WaveFormatGet)(SDATA* pdWFEX) = 0;
    STDMETHOD(WaveFormatSet)(SDATA dWFEX) = 0;
};

struct __declspec(uuid("{2EC34DA0-C743-11cd-80E5-00AA003E4B50}")) IAudioDest : public IUnknown {
    STDMETHOD(FreeSpace)(DWORD* pdwBytes, BOOL* pfEOF) = 0;
    STDMETHOD(DataSet)(void* pBuffer, DWORD dwSize) = 0;
    STDMETHOD(BookMark)(DWORD dwMarkID) = 0;
};

struct __declspec(uuid("{ACB08C00-C743-11cd-80E5-00AA003E4B50}")) IAudioDestNotifySink : public IUnknown {
    STDMETHOD(AudioStop)(WORD wReason) = 0;
    STDMETHOD(AudioStart)() = 0;
    STDMETHOD(FreeSpace)(DWORD dwBytes, BOOL fEOF) = 0;
    STDMETHOD(BookMark)(DWORD dwMarkID, BOOL fFlushed) = 0;
};

}  // namespace sapi4
}  // namespace ivx
