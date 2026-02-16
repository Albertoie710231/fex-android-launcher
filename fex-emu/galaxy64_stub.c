/**
 * Stub DLL for Galaxy64.dll (GOG Galaxy SDK) + SIGILL VEH trap.
 *
 * The real Galaxy64.dll contains 145 AVX2 instructions that FEX-Emu cannot
 * emulate, causing SIGILL. This stub satisfies ys9.exe's import table.
 * All interface accessors return NULL, Init/Shutdown are no-ops.
 * The game runs via Steam, so GOG Galaxy features aren't needed.
 *
 * Additionally, DllMain installs a Vectored Exception Handler (VEH) that
 * catches EXCEPTION_ILLEGAL_INSTRUCTION and dumps full diagnostics:
 *   - All registers (RIP, RSP, RAX..R15)
 *   - 32 bytes of instruction data at RIP
 *   - Instruction prefix analysis (VEX=AVX, EVEX=AVX-512, etc.)
 *   - Faulting module name + base + RVA offset
 *   - PE image size check (is RIP inside main exe?)
 *   - Stack vicinity (16 QWORDs from RSP)
 *
 * Export name mapping is done via galaxy64_stub.def (= redirect syntax).
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
            fprintf(stderr, "  Legacy prefix: 0x%02x\n", b);
            pos++;
        } else {
            break;
        }
    }

    unsigned char b = rip[pos];

    if (b == 0xC4) {
        /* 3-byte VEX (AVX/AVX2) */
        unsigned char vex1 = rip[pos+1], vex2 = rip[pos+2];
        int L = (vex2 >> 2) & 1;
        int pp = vex2 & 3;
        int mmmmm = vex1 & 0x1F;
        fprintf(stderr, "  >>> VEX3 prefix (AVX/AVX2)\n");
        fprintf(stderr, "  VEX.L=%d (%s), VEX.pp=%d, VEX.mmmmm=0x%x\n",
                L, L ? "256-bit" : "128-bit", pp, mmmmm);
        fprintf(stderr, "  Opcode byte: 0x%02x\n", rip[pos+3]);
    } else if (b == 0xC5) {
        /* 2-byte VEX (AVX/AVX2) */
        unsigned char vex1 = rip[pos+1];
        int L = (vex1 >> 2) & 1;
        fprintf(stderr, "  >>> VEX2 prefix (AVX/AVX2)\n");
        fprintf(stderr, "  VEX.L=%d (%s)\n", L, L ? "256-bit" : "128-bit");
        fprintf(stderr, "  Opcode byte: 0x%02x\n", rip[pos+2]);
    } else if (b == 0x62) {
        /* EVEX (AVX-512) */
        fprintf(stderr, "  >>> EVEX prefix (AVX-512!)\n");
        fprintf(stderr, "  EVEX bytes: %02x %02x %02x %02x\n",
                rip[pos], rip[pos+1], rip[pos+2], rip[pos+3]);
        fprintf(stderr, "  Opcode byte: 0x%02x\n", rip[pos+4]);
    } else if (b == 0x0F && rip[pos+1] == 0x0B) {
        fprintf(stderr, "  >>> UD2 (intentional undefined instruction)\n");
    } else if (b == 0x0F && rip[pos+1] == 0x01) {
        /* 0F 01 = system instructions (XSAVE, RDTSCP, etc.) */
        fprintf(stderr, "  >>> System instruction: 0F 01 %02x\n", rip[pos+2]);
        unsigned char modrm = rip[pos+2];
        if (modrm == 0xF9) fprintf(stderr, "  = RDTSCP\n");
        else if (modrm == 0xD0) fprintf(stderr, "  = XGETBV\n");
        else if ((modrm >> 3 & 7) == 4) fprintf(stderr, "  = XSAVE family\n");
        else if ((modrm >> 3 & 7) == 5) fprintf(stderr, "  = XRSTOR family\n");
    } else if (b == 0x0F && rip[pos+1] == 0xC7) {
        /* CMPXCHG16B, RDRAND, RDSEED */
        unsigned char modrm = rip[pos+2];
        int reg = (modrm >> 3) & 7;
        fprintf(stderr, "  >>> 0F C7 /%d\n", reg);
        if (reg == 6) fprintf(stderr, "  = RDRAND (or VMPTRLD)\n");
        else if (reg == 7) fprintf(stderr, "  = RDSEED (or VMPTRST)\n");
        else if (reg == 1) fprintf(stderr, "  = CMPXCHG16B\n");
    } else if (b == 0x0F && rip[pos+1] == 0xAE) {
        /* XSAVE/XRSTOR/LDMXCSR/STMXCSR/FXSAVE/FXRSTOR */
        unsigned char modrm = rip[pos+2];
        int reg = (modrm >> 3) & 7;
        fprintf(stderr, "  >>> 0F AE /%d (XSAVE family)\n", reg);
        if (reg == 4) fprintf(stderr, "  = XSAVE\n");
        else if (reg == 5) fprintf(stderr, "  = XRSTOR\n");
        else if (reg == 0) fprintf(stderr, "  = FXSAVE\n");
        else if (reg == 1) fprintf(stderr, "  = FXRSTOR\n");
    } else if ((b & 0xF0) == 0x40) {
        /* REX prefix */
        fprintf(stderr, "  REX prefix: 0x%02x (W=%d R=%d X=%d B=%d)\n",
                b, (b>>3)&1, (b>>2)&1, (b>>1)&1, b&1);
        pos++;
        if (rip[pos] == 0x0F) {
            fprintf(stderr, "  Two-byte opcode: 0F %02x", rip[pos+1]);
            if (rip[pos+1] == 0x38)
                fprintf(stderr, " %02x (0F 38 extended)", rip[pos+2]);
            else if (rip[pos+1] == 0x3A)
                fprintf(stderr, " %02x (0F 3A extended)", rip[pos+2]);
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  Opcode after REX: 0x%02x\n", rip[pos]);
        }
    } else if (b == 0x0F) {
        fprintf(stderr, "  Two-byte opcode: 0F %02x", rip[pos+1]);
        if (rip[pos+1] == 0x38)
            fprintf(stderr, " %02x (0F 38 extended)", rip[pos+2]);
        else if (rip[pos+1] == 0x3A)
            fprintf(stderr, " %02x (0F 3A extended)", rip[pos+2]);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "  Single-byte opcode: 0x%02x\n", b);
    }
}

