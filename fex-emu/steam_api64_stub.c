/**
 * Stub DLL for steam_api64.dll (Steamworks SDK).
 *
 * The real steam_api64.dll tries to connect to the Steam client via IPC.
 * When the Steam client is not running (as on Android/FEX), the SDK's
 * initialization either fails or enters a busy-wait loop trying to connect.
 *
 * This stub returns success from SteamAPI_Init() and provides mock interfaces
 * so the game proceeds past Steam initialization.
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -shared -o steam_api64.dll steam_api64_stub.c \
 *       steam_api64_stub.def -O2 -s -Wl,--enable-stdcall-fixup
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint32_t AppId_t;
typedef uint64_t CSteamID;
typedef uint32_t AccountID_t;

/* ========================================================================
 * Mock Interfaces (same approach as Galaxy64 stub)
 *
 * All vtable entries return 0 (false/NULL/0).
 * On Win64, 'this' is in RCX, args in RDX/R8/R9/stack.
 * ======================================================================== */

static long long __cdecl mock_method(void) {
    return 0;
}

/* Return 1 (true) for methods that need to report success */
static long long __cdecl mock_method_true(void) {
    return 1;
}

/* Return a fake Steam ID (non-zero, prevents "invalid user" checks) */
static unsigned long long __cdecl mock_get_steam_id(void) {
    /* Return a valid-looking offline Steam ID: universe=1, type=1, instance=1 */
    return 0x0110000100000001ULL;
}

/* Return a fake app ID */
static unsigned int __cdecl mock_get_app_id(void) {
    /* Read from SteamAppId env var, fallback to 814380 (Sekiro) */
    const char *env = getenv("SteamAppId");
    return env ? (unsigned int)atoi(env) : 814380;
}

/* Return "en" for language queries */
static const char* __cdecl mock_get_language(void) {
    return "english";
}

/* Shared vtable: 256 entries all pointing to mock_method.
 * Steam interfaces have up to ~100 virtual methods each. */
static void *mock_vtable[256];

/* Special vtables for specific interfaces */
static void *user_vtable[256];
static void *apps_vtable[256];
static void *utils_vtable[256];

typedef struct { void **vptr; } MockObj;

static MockObj mock_user;
static MockObj mock_friends;
static MockObj mock_apps;
static MockObj mock_utils;
static MockObj mock_user_stats;
static MockObj mock_matchmaking;
static MockObj mock_networking;
static MockObj mock_remote_storage;
static MockObj mock_screenshots;
static MockObj mock_http;
static MockObj mock_controller;
static MockObj mock_ugc;
static MockObj mock_applist;
static MockObj mock_music;
static MockObj mock_video;
static MockObj mock_input;
static MockObj mock_parties;
static MockObj mock_remote_play;
static MockObj mock_client;
static MockObj mock_inventory;

static int mocks_initialized = 0;

static void init_mocks(void)
{
    if (mocks_initialized) return;
    mocks_initialized = 1;

    /* Default vtable: all methods return 0 */
    for (int i = 0; i < 256; i++)
        mock_vtable[i] = (void *)mock_method;

    /* ISteamUser vtable: override GetSteamID (index 2 in most SDK versions) */
    for (int i = 0; i < 256; i++)
        user_vtable[i] = (void *)mock_method;
    user_vtable[2] = (void *)mock_get_steam_id;
    /* BLoggedOn (index 0) — return true */
    user_vtable[0] = (void *)mock_method_true;

    /* ISteamApps vtable: mix of bool (true) and pointer/string returns */
    for (int i = 0; i < 256; i++)
        apps_vtable[i] = (void *)mock_method;  /* Default: return 0 (safe for pointers) */
    /* BIsSubscribed (0), BIsLowViolence (1), BIsCybercafe (2), BIsVACBanned (3) */
    apps_vtable[0] = (void *)mock_method_true;   /* BIsSubscribed */
    /* BGetDLCDataByIndex (4) → 0, GetCurrentBetaName (5) → 0 */
    apps_vtable[6] = (void *)mock_method_true;   /* MarkContentCorrupt → true */
    apps_vtable[7] = (void *)mock_get_language;   /* GetCurrentGameLanguage → "english" */
    /* GetAvailableGameLanguages (8) → return "english" */
    apps_vtable[8] = (void *)mock_get_language;
    /* BIsSubscribedApp (9) */
    apps_vtable[9] = (void *)mock_method_true;
    /* BIsDlcInstalled (10) */
    apps_vtable[10] = (void *)mock_method;  /* false — no DLC */

    /* ISteamUtils vtable: GetAppID, GetCurrentBatteryPower, etc. */
    for (int i = 0; i < 256; i++)
        utils_vtable[i] = (void *)mock_method;
    utils_vtable[9] = (void *)mock_get_app_id;  /* GetAppID */

    mock_user.vptr = user_vtable;
    mock_friends.vptr = mock_vtable;
    mock_apps.vptr = apps_vtable;
    mock_utils.vptr = utils_vtable;
    mock_user_stats.vptr = mock_vtable;
    mock_matchmaking.vptr = mock_vtable;
    mock_networking.vptr = mock_vtable;
    mock_remote_storage.vptr = mock_vtable;
    mock_screenshots.vptr = mock_vtable;
    mock_http.vptr = mock_vtable;
    mock_controller.vptr = mock_vtable;
    mock_ugc.vptr = mock_vtable;
    mock_applist.vptr = mock_vtable;
    mock_music.vptr = mock_vtable;
    mock_video.vptr = mock_vtable;
    mock_input.vptr = mock_vtable;
    mock_parties.vptr = mock_vtable;
    mock_remote_play.vptr = mock_vtable;
    mock_client.vptr = mock_vtable;
    mock_inventory.vptr = mock_vtable;
}

/* ========================================================================
 * Tracing
 * ======================================================================== */

/* Call counters for watchdog reporting */
static volatile long call_counts[32];
enum {
    FN_INIT = 0, FN_INIT_SAFE, FN_INIT_FLAT, FN_SHUTDOWN,
    FN_RUN_CALLBACKS, FN_RESTART_APP, FN_IS_RUNNING,
    FN_USER, FN_FRIENDS, FN_APPS, FN_UTILS, FN_USERSTATS,
    FN_CONTEXT_INIT, FN_FIND_INTERFACE, FN_CREATE_INTERFACE,
    FN_REGISTER_CB, FN_PIPE, FN_HUSER,
    FN_MAX
};
static const char *fn_names[] = {
    "Init", "InitSafe", "InitFlat", "Shutdown",
    "RunCallbacks", "RestartApp", "IsSteamRunning",
    "User", "Friends", "Apps", "Utils", "UserStats",
    "ContextInit", "FindInterface", "CreateInterface",
    "RegisterCB", "GetPipe", "GetHUser"
};

