/**
 * Stub DLL for GFSDK_SSAO_D3D11.win64.dll (NVIDIA HBAO+ / GameWorks SSAO)
 *
 * The real DLL is compiled with AVX2 instructions that FEX-Emu cannot emulate,
 * causing SIGILL (exit 132). This stub satisfies the game's import table while
 * returning "not supported" errors that the game can handle gracefully.
 *
 * Exports:
 *   GFSDK_SSAO_CreateContext_D3D11 — returns GFSDK_SSAO_D3D_RESOURCE_CREATION_FAILED
 *   GFSDK_SSAO_GetVersion          — fills version struct, returns OK
 *
 * Compile (x86-64 mingw):
 *   x86_64-w64-mingw32-gcc -shared -o GFSDK_SSAO_D3D11.win64.dll gfsdk_ssao_stub.c \
 *       -Wl,--out-implib,libGFSDK_SSAO.a -O2 -s
 */

#include <stddef.h>

/* GFSDK_SSAO_Status enum values */
#define GFSDK_SSAO_OK                              0
#define GFSDK_SSAO_VERSION_MISMATCH                1
#define GFSDK_SSAO_D3D_FEATURE_LEVEL_NOT_SUPPORTED 14
#define GFSDK_SSAO_D3D_RESOURCE_CREATION_FAILED    15

typedef unsigned int GFSDK_SSAO_Status;
typedef unsigned int GFSDK_SSAO_UINT;

typedef struct {
    GFSDK_SSAO_UINT Major;
    GFSDK_SSAO_UINT Minor;
    GFSDK_SSAO_UINT Branch;
    GFSDK_SSAO_UINT Revision;
} GFSDK_SSAO_Version;

typedef struct {
    void* (*new_)(size_t);
    void (*delete_)(void*);
} GFSDK_SSAO_CustomHeap;

/**
 * GFSDK_SSAO_CreateContext_D3D11 — Create an SSAO rendering context.
 *
 * Returns D3D_RESOURCE_CREATION_FAILED to signal "SSAO unavailable".
 * Games typically handle this by disabling SSAO and continuing.
 *
 * Note: GFSDK_SSAO_Version is 16 bytes, passed by value on x64 Windows ABI
 * it goes in 2 registers (or stack). We accept it as a struct.
 */
__attribute__((dllexport))
GFSDK_SSAO_Status GFSDK_SSAO_CreateContext_D3D11(
    void* pD3DDevice,
    void** ppContext,
    const GFSDK_SSAO_CustomHeap* pCustomHeap,
    GFSDK_SSAO_Version HeaderVersion)
{
    (void)pD3DDevice;
    (void)pCustomHeap;
    (void)HeaderVersion;

    if (ppContext)
        *ppContext = NULL;

    return GFSDK_SSAO_D3D_RESOURCE_CREATION_FAILED;
}

/**
 * GFSDK_SSAO_GetVersion — Report the SSAO library version.
 * Returns version 4.0.0.0 (matches typical HBAO+ 4.x header).
 */
__attribute__((dllexport))
GFSDK_SSAO_Status GFSDK_SSAO_GetVersion(GFSDK_SSAO_Version* pVersion)
{
    if (pVersion) {
        pVersion->Major    = 4;
        pVersion->Minor    = 0;
        pVersion->Branch   = 0;
        pVersion->Revision = 0;
    }
    return GFSDK_SSAO_OK;
}

/* DllMain — minimal, no-op */
int __stdcall DllMain(void* hInstance, unsigned long dwReason, void* lpReserved)
{
    (void)hInstance;
    (void)dwReason;
    (void)lpReserved;
    return 1;
}
