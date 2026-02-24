/**
 * Stub DLL for Galaxy64.dll (GOG Galaxy SDK) + SIGILL VEH trap.
 *
 * The real Galaxy64.dll contains 145 AVX2 instructions that FEX-Emu cannot
 * emulate, causing SIGILL. This stub satisfies ys9.exe's import table.
 *
 * KEY FIX: Interface getters (User, Friends, etc.) return mock objects with
 * vtables where all methods return 0/false/NULL. This prevents infinite
 * spin loops where the game polls until User() returns non-NULL.
 * ProcessData() includes a Sleep(10) as additional safety.
 *
 * Additionally, DllMain installs a Vectored Exception Handler (VEH) that
 * catches EXCEPTION_ILLEGAL_INSTRUCTION and dumps full diagnostics.
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -shared -o Galaxy64.dll galaxy64_stub.c \
 *       galaxy64_stub.def -O2 -s
 */

#include <windows.h>
#include <stdio.h>
#include <signal.h>

/* Forward-declare GetModuleHandleExA flags (mingw may lack them) */
#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#endif
#ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#endif

/* ========================================================================
 * Mock GOG Galaxy Interfaces
 *
 * All vtable entries return 0 (= false / NULL / 0).
 * On Win64, virtual calls pass 'this' in RCX. Our mock ignores all args
 * and returns 0 in RAX. This works for:
 *   - bool methods → false
 *   - pointer methods → NULL
 *   - integer methods → 0
 *   - void methods → 0 ignored by caller
 * ======================================================================== */

static long long mock_method(void) {
    static volatile LONG count = 0;
    LONG c = InterlockedIncrement(&count);
    /* Log first 10, then every 500th call to detect polling loops */
    if (c <= 10 || (c % 500) == 0)
        fprintf(stderr, "[Galaxy64] mock vtable method called (count=%ld, tid=%lu)\n",
                c, GetCurrentThreadId());
    return 0;
}

/* Return 1 (true) for methods that need to report success */
static long long __cdecl mock_method_true(void) {
    return 1;
}

/* Return a fake GalaxyID (non-zero, prevents "invalid user" checks) */
static unsigned long long __cdecl mock_get_galaxy_id(void) {
    /* Return a valid-looking Galaxy user ID */
    return 0x0110000100000001ULL;
}

/* Shared vtable: 128 entries all pointing to mock_method.
 * GOG Galaxy interfaces have ~20-40 virtual methods each. */
static void *mock_vtable[128];

/* IUser vtable: auth methods return true, GetGalaxyID returns valid ID.
 *
 * GOG Galaxy SDK IUser vtable layout (with virtual destructor at [0]):
 *   [0] ~IUser()            [1] SignedIn()          [2] GetGalaxyID()
 *   [3-15] SignIn variants   [16] SignOut()
 *   [17] RequestUserData()   [18] IsUserDataAvailable()
 *   [19-24] UserData methods [25] IsLoggedOn()
 *   [26+] EncryptedAppTicket, tokens, etc.
 *
 * We cover BOTH layouts (with/without virtual destructor) by setting
 * both possible offsets. Returning 1 from a destructor is harmless
 * (void return, caller ignores value). */
static void *galaxy_user_vtable[128];

/* IApps vtable: ownership checks return true */
static void *galaxy_apps_vtable[128];

typedef struct { void **vptr; } MockObj;

static MockObj mock_user;
static MockObj mock_friends;
static MockObj mock_stats;
static MockObj mock_utils;
static MockObj mock_apps;
static MockObj mock_storage;
static MockObj mock_networking;
static MockObj mock_matchmaking;
static MockObj mock_chat;
static MockObj mock_listener_reg;
static MockObj mock_custom_networking;
static MockObj mock_logger;

