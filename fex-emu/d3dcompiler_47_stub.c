/*
 * Stub D3DCompiler_47.dll with working D3DReflect — DXBC bytecode reflection.
 *
 * DXVK handles shader compilation internally (DXBC → SPIR-V).
 * Wine's builtin d3dcompiler_47 is unavailable in our Proton-GE rootfs.
 * This stub exports the same symbols, returns E_FAIL for compilation,
 * but implements D3DReflect with real DXBC parsing.
 *
 * The game (Ys IX) calls D3DReflect ~229 times during shader init to
 * get ID3D11ShaderReflection::GetDesc() for each shader. Without this,
 * the main thread blocks forever waiting for shader setup to complete.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -shared -o d3dcompiler_47.dll d3dcompiler_47_stub.c \
 *       d3dcompiler_47_stub.def -O2 -s
 */

#include <windows.h>
#include <string.h>
#include <stdio.h>

/* E_FAIL, E_INVALIDARG, E_NOINTERFACE already defined in winerror.h */

typedef void *ID3DBlob;
typedef void *ID3D11Module;
typedef void *ID3D11Linker;

/* ========================================================================
 * DXBC chunk FourCC codes
 * ======================================================================== */
#define FOURCC(a,b,c,d) ((UINT)(a)|((UINT)(b)<<8)|((UINT)(c)<<16)|((UINT)(d)<<24))
#define DXBC_MAGIC      FOURCC('D','X','B','C')
#define CHUNK_ISGN      FOURCC('I','S','G','N')
#define CHUNK_ISG1      FOURCC('I','S','G','1')
#define CHUNK_OSGN      FOURCC('O','S','G','N')
#define CHUNK_OSG1      FOURCC('O','S','G','1')
#define CHUNK_OSG5      FOURCC('O','S','G','5')
#define CHUNK_PCSG      FOURCC('P','C','S','G')
#define CHUNK_RDEF      FOURCC('R','D','E','F')
#define CHUNK_SHDR      FOURCC('S','H','D','R')
#define CHUNK_SHEX      FOURCC('S','H','E','X')
#define CHUNK_STAT      FOURCC('S','T','A','T')

/* ========================================================================
 * D3D11_SHADER_DESC — filled by GetDesc()
 * ======================================================================== */
typedef struct {
    UINT Version;
    const char *Creator;
    UINT Flags;
    UINT ConstantBuffers;
    UINT BoundResources;
    UINT InputParameters;
    UINT OutputParameters;
    UINT InstructionCount;
    UINT TempRegisterCount;
    UINT TempArrayCount;
    UINT DefCount;
    UINT DclCount;
    UINT TextureNormalInstructions;
    UINT TextureLoadInstructions;
    UINT TextureCompInstructions;
    UINT TextureBiasInstructions;
    UINT TextureGradientInstructions;
    UINT FloatInstructionCount;
    UINT IntInstructionCount;
    UINT UintInstructionCount;
    UINT StaticFlowControlCount;
    UINT DynamicFlowControlCount;
    UINT MacroInstructionCount;
    UINT ArrayInstructionCount;
    UINT CutInstructionCount;
    UINT EmitInstructionCount;
    UINT GSOutputTopology;       /* D3D_PRIMITIVE_TOPOLOGY */
    UINT GSMaxOutputVertexCount;
    UINT InputPrimitive;         /* D3D_PRIMITIVE */
    UINT PatchConstantParameters;
    UINT cGSInstanceCount;
    UINT cControlPoints;
    UINT HSOutputPrimitive;      /* D3D_TESSELLATOR_OUTPUT_PRIMITIVE */
    UINT HSPartitioning;         /* D3D_TESSELLATOR_PARTITIONING */
    UINT TessellatorDomain;      /* D3D_TESSELLATOR_DOMAIN */
    UINT cBarrierInstructions;
    UINT cInterlockedInstructions;
    UINT cTextureStoreInstructions;
} D3D11_SHADER_DESC;

/* ========================================================================
 * D3D11_SIGNATURE_PARAMETER_DESC — for GetInputParameterDesc/GetOutputParameterDesc
 * ======================================================================== */
