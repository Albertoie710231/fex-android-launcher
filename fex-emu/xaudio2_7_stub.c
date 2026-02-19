/**
 * Stub DLL for xaudio2_7.dll (XAudio2 2.7 COM server)
 *
 * FAudio's xaudio2_7.dll crashes with ACCESS_VIOLATION under FEX-Emu.
 * This stub provides a minimal COM server that returns mock IXAudio2
 * and IXAudio2Voice objects. All methods return S_OK and produce no audio.
 *
 * The game (Ys IX) loads XAudio2 via CoCreateInstance. Wine's COM system
 * calls DllGetClassObject on xaudio2_7.dll → IClassFactory::CreateInstance
 * → returns our mock IXAudio2.
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -shared -o xaudio2_7.dll xaudio2_7_stub.c \
 *       xaudio2_7_stub.def -O2 -s -lole32
 */

#include <windows.h>
#include <stdio.h>

/* S_OK and S_FALSE are already defined in winerror.h (via windows.h) */

/* ========================================================================
 * Mock Voice Objects (IXAudio2MasteringVoice, IXAudio2SourceVoice)
 *
 * IXAudio2Voice: 19 vtable entries (GetVoiceDetails .. DestroyVoice)
 * IXAudio2SourceVoice extends with 10 more (Start .. SetSourceSampleRate)
 * IXAudio2MasteringVoice: no additional methods
 *
 * NOTE: Voice interfaces do NOT inherit IUnknown. No QI/AddRef/Release.
 * All methods return 0 (S_OK for HRESULT, no-op for void).
 * ======================================================================== */

static long long voice_noop(void) { return 0; }

static void *voice_vtable[32];
typedef struct { void **vptr; } MockVoice;

static MockVoice mock_mastering_voice;
static MockVoice mock_source_voice;

/* ========================================================================
 * Mock IXAudio2 COM Object
 *
 * vtable layout (XAudio2 2.7, inherits IUnknown):
 *  [0]  QueryInterface
 *  [1]  AddRef
 *  [2]  Release
 *  [3]  GetDeviceCount
 *  [4]  GetDeviceDetails
 *  [5]  Initialize
 *  [6]  RegisterForCallbacks
 *  [7]  UnregisterForCallbacks
 *  [8]  CreateSourceVoice
 *  [9]  CreateSubmixVoice
 *  [10] CreateMasteringVoice
 *  [11] StartEngine
 *  [12] StopEngine
 *  [13] CommitChanges
 *  [14] GetPerformanceData
 *  [15] SetDebugConfiguration
 * ======================================================================== */

typedef struct {
    void **vptr;
    volatile LONG refcount;
} MockXAudio2;

/* IUnknown */
static HRESULT __stdcall xa2_QueryInterface(MockXAudio2 *this, const GUID *riid, void **ppv)
{
    if (!ppv) return 0x80004002L; /* E_NOINTERFACE */
    *ppv = this;
    InterlockedIncrement(&this->refcount);
    return S_OK;
}

static ULONG __stdcall xa2_AddRef(MockXAudio2 *this)
{
    return InterlockedIncrement(&this->refcount);
}

static ULONG __stdcall xa2_Release(MockXAudio2 *this)
{
    LONG ref = InterlockedDecrement(&this->refcount);
    if (ref <= 0) this->refcount = 1; /* singleton — never free */
    return ref > 0 ? ref : 1;
}

/* IXAudio2 */
static HRESULT __stdcall xa2_GetDeviceCount(MockXAudio2 *this, UINT *pCount)
{
    if (pCount) *pCount = 1;
    return S_OK;
}

static HRESULT __stdcall xa2_GetDeviceDetails(MockXAudio2 *this, UINT Index, void *pDetails)
{
    /* XAUDIO2_DEVICE_DETAILS is ~300 bytes. Zero it to provide safe defaults. */
    if (pDetails) memset(pDetails, 0, 300);
    return S_OK;
}

static HRESULT __stdcall xa2_Initialize(MockXAudio2 *this, UINT Flags, UINT Processor)
{
    fprintf(stderr, "[XAudio2Stub] Initialize(flags=0x%x) -> S_OK\n", Flags);
    fflush(stderr);
    return S_OK;
}

static HRESULT __stdcall xa2_RegisterForCallbacks(void *this, void *pCb) { return S_OK; }
static HRESULT __stdcall xa2_UnregisterForCallbacks(void *this, void *pCb) { return S_OK; }

static HRESULT __stdcall xa2_CreateSourceVoice(MockXAudio2 *this, void **ppVoice,
    void *pFmt, UINT Flags, float MaxFreq, void *pCb, void *pSend, void *pFx)
{
    static volatile LONG sv_count = 0;
    LONG c = InterlockedIncrement(&sv_count);
    if (c <= 5 || (c % 100) == 0)
        fprintf(stderr, "[XAudio2Stub] CreateSourceVoice #%ld -> mock\n", c);
    if (ppVoice) *ppVoice = &mock_source_voice;
    return S_OK;
}

static HRESULT __stdcall xa2_CreateSubmixVoice(MockXAudio2 *this, void **ppVoice,
    UINT Ch, UINT Rate, UINT Flags, UINT Stage, void *pSend, void *pFx)
{
    if (ppVoice) *ppVoice = &mock_source_voice;
    return S_OK;
}