static void init_mocks(void)
{
    /* Default vtable: all methods return 0 */
    for (int i = 0; i < 128; i++)
        mock_vtable[i] = (void *)mock_method;

    /* IUser vtable: override auth-related methods */
    for (int i = 0; i < 128; i++)
        galaxy_user_vtable[i] = (void *)mock_method;

    /* With virtual destructor at [0]:
     *   [1] = SignedIn, [2] = GetGalaxyID, [18] = IsUserDataAvailable, [25] = IsLoggedOn
     * Without virtual destructor:
     *   [0] = SignedIn, [1] = GetGalaxyID, [17] = IsUserDataAvailable, [24] = IsLoggedOn */
    galaxy_user_vtable[0]  = (void *)mock_method_true;    /* SignedIn (no-dtor) or dtor (harmless) */
    galaxy_user_vtable[1]  = (void *)mock_method_true;    /* SignedIn (with-dtor) or GetGalaxyID (non-zero=valid) */
    galaxy_user_vtable[2]  = (void *)mock_get_galaxy_id;  /* GetGalaxyID (with-dtor) */
    galaxy_user_vtable[17] = (void *)mock_method_true;    /* IsUserDataAvailable (no-dtor) */
    galaxy_user_vtable[18] = (void *)mock_method_true;    /* IsUserDataAvailable (with-dtor) */
    galaxy_user_vtable[24] = (void *)mock_method_true;    /* IsLoggedOn (no-dtor) */
    galaxy_user_vtable[25] = (void *)mock_method_true;    /* IsLoggedOn (with-dtor) */

    /* IApps vtable: most queries should return true (ownership, subscriptions) */
    for (int i = 0; i < 128; i++)
        galaxy_apps_vtable[i] = (void *)mock_method_true;

    mock_user.vptr = galaxy_user_vtable;
    mock_friends.vptr = mock_vtable;
    mock_stats.vptr = mock_vtable;
    mock_utils.vptr = mock_vtable;
    mock_apps.vptr = galaxy_apps_vtable;
    mock_storage.vptr = mock_vtable;
    mock_networking.vptr = mock_vtable;
    mock_matchmaking.vptr = mock_vtable;
    mock_chat.vptr = mock_vtable;
    mock_listener_reg.vptr = mock_vtable;
    mock_custom_networking.vptr = mock_vtable;
    mock_logger.vptr = mock_vtable;
}

/* ========================================================================
 * Galaxy64.dll Stub Exports
 * .def file maps mangled C++ names to these
 * ======================================================================== */

/* Interface getters — return mock objects (NOT NULL!) + trace calls */
static void trace(const char *fn) {
    static volatile LONG total = 0;
    LONG t = InterlockedIncrement(&total);
    /* Log first 20, then every 200th call */
    if (t <= 20 || (t % 200) == 0)
        fprintf(stderr, "[Galaxy64] %s called (total=%ld, tid=%lu)\n",
                fn, t, GetCurrentThreadId());
}

void *stub_get_user(void)              { trace("User()"); return &mock_user; }
void *stub_get_friends(void)           { trace("Friends()"); return &mock_friends; }
void *stub_get_stats(void)             { trace("Stats()"); return &mock_stats; }
void *stub_get_utils(void)             { trace("Utils()"); return &mock_utils; }
void *stub_get_apps(void)              { trace("Apps()"); return &mock_apps; }
void *stub_get_storage(void)           { trace("Storage()"); return &mock_storage; }
void *stub_get_networking(void)        { trace("Networking()"); return &mock_networking; }
void *stub_get_matchmaking(void)       { trace("Matchmaking()"); return &mock_matchmaking; }
void *stub_get_chat(void)              { trace("Chat()"); return &mock_chat; }
void *stub_get_listener_reg(void)      { trace("ListenerRegistrar()"); return &mock_listener_reg; }
void *stub_get_custom_networking(void) { trace("CustomNetworking()"); return &mock_custom_networking; }
void *stub_get_logger(void)            { trace("Logger()"); return &mock_logger; }