typedef struct {
    const char *SemanticName;
    UINT SemanticIndex;
    UINT Register;
    UINT SystemValueType;    /* D3D_NAME */
    UINT ComponentType;      /* D3D_REGISTER_COMPONENT_TYPE */
    BYTE Mask;
    BYTE ReadWriteMask;
    UINT Stream;
    UINT MinPrecision;       /* D3D_MIN_PRECISION */
} D3D11_SIGNATURE_PARAMETER_DESC;

/* ========================================================================
 * D3D11_SHADER_INPUT_BIND_DESC — for GetResourceBindingDesc
 * ======================================================================== */
typedef struct {
    const char *Name;
    UINT Type;               /* D3D_SHADER_INPUT_TYPE */
    UINT BindPoint;
    UINT BindCount;
    UINT uFlags;
    UINT ReturnType;         /* D3D_RESOURCE_RETURN_TYPE */
    UINT Dimension;          /* D3D_SRV_DIMENSION */
    UINT NumSamples;
    UINT Space;
    UINT uID;
} D3D11_SHADER_INPUT_BIND_DESC;

/* ========================================================================
 * DXBC signature element (on-disk format)
 * ======================================================================== */
typedef struct {
    UINT name_offset;    /* offset from chunk data start to name string */
    UINT semantic_index;
    UINT system_value;   /* D3D_NAME */
    UINT component_type; /* D3D_REGISTER_COMPONENT_TYPE */
    UINT register_num;
    BYTE mask;
    BYTE rw_mask;
    BYTE unused[2];
} DXBC_SIGNATURE_ELEMENT;

/* ========================================================================
 * DXBC RDEF resource binding (on-disk format)
 * ======================================================================== */
typedef struct {
    UINT name_offset;
    UINT type;           /* D3D_SHADER_INPUT_TYPE */
    UINT return_type;    /* D3D_RESOURCE_RETURN_TYPE */
    UINT dimension;      /* D3D_SRV_DIMENSION */
    UINT num_samples;
    UINT bind_point;
    UINT bind_count;
    UINT flags;
} DXBC_RDEF_BINDING;

/* Max parameters we track per shader */
#define MAX_SIG_PARAMS 64
#define MAX_RESOURCES  128

/* ========================================================================
 * Mock ID3D11ShaderReflection with parsed DXBC data
 * ======================================================================== */
typedef struct MockReflection MockReflection;

/* vtable type (ID3D11ShaderReflection) */
typedef struct {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(MockReflection*, const GUID*, void**);
    ULONG   (__stdcall *AddRef)(MockReflection*);
    ULONG   (__stdcall *Release)(MockReflection*);
    /* ID3D11ShaderReflection */
    HRESULT (__stdcall *GetDesc)(MockReflection*, D3D11_SHADER_DESC*);
    void*   (__stdcall *GetConstantBufferByIndex)(MockReflection*, UINT);
    void*   (__stdcall *GetConstantBufferByName)(MockReflection*, const char*);
    HRESULT (__stdcall *GetResourceBindingDesc)(MockReflection*, UINT, D3D11_SHADER_INPUT_BIND_DESC*);
    HRESULT (__stdcall *GetInputParameterDesc)(MockReflection*, UINT, D3D11_SIGNATURE_PARAMETER_DESC*);
    HRESULT (__stdcall *GetOutputParameterDesc)(MockReflection*, UINT, D3D11_SIGNATURE_PARAMETER_DESC*);
    HRESULT (__stdcall *GetPatchConstantParameterDesc)(MockReflection*, UINT, D3D11_SIGNATURE_PARAMETER_DESC*);
    void*   (__stdcall *GetVariableByName)(MockReflection*, const char*);
    HRESULT (__stdcall *GetResourceBindingDescByName)(MockReflection*, const char*, D3D11_SHADER_INPUT_BIND_DESC*);
    UINT    (__stdcall *GetMovInstructionCount)(MockReflection*);
    UINT    (__stdcall *GetMovcInstructionCount)(MockReflection*);
    UINT    (__stdcall *GetConversionInstructionCount)(MockReflection*);
    UINT    (__stdcall *GetBitwiseInstructionCount)(MockReflection*);
    UINT    (__stdcall *GetGSInputPrimitive)(MockReflection*);
    BOOL    (__stdcall *IsLevel9Shader)(MockReflection*);
    BOOL    (__stdcall *IsSampleFrequencyShader)(MockReflection*);
    UINT    (__stdcall *GetNumInterfaceSlots)(MockReflection*);
    HRESULT (__stdcall *GetMinFeatureLevel)(MockReflection*, UINT*);
    UINT    (__stdcall *GetThreadGroupSize)(MockReflection*, UINT*, UINT*, UINT*);
    UINT64  (__stdcall *GetRequiresFlags)(MockReflection*);
} ReflectionVtbl;