static HRESULT __stdcall xa2_CreateMasteringVoice(MockXAudio2 *this, void **ppVoice,
    UINT Ch, UINT Rate, UINT Flags, UINT DevIdx, void *pFx)
{
    fprintf(stderr, "[XAudio2Stub] CreateMasteringVoice(ch=%u, rate=%u) -> mock\n", Ch, Rate);
    fflush(stderr);
    if (ppVoice) *ppVoice = &mock_mastering_voice;
    return S_OK;
}

static HRESULT __stdcall xa2_StartEngine(MockXAudio2 *this)
{
    fprintf(stderr, "[XAudio2Stub] StartEngine() -> S_OK\n");
    fflush(stderr);
    return S_OK;
}

static void __stdcall xa2_StopEngine(void *this) {}
static HRESULT __stdcall xa2_CommitChanges(void *this, UINT OpSet) { return S_OK; }
static void __stdcall xa2_GetPerformanceData(void *this, void *p) { if (p) memset(p, 0, 128); }
static void __stdcall xa2_SetDebugConfiguration(void *this, void *p, void *r) {}

static void *xa2_vtable[] = {
    xa2_QueryInterface,       /* [0] */
    xa2_AddRef,               /* [1] */
    xa2_Release,              /* [2] */
    xa2_GetDeviceCount,       /* [3] */
    xa2_GetDeviceDetails,     /* [4] */
    xa2_Initialize,           /* [5] */
    xa2_RegisterForCallbacks, /* [6] */
    xa2_UnregisterForCallbacks, /* [7] */
    xa2_CreateSourceVoice,    /* [8] */
    xa2_CreateSubmixVoice,    /* [9] */
    xa2_CreateMasteringVoice, /* [10] */
    xa2_StartEngine,          /* [11] */
    xa2_StopEngine,           /* [12] */
    xa2_CommitChanges,        /* [13] */
    xa2_GetPerformanceData,   /* [14] */
    xa2_SetDebugConfiguration,/* [15] */
};

static MockXAudio2 g_xaudio2 = { xa2_vtable, 1 };

/* ========================================================================
 * COM Class Factory (IClassFactory)
 * ======================================================================== */

typedef struct {
    void **vptr;
    volatile LONG refcount;
} MockClassFactory;

static HRESULT __stdcall cf_QueryInterface(MockClassFactory *this, const GUID *riid, void **ppv)
{
    if (!ppv) return 0x80004002L;
    *ppv = this;
    InterlockedIncrement(&this->refcount);
    return S_OK;
}

static ULONG __stdcall cf_AddRef(MockClassFactory *this)
{
    return InterlockedIncrement(&this->refcount);
}

static ULONG __stdcall cf_Release(MockClassFactory *this)
{
    LONG ref = InterlockedDecrement(&this->refcount);
    if (ref <= 0) this->refcount = 1;
    return ref > 0 ? ref : 1;
}

static HRESULT __stdcall cf_CreateInstance(MockClassFactory *this, void *pOuter,
    const GUID *riid, void **ppv)
{
    fprintf(stderr, "[XAudio2Stub] ClassFactory::CreateInstance -> mock IXAudio2\n");
    fflush(stderr);
    if (!ppv) return 0x80004002L;
    *ppv = &g_xaudio2;
    InterlockedIncrement(&g_xaudio2.refcount);
    return S_OK;
}

static HRESULT __stdcall cf_LockServer(void *this, BOOL fLock) { return S_OK; }

static void *cf_vtable[] = {
    cf_QueryInterface,
    cf_AddRef,
    cf_Release,
    cf_CreateInstance,
    cf_LockServer,
};

static MockClassFactory g_factory = { cf_vtable, 1 };

/* ========================================================================
 * DLL Exports — COM server entry points
 * ======================================================================== */

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(const GUID *rclsid,
    const GUID *riid, void **ppv)
{
    fprintf(stderr, "[XAudio2Stub] DllGetClassObject called\n");
    fflush(stderr);
    if (!ppv) return 0x80004002L;
    *ppv = &g_factory;
    InterlockedIncrement(&g_factory.refcount);
    return S_OK;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE; /* don't unload */
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

/* XAudio2Create — convenience function, some games call directly */
__declspec(dllexport) HRESULT WINAPI XAudio2Create(void **ppXAudio2, UINT Flags, UINT Processor)
{
    fprintf(stderr, "[XAudio2Stub] XAudio2Create called\n");
    fflush(stderr);
    if (!ppXAudio2) return 0x80004002L;
    *ppXAudio2 = &g_xaudio2;
    InterlockedIncrement(&g_xaudio2.refcount);
    return S_OK;
}

/* ========================================================================
 * DllMain — initialize voice vtables
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);

        /* Initialize voice vtables — all methods return 0 (S_OK / no-op) */
        for (int i = 0; i < 32; i++)
            voice_vtable[i] = (void *)voice_noop;
        mock_mastering_voice.vptr = voice_vtable;
        mock_source_voice.vptr = voice_vtable;

        fprintf(stderr, "[XAudio2Stub] xaudio2_7.dll stub loaded in PID %lu\n",
                GetCurrentProcessId());
        fflush(stderr);
    }
    return TRUE;
}
