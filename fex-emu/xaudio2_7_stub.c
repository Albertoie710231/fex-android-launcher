/**
 * Stub DLL for xaudio2_7.dll (XAudio2 2.7 COM server)
 *
 * FAudio's xaudio2_7.dll crashes with ACCESS_VIOLATION under FEX-Emu.
 * This stub provides a minimal COM server that returns mock IXAudio2
 * and IXAudio2Voice objects. All methods return S_OK and produce no audio.
 *
 * 2026-04-22: Upgraded to fire IXAudio2VoiceCallback::OnBufferEnd on a
 * ~10ms timer for every source voice that has pending submitted buffers.
 * Without this, Ys IX's CSound state machine parks waiting for OnBufferEnd
 * after submitting the intro BGM buffer — GameNative with real
 * FAudio+PulseAudio fires this callback naturally, we have to synthesize.
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
#include <stdint.h>

/* Side-channel log: stderr capture dies when wineRun's outer proc exits,
 * so log important events to a file we can read via adb. APPEND mode so
 * multiple loaders don't clobber. */
static void file_log(const char *msg)
{
    HANDLE hf = CreateFileA("C:\\\\xaudio2_stub.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;
    SetFilePointer(hf, 0, NULL, FILE_END);
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[pid=%lu tick=%lu] %s\n",
        GetCurrentProcessId(), GetTickCount(), msg);
    DWORD w;
    WriteFile(hf, buf, n, &w, NULL);
    CloseHandle(hf);
}

/* S_OK and S_FALSE are already defined in winerror.h (via windows.h) */

/* ========================================================================
 * Per-Voice State (source voices need their own vtable so SubmitSourceBuffer
 * can find per-voice callback + pending-buffer queue via `this` pointer).
 * ======================================================================== */

#define MAX_SOURCE_VOICES 64

typedef struct SourceVoice {
    void **vptr;                     /* MUST be first — COM layout */
    void *callback;                  /* IXAudio2VoiceCallback* (may be NULL) */
    volatile void *recent_ctx;       /* latest pContext from SubmitSourceBuffer */
    volatile LONG pending_buffers;   /* count of submitted buffers still "queued" */
    volatile ULONGLONG samples_played; /* cumulative — grows monotonically over time */
    volatile int active;             /* 1 once allocated */
} SourceVoice;

static SourceVoice source_voices[MAX_SOURCE_VOICES];
static volatile LONG source_voice_count = 0;

/* ========================================================================
 * IXAudio2Voice vtable (first 19 entries, shared by all voice types)
 * IXAudio2MasteringVoice adds nothing
 * IXAudio2SourceVoice adds 10 more (indices 19..28)
 * ========================================================================
 *  IXAudio2Voice:
 *   [0]  GetVoiceDetails          [1]  SetOutputVoices
 *   [2]  SetEffectChain           [3]  EnableEffect
 *   [4]  DisableEffect            [5]  GetEffectState
 *   [6]  SetEffectParameters      [7]  GetEffectParameters
 *   [8]  SetFilterParameters      [9]  GetFilterParameters
 *   [10] SetOutputFilterParameters[11] GetOutputFilterParameters
 *   [12] SetVolume                [13] GetVolume
 *   [14] SetChannelVolumes        [15] GetChannelVolumes
 *   [16] SetOutputMatrix          [17] GetOutputMatrix
 *   [18] DestroyVoice
 *  IXAudio2SourceVoice adds:
 *   [19] Start                    [20] Stop
 *   [21] SubmitSourceBuffer       [22] FlushSourceBuffers
 *   [23] Discontinuity            [24] ExitLoop
 *   [25] GetState                 [26] SetFrequencyRatio
 *   [27] GetFrequencyRatio        [28] SetSourceSampleRate
 * ======================================================================== */

/* XAUDIO2_BUFFER layout (x86-64):
 *   UINT32 Flags             offset 0
 *   UINT32 AudioBytes        offset 4
 *   const BYTE *pAudioData   offset 8
 *   UINT32 PlayBegin         offset 16
 *   UINT32 PlayLength        offset 20
 *   UINT32 LoopBegin         offset 24
 *   UINT32 LoopLength        offset 28
 *   UINT32 LoopCount         offset 32
 *   void *pContext           offset 40
 */