struct MockReflection {
    const ReflectionVtbl *vptr;
    volatile LONG refcount;

    /* Parsed DXBC data */
    D3D11_SHADER_DESC desc;

    /* Input signature */
    UINT input_count;
    D3D11_SIGNATURE_PARAMETER_DESC inputs[MAX_SIG_PARAMS];

    /* Output signature */
    UINT output_count;
    D3D11_SIGNATURE_PARAMETER_DESC outputs[MAX_SIG_PARAMS];

    /* Patch constant signature */
    UINT patch_count;
    D3D11_SIGNATURE_PARAMETER_DESC patches[MAX_SIG_PARAMS];

    /* Resource bindings */
    UINT resource_count;
    D3D11_SHADER_INPUT_BIND_DESC resources[MAX_RESOURCES];

    /* Raw DXBC data pointer (kept alive for string references) */
    const BYTE *dxbc_data;
    size_t dxbc_size;
};

/* ========================================================================
 * DXBC parsing helpers
 * ======================================================================== */

/* Find a chunk in DXBC data. Returns pointer to chunk data (after fourcc+size), sets *out_size. */
static const BYTE *find_chunk(const BYTE *dxbc, size_t dxbc_size,
                               UINT fourcc, UINT *out_size)
{
    if (dxbc_size < 32) return NULL;
    UINT chunk_count = *(const UINT*)(dxbc + 28);
    if (dxbc_size < 32 + chunk_count * 4) return NULL;

    const UINT *offsets = (const UINT*)(dxbc + 32);
    for (UINT i = 0; i < chunk_count; i++) {
        UINT off = offsets[i];
        if (off + 8 > dxbc_size) continue;
        UINT cc = *(const UINT*)(dxbc + off);
        UINT sz = *(const UINT*)(dxbc + off + 4);
        if (cc == fourcc) {
            if (out_size) *out_size = sz;
            return dxbc + off + 8;
        }
    }
    return NULL;
}

/* Parse a signature chunk (ISGN/OSGN/PCSG) into parameter descriptors */
static UINT parse_signature(const BYTE *chunk, UINT chunk_size,
                            D3D11_SIGNATURE_PARAMETER_DESC *out, UINT max_out)
{
    if (!chunk || chunk_size < 8) return 0;
    UINT count = *(const UINT*)chunk;
    /* UINT flags = *(const UINT*)(chunk + 4); */
    if (count > max_out) count = max_out;

    const DXBC_SIGNATURE_ELEMENT *elems =
        (const DXBC_SIGNATURE_ELEMENT *)(chunk + 8);

    for (UINT i = 0; i < count; i++) {
        if ((const BYTE*)&elems[i+1] > chunk + chunk_size) {
            count = i;
            break;
        }
        /* name_offset is relative to start of chunk data (after fourcc+size) */
        if (elems[i].name_offset < chunk_size)
            out[i].SemanticName = (const char*)(chunk + elems[i].name_offset);
        else
            out[i].SemanticName = "UNKNOWN";
        out[i].SemanticIndex = elems[i].semantic_index;
        out[i].Register = elems[i].register_num;
        out[i].SystemValueType = elems[i].system_value;
        out[i].ComponentType = elems[i].component_type;
        out[i].Mask = elems[i].mask;
        out[i].ReadWriteMask = elems[i].rw_mask;
        out[i].Stream = 0;
        out[i].MinPrecision = 0;
    }
    return count;
}