/* Functions that should still return NULL */
void *stub_return_null(void)           { return NULL; }

/* Init/Shutdown — trace + no-ops */
void stub_void(void)                   { trace("Shutdown()"); }
void stub_void_ptr(void *p)            { trace("Init()"); (void)p; }

/* ProcessData — trace + sleep to prevent busy-spin */
void stub_process_data(void) {
    static volatile LONG pd_count = 0;
    LONG c = InterlockedIncrement(&pd_count);
    /* Log first 5, then every 100th call */
    if (c <= 5 || (c % 100) == 0)
        fprintf(stderr, "[Galaxy64] ProcessData() count=%ld tid=%lu\n",
                c, GetCurrentThreadId());
    Sleep(10);
}

/* ========================================================================
 * VEH: SIGILL Trap — catches EXCEPTION_ILLEGAL_INSTRUCTION
 * ======================================================================== */

static void dump_module_for_addr(DWORD64 addr, const char *label)
{
    HMODULE mod = NULL;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)(ULONG_PTR)addr, &mod)) {
        char name[MAX_PATH];
        GetModuleFileNameA(mod, name, MAX_PATH);
        fprintf(stderr, "  %s: %s (base 0x%llx, +0x%llx)\n",
                label, name,
                (unsigned long long)(ULONG_PTR)mod,
                (unsigned long long)(addr - (ULONG_PTR)mod));
    }
}

static void analyze_instruction(const unsigned char *rip)
{
    int pos = 0;

    /* Skip legacy prefixes */
    while (pos < 15) {
        unsigned char b = rip[pos];
        if (b == 0x66 || b == 0x67 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x3E || b == 0x26 || b == 0x64 ||
            b == 0x65 || b == 0x36 || b == 0xF0) {
            pos++;
        } else {
            break;
        }
    }

    unsigned char b = rip[pos];

    if (b == 0xC4) {
        unsigned char vex1 = rip[pos+1], vex2 = rip[pos+2];
        int L = (vex2 >> 2) & 1;
        fprintf(stderr, "  >>> VEX3 prefix (AVX%s)\n", L ? "2/256-bit" : "/128-bit");
        fprintf(stderr, "  Opcode: 0x%02x\n", rip[pos+3]);
    } else if (b == 0xC5) {
        unsigned char vex1 = rip[pos+1];
        int L = (vex1 >> 2) & 1;
        fprintf(stderr, "  >>> VEX2 prefix (AVX%s)\n", L ? "2/256-bit" : "/128-bit");
        fprintf(stderr, "  Opcode: 0x%02x\n", rip[pos+2]);
    } else if (b == 0x62) {
        fprintf(stderr, "  >>> EVEX prefix (AVX-512)\n");
    } else if (b == 0x0F && rip[pos+1] == 0x0B) {
        fprintf(stderr, "  >>> UD2 (intentional undefined instruction)\n");
    } else {
        fprintf(stderr, "  Opcode: 0x%02x", b);
        if (b == 0x0F) fprintf(stderr, " %02x", rip[pos+1]);
        fprintf(stderr, "\n");
    }
}

/* Walk the stack by reading return addresses from RSP.
 * x64 ABI: return addresses are at [RSP], [RSP+N*8] for some N.
 * We scan stack slots for values that look like code addresses. */
