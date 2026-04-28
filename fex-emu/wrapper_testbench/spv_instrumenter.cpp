// SPIR-V instrumentation engine for the wrapper testbench shim.
//
// This file is the C++ side of the shim. It hooks vkCreateShaderModule
// at the C side (shim_maintenance5.c), receives the input SPIR-V, runs
// it through SPIRV-Tools' optimizer infrastructure, and returns a
// possibly-modified SPIR-V binary that the C side forwards to the real
// wrapper.
//
// Current state: passthrough. We parse the input with SPIRV-Tools'
// IR builder, immediately re-serialize, and return. This proves the
// link works end-to-end (binaries link, parse succeeds, the wrapper
// accepts the round-tripped output) before we start the real
// robustBufferAccess2 / robustImageAccess2 pass.
//
// The eventual pass cribs from Khronos' GPU-Assisted Validation
// (Vulkan-ValidationLayers/layers/gpuav/spirv/descriptor_class_general_buffer_pass.cpp)
// with the report-call replaced by an OpSelect-with-zero clamp and
// the error-output infrastructure dropped.

#include "spv_instrumenter.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "spirv-tools/optimizer.hpp"

namespace {

// Counter exposed via dlsym from C side, lets the testbench prove the
// instrumenter actually ran (analogous to shim_m5_buffer_flags2_fold_count).
volatile int g_instrument_count = 0;

}  // namespace

extern "C" {

__attribute__((visibility("default")))
volatile int shim_m5_spirv_instrument_count = 0;

// Returns 1 on success (out_code/out_size populated, caller must free
// out_code with shim_spv_free), 0 on failure.
//
// Caller-owned allocation contract: on success, *out_code is a malloc'd
// buffer of *out_size_bytes bytes. Caller frees via shim_spv_free.
__attribute__((visibility("default")))
int shim_spv_instrument(const uint32_t *in_code, size_t in_size_bytes,
                        uint32_t **out_code, size_t *out_size_bytes) {
  if (!in_code || !out_code || !out_size_bytes || in_size_bytes < 20) return 0;
  // Magic-number sanity check: input must be valid SPIR-V.
  if (in_code[0] != 0x07230203u) return 0;

  const size_t in_words = in_size_bytes / sizeof(uint32_t);
  std::vector<uint32_t> input(in_code, in_code + in_words);
  std::vector<uint32_t> output;

  // SPIR-V 1.6 covers everything Vulkan 1.3 emits. Optimizer takes the
  // env at construction; if a shader uses newer features we may need
  // to bump.
  spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
  optimizer.SetMessageConsumer([](spv_message_level_t /*level*/,
                                   const char * /*source*/,
                                   const spv_position_t & /*pos*/,
                                   const char * /*msg*/) {
    // Drop messages on the floor for now. When real passes land, route
    // these to stderr with the shim's prefix.
  });

  // No passes registered yet — Run() with an empty pass list still parses
  // and reserializes, which validates the round-trip. When the real
  // descriptor-instrumentation pass lands, it goes here via
  // optimizer.RegisterPass(spvtools::Create...).
  if (!optimizer.Run(input.data(), input.size(), &output)) return 0;

  const size_t out_bytes = output.size() * sizeof(uint32_t);
  uint32_t *buf = static_cast<uint32_t *>(std::malloc(out_bytes));
  if (!buf) return 0;
  std::memcpy(buf, output.data(), out_bytes);

  __atomic_fetch_add(&shim_m5_spirv_instrument_count, 1, __ATOMIC_RELAXED);
  *out_code = buf;
  *out_size_bytes = out_bytes;
  return 1;
}

__attribute__((visibility("default")))
void shim_spv_free(uint32_t *code) { std::free(code); }

}  // extern "C"
