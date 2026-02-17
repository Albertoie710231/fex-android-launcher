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
    static int count = 0;
    if (count++ < 10)
        fprintf(stderr, "[Galaxy64] mock vtable method called (count=%d)\n", count);
    return 0;
}

/* Shared vtable: 128 entries all pointing to mock_method.
 * GOG Galaxy interfaces have ~20-40 virtual methods each. */
static void *mock_vtable[128];

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
    for (int i = 0; i < 128; i++)
        mock_vtable[i] = (void *)mock_method;

    mock_user.vptr = mock_vtable;
    mock_friends.vptr = mock_vtable;
    mock_stats.vptr = mock_vtable;
    mock_utils.vptr = mock_vtable;
    mock_apps.vptr = mock_vtable;
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

/* Interface getters — return mock objects (NOT NULL!) + trace first calls */
static void trace(const char *fn) {
    static int total = 0;
    if (total++ < 20)
        fprintf(stderr, "[Galaxy64] %s called\n", fn);
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
void stub_process_data(void)           { trace("ProcessData()"); Sleep(10); }

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

static LONG CALLBACK sigill_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *ctx = ep->ContextRecord;
    unsigned char *rip = (unsigned char *)(ULONG_PTR)ctx->Rip;

    fprintf(stderr, "\n================================================\n");
    fprintf(stderr, "=== SIGILL TRAP: Exception 0x%08lx ===\n", code);
    fprintf(stderr, "================================================\n");
    fprintf(stderr, "PID: %lu  TID: %lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    fprintf(stderr, "RIP: 0x%016llx  RSP: 0x%016llx\n",
            (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp);

    /* Instruction bytes */
    fprintf(stderr, "Bytes:");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %02x", rip[i]);
    fprintf(stderr, "\n");

    analyze_instruction(rip);

    /* Module lookup */
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

    fprintf(stderr, "================================================\n");
    fflush(stderr);

    ExitProcess(0xDEAD);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ========================================================================
 * DllMain — installs VEH + initializes mock interfaces
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);

        /* Initialize mock Galaxy interfaces */
        init_mocks();

        /* Install VEH as FIRST handler */
        AddVectoredExceptionHandler(1, sigill_handler);

        char exename[MAX_PATH];
        GetModuleFileNameA(NULL, exename, MAX_PATH);
        fprintf(stderr, "[Galaxy64+VEH] SIGILL trap installed in PID %lu (%s)\n",
                GetCurrentProcessId(), exename);
        fprintf(stderr, "[Galaxy64] Mock interfaces active (User, Friends, etc. return non-NULL)\n");
        fflush(stderr);
    }
    return TRUE;
}