static LONG CALLBACK sigill_handler(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    /* Only catch illegal/privileged instruction exceptions */
    if (code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT *ctx = ep->ContextRecord;
    unsigned char *rip = (unsigned char *)(ULONG_PTR)ctx->Rip;

    fprintf(stderr, "\n");
    fprintf(stderr, "================================================\n");
    fprintf(stderr, "=== SIGILL TRAP: Exception 0x%08lx ===\n", code);
    fprintf(stderr, "================================================\n");

    if (code == EXCEPTION_ILLEGAL_INSTRUCTION)
        fprintf(stderr, "Type: ILLEGAL INSTRUCTION\n");
    else
        fprintf(stderr, "Type: PRIVILEGED INSTRUCTION\n");

    fprintf(stderr, "PID: %lu\n", GetCurrentProcessId());
    fprintf(stderr, "TID: %lu\n", GetCurrentThreadId());
    fprintf(stderr, "\n");

    /* Registers */
    fprintf(stderr, "RIP: 0x%016llx\n", (unsigned long long)ctx->Rip);
    fprintf(stderr, "RSP: 0x%016llx   RBP: 0x%016llx\n",
            (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
    fprintf(stderr, "RAX: 0x%016llx   RBX: 0x%016llx\n",
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx);
    fprintf(stderr, "RCX: 0x%016llx   RDX: 0x%016llx\n",
            (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
    fprintf(stderr, "RSI: 0x%016llx   RDI: 0x%016llx\n",
            (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi);
    fprintf(stderr, "R8:  0x%016llx   R9:  0x%016llx\n",
            (unsigned long long)ctx->R8, (unsigned long long)ctx->R9);
    fprintf(stderr, "R10: 0x%016llx   R11: 0x%016llx\n",
            (unsigned long long)ctx->R10, (unsigned long long)ctx->R11);
    fprintf(stderr, "R12: 0x%016llx   R13: 0x%016llx\n",
            (unsigned long long)ctx->R12, (unsigned long long)ctx->R13);
    fprintf(stderr, "R14: 0x%016llx   R15: 0x%016llx\n",
            (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);
    fprintf(stderr, "EFLAGS: 0x%08lx\n", ctx->EFlags);
    fprintf(stderr, "\n");

    /* Instruction bytes at RIP */
    fprintf(stderr, "Instruction bytes at RIP (32 bytes):\n");
    fprintf(stderr, "  %016llx:", (unsigned long long)ctx->Rip);
    int i;
    for (i = 0; i < 16; i++)
        fprintf(stderr, " %02x", rip[i]);
    fprintf(stderr, "\n  %016llx:", (unsigned long long)(ctx->Rip + 16));
    for (i = 16; i < 32; i++)
        fprintf(stderr, " %02x", rip[i]);
    fprintf(stderr, "\n\n");

    /* Instruction analysis */
    fprintf(stderr, "Instruction prefix analysis:\n");
    analyze_instruction(rip);
    fprintf(stderr, "\n");

    /* Module lookup */
    {
        HMODULE mod = NULL;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)(ULONG_PTR)ctx->Rip, &mod)) {
            char name[MAX_PATH];
            GetModuleFileNameA(mod, name, MAX_PATH);
            fprintf(stderr, "Faulting module: %s\n", name);
            fprintf(stderr, "  Base:   0x%016llx\n", (unsigned long long)(ULONG_PTR)mod);
            fprintf(stderr, "  RVA:    +0x%llx\n",
                    (unsigned long long)(ctx->Rip - (ULONG_PTR)mod));

            /* Check PE SizeOfImage */
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
            if (dos->e_magic == 0x5A4D) {
                IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
                fprintf(stderr, "  SizeOfImage: 0x%lx (%lu KB)\n",
                        nt->OptionalHeader.SizeOfImage,
                        nt->OptionalHeader.SizeOfImage / 1024);
            }
        } else {
            fprintf(stderr, "Faulting module: UNKNOWN (RIP not in any loaded module)\n");
            fprintf(stderr, "  Possibly JIT-generated code or unmapped memory.\n");
        }
    }

    /* Main exe info */
    {
        HMODULE exe = GetModuleHandleA(NULL);
        if (exe) {
            char exename[MAX_PATH];
            GetModuleFileNameA(exe, exename, MAX_PATH);
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
            ULONG_PTR exe_end = 0;
            if (dos->e_magic == 0x5A4D) {
                IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)exe + dos->e_lfanew);
                exe_end = (ULONG_PTR)exe + nt->OptionalHeader.SizeOfImage;
            }
            fprintf(stderr, "\nMain exe: %s\n", exename);
            fprintf(stderr, "  Base: 0x%016llx, End: 0x%016llx\n",
                    (unsigned long long)(ULONG_PTR)exe,
                    (unsigned long long)exe_end);
            if (ctx->Rip >= (DWORD64)(ULONG_PTR)exe && ctx->Rip < (DWORD64)exe_end) {
                fprintf(stderr, "  *** RIP IS INSIDE MAIN EXE ***\n");
                fprintf(stderr, "  File RVA: +0x%llx\n",
                        (unsigned long long)(ctx->Rip - (ULONG_PTR)exe));
            }
        }
    }

    /* Stack vicinity */
    fprintf(stderr, "\nStack (16 QWORDs from RSP):\n");
    {
        ULONG_PTR *sp = (ULONG_PTR *)(ULONG_PTR)ctx->Rsp;
        int j;
        for (j = 0; j < 16; j++) {
            MEMORY_BASIC_INFORMATION mbi;
            ULONG_PTR addr_to_check = (ULONG_PTR)&sp[j];
            if (VirtualQuery((LPCVOID)addr_to_check, &mbi, sizeof(mbi)) &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                fprintf(stderr, "  RSP+%02x: 0x%016llx",
                        j * 8, (unsigned long long)sp[j]);
                dump_module_for_addr((DWORD64)sp[j], "");
                if (sp[j] == 0)
                    fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "  RSP+%02x: <unreadable>\n", j * 8);
            }
        }
    }

    fprintf(stderr, "\n================================================\n");
    fprintf(stderr, "=== END SIGILL TRAP ===\n");
    fprintf(stderr, "================================================\n");
    fflush(stderr);
    fflush(stdout);

    /* Exit with recognizable code (0xDEAD = 57005) */
    ExitProcess(0xDEAD);

    return EXCEPTION_CONTINUE_SEARCH; /* unreachable */
}

/* ========================================================================
 * Galaxy64.dll Stub Exports
 * .def file maps mangled C++ names to these
 * ======================================================================== */

void* stub_return_null(void) { return NULL; }
void  stub_void(void) { }
void  stub_void_ptr(void* p) { (void)p; }

/* ========================================================================
 * DllMain — installs VEH + stubs Galaxy SDK
 * ======================================================================== */

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        /* Install VEH as FIRST handler (priority 1) */
        AddVectoredExceptionHandler(1, sigill_handler);

        char exename[MAX_PATH];
        GetModuleFileNameA(NULL, exename, MAX_PATH);
        fprintf(stderr, "[Galaxy64+VEH] SIGILL trap installed in PID %lu (%s)\n",
                GetCurrentProcessId(), exename);
        fflush(stderr);
    }
    return TRUE;
}