/* Parse RDEF chunk for resource binding info */
static UINT parse_rdef_bindings(const BYTE *chunk, UINT chunk_size,
                                D3D11_SHADER_INPUT_BIND_DESC *out, UINT max_out,
                                UINT *out_cbuf_count)
{
    if (!chunk || chunk_size < 16) return 0;
    UINT cbuf_count = *(const UINT*)(chunk + 0);
    UINT cbuf_offset = *(const UINT*)(chunk + 4);
    UINT bind_count = *(const UINT*)(chunk + 8);
    UINT bind_offset = *(const UINT*)(chunk + 12);
    (void)cbuf_offset;

    if (out_cbuf_count) *out_cbuf_count = cbuf_count;
    if (bind_count > max_out) bind_count = max_out;

    if (bind_offset + bind_count * sizeof(DXBC_RDEF_BINDING) > chunk_size)
        return 0;

    const DXBC_RDEF_BINDING *bindings =
        (const DXBC_RDEF_BINDING *)(chunk + bind_offset);

    for (UINT i = 0; i < bind_count; i++) {
        if (bindings[i].name_offset < chunk_size)
            out[i].Name = (const char*)(chunk + bindings[i].name_offset);
        else
            out[i].Name = "unknown";
        out[i].Type = bindings[i].type;
        out[i].BindPoint = bindings[i].bind_point;
        out[i].BindCount = bindings[i].bind_count;
        out[i].uFlags = bindings[i].flags;
        out[i].ReturnType = bindings[i].return_type;
        out[i].Dimension = bindings[i].dimension;
        out[i].NumSamples = bindings[i].num_samples;
        out[i].Space = 0;
        out[i].uID = i;
    }
    return bind_count;
}

/* ========================================================================
 * ID3D11ShaderReflection implementation
 * ======================================================================== */

static HRESULT __stdcall refl_QueryInterface(MockReflection *self, const GUID *riid, void **ppv)
{
    if (!ppv) return E_NOINTERFACE;
    *ppv = self;
    InterlockedIncrement(&self->refcount);
    return S_OK;
}

static ULONG __stdcall refl_AddRef(MockReflection *self)
{
    return InterlockedIncrement(&self->refcount);
}

static ULONG __stdcall refl_Release(MockReflection *self)
{
    LONG ref = InterlockedDecrement(&self->refcount);
    if (ref <= 0) {
        HeapFree(GetProcessHeap(), 0, self);
        return 0;
    }
    return ref;
}

static HRESULT __stdcall refl_GetDesc(MockReflection *self, D3D11_SHADER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    memcpy(desc, &self->desc, sizeof(*desc));
    return S_OK;
}

/* Mock constant buffer — returns NULL for now (game may not need it) */
static void* __stdcall refl_GetConstantBufferByIndex(MockReflection *self, UINT index)
{
    return NULL;
}

static void* __stdcall refl_GetConstantBufferByName(MockReflection *self, const char *name)
{
    return NULL;
}

static HRESULT __stdcall refl_GetResourceBindingDesc(MockReflection *self, UINT index,
    D3D11_SHADER_INPUT_BIND_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    if (index >= self->resource_count) return E_INVALIDARG;
    memcpy(desc, &self->resources[index], sizeof(*desc));
    return S_OK;
}

static HRESULT __stdcall refl_GetInputParameterDesc(MockReflection *self, UINT index,
    D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    if (index >= self->input_count) return E_INVALIDARG;
    memcpy(desc, &self->inputs[index], sizeof(*desc));
    return S_OK;
}

static HRESULT __stdcall refl_GetOutputParameterDesc(MockReflection *self, UINT index,
    D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    if (index >= self->output_count) return E_INVALIDARG;
    memcpy(desc, &self->outputs[index], sizeof(*desc));
    return S_OK;
}

static HRESULT __stdcall refl_GetPatchConstantParameterDesc(MockReflection *self, UINT index,
    D3D11_SIGNATURE_PARAMETER_DESC *desc)
{
    if (!desc) return E_INVALIDARG;
    if (index >= self->patch_count) return E_INVALIDARG;
    memcpy(desc, &self->patches[index], sizeof(*desc));
    return S_OK;
}

static void* __stdcall refl_GetVariableByName(MockReflection *self, const char *name)
{
    return NULL;
}