static void trace(const char *fn) {
    static int total = 0;
    if (total++ < 50) {
        fprintf(stderr, "[steam_api64] %s\n", fn);
        fflush(stderr);
    }
}

/* Dump all threads' instruction pointers using Win32 API */
static void dump_thread_ips(void) {
    DWORD myPid = GetCurrentProcessId();
    DWORD myTid = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[watchdog] CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return;
    }

    THREADENTRY32 te;
    te.dwSize = sizeof(te);

    if (!Thread32First(snap, &te)) {
        CloseHandle(snap);
        return;
    }

    do {
        if (te.th32OwnerProcessID != myPid) continue;
        if (te.th32ThreadID == myTid) continue; /* skip watchdog */

        HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!ht) {
            fprintf(stderr, "  TID %lu: OpenThread failed (%lu)\n",
                    te.th32ThreadID, GetLastError());
            continue;
        }

        DWORD suspCount = SuspendThread(ht);

        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;

        if (GetThreadContext(ht, &ctx)) {
            fprintf(stderr, "  TID %lu: RIP=0x%016llx RSP=0x%016llx\n",
                    te.th32ThreadID,
                    (unsigned long long)ctx.Rip,
                    (unsigned long long)ctx.Rsp);

            /* Look up which module contains RIP */
            HMODULE mod = NULL;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)(ULONG_PTR)ctx.Rip, &mod)) {
                char name[MAX_PATH];
                GetModuleFileNameA(mod, name, MAX_PATH);
                fprintf(stderr, "         Module: %s (base=0x%llx, +0x%llx)\n",
                        name,
                        (unsigned long long)(ULONG_PTR)mod,
                        (unsigned long long)(ctx.Rip - (ULONG_PTR)mod));
            } else {
                fprintf(stderr, "         Module: UNKNOWN (not in any loaded module)\n");
            }

            /* Dump 16 bytes at RIP for instruction analysis */
            unsigned char *rip = (unsigned char *)(ULONG_PTR)ctx.Rip;
            if (rip && !IsBadReadPtr(rip, 16)) {
                fprintf(stderr, "         Bytes:");
                for (int i = 0; i < 16; i++)
                    fprintf(stderr, " %02x", rip[i]);
                fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "         Bytes: (unreadable)\n");
            }
        } else {
            fprintf(stderr, "  TID %lu: GetThreadContext failed (%lu)\n",
                    te.th32ThreadID, GetLastError());
        }

        ResumeThread(ht);
        CloseHandle(ht);
    } while (Thread32Next(snap, &te));

    CloseHandle(snap);
}

/* Rapid-sample one thread's RIP to profile where it's spending time */
static void profile_thread(DWORD tid, int samples) {
    HANDLE ht = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                           THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!ht) return;

    /* Collect unique RIPs */
    unsigned long long rips[64];
    int rip_counts[64];
    int unique = 0;

    for (int s = 0; s < samples; s++) {
        SuspendThread(ht);
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(ht, &ctx)) {
            /* Find or add this RIP */
            int found = 0;
            for (int i = 0; i < unique; i++) {
                if (rips[i] == ctx.Rip) {
                    rip_counts[i]++;
                    found = 1;
                    break;
                }
            }
            if (!found && unique < 64) {
                rips[unique] = ctx.Rip;
                rip_counts[unique] = 1;
                unique++;
            }
        }
        ResumeThread(ht);
        /* Tiny delay between samples */
        Sleep(1);
    }

    fprintf(stderr, "[watchdog] Profile of TID %lu (%d samples, %d unique RIPs):\n",
            tid, samples, unique);
    for (int i = 0; i < unique; i++) {
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "UNKNOWN";
        unsigned long long offset = 0;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)(ULONG_PTR)rips[i], &mod)) {
            GetModuleFileNameA(mod, modname, MAX_PATH);
            offset = rips[i] - (unsigned long long)(ULONG_PTR)mod;
        }
        fprintf(stderr, "  RIP=0x%016llx  count=%d/%d (%d%%)  %s+0x%llx\n",
                rips[i], rip_counts[i], samples,
                (rip_counts[i] * 100) / samples,
                modname, offset);
    }
    fflush(stderr);
    CloseHandle(ht);
}

/* Thread helper for vkQueueWaitIdle with timeout */
typedef int32_t (WINAPI *FN_qwi_global)(void*);
typedef struct { FN_qwi_global fn; void *queue; volatile long done; long result; } QWIArg;
static DWORD WINAPI qwi_thread_func(LPVOID p) {
    QWIArg *a = (QWIArg*)p;
    a->result = a->fn(a->queue);
    InterlockedExchange(&a->done, 1);
    return 0;
}

/* In-process Vulkan test: creates its own instance/device/cmdbuf INSIDE the game process.
 * If this also spins → process-wide corruption. If it works → DXVK's handle is bad. */