#define XAUDIO2_BUFFER_CONTEXT_OFFSET 40

static HRESULT __stdcall voice_noop_hr(void *this) { (void)this; return 0; }
static void    __stdcall voice_noop_v(void *this)  { (void)this; }

/* SubmitSourceBuffer — save pContext, increment pending count.
 * The per-voice background callback thread will fire OnBufferEnd with this
 * pContext on its next tick. */
static HRESULT __stdcall src_SubmitSourceBuffer(SourceVoice *this, const void *pBuffer, const void *pBufferWMA)
{
    (void)pBufferWMA;
    if (!this || !pBuffer) return 0;
    void *ctx = *(void **)((const char *)pBuffer + XAUDIO2_BUFFER_CONTEXT_OFFSET);
    this->recent_ctx = ctx;
    LONG p = InterlockedIncrement(&this->pending_buffers);
    static volatile LONG log_count = 0;
    LONG lc = InterlockedIncrement(&log_count);
    if (lc <= 10 || (lc % 500) == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
            "SubmitSourceBuffer #%ld voice=%p ctx=%p pending=%ld",
            lc, (void*)this, ctx, p);
        file_log(msg);
    }
    return 0;
}

/* GetState — XAUDIO2_VOICE_STATE layout (x64):
 *   offset 0:  void *pCurrentBufferContext
 *   offset 8:  UINT32 BuffersQueued
 *   offset 16: UINT64 SamplesPlayed
 * ~24 bytes total. SamplesPlayed MUST grow monotonically or the game's
 * audio state machine gets confused. */
static void __stdcall src_GetState(SourceVoice *this, void *pState, UINT Flags)
{
    (void)Flags;
    if (!pState) return;
    memset(pState, 0, 32);
    if (this) {
        *(void **)((char *)pState + 0)     = (void *)this->recent_ctx;
        *(UINT32 *)((char *)pState + 8)    = (UINT32)this->pending_buffers;
        *(ULONGLONG *)((char *)pState + 16) = this->samples_played;
    }
}

/* Per-source-voice vtable (29 entries). Most entries point to voice_noop_*.
 * SubmitSourceBuffer at [21] and GetState at [25] have real impls. */
static void *src_voice_vtable[29];

/* ========================================================================
 * Mastering voice vtable — separate so SubmitSourceBuffer isn't there
 * (mastering voices don't support it, games never call it on them).
 * ======================================================================== */
static void *mastering_voice_vtable[19];

typedef struct { void **vptr; } MockMasteringVoice;
static MockMasteringVoice mock_mastering_voice;

/* ========================================================================
 * Callback-firing thread — fires OnBufferEnd for each active voice's
 * pending buffers every 10ms. This is what unblocks Ys IX's CSound state
 * machine waiting for the intro BGM buffer to "complete".
 * ======================================================================== */

typedef void (__stdcall *OnBufferEnd_t)(void *cb, void *pContext);
typedef void (__stdcall *OnVoiceProcessingPassStart_t)(void *cb, UINT32 BytesRequired);

static DWORD WINAPI callback_thread(LPVOID param)
{
    (void)param;
    file_log("callback thread starting");

    /* Assume 48kHz stereo → 48000 samples/sec. One 10ms tick = 480 samples. */
    const ULONGLONG SAMPLES_PER_TICK = 480;

    while (1) {
        Sleep(10);  /* 100 Hz */

        LONG n = source_voice_count;
        for (LONG i = 0; i < n && i < MAX_SOURCE_VOICES; i++) {
            SourceVoice *v = &source_voices[i];
            if (!v->active) continue;

            /* Advance samples_played unconditionally — the game may poll
             * GetState().SamplesPlayed to sync animation/logic ticks, and it
             * must grow monotonically. */
            v->samples_played += SAMPLES_PER_TICK;

            /* Drain one "consumed" buffer per tick (if any pending). This is
             * what the game sees via GetState().BuffersQueued — without this
             * the count grows forever and the game parks thinking its audio
             * submissions are never consumed. */
            LONG pending = v->pending_buffers;
            if (pending > 0) {
                InterlockedDecrement(&v->pending_buffers);
            }

            /* Only fire the vtable callback IF the game registered one. Many
             * games (Ys IX included) pass NULL callback — they rely on
             * GetState polling instead. */
            if (!v->callback) continue;
            void **cb_vtable = *(void ***)v->callback;
            if (!cb_vtable) continue;

            OnVoiceProcessingPassStart_t pps = (OnVoiceProcessingPassStart_t)cb_vtable[0];
            if (pps) pps(v->callback, 0);

            if (pending > 0) {
                void *ctx = (void *)v->recent_ctx;
                OnBufferEnd_t obe = (OnBufferEnd_t)cb_vtable[4];
                if (obe) obe(v->callback, ctx);
            }
        }
    }
    return 0;
}

