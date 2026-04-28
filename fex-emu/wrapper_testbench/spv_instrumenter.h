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

// Returns 1 if the buffer is a valid Vulkan 1.3 SPIR-V module, else 0.
// On invalid, the first error message is copied into err_buf (truncated).
int shim_spv_validate(const uint32_t *code, size_t size_bytes,
                      char *err_buf, size_t err_buf_sz);

extern volatile int shim_m5_spirv_instrument_count;
extern volatile int shim_m5_spirv_loads_seen;
extern volatile int shim_m5_spirv_descriptor_loads_seen;
extern volatile int shim_m5_spirv_metadata_injected;
extern volatile int shim_m5_spirv_metadata_skipped_pre_1_3;
extern volatile int shim_m5_spirv_loads_clamped;
extern volatile int shim_m5_spirv_loads_skipped_no_array;

#ifdef __cplusplus
}
#endif

#endif