static void in_process_vulkan_test(void) {
    fprintf(stderr, "[watchdog] === IN-PROCESS VULKAN TEST ===\n"); fflush(stderr);

    HMODULE hVk = LoadLibraryA("vulkan-1.dll");
    if (!hVk) {
        fprintf(stderr, "[watchdog] LoadLibrary(vulkan-1.dll) FAILED: %lu\n", GetLastError());
        fflush(stderr);
        return;
    }

    typedef int32_t VkR;
    typedef void* VkI;
    typedef void* VkPD;
    typedef void* VkD;
    typedef void* VkCB;
    typedef void* VkCP;

    /* Minimal struct types — use uint8_t arrays to avoid alignment issues */
    typedef struct { uint32_t sType; const void* pN; uint32_t f; const void* pA;
                     uint32_t elC; const char*const* el; uint32_t eeC; const char*const* ee; } ICI;
    typedef struct { uint32_t sType; const void* pN; uint32_t f; uint32_t qfi;
                     uint32_t qc; const float* pqp; } DQCI;
    typedef struct { uint32_t sType; const void* pN; uint32_t f; uint32_t qcic;
                     const DQCI* pqci; uint32_t elc; const char*const* el;
                     uint32_t eec; const char*const* ee; const void* pef; } DCI;
    typedef struct { uint32_t sType; const void* pN; uint32_t f; uint32_t qfi; } CPCI;
    typedef struct { uint32_t sType; const void* pN; VkCP cp; uint32_t lv; uint32_t c; } CBAI;
    typedef struct { uint32_t sType; const void* pN; uint32_t f; const void* pInh; } CBBI;

    typedef VkR (WINAPI *FN_ci)(const ICI*, const void*, VkI*);
    typedef VkR (WINAPI *FN_epd)(VkI, uint32_t*, VkPD*);
    typedef VkR (WINAPI *FN_cd)(VkPD, const DCI*, const void*, VkD*);
    typedef VkR (WINAPI *FN_ccp)(VkD, const CPCI*, const void*, VkCP*);
    typedef VkR (WINAPI *FN_acb)(VkD, const CBAI*, VkCB*);
    typedef VkR (WINAPI *FN_bcb)(VkCB, const CBBI*);
    typedef VkR (WINAPI *FN_ecb)(VkCB);
    typedef void (WINAPI *FN_dcp)(VkD, VkCP, const void*);
    typedef void (WINAPI *FN_dd)(VkD, const void*);
    typedef void (WINAPI *FN_di)(VkI, const void*);

    #define GPA(t, n) t p_##n = (t)GetProcAddress(hVk, #n); \
        if (!p_##n) { fprintf(stderr, "[watchdog] GPA(%s)=NULL\n", #n); fflush(stderr); return; }

    GPA(FN_ci, vkCreateInstance);
    GPA(FN_epd, vkEnumeratePhysicalDevices);
    GPA(FN_cd, vkCreateDevice);
    GPA(FN_ccp, vkCreateCommandPool);
    GPA(FN_acb, vkAllocateCommandBuffers);
    GPA(FN_bcb, vkBeginCommandBuffer);
    GPA(FN_ecb, vkEndCommandBuffer);
    GPA(FN_dcp, vkDestroyCommandPool);
    GPA(FN_dd, vkDestroyDevice);
    GPA(FN_di, vkDestroyInstance);

    VkR r;

    /* 1. Create instance */
    ICI ici = {0}; ici.sType = 1;
    VkI inst = NULL;
    r = p_vkCreateInstance(&ici, NULL, &inst);
    fprintf(stderr, "[watchdog] vkCreateInstance: %d inst=%p\n", r, inst); fflush(stderr);
    if (r != 0) return;

    /* 2. Get GPU */
    uint32_t gc = 1; VkPD gpu = NULL;
    p_vkEnumeratePhysicalDevices(inst, &gc, &gpu);
    fprintf(stderr, "[watchdog] GPU: %p\n", gpu); fflush(stderr);
    if (!gpu) { p_vkDestroyInstance(inst, NULL); return; }

    /* 3. Create device with VK_KHR_swapchain */
    float qp = 1.0f;
    DQCI dqci = {0}; dqci.sType = 2; dqci.qc = 1; dqci.pqp = &qp;
    const char *ext = "VK_KHR_swapchain";
    DCI dci = {0}; dci.sType = 3; dci.qcic = 1; dci.pqci = &dqci; dci.eec = 1; dci.ee = &ext;
    VkD dev = NULL;
    r = p_vkCreateDevice(gpu, &dci, NULL, &dev);
    fprintf(stderr, "[watchdog] vkCreateDevice: %d dev=%p\n", r, dev); fflush(stderr);
    if (r != 0) { p_vkDestroyInstance(inst, NULL); return; }

    /* 4. Create command pool */
    CPCI cpci = {0}; cpci.sType = 39; cpci.f = 2; /* RESET_COMMAND_BUFFER_BIT */
    VkCP pool = NULL;
    r = p_vkCreateCommandPool(dev, &cpci, NULL, &pool);
    fprintf(stderr, "[watchdog] vkCreateCommandPool: %d pool=%p\n", r, pool); fflush(stderr);
    if (r != 0) { p_vkDestroyDevice(dev, NULL); p_vkDestroyInstance(inst, NULL); return; }

    /* 5. Allocate command buffer */
    CBAI cbai = {0}; cbai.sType = 40; cbai.cp = pool; cbai.c = 1;
    VkCB cb = NULL;
    r = p_vkAllocateCommandBuffers(dev, &cbai, &cb);
    fprintf(stderr, "[watchdog] vkAllocateCommandBuffers: %d cb=%p\n", r, cb); fflush(stderr);
    if (r != 0) { p_vkDestroyCommandPool(dev, pool, NULL); p_vkDestroyDevice(dev, NULL); p_vkDestroyInstance(inst, NULL); return; }

    /* 5b. Test other device operations BEFORE vkBeginCommandBuffer */
    typedef void (WINAPI *FN_gdq)(VkD, uint32_t, uint32_t, void**);
    typedef VkR (WINAPI *FN_dwi)(VkD);
    typedef VkR (WINAPI *FN_qwi)(void*);
    typedef VkR (WINAPI *FN_qs)(void*, uint32_t, const void*, uint64_t);

    FN_gdq p_vkGetDeviceQueue = (FN_gdq)GetProcAddress(hVk, "vkGetDeviceQueue");
    FN_dwi p_vkDeviceWaitIdle = (FN_dwi)GetProcAddress(hVk, "vkDeviceWaitIdle");
    FN_qwi p_vkQueueWaitIdle = (FN_qwi)GetProcAddress(hVk, "vkQueueWaitIdle");
    FN_qs p_vkQueueSubmit = (FN_qs)GetProcAddress(hVk, "vkQueueSubmit");

    /* Test vkDeviceWaitIdle first (has dispatch trampoline in fex_thunk_icd) */
    if (p_vkDeviceWaitIdle) {
        QWIArg dwi_arg = { (FN_qwi_global)p_vkDeviceWaitIdle, dev, 0, -999 };
        fprintf(stderr, "[watchdog] vkDeviceWaitIdle CALLING (2s timeout)...\n"); fflush(stderr);
        HANDLE ht_dwi = CreateThread(NULL, 0, qwi_thread_func, &dwi_arg, 0, NULL);
        for (int i = 0; i < 200 && !dwi_arg.done; i++) Sleep(10);
        if (dwi_arg.done) {
            fprintf(stderr, "[watchdog] vkDeviceWaitIdle: %d %s\n",
                    dwi_arg.result, dwi_arg.result == 0 ? "SUCCESS" : "FAILED");
        } else {
            fprintf(stderr, "[watchdog] vkDeviceWaitIdle: *** TIMEOUT (2s) — HANGING ***\n");
        }
        fflush(stderr);
    }

    if (p_vkGetDeviceQueue) {
        void *q = NULL;
        p_vkGetDeviceQueue(dev, 0, 0, &q);
        fprintf(stderr, "[watchdog] vkGetDeviceQueue: queue=%p\n", q); fflush(stderr);

        if (q && p_vkQueueWaitIdle) {
            QWIArg arg = { (FN_qwi_global)p_vkQueueWaitIdle, q, 0, -999 };
            fprintf(stderr, "[watchdog] vkQueueWaitIdle CALLING (2s timeout)...\n"); fflush(stderr);
            HANDLE ht = CreateThread(NULL, 0, qwi_thread_func, &arg, 0, NULL);
            /* Wait up to 2 seconds */
            for (int i = 0; i < 200 && !arg.done; i++) Sleep(10);
            if (arg.done) {
                fprintf(stderr, "[watchdog] vkQueueWaitIdle: %d %s\n",
                        arg.result, arg.result == 0 ? "SUCCESS" : "FAILED");
            } else {
                fprintf(stderr, "[watchdog] vkQueueWaitIdle: *** TIMEOUT (2s) — HANGING ***\n");
            }
            fflush(stderr);
        }
    }

    /* Skip remaining tests if queue ops hang — just cleanup */
    fprintf(stderr, "[watchdog] Cleanup...\n"); fflush(stderr);
    p_vkDestroyCommandPool(dev, pool, NULL);
    p_vkDestroyDevice(dev, NULL);
    p_vkDestroyInstance(inst, NULL);
    fprintf(stderr, "[watchdog] === IN-PROCESS VULKAN TEST DONE ===\n"); fflush(stderr);
}

/* Watchdog thread: reports call counts + thread IPs + profile spinning thread */
static DWORD WINAPI watchdog_thread(LPVOID arg) {
    (void)arg;
    /* DISABLED: in-process Vulkan test causes malloc() heap corruption
     * ("corrupted top size") which poisons DXVK's allocator and prevents
     * it from even opening its log file. The watchdog Vulkan test uses the
     * same loader/ICD/layer stack concurrently with DXVK init. */
    Sleep(2000);
    fprintf(stderr, "\n[steam_api64] === WATCHDOG t+2s (Vulkan test SKIPPED) ===\n");
    fflush(stderr);

    for (int iter = 0; iter < 3; iter++) { /* 30 seconds total */
        Sleep(10000);
        fprintf(stderr, "\n[steam_api64] === WATCHDOG t+%ds ===\n", (iter+1)*10 + 2);

        /* Steam API call counts */
        int any = 0;
        for (int i = 0; i < FN_MAX; i++) {
            if (call_counts[i] > 0) {
                fprintf(stderr, "  %s: %ld calls\n", fn_names[i], call_counts[i]);
                any = 1;
            }
        }
        if (!any)
            fprintf(stderr, "  (NO steam_api64 functions called!)\n");

        /* Single-shot thread IPs (first iteration only) */
        if (iter == 0) {
            fprintf(stderr, "[watchdog] --- Thread IPs ---\n");
            dump_thread_ips();
        }

        /* On second iteration, profile the last thread (likely the spinning one) */
        if (iter == 1) {
            /* Find the last thread (highest TID = usually the spinner) */
            DWORD myPid = GetCurrentProcessId();
            DWORD myTid = GetCurrentThreadId();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof(te);
            DWORD lastTid = 0;
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == myPid && te.th32ThreadID != myTid)
                        lastTid = te.th32ThreadID;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);

            if (lastTid) {
                fprintf(stderr, "[watchdog] Profiling TID %lu (100 samples)...\n", lastTid);
                profile_thread(lastTid, 100);
            }
        }

        fprintf(stderr, "[steam_api64] === END WATCHDOG ===\n");
        fflush(stderr);
    }
    return 0;
}