/* ========================================================================
 * Mock IXAudio2 COM Object
 * ======================================================================== */

typedef struct {
    void **vptr;
    volatile LONG refcount;
} MockXAudio2;

static HRESULT __stdcall xa2_QueryInterface(MockXAudio2 *this, const GUID *riid, void **ppv)
{
    (void)riid;
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

static HRESULT __stdcall xa2_GetDeviceCount(MockXAudio2 *this, UINT *pCount)
{
    (void)this;
    if (pCount) *pCount = 1;
    return S_OK;
}

static HRESULT __stdcall xa2_GetDeviceDetails(MockXAudio2 *this, UINT Index, void *pDetails)
{
    (void)this; (void)Index;
    if (pDetails) memset(pDetails, 0, 300);
    return S_OK;
}

static HRESULT __stdcall xa2_Initialize(MockXAudio2 *this, UINT Flags, UINT Processor)
{
    (void)this; (void)Processor;
    fprintf(stderr, "[XAudio2Stub] Initialize(flags=0x%x) -> S_OK\n", Flags);
    fflush(stderr);
    return S_OK;
}

static HRESULT __stdcall xa2_RegisterForCallbacks(void *this, void *pCb) { (void)this; (void)pCb; return S_OK; }
static HRESULT __stdcall xa2_UnregisterForCallbacks(void *this, void *pCb) { (void)this; (void)pCb; return S_OK; }

/* CreateSourceVoice — allocate a new SourceVoice from the pool, save the
 * callback pointer for the background thread to call. */
static HRESULT __stdcall xa2_CreateSourceVoice(MockXAudio2 *this, void **ppVoice,
    void *pFmt, UINT Flags, float MaxFreq, void *pCb, void *pSend, void *pFx)
{
    (void)this; (void)pFmt; (void)Flags; (void)MaxFreq; (void)pSend; (void)pFx;

    LONG idx = InterlockedIncrement(&source_voice_count) - 1;
    if (idx >= MAX_SOURCE_VOICES) {
        /* pool exhausted — give out slot 0 (least worst fallback) */
        idx = 0;
    }
    SourceVoice *v = &source_voices[idx];
    v->vptr = src_voice_vtable;
    v->callback = pCb;
    v->recent_ctx = NULL;
    v->pending_buffers = 0;
    v->active = 1;

    if (ppVoice) *ppVoice = v;

    char msg[256];
    snprintf(msg, sizeof(msg),
        "CreateSourceVoice idx=%ld cb=%p voice=%p",
        idx, pCb, (void*)v);
    file_log(msg);
    return S_OK;
}

static HRESULT __stdcall xa2_CreateSubmixVoice(MockXAudio2 *this, void **ppVoice,
    UINT Ch, UINT Rate, UINT Flags, UINT Stage, void *pSend, void *pFx)
{
    (void)this; (void)Ch; (void)Rate; (void)Flags; (void)Stage; (void)pSend; (void)pFx;
    /* submix voices don't take buffers — reuse mastering mock */
    if (ppVoice) *ppVoice = &mock_mastering_voice;
    return S_OK;
}

static HRESULT __stdcall xa2_CreateMasteringVoice(MockXAudio2 *this, void **ppVoice,
    UINT Ch, UINT Rate, UINT Flags, UINT DevIdx, void *pFx)
{
    (void)this; (void)Flags; (void)DevIdx; (void)pFx;
    char msg[128];
    snprintf(msg, sizeof(msg), "CreateMasteringVoice ch=%u rate=%u", Ch, Rate);
    file_log(msg);
    if (ppVoice) *ppVoice = &mock_mastering_voice;
    return S_OK;
}

static HRESULT __stdcall xa2_StartEngine(MockXAudio2 *this)
{
    (void)this;
    fprintf(stderr, "[XAudio2Stub] StartEngine() -> S_OK\n");
    fflush(stderr);
    return S_OK;
}

static void __stdcall xa2_StopEngine(void *this) { (void)this; }
static HRESULT __stdcall xa2_CommitChanges(void *this, UINT OpSet) { (void)this; (void)OpSet; return S_OK; }
static void __stdcall xa2_GetPerformanceData(void *this, void *p) { (void)this; if (p) memset(p, 0, 128); }
static void __stdcall xa2_SetDebugConfiguration(void *this, void *p, void *r) { (void)this; (void)p; (void)r; }

static void *xa2_vtable[] = {
    xa2_QueryInterface,       /* [0]  */
    xa2_AddRef,               /* [1]  */
    xa2_Release,              /* [2]  */
    xa2_GetDeviceCount,       /* [3]  */
    xa2_GetDeviceDetails,     /* [4]  */
    xa2_Initialize,           /* [5]  */
    xa2_RegisterForCallbacks, /* [6]  */
    xa2_UnregisterForCallbacks,/*[7]  */
    xa2_CreateSourceVoice,    /* [8]  */
    xa2_CreateSubmixVoice,    /* [9]  */
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
    (void)riid;
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
    (void)this; (void)pOuter; (void)riid;
    fprintf(stderr, "[XAudio2Stub] ClassFactory::CreateInstance -> mock IXAudio2\n");
    fflush(stderr);
    if (!ppv) return 0x80004002L;
    *ppv = &g_xaudio2;
    InterlockedIncrement(&g_xaudio2.refcount);
    return S_OK;
}

static HRESULT __stdcall cf_LockServer(void *this, BOOL fLock) { (void)this; (void)fLock; return S_OK; }

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
    (void)rclsid; (void)riid;
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
    (void)Flags; (void)Processor;
    fprintf(stderr, "[XAudio2Stub] XAudio2Create called\n");
    fflush(stderr);
    if (!ppXAudio2) return 0x80004002L;
    *ppXAudio2 = &g_xaudio2;
    InterlockedIncrement(&g_xaudio2.refcount);
    return S_OK;
}