static void dump_stack_trace(CONTEXT *ctx)
{
    DWORD64 rsp = ctx->Rsp;
    fprintf(stderr, "Stack trace (scanning RSP=0x%llx for return addresses):\n",
            (unsigned long long)rsp);

    /* RIP is the current instruction */
    dump_module_for_addr(ctx->Rip, "  [0] RIP");

    /* Scan stack for plausible return addresses */
    int found = 0;
    for (int i = 0; i < 128 && found < 16; i++) {
        DWORD64 val = 0;
        /* Read 8 bytes from stack safely */
        if (!IsBadReadPtr((void*)(ULONG_PTR)(rsp + i*8), 8)) {
            val = *(DWORD64*)(ULONG_PTR)(rsp + i*8);
        } else {
            continue;
        }
        /* Check if it looks like a code address (> 0x100000, < 0x800000000000) */
        if (val > 0x100000 && val < 0x800000000000ULL) {
            HMODULE mod = NULL;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)(ULONG_PTR)val, &mod)) {
                char name[MAX_PATH];
                GetModuleFileNameA(mod, name, MAX_PATH);
                found++;
                fprintf(stderr, "  [%d] RSP+0x%x: 0x%llx  %s +0x%llx\n",
                        found, i*8, (unsigned long long)val, name,
                        (unsigned long long)(val - (ULONG_PTR)mod));
            }
        }
    }
    if (found == 0)
        fprintf(stderr, "  (no return addresses found on stack)\n");
}

static LONG CALLBACK sigill_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    /* Catch SIGILL, privileged instruction, AND ACCESS_VIOLATION.
     * For AV: only log + dump, then CONTINUE_SEARCH so Wine/game can handle it.
     * This gives us the register state at the actual crash point. */
    int is_av = (code == EXCEPTION_ACCESS_VIOLATION);
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        !is_av)
        return EXCEPTION_CONTINUE_SEARCH;

    /* For AV: use a one-shot flag so we only dump the FIRST one
     * (Wine generates many AVs for guard pages, copy-on-write, etc.) */
    static volatile LONG av_count = 0;
    if (is_av) {
        LONG c = InterlockedIncrement(&av_count);
        if (c > 3) return EXCEPTION_CONTINUE_SEARCH; /* only dump first 3 AVs */
    }

    CONTEXT *ctx = ep->ContextRecord;
    unsigned char *rip = (unsigned char *)(ULONG_PTR)ctx->Rip;

    fprintf(stderr, "\n================================================\n");
    fprintf(stderr, "=== SIGILL TRAP: Exception 0x%08lx ===\n", code);
    fprintf(stderr, "================================================\n");
    fprintf(stderr, "PID: %lu  TID: %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    fprintf(stderr, "RIP: 0x%016llx  RSP: 0x%016llx  RBP: 0x%016llx\n",
            (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp,
            (unsigned long long)ctx->Rbp);
    fprintf(stderr, "RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx\n",
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
            (unsigned long long)ctx->Rcx);
    fprintf(stderr, "RDX: 0x%016llx  RSI: 0x%016llx  RDI: 0x%016llx\n",
            (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi,
            (unsigned long long)ctx->Rdi);
    fprintf(stderr, "R8:  0x%016llx  R9:  0x%016llx  R10: 0x%016llx\n",
            (unsigned long long)ctx->R8, (unsigned long long)ctx->R9,
            (unsigned long long)ctx->R10);
    fprintf(stderr, "R11: 0x%016llx  R12: 0x%016llx  R13: 0x%016llx\n",
            (unsigned long long)ctx->R11, (unsigned long long)ctx->R12,
            (unsigned long long)ctx->R13);
    fprintf(stderr, "R14: 0x%016llx  R15: 0x%016llx\n",
            (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);

    /* Exception params */
    fprintf(stderr, "ExceptionAddress: 0x%016llx\n",
            (unsigned long long)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress);
    fprintf(stderr, "NumberParameters: %lu\n", ep->ExceptionRecord->NumberParameters);
    for (DWORD p = 0; p < ep->ExceptionRecord->NumberParameters && p < 4; p++)
        fprintf(stderr, "  Param[%lu]: 0x%016llx\n", p,
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[p]);

    /* Stack dump — print first 16 QWORDs from RSP */
    fprintf(stderr, "Stack dump (RSP=0x%016llx):\n", (unsigned long long)ctx->Rsp);
    {
        DWORD64 *sp = (DWORD64 *)(ULONG_PTR)ctx->Rsp;
        for (int i = 0; i < 16; i++) {
            if (!IsBadReadPtr(&sp[i], 8))
                fprintf(stderr, "  [RSP+%02x] 0x%016llx\n", i*8, (unsigned long long)sp[i]);
        }
    }

    /* Instruction bytes */
    fprintf(stderr, "Bytes:");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %02x", rip[i]);
    fprintf(stderr, "\n");
    analyze_instruction(rip);

    /* Module lookup for RIP */
    {
        HMODULE mod = NULL;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)(ULONG_PTR)ctx->Rip, &mod)) {
            char name[MAX_PATH];
            GetModuleFileNameA(mod, name, MAX_PATH);
            fprintf(stderr, "Module: %s (base 0x%llx, +0x%llx)\n",
                    name,
                    (unsigned long long)(ULONG_PTR)mod,
                    (unsigned long long)(ctx->Rip - (ULONG_PTR)mod));
        } else {
            fprintf(stderr, "Module: UNKNOWN (RIP not in any loaded module)\n");
        }
    }

    /* Stack trace */
    dump_stack_trace(ctx);

    fprintf(stderr, "================================================\n");
    fflush(stderr);

    if (is_av) {
        /* Let Wine/game handle AV — we just logged it */
        return EXCEPTION_CONTINUE_SEARCH;
    }

    ExitProcess(0xDEAD);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ========================================================================
 * CRT SIGABRT handler — prevents Wine assertion from killing the process.
 *
 * Wine's thread 0090 hits assert(!status) in winevulkan/loader.c:668.
 * The call chain: _wassert → abort() → msvcrt raise(SIGABRT).
 *
 * Wine's msvcrt raise() checks the CRT signal handler table (NOT POSIX).
 * With SIG_DFL it calls ExitProcess(3), killing ALL threads.
 *
 * Fix: Install a CRT handler via signal() that calls ExitThread(0),
 * killing ONLY the asserting thread. The game thread survives.
 *
 * Note: msvcrt resets the handler to SIG_DFL before calling it,
 * so we re-install inside the handler for subsequent assertions.
 * ======================================================================== */