/* ========================================================================
 * MessageBox IAT Hook — intercept error dialogs to see text on Android
 * ======================================================================== */

static int (WINAPI *real_MessageBoxA)(HWND, LPCSTR, LPCSTR, UINT) = NULL;
static int (WINAPI *real_MessageBoxW)(HWND, LPCWSTR, LPCWSTR, UINT) = NULL;

static void log_msgbox(const char *type, const char *caption, const char *text) {
    fprintf(stderr, "[steam_api64] %s: caption='%s' text='%s'\n",
            type, caption ? caption : "(null)", text ? text : "(null)");
    fflush(stderr);
    /* Also write to file so we can read it on Android */
    FILE *f = fopen("Z:\\tmp\\msgbox_log.txt", "a");
    if (!f) f = fopen("C:\\msgbox_log.txt", "a");
    if (f) {
        fprintf(f, "%s: caption='%s' text='%s'\n",
                type, caption ? caption : "(null)", text ? text : "(null)");
        fclose(f);
    }
}

static int WINAPI hooked_MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type) {
    log_msgbox("MessageBoxA", caption, text);
    /* Return IDOK to let the game continue past the error */
    return IDOK;
}

static int WINAPI hooked_MessageBoxW(HWND h, LPCWSTR text, LPCWSTR caption, UINT type) {
    char cap_buf[256] = "(null)";
    char txt_buf[1024] = "(null)";
    if (caption) WideCharToMultiByte(CP_UTF8, 0, caption, -1, cap_buf, sizeof(cap_buf), NULL, NULL);
    if (text)    WideCharToMultiByte(CP_UTF8, 0, text, -1, txt_buf, sizeof(txt_buf), NULL, NULL);
    log_msgbox("MessageBoxW", cap_buf, txt_buf);
    /* Return IDOK to auto-dismiss and let the game continue */
    return IDOK;
}

/* Patch IAT of a module to redirect imports */
static void patch_iat(HMODULE module, const char *target_dll,
                      const char *func_name, void *hook, void **original) {
    if (!module) return;

    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    IMAGE_DATA_DIRECTORY *imp_dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp_dir->VirtualAddress || !imp_dir->Size) return;

    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp_dir->VirtualAddress);
    for (; imp->Name; imp++) {
        const char *dll_name = (const char *)(base + imp->Name);
        if (_stricmp(dll_name, target_dll) != 0) continue;

        IMAGE_THUNK_DATA *orig_thunk = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat_thunk  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; orig_thunk->u1.AddressOfData; orig_thunk++, iat_thunk++) {
            if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;

            IMAGE_IMPORT_BY_NAME *ibn =
                (IMAGE_IMPORT_BY_NAME *)(base + orig_thunk->u1.AddressOfData);
            if (strcmp(ibn->Name, func_name) == 0) {
                DWORD old_prot;
                if (VirtualProtect(&iat_thunk->u1.Function, sizeof(void *),
                                   PAGE_READWRITE, &old_prot)) {
                    if (original) *original = (void *)iat_thunk->u1.Function;
                    iat_thunk->u1.Function = (ULONGLONG)hook;
                    VirtualProtect(&iat_thunk->u1.Function, sizeof(void *),
                                   old_prot, &old_prot);
                    fprintf(stderr, "[steam_api64] IAT hooked %s!%s\n",
                            target_dll, func_name);
                }
                return;
            }
        }
    }
}

