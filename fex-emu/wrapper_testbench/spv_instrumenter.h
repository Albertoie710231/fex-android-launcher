// C-side header for the SPIR-V instrumentation engine. The shim's
// vkCreateShaderModule hook calls these functions; the implementation
// lives in spv_instrumenter.cpp.
#ifndef SHIM_SPV_INSTRUMENTER_H
#define SHIM_SPV_INSTRUMENTER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 on success (out_code populated; caller frees via shim_spv_free).
int shim_spv_instrument(const uint32_t *in_code, size_t in_size_bytes,
                        uint32_t **out_code, size_t *out_size_bytes);

void shim_spv_free(uint32_t *code);

extern volatile int shim_m5_spirv_instrument_count;
extern volatile int shim_m5_spirv_loads_seen;
extern volatile int shim_m5_spirv_descriptor_loads_seen;

#ifdef __cplusplus
}
#endif

#endif
