/*
 * Stub D3DCompiler_47.dll — satisfies import without wined3d dependency.
 *
 * DXVK handles shader compilation internally (DXBC → SPIR-V).
 * Wine's builtin d3dcompiler_47 depends on wined3d.so which causes SIGILL
 * under FEX (unsupported instructions). This stub exports the same symbols
 * but returns E_FAIL for actual compilation calls.
 *
 * Build: x86_64-w64-mingw32-gcc -shared -o d3dcompiler_47.dll d3dcompiler_47_stub.c
 */

#include <stdint.h>

#define WINAPI __attribute__((ms_abi))
#define HRESULT int32_t
#define S_OK 0
#define E_FAIL ((HRESULT)0x80004005)

typedef void *ID3DBlob;
typedef void *ID3D11Module;
typedef void *ID3D11Linker;

HRESULT WINAPI D3DCompile(const void *data, size_t data_size,
    const char *filename, const void *defines, void *include,
    const char *entrypoint, const char *target, unsigned flags1,
    unsigned flags2, ID3DBlob **code, ID3DBlob **errors)
{
    return E_FAIL;
}

HRESULT WINAPI D3DCompile2(const void *data, size_t data_size,
    const char *filename, const void *defines, void *include,
    const char *entrypoint, const char *target, unsigned flags1,
    unsigned flags2, unsigned secondary_flags, const void *secondary,
    size_t secondary_size, ID3DBlob **code, ID3DBlob **errors)
{
    return E_FAIL;
}

HRESULT WINAPI D3DCompileFromFile(const void *filename, const void *defines,
    void *include, const char *entrypoint, const char *target,
    unsigned flags1, unsigned flags2, ID3DBlob **code, ID3DBlob **errors)
{
    return E_FAIL;
}

HRESULT WINAPI D3DCreateBlob(size_t size, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DDisassemble(const void *data, size_t data_size,
    unsigned flags, const char *comments, ID3DBlob **disassembly)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetBlobPart(const void *data, size_t data_size,
    int part, unsigned flags, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetDebugInfo(const void *data, size_t data_size,
    ID3DBlob **debug_info)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetInputAndOutputSignatureBlob(const void *data,
    size_t data_size, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetInputSignatureBlob(const void *data,
    size_t data_size, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetOutputSignatureBlob(const void *data,
    size_t data_size, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DGetTraceInstructionOffsets(const void *data,
    size_t data_size, unsigned flags, size_t start, size_t count,
    size_t *offsets, size_t *total)
{
    return E_FAIL;
}

HRESULT WINAPI D3DPreprocess(const void *data, size_t data_size,
    const char *filename, const void *defines, void *include,
    ID3DBlob **shader, ID3DBlob **errors)
{
    return E_FAIL;
}

HRESULT WINAPI D3DReadFileToBlob(const void *filename, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DReflect(const void *data, size_t data_size,
    const void *iid, void **reflector)
{
    return E_FAIL;
}

HRESULT WINAPI D3DReflectLibrary(const void *data, size_t data_size,
    const void *iid, void **reflector)
{
    return E_FAIL;
}

HRESULT WINAPI D3DSetBlobPart(const void *data, size_t data_size,
    int part, unsigned flags, const void *new_part, size_t new_part_size,
    ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DStripShader(const void *data, size_t data_size,
    unsigned flags, ID3DBlob **blob)
{
    return E_FAIL;
}

HRESULT WINAPI D3DWriteBlobToFile(ID3DBlob *blob, const void *filename,
    int overwrite)
{
    return E_FAIL;
}

HRESULT WINAPI D3DCreateLinker(ID3D11Linker **linker)
{
    return E_FAIL;
}

HRESULT WINAPI D3DLoadModule(const void *data, size_t size,
    ID3D11Module **module)
{
    return E_FAIL;
}

int WINAPI DllMain(void *instance, unsigned reason, void *reserved)
{
    return 1;
}