/* Hook MessageBox in ALL loaded modules (exe + DLLs) */
static void install_messagebox_hooks(void) {
    HMODULE exe = GetModuleHandleA(NULL);
    patch_iat(exe, "user32.dll", "MessageBoxA",
              (void *)hooked_MessageBoxA, (void **)&real_MessageBoxA);
    patch_iat(exe, "user32.dll", "MessageBoxW",
              (void *)hooked_MessageBoxW, (void **)&real_MessageBoxW);

    /* Also try common DLLs that might call MessageBox on behalf of the game */
    const char *dlls[] = { "msvcrt.dll", "msvcp140.dll", "vcruntime140.dll",
                           "kernelbase.dll", "kernel32.dll", NULL };
    for (int i = 0; dlls[i]; i++) {
        HMODULE h = GetModuleHandleA(dlls[i]);
        if (h) {
            patch_iat(h, "user32.dll", "MessageBoxA",
                      (void *)hooked_MessageBoxA, (void **)&real_MessageBoxA);
            patch_iat(h, "user32.dll", "MessageBoxW",
                      (void *)hooked_MessageBoxW, (void **)&real_MessageBoxW);
        }
    }

    /* Fallback: get real functions for the hooked versions to call */
    if (!real_MessageBoxA) {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (u32) {
            real_MessageBoxA = (int (WINAPI *)(HWND, LPCSTR, LPCSTR, UINT))
                               GetProcAddress(u32, "MessageBoxA");
            real_MessageBoxW = (int (WINAPI *)(HWND, LPCWSTR, LPCWSTR, UINT))
                               GetProcAddress(u32, "MessageBoxW");
        }
    }
}

/* ========================================================================
 * Missing Exports for Older Steamworks SDK (Sekiro ~2019)
 * ======================================================================== */

/* Global getters (non-SteamAPI_ prefixed) */
HSteamPipe __cdecl GetHSteamPipe(void) {
    trace("GetHSteamPipe()");
    return 1;
}

HSteamUser __cdecl GetHSteamUser(void) {
    trace("GetHSteamUser()");
    return 1;
}

HSteamUser __cdecl Steam_GetHSteamUserCurrent(void) {
    trace("Steam_GetHSteamUserCurrent()");
    return 1;
}

/* SteamAPI_GetSteamInstallPath — KEY: game may check this for DRM */
const char* __cdecl SteamAPI_GetSteamInstallPath(void) {
    trace("SteamAPI_GetSteamInstallPath()");
    return "C:\\Program Files (x86)\\Steam";
}

/* Breakpad/minidump (no-ops) */
void __cdecl SteamAPI_SetBreakpadAppID(uint32_t appId) {
    trace("SteamAPI_SetBreakpadAppID()");
}

void __cdecl SteamAPI_SetMiniDumpComment(const char *msg) {
    trace("SteamAPI_SetMiniDumpComment()");
}

void __cdecl SteamAPI_SetTryCatchCallbacks(int bTryCatchCallbacks) {
    trace("SteamAPI_SetTryCatchCallbacks()");
}

void __cdecl SteamAPI_UseBreakpadCrashHandler(const char *pchVersion,
        const char *pchDate, const char *pchTime, int bFullMemoryDumps,
        void *pvContext, void *pfnPreMinidumpCallback) {
    trace("SteamAPI_UseBreakpadCrashHandler()");
}

void __cdecl SteamAPI_WriteMiniDump(uint32_t uStructuredExceptionCode,
        void *pvExceptionInfo, uint32_t uBuildID) {
    trace("SteamAPI_WriteMiniDump()");
}

/* Steam_RegisterInterfaceFuncs / Steam_RunCallbacks */
void __cdecl Steam_RegisterInterfaceFuncs(void *hModule) {
    trace("Steam_RegisterInterfaceFuncs()");
}

void __cdecl Steam_RunCallbacks(HSteamPipe hSteamPipe, int bGameServerCallbacks) {
    trace("Steam_RunCallbacks()");
}

/* SteamContentServer — return NULL (not used by games) */
void* __cdecl SteamContentServer(void) {
    return NULL;
}

void* __cdecl SteamContentServerUtils(void) {
    return NULL;
}

/* SteamGameServer interfaces */
static MockObj mock_gameserver;
static int gameserver_mocks_initialized = 0;
static void init_gameserver_mocks(void) {
    if (gameserver_mocks_initialized) return;
    gameserver_mocks_initialized = 1;
    mock_gameserver.vptr = mock_vtable;
}

