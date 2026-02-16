/**
 * DLL loading isolation test.
 * Loads each of ys9.exe's game DLLs one by one to find which DllMain crashes.
 * If a DLL's DllMain has an illegal instruction, we'll catch it here.
 *
 * Compile: x86_64-w64-mingw32-gcc test_dll_load.c -o test_dll_load.exe -O2 -s
 */
#include <windows.h>
#include <stdio.h>

static void test_load(const char *name)
{
    HMODULE h;
    printf("Loading %s ... ", name);
    fflush(stdout);
    h = LoadLibraryA(name);
    if (h) {
        printf("OK (at %p)\n", (void*)h);
        FreeLibrary(h);
    } else {
        printf("FAILED (error %lu)\n", GetLastError());
    }
    fflush(stdout);
}

int main(void)
{
    printf("=== DLL Load Isolation Test ===\n");
    fflush(stdout);

    /* Phase 1: System DLLs (Wine builtins) */
    printf("\n--- Phase 1: System DLLs ---\n"); fflush(stdout);
    test_load("kernel32.dll");
    test_load("user32.dll");
    test_load("gdi32.dll");
    test_load("advapi32.dll");
    test_load("ole32.dll");
    test_load("shell32.dll");
    test_load("msvcrt.dll");

    /* Phase 2: CRT (game uses MSVCR100) */
    printf("\n--- Phase 2: CRT ---\n"); fflush(stdout);
    test_load("MSVCR100.dll");
    test_load("ucrtbase.dll");

    /* Phase 3: DirectX / DXVK (native) */
    printf("\n--- Phase 3: DirectX/DXVK ---\n"); fflush(stdout);
    test_load("d3d11.dll");
    test_load("dxgi.dll");
    test_load("D3DCOMPILER_47.dll");

    /* Phase 4: Media / Input */
    printf("\n--- Phase 4: Media/Input ---\n"); fflush(stdout);
    test_load("WINMM.dll");
    test_load("msacm32.dll");
    test_load("xaudio2_7.dll");
    test_load("XINPUT9_1_0.dll");
    test_load("mfplat.dll");
    test_load("mfreadwrite.dll");

    /* Phase 5: Game's bundled DLLs (native, in game dir) */
    printf("\n--- Phase 5: Game DLLs (from game dir) ---\n"); fflush(stdout);
    test_load("libogg.dll");
    test_load("libvorbis.dll");
    test_load("libvorbisfile.dll");
    test_load("steam_api64.dll");
    test_load("GFSDK_SSAO_D3D11.win64.dll");
    test_load("Galaxy64.dll");

    printf("\n=== All DLLs loaded OK (no SIGILL!) ===\n");
    fflush(stdout);
    return 0;
}