static void abort_handler(int sig) {
    /* Re-install immediately (msvcrt resets to SIG_DFL before calling us) */
    signal(SIGABRT, abort_handler);
    (void)sig;
    DWORD tid = GetCurrentThreadId();
    fprintf(stderr, "[Galaxy64] SIGABRT caught on thread %lu — suspending thread (not killing)\n", tid);
    fflush(stderr);
    /* SuspendThread instead of ExitThread:
     * ExitThread triggers DLL_THREAD_DETACH and destroys the thread, which
     * orphans any Wine critical sections/mutexes the thread holds → deadlock.
     * SuspendThread freezes the thread in place — locks are still held but
     * the thread isn't destroyed, so Wine's lock tracking remains consistent.
     * The thread will never resume, but that's OK for an assertion-failed thread. */
    SuspendThread(GetCurrentThread());
    /* If SuspendThread somehow fails/returns, sleep forever as fallback */
    for (;;) Sleep(60000);
}

/* ========================================================================
 * DllMain — installs VEH + SIGABRT handler + initializes mock interfaces
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);

        /* Initialize mock Galaxy interfaces */
        init_mocks();

        /* Install CRT SIGABRT handler — prevents Wine assertion ExitProcess(3) */
        signal(SIGABRT, abort_handler);

        /* Install VEH as FIRST handler */
        AddVectoredExceptionHandler(1, sigill_handler);

        char exename[MAX_PATH];
        GetModuleFileNameA(NULL, exename, MAX_PATH);
        fprintf(stderr, "[Galaxy64+VEH] SIGILL trap installed in PID %lu (%s)\n",
                GetCurrentProcessId(), exename);
        fprintf(stderr, "[Galaxy64] Mock interfaces active: User(SignedIn=true,IsLoggedOn=true), Apps(all true)\n");
        fflush(stderr);
    }
    return TRUE;
}