void* __cdecl SteamGameServer(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerApps(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerHTTP(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerInventory(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerNetworking(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerNetworkingSockets(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerNetworkingUtils(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerStats(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerUGC(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

void* __cdecl SteamGameServerUtils(void) {
    init_gameserver_mocks();
    return &mock_gameserver;
}

int __cdecl SteamGameServer_Init(uint32_t unIP, uint16_t usSteamPort,
        uint16_t usGamePort, uint16_t usQueryPort, int eServerMode,
        const char *pchVersionString) {
    trace("SteamGameServer_Init()");
    init_gameserver_mocks();
    return 1;
}

int __cdecl SteamGameServer_InitSafe(uint32_t unIP, uint16_t usSteamPort,
        uint16_t usGamePort, uint16_t usQueryPort, int eServerMode,
        const char *pchVersionString) {
    trace("SteamGameServer_InitSafe()");
    init_gameserver_mocks();
    return 1;
}

void __cdecl SteamGameServer_Shutdown(void) {
    trace("SteamGameServer_Shutdown()");
}

void __cdecl SteamGameServer_RunCallbacks(void) {
}

int __cdecl SteamGameServer_BSecure(void) {
    return 0;
}

uint64_t __cdecl SteamGameServer_GetSteamID(void) {
    return 0x0110000100000001ULL;
}

HSteamPipe __cdecl SteamGameServer_GetHSteamPipe(void) {
    return 1;
}

HSteamUser __cdecl SteamGameServer_GetHSteamUser(void) {
    return 1;
}

/* SteamUnifiedMessages (deprecated but may still be imported) */
void* __cdecl SteamUnifiedMessages(void) {
    return &mock_networking;  /* reuse any mock */
}

/* Global variable: g_pSteamClientGameServer */
void *g_pSteamClientGameServer = NULL;

/* SteamAPI_GetSteamInstallPath alternative names */
const char* __cdecl SteamAPI_GetSteamInstallPath_alt(void) {
    return "C:\\Program Files (x86)\\Steam";
}

/* ========================================================================
 * Core Steamworks API
 * ======================================================================== */

int __cdecl SteamAPI_Init(void) {
    call_counts[FN_INIT]++;
    trace("SteamAPI_Init() -> true");
    init_mocks();
    return 1; /* true = success */
}

int __cdecl SteamAPI_InitSafe(void) {
    call_counts[FN_INIT_SAFE]++;
    trace("SteamAPI_InitSafe() -> true");
    init_mocks();
    return 1;
}

/* Flat API init (newer SDK) */
int __cdecl SteamAPI_InitFlat(void *errMsg) {
    call_counts[FN_INIT_FLAT]++;
    trace("SteamAPI_InitFlat() -> 0 (ok)");
    init_mocks();
    return 0; /* ESteamAPIInitResult_OK */
}

void __cdecl SteamAPI_Shutdown(void) {
    call_counts[FN_SHUTDOWN]++;
    trace("SteamAPI_Shutdown()");
}

void __cdecl SteamAPI_RunCallbacks(void) {
    call_counts[FN_RUN_CALLBACKS]++;
    Sleep(1); /* Yield to prevent busy-spin if game polls */
}

int __cdecl SteamAPI_RestartAppIfNecessary(AppId_t appId) {
    call_counts[FN_RESTART_APP]++;
    trace("SteamAPI_RestartAppIfNecessary() -> false");
    return 0; /* false = no restart needed */
}

int __cdecl SteamAPI_IsSteamRunning(void) {
    call_counts[FN_IS_RUNNING]++;
    return 1; /* true */
}

HSteamPipe __cdecl SteamAPI_GetHSteamPipe(void) {
    call_counts[FN_PIPE]++;
    return 1;
}

HSteamUser __cdecl SteamAPI_GetHSteamUser(void) {
    call_counts[FN_HUSER]++;
    return 1;
}

/* ========================================================================
 * Interface Getters (C API — used by older SDKs)
 * ======================================================================== */

void* __cdecl SteamUser(void) {
    call_counts[FN_USER]++;
    trace("SteamUser()");
    return &mock_user;
}

void* __cdecl SteamFriends(void) {
    call_counts[FN_FRIENDS]++;
    trace("SteamFriends()");
    return &mock_friends;
}

void* __cdecl SteamApps(void) {
    call_counts[FN_APPS]++;
    trace("SteamApps()");
    return &mock_apps;
}

void* __cdecl SteamUtils(void) {
    call_counts[FN_UTILS]++;
    trace("SteamUtils()");
    return &mock_utils;
}

void* __cdecl SteamUserStats(void) {
    call_counts[FN_USERSTATS]++;
    trace("SteamUserStats()");
    return &mock_user_stats;
}

void* __cdecl SteamMatchmaking(void) {
    trace("SteamMatchmaking()");
    return &mock_matchmaking;
}

void* __cdecl SteamNetworking(void) {
    trace("SteamNetworking()");
    return &mock_networking;
}

void* __cdecl SteamRemoteStorage(void) {
    trace("SteamRemoteStorage()");
    return &mock_remote_storage;
}

void* __cdecl SteamScreenshots(void) {
    trace("SteamScreenshots()");
    return &mock_screenshots;
}

void* __cdecl SteamHTTP(void) {
    trace("SteamHTTP()");
    return &mock_http;
}

void* __cdecl SteamController(void) {
    trace("SteamController()");
    return &mock_controller;
}

void* __cdecl SteamUGC(void) {
    trace("SteamUGC()");
    return &mock_ugc;
}

void* __cdecl SteamAppList(void) {
    return &mock_applist;
}

void* __cdecl SteamMusic(void) {
    return &mock_music;
}

void* __cdecl SteamVideo(void) {
    return &mock_video;
}

void* __cdecl SteamInput(void) {
    trace("SteamInput()");
    return &mock_input;
}

void* __cdecl SteamParties(void) {
    return &mock_parties;
}

void* __cdecl SteamRemotePlay(void) {
    return &mock_remote_play;
}

void* __cdecl SteamClient(void) {
    trace("SteamClient()");
    return &mock_client;
}

void* __cdecl SteamInventory(void) {
    return &mock_inventory;
}

void* __cdecl SteamNetworkingUtils(void) {
    return &mock_networking;
}

void* __cdecl SteamNetworkingSockets(void) {
    return &mock_networking;
}

void* __cdecl SteamNetworkingMessages(void) {
    return &mock_networking;
}

void* __cdecl SteamMatchmakingServers(void) {
    return &mock_matchmaking;
}

void* __cdecl SteamGameSearch(void) {
    return &mock_matchmaking;
}

/* ========================================================================
 * SteamInternal API (newer SDK versions use these)
 * ======================================================================== */

/* SteamInternal_ContextInit — called to lazily init interface pointers */
void* __cdecl SteamInternal_ContextInit(void *pCtx) {
    call_counts[FN_CONTEXT_INIT]++;
    trace("SteamInternal_ContextInit()");
    /*
     * The context is a struct like:
     *   struct { Counter counter; CSteamAPIContext ctx; }
     * We need to fill in ctx with our mock pointers.
     * The layout varies by SDK version, so we fill the first 20 pointer slots.
     */
    if (pCtx) {
        void **ptrs = (void **)pCtx;
        /* Skip the first 8 bytes (counter) and fill interface pointers */
        void **ctx = ptrs + 1;
        ctx[0] = &mock_client;        /* m_pSteamClient */
        ctx[1] = &mock_user;          /* m_pSteamUser */
        ctx[2] = &mock_friends;       /* m_pSteamFriends */
        ctx[3] = &mock_utils;         /* m_pSteamUtils */
        ctx[4] = &mock_matchmaking;   /* m_pSteamMatchmaking */
        ctx[5] = &mock_user_stats;    /* m_pSteamUserStats */
        ctx[6] = &mock_apps;          /* m_pSteamApps */
        ctx[7] = &mock_matchmaking;   /* m_pSteamMatchmakingServers */
        ctx[8] = &mock_networking;    /* m_pSteamNetworking */
        ctx[9] = &mock_remote_storage;/* m_pSteamRemoteStorage */
        ctx[10] = &mock_screenshots;  /* m_pSteamScreenshots */
        ctx[11] = &mock_http;         /* m_pSteamHTTP */
        ctx[12] = &mock_controller;   /* m_pController */
        ctx[13] = &mock_ugc;          /* m_pSteamUGC */
        ctx[14] = &mock_applist;      /* m_pSteamAppList */
        ctx[15] = &mock_music;        /* m_pSteamMusic */
        ctx[16] = &mock_music;        /* m_pSteamMusicRemote */
        ctx[17] = &mock_http;         /* m_pSteamHTMLSurface */
        ctx[18] = &mock_inventory;    /* m_pSteamInventory */
        ctx[19] = &mock_video;        /* m_pSteamVideo */
    }
    return pCtx;
}

void* __cdecl SteamInternal_FindOrCreateUserInterface(HSteamUser user, const char *version) {
    call_counts[FN_FIND_INTERFACE]++;
    trace("SteamInternal_FindOrCreateUserInterface()");
    if (version) {
        fprintf(stderr, "[steam_api64]   version: %s\n", version);
        /* Return appropriate mock based on interface name */
        if (strstr(version, "SteamUser"))        return &mock_user;
        if (strstr(version, "SteamFriends"))     return &mock_friends;
        if (strstr(version, "SteamApps"))        return &mock_apps;
        if (strstr(version, "SteamUtils"))       return &mock_utils;
        if (strstr(version, "SteamMatchmaking")) return &mock_matchmaking;
        if (strstr(version, "SteamUserStats"))   return &mock_user_stats;
        if (strstr(version, "SteamNetworking"))  return &mock_networking;
        if (strstr(version, "SteamRemoteStorage"))return &mock_remote_storage;
        if (strstr(version, "SteamScreenshots")) return &mock_screenshots;
        if (strstr(version, "STEAMHTTP"))        return &mock_http;
        if (strstr(version, "SteamController"))  return &mock_controller;
        if (strstr(version, "STEAMUGC"))         return &mock_ugc;
        if (strstr(version, "SteamInput"))       return &mock_input;
        if (strstr(version, "SteamInventory"))   return &mock_inventory;
    }
    return &mock_user; /* fallback */
}

void* __cdecl SteamInternal_FindOrCreateGameServerInterface(HSteamUser user, const char *version) {
    trace("SteamInternal_FindOrCreateGameServerInterface()");
    return &mock_user; /* games rarely use game server interfaces */
}

void* __cdecl SteamInternal_CreateInterface(const char *version) {
    call_counts[FN_CREATE_INTERFACE]++;
    trace("SteamInternal_CreateInterface()");
    if (version)
        fprintf(stderr, "[steam_api64]   version: %s\n", version);
    return &mock_client;
}

/* ========================================================================
 * Callback Registration (no-ops — we never fire callbacks)
 * ======================================================================== */

void __cdecl SteamAPI_RegisterCallback(void *pCallback, int iCallback) {
    call_counts[FN_REGISTER_CB]++;
    if (call_counts[FN_REGISTER_CB] <= 10) {
        fprintf(stderr, "[steam_api64] RegisterCallback(id=%d)\n", iCallback);
        fflush(stderr);
    }
}

void __cdecl SteamAPI_UnregisterCallback(void *pCallback) {
}

void __cdecl SteamAPI_RegisterCallResult(void *pCallback, uint64_t hAPICall) {
}

void __cdecl SteamAPI_UnregisterCallResult(void *pCallback, uint64_t hAPICall) {
}

/* ========================================================================
 * Flat API wrappers (SteamAPI_ISteamXxx_Method pattern)
 * Newer SDK versions use these instead of vtable calls.
 * We only need the ones the game actually calls.
 * ======================================================================== */

int __cdecl SteamAPI_ISteamApps_BIsSubscribedApp(void *self, AppId_t appID) {
    return 1; /* true — yes you own the game */
}

int __cdecl SteamAPI_ISteamApps_BIsDlcInstalled(void *self, AppId_t appID) {
    return 0; /* No DLC */
}

const char* __cdecl SteamAPI_ISteamApps_GetCurrentGameLanguage(void *self) {
    return "english";
}

int __cdecl SteamAPI_ISteamUser_BLoggedOn(void *self) {
    return 1; /* true */
}

uint64_t __cdecl SteamAPI_ISteamUser_GetSteamID(void *self) {
    return 0x0110000100000001ULL;
}

uint32_t __cdecl SteamAPI_ISteamUtils_GetAppID(void *self) {
    return 1351630;
}

int __cdecl SteamAPI_ISteamUtils_IsOverlayEnabled(void *self) {
    return 0; /* No overlay */
}

const char* __cdecl SteamAPI_ISteamUtils_GetSteamUILanguage(void *self) {
    return "english";
}

int __cdecl SteamAPI_ISteamUserStats_RequestCurrentStats(void *self) {
    return 1; /* true */
}

int __cdecl SteamAPI_ISteamInput_Init(void *self, int bExplicitlyCallRunFrame) {
    return 1;
}

void __cdecl SteamAPI_ISteamInput_RunFrame(void *self, int bReservedValue) {
}

void __cdecl SteamAPI_ManualDispatch_Init(void) {
    trace("SteamAPI_ManualDispatch_Init()");
}

void __cdecl SteamAPI_ManualDispatch_RunFrame(HSteamPipe pipe) {
}

int __cdecl SteamAPI_ManualDispatch_GetNextCallback(HSteamPipe pipe, void *pCallbackMsg) {
    return 0; /* No callbacks */
}

void __cdecl SteamAPI_ManualDispatch_FreeLastCallback(HSteamPipe pipe) {
}

/* ========================================================================
 * Vectored Exception Handler — fix NULL entity table in ys9.exe
 *
 * Multiple functions read the entity table pointer from
 * [global_at_0x140780c78 + 0x26ddb98] + 0x1109e8. When the entity system
 * isn't initialized yet, this pointer is NULL, causing ACCESS_VIOLATION
 * at small addresses (NULL + stride*index + field_offset).
 *
 * Known crash sites: +0x174206, +0x364900 (and likely more).
 *
 * Fix: on first entity-table crash, allocate a zero-filled dummy table
 * and write it to the entity table slot. All zero fields cause the
 * iteration code's own null/type checks to skip each entry harmlessly.
 * Then re-execute the crashing instruction — it now finds valid memory.
 * ======================================================================== */

/* 1000 entities * 0x58 (88) bytes each */
#define ENTITY_TABLE_SIZE (1000 * 0x58)
static char *dummy_entity_table = NULL;
static volatile LONG veh_entity_fix_count = 0;

static LONG CALLBACK ys9_null_table_veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    DWORD64 rip = ep->ContextRecord->Rip;
    ULONG_PTR fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

    /* Only handle crashes in ys9.exe (base 0x140000000, size 0x936000)
     * with small fault addresses (NULL + small offset) */
    if (rip < 0x140000000 || rip >= 0x140936000 || fault_addr >= 0x10000)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Try to patch the entity table pointer in the global chain:
     * [0x140780c78] -> global -> [+0x26ddb98] -> sub_obj -> [+0x1109e8] -> table
     */
    LONG n = InterlockedIncrement(&veh_entity_fix_count);
    fprintf(stderr, "[steam_api64] VEH: NULL deref in ys9.exe+0x%llx "
            "(fault=0x%llx, fix #%ld)\n",
            (unsigned long long)(rip - 0x140000000),
            (unsigned long long)fault_addr, (long)n);
    fflush(stderr);

    /* Allocate dummy table once */
    if (!dummy_entity_table) {
        dummy_entity_table = (char *)VirtualAlloc(NULL, ENTITY_TABLE_SIZE,
                                                   MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE);
        if (dummy_entity_table) {
            memset(dummy_entity_table, 0, ENTITY_TABLE_SIZE);
            fprintf(stderr, "[steam_api64] VEH: allocated dummy entity table at %p\n",
                    dummy_entity_table);
        }
    }

    if (dummy_entity_table) {
        /* Walk the global pointer chain and patch the entity table slot.
         * Use IsBadReadPtr/IsBadWritePtr for safety (no __try in MinGW). */
        uint64_t *global_ptr_slot = (uint64_t *)0x140780c78;
        if (!IsBadReadPtr(global_ptr_slot, 8)) {
            uint64_t global_val = *global_ptr_slot;
            if (global_val && !IsBadReadPtr((void *)(global_val + 0x26ddb98), 8)) {
                uint64_t sub_obj = *(uint64_t *)(global_val + 0x26ddb98);
                if (sub_obj && !IsBadWritePtr((void *)(sub_obj + 0x1109e8), 8)) {
                    uint64_t *entity_slot = (uint64_t *)(sub_obj + 0x1109e8);
                    if (*entity_slot == 0) {
                        *entity_slot = (uint64_t)dummy_entity_table;
                        fprintf(stderr, "[steam_api64] VEH: patched entity table at "
                                "%p: 0 -> %p\n", (void *)entity_slot, dummy_entity_table);
                        fflush(stderr);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
        }
    }

    /* Fallback: skip known crash sites by jumping to their epilogues */
    if (rip == 0x140174206) {
        ep->ContextRecord->Rip = 0x1401742b0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip == 0x140364900) {
        ep->ContextRecord->Rip = 0x140364d62;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* Unknown crash site in ys9.exe with small fault addr — log but don't handle */
    fprintf(stderr, "[steam_api64] VEH: unhandled NULL deref at +0x%llx, passing\n",
            (unsigned long long)(rip - 0x140000000));
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ========================================================================
 * DllMain
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        init_mocks();
        init_gameserver_mocks();
        g_pSteamClientGameServer = &mock_client;

        /* Hook MessageBoxA/W to capture error text on Android */
        install_messagebox_hooks();

        /* Install VEH ONLY for Ys IX — check exe name */
        {
            char _exename[MAX_PATH];
            GetModuleFileNameA(NULL, _exename, MAX_PATH);
            if (strstr(_exename, "ys9") || strstr(_exename, "Ys9") || strstr(_exename, "YS9"))
                AddVectoredExceptionHandler(1, ys9_null_table_veh);
        }

        char exename[MAX_PATH];
        GetModuleFileNameA(NULL, exename, MAX_PATH);
        fprintf(stderr, "[steam_api64] Stub loaded in PID %lu (%s)\n",
                GetCurrentProcessId(), exename);
        fprintf(stderr, "[steam_api64] SteamAPI_Init() will return true with mock interfaces\n");
        fflush(stderr);

        /* === FIX CWD ===
         * Wine can't map FEX's virtual CWD (/home/user/...) to Z: drive
         * because z: readlink returns HOST rootfs path. GetModuleFileName
         * returns "unix\home\user\...\ys9.exe". Convert to Z: path and
         * SetCurrentDirectory so relative file access works. */
        {
            char exedir[MAX_PATH];
            strncpy(exedir, exename, MAX_PATH);
            exedir[MAX_PATH-1] = '\0';
            char *lastslash = strrchr(exedir, '\\');
            if (!lastslash) lastslash = strrchr(exedir, '/');
            if (lastslash) *lastslash = '\0';

            char old_cwd[MAX_PATH];
            GetCurrentDirectoryA(MAX_PATH, old_cwd);
            fprintf(stderr, "[steam_api64] Original CWD: %s\n", old_cwd);
            fprintf(stderr, "[steam_api64] EXE dir: %s\n", exedir);

            char fixed_cwd[MAX_PATH];
            int need_fix = 0;

            /* Check for "unix\..." or "unix/..." prefix */
            if ((strncmp(exedir, "unix\\", 5) == 0) || (strncmp(exedir, "unix/", 5) == 0)) {
                snprintf(fixed_cwd, MAX_PATH, "Z:\\%s", exedir + 5);
                need_fix = 1;
            }
            /* Check for "\\?\unix\..." prefix */
            else if (strncmp(exedir, "\\\\?\\unix\\", 9) == 0) {
                snprintf(fixed_cwd, MAX_PATH, "Z:\\%s", exedir + 9);
                need_fix = 1;
            }

            if (need_fix) {
                /* Normalize slashes */
                for (char *p = fixed_cwd; *p; p++)
                    if (*p == '/') *p = '\\';

                if (SetCurrentDirectoryA(fixed_cwd)) {
                    char new_cwd[MAX_PATH];
                    GetCurrentDirectoryA(MAX_PATH, new_cwd);
                    fprintf(stderr, "[steam_api64] CWD FIXED: %s\n", new_cwd);
                } else {
                    fprintf(stderr, "[steam_api64] SetCurrentDirectory(%s) FAILED err=%lu\n",
                            fixed_cwd, GetLastError());
                }
            } else {
                fprintf(stderr, "[steam_api64] CWD already OK (no unix prefix)\n");
            }

            /* Quick file access test */
            HANDLE h = CreateFileA("text\\item.tbb", GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING, 0, NULL);
            fprintf(stderr, "[steam_api64] Test text\\item.tbb: %s (err=%lu)\n",
                    h != INVALID_HANDLE_VALUE ? "OK" : "FAILED", GetLastError());
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            fflush(stderr);
        }
        /* === END FIX CWD === */

        /* Start watchdog thread to report call counts */
        CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    }
    return TRUE;
}