static HRESULT __stdcall refl_GetResourceBindingDescByName(MockReflection *self,
    const char *name, D3D11_SHADER_INPUT_BIND_DESC *desc)
{
    if (!desc || !name) return E_INVALIDARG;
    for (UINT i = 0; i < self->resource_count; i++) {
        if (self->resources[i].Name && strcmp(self->resources[i].Name, name) == 0) {
            memcpy(desc, &self->resources[i], sizeof(*desc));
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

static UINT __stdcall refl_GetMovInstructionCount(MockReflection *self) { return 0; }
static UINT __stdcall refl_GetMovcInstructionCount(MockReflection *self) { return 0; }
static UINT __stdcall refl_GetConversionInstructionCount(MockReflection *self) { return 0; }
static UINT __stdcall refl_GetBitwiseInstructionCount(MockReflection *self) { return 0; }
static UINT __stdcall refl_GetGSInputPrimitive(MockReflection *self) { return 0; }
static BOOL __stdcall refl_IsLevel9Shader(MockReflection *self) { return FALSE; }
static BOOL __stdcall refl_IsSampleFrequencyShader(MockReflection *self) { return FALSE; }
static UINT __stdcall refl_GetNumInterfaceSlots(MockReflection *self) { return 0; }
static HRESULT __stdcall refl_GetMinFeatureLevel(MockReflection *self, UINT *level)
{
    if (level) *level = 0xb000; /* D3D_FEATURE_LEVEL_11_0 */
    return S_OK;
}
static UINT __stdcall refl_GetThreadGroupSize(MockReflection *self, UINT *x, UINT *y, UINT *z)
{
    if (x) *x = 0;
    if (y) *y = 0;
    if (z) *z = 0;
    return 0;
}
static UINT64 __stdcall refl_GetRequiresFlags(MockReflection *self) { return 0; }

static const ReflectionVtbl g_refl_vtbl = {
    refl_QueryInterface,
    refl_AddRef,
    refl_Release,
    refl_GetDesc,
    refl_GetConstantBufferByIndex,
    refl_GetConstantBufferByName,
    refl_GetResourceBindingDesc,
    refl_GetInputParameterDesc,
    refl_GetOutputParameterDesc,
    refl_GetPatchConstantParameterDesc,
    refl_GetVariableByName,
    refl_GetResourceBindingDescByName,
    refl_GetMovInstructionCount,
    refl_GetMovcInstructionCount,
    refl_GetConversionInstructionCount,
    refl_GetBitwiseInstructionCount,
    refl_GetGSInputPrimitive,
    refl_IsLevel9Shader,
    refl_IsSampleFrequencyShader,
    refl_GetNumInterfaceSlots,
    refl_GetMinFeatureLevel,
    refl_GetThreadGroupSize,
    refl_GetRequiresFlags,
};

/* ========================================================================
 * D3DReflect — parse DXBC and return ID3D11ShaderReflection
 * ======================================================================== */

HRESULT WINAPI D3DReflect(const void *data, size_t data_size,
    const void *iid, void **reflector)
{
    if (!data || !reflector || data_size < 32)
        return E_INVALIDARG;

    const BYTE *dxbc = (const BYTE *)data;

    /* Validate DXBC magic */
    if (*(const UINT*)dxbc != DXBC_MAGIC)
        return E_INVALIDARG;

    MockReflection *refl = (MockReflection *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MockReflection));
    if (!refl) return E_FAIL;

    refl->vptr = &g_refl_vtbl;
    refl->refcount = 1;
    refl->dxbc_data = dxbc;
    refl->dxbc_size = data_size;
    refl->desc.Creator = "d3dcompiler_47_stub";

    /* Parse SHDR/SHEX for version */
    UINT shdr_size = 0;
    const BYTE *shdr = find_chunk(dxbc, data_size, CHUNK_SHDR, &shdr_size);
    if (!shdr) shdr = find_chunk(dxbc, data_size, CHUNK_SHEX, &shdr_size);
    if (shdr && shdr_size >= 4) {
        refl->desc.Version = *(const UINT*)shdr;
    }

    /* Parse ISGN / ISG1 for input signature */
    UINT isgn_size = 0;
    const BYTE *isgn = find_chunk(dxbc, data_size, CHUNK_ISGN, &isgn_size);
    if (!isgn) isgn = find_chunk(dxbc, data_size, CHUNK_ISG1, &isgn_size);
    refl->input_count = parse_signature(isgn, isgn_size, refl->inputs, MAX_SIG_PARAMS);
    refl->desc.InputParameters = refl->input_count;

    /* Parse OSGN / OSG1 / OSG5 for output signature */
    UINT osgn_size = 0;
    const BYTE *osgn = find_chunk(dxbc, data_size, CHUNK_OSGN, &osgn_size);
    if (!osgn) osgn = find_chunk(dxbc, data_size, CHUNK_OSG1, &osgn_size);
    if (!osgn) osgn = find_chunk(dxbc, data_size, CHUNK_OSG5, &osgn_size);
    refl->output_count = parse_signature(osgn, osgn_size, refl->outputs, MAX_SIG_PARAMS);
    refl->desc.OutputParameters = refl->output_count;

    /* Parse PCSG for patch constant signature */
    UINT pcsg_size = 0;
    const BYTE *pcsg = find_chunk(dxbc, data_size, CHUNK_PCSG, &pcsg_size);
    refl->patch_count = parse_signature(pcsg, pcsg_size, refl->patches, MAX_SIG_PARAMS);
    refl->desc.PatchConstantParameters = refl->patch_count;

    /* Parse RDEF for resource bindings and constant buffer count */
    UINT rdef_size = 0;
    const BYTE *rdef = find_chunk(dxbc, data_size, CHUNK_RDEF, &rdef_size);
    UINT cbuf_count = 0;
    refl->resource_count = parse_rdef_bindings(rdef, rdef_size,
        refl->resources, MAX_RESOURCES, &cbuf_count);
    refl->desc.ConstantBuffers = cbuf_count;
    refl->desc.BoundResources = refl->resource_count;

    /* Parse STAT chunk for instruction counts (if present) */
    UINT stat_size = 0;
    const BYTE *stat = find_chunk(dxbc, data_size, CHUNK_STAT, &stat_size);
    if (stat && stat_size >= 29 * 4) {
        const UINT *s = (const UINT*)stat;
        refl->desc.InstructionCount = s[0];
        refl->desc.TempRegisterCount = s[1];
        refl->desc.DefCount = s[2];
        refl->desc.DclCount = s[3];
        refl->desc.FloatInstructionCount = s[4];
        refl->desc.IntInstructionCount = s[5];
        refl->desc.UintInstructionCount = s[6];
        refl->desc.StaticFlowControlCount = s[7];
        refl->desc.DynamicFlowControlCount = s[8];
        /* s[9] is unused / macro instruction count */
        refl->desc.TempArrayCount = s[10];
        refl->desc.ArrayInstructionCount = s[11];
        refl->desc.CutInstructionCount = s[12];
        refl->desc.EmitInstructionCount = s[13];
        refl->desc.TextureNormalInstructions = s[14];
        refl->desc.TextureLoadInstructions = s[15];
        refl->desc.TextureCompInstructions = s[16];
        refl->desc.TextureBiasInstructions = s[17];
        refl->desc.TextureGradientInstructions = s[18];
        /* s[19..28] are GSOutputTopology, GSMaxOutputVertexCount, etc. */
        if (stat_size >= 37 * 4) {
            refl->desc.GSOutputTopology = s[19];
            refl->desc.GSMaxOutputVertexCount = s[20];
            refl->desc.InputPrimitive = s[21];
            /* s[22] = PatchConstantParameters (redundant) */
            refl->desc.cGSInstanceCount = s[23];
            refl->desc.cControlPoints = s[24];
            refl->desc.HSOutputPrimitive = s[25];
            refl->desc.HSPartitioning = s[26];
            refl->desc.TessellatorDomain = s[27];
            refl->desc.cBarrierInstructions = s[28];
            refl->desc.cInterlockedInstructions = s[29];
            refl->desc.cTextureStoreInstructions = s[30];
        }
    }

    *reflector = refl;
    return S_OK;
}

/* ========================================================================
 * Other D3DCompiler exports (stubs returning E_FAIL)
 * ======================================================================== */

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

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hDll);
        fprintf(stderr, "[D3DCompiler47Stub] Loaded with D3DReflect support (PID %lu)\n",
                GetCurrentProcessId());
        fflush(stderr);
    }
    return TRUE;
}
