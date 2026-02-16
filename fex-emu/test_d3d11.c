/**
 * Minimal D3D11 test for DXVK/Wine Vulkan initialization.
 * If this crashes with SIGILL, the problem is in DXVK/winevulkan.
 * If this works, the problem is in ys9.exe's own code.
 *
 * Compile: x86_64-w64-mingw32-gcc test_d3d11.c -o test_d3d11.exe -ld3d11 -ldxgi -O2 -s
 */
#include <windows.h>
#include <stdio.h>

/* Minimal D3D11 types — avoid needing full SDK headers */
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct IDXGIFactory1 IDXGIFactory1;
typedef struct IDXGIAdapter1 IDXGIAdapter1;

/* D3D11CreateDevice prototype */
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(
    void* pAdapter,
    int DriverType,
    HMODULE Software,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    void* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext
);

/* CreateDXGIFactory1 prototype */
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(
    const void* riid,
    void** ppFactory
);

int main(void)
{
    HMODULE hD3D11, hDXGI;
    PFN_D3D11CreateDevice pfnCreate;
    HRESULT hr;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;

    printf("=== D3D11 DXVK Test ===\n");
    fflush(stdout);

    /* Step 1: Load d3d11.dll (DXVK native) */
    printf("Loading d3d11.dll...\n");
    fflush(stdout);
    hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
        printf("FAILED: LoadLibrary d3d11.dll error %lu\n", GetLastError());
        return 1;
    }
    printf("OK: d3d11.dll loaded at %p\n", (void*)hD3D11);
    fflush(stdout);

    /* Step 2: Load dxgi.dll (DXVK native) */
    printf("Loading dxgi.dll...\n");
    fflush(stdout);
    hDXGI = LoadLibraryA("dxgi.dll");
    if (!hDXGI) {
        printf("FAILED: LoadLibrary dxgi.dll error %lu\n", GetLastError());
        return 2;
    }
    printf("OK: dxgi.dll loaded at %p\n", (void*)hDXGI);
    fflush(stdout);

    /* Step 3: Get D3D11CreateDevice */
    printf("Getting D3D11CreateDevice...\n");
    fflush(stdout);
    pfnCreate = (PFN_D3D11CreateDevice)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pfnCreate) {
        printf("FAILED: D3D11CreateDevice not found\n");
        return 3;
    }
    printf("OK: D3D11CreateDevice at %p\n", (void*)pfnCreate);
    fflush(stdout);

    /* Step 4: Create D3D11 device (triggers DXVK Vulkan init) */
    printf("Calling D3D11CreateDevice (triggers DXVK->Vulkan)...\n");
    fflush(stdout);
    hr = pfnCreate(
        NULL,           /* pAdapter — NULL = default */
        1,              /* D3D_DRIVER_TYPE_HARDWARE */
        NULL,           /* Software */
        0,              /* Flags */
        NULL,           /* pFeatureLevels — NULL = all */
        0,              /* FeatureLevels */
        7,              /* D3D11_SDK_VERSION */
        &device,
        NULL,           /* pFeatureLevel */
        &ctx
    );

    if (hr >= 0) {
        printf("SUCCESS! D3D11 device created (DXVK Vulkan init worked)\n");
        printf("  Device: %p\n", (void*)device);
        printf("  Context: %p\n", (void*)ctx);
        fflush(stdout);
        /* Don't bother releasing — just exit cleanly */
    } else {
        printf("D3D11CreateDevice returned 0x%08lX\n", (unsigned long)hr);
        if (hr == (HRESULT)0x887a0004)
            printf("  = DXGI_ERROR_UNSUPPORTED\n");
        else if (hr == (HRESULT)0x80070057)
            printf("  = E_INVALIDARG\n");
        else
            printf("  (check HRESULT)\n");
        fflush(stdout);
    }

    printf("=== Test complete (no SIGILL!) ===\n");
    fflush(stdout);
    return 0;
}