/* ========================================================================
 * DllMain — initialize voice vtables + spawn callback thread
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);

        /* Default all voice methods to return 0. IXAudio2Voice returns HRESULT
         * S_OK (0) for everything; void-returning methods just no-op. The two
         * below are real impls. */
        for (int i = 0; i < 29; i++)
            src_voice_vtable[i] = (void *)voice_noop_hr;

        /* DestroyVoice [18] is void — use noop_v */
        src_voice_vtable[18] = (void *)voice_noop_v;
        /* Source voice extras: Start [19], Stop [20] are void */
        src_voice_vtable[19] = (void *)voice_noop_v;
        src_voice_vtable[20] = (void *)voice_noop_v;
        /* SubmitSourceBuffer [21] — REAL impl: track pending + context */
        src_voice_vtable[21] = (void *)src_SubmitSourceBuffer;
        /* Discontinuity [23], ExitLoop [24] are void-like */
        /* GetState [25] — REAL impl: report our pending count */
        src_voice_vtable[25] = (void *)src_GetState;

        /* Mastering voice vtable: all noops. */
        for (int i = 0; i < 19; i++)
            mastering_voice_vtable[i] = (void *)voice_noop_hr;
        mastering_voice_vtable[18] = (void *)voice_noop_v;
        mock_mastering_voice.vptr = mastering_voice_vtable;

        /* Zero the source-voice pool. */
        memset(source_voices, 0, sizeof(source_voices));
        source_voice_count = 0;

        /* Spawn the callback-firing thread. Detached — lives until process
         * exit. */
        HANDLE th = CreateThread(NULL, 0, callback_thread, NULL, 0, NULL);
        if (th) CloseHandle(th);

        fprintf(stderr, "[XAudio2Stub] xaudio2_7.dll stub loaded in PID %lu (with callback thread)\n",
                GetCurrentProcessId());
        fflush(stderr);

        file_log("DllMain DLL_PROCESS_ATTACH");
    }
    return TRUE;
}
