// SPIR-V instrumentation engine for the wrapper testbench shim.
//
// This file implements the C++ side of the shim. The C side
// (shim_maintenance5.c) hooks vkCreateShaderModule and calls into here
// with the user's SPIR-V binary. We parse it via SPIRV-Tools, run our
// instrumentation pass(es), and return a possibly-modified binary that
// the C side forwards to the real wrapper.
//
// Roadmap (multi-session):
//   Phase A1 (this commit): pass walks the IR, identifies StorageBuffer /
//     UniformBuffer OpLoad operations through descriptors, counts them.
//     No transformation yet — proves we can navigate SPIR-V correctly.
//   Phase A2: inject a metadata storage_buffer descriptor binding into
//     the module (set N+1, binding 0) of type uint[] — one entry per
//     (set, binding) carrying the descriptor's range in bytes.
//   Phase A3: for each identified load, emit OpULessThan + OpSelect
//     against the metadata-buffer entry (real bounds check, not
//     hardcoded).
//   Phase A4: shim-side runtime — vkCreatePipelineLayout interception
//     to add the metadata descriptor set, vkUpdateDescriptorSets to
//     populate, vkCmdBindDescriptorSets to bind at draw/dispatch time.
//   Phase A5: extend to images (robustImageAccess2).
//
// Cribs from Khronos GPU-AV's descriptor_class_general_buffer_pass.cpp
// in Vulkan-ValidationLayers, which uses the same SPIRV-Tools opt
// internals but emits a report-call instead of a clamp. We swap the
// report arm for OpSelect-zero, and drop the error-output infra.

#include "spv_instrumenter.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "source/opt/build_module.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/pass.h"
#include "spirv-tools/libspirv.hpp"

// Forward-declare the C-linkage counters here so the in-pass increments
// inside the anonymous namespace below bind to the same symbols as the
// extern "C" definitions further down.
extern "C" {
extern volatile int shim_m5_spirv_loads_seen;
extern volatile int shim_m5_spirv_descriptor_loads_seen;
}

namespace {

// Phase A1 pass: identify StorageBuffer / UniformBuffer OpLoad
// operations through descriptor-decorated variables. Increments
// shim_m5_spirv_loads_seen for each candidate.
class IdentifyDescriptorLoadsPass : public spvtools::opt::Pass {
 public:
  const char* name() const override { return "shim-m5-identify-descriptor-loads"; }

  Status Process() override {
    spvtools::opt::IRContext* ctx = context();
    spvtools::opt::analysis::DefUseManager* du = ctx->get_def_use_mgr();

    int loads_seen = 0;
    int loads_through_descriptor = 0;

    for (auto& fn : *ctx->module()) {
      fn.ForEachInst([&](spvtools::opt::Instruction* inst) {
        if (inst->opcode() != spv::Op::OpLoad) return;
        loads_seen++;

        // Trace the pointer back: OpLoad's operand 0 is the pointer.
        // For descriptor accesses, the pointer comes from an
        // OpAccessChain whose base is an OpVariable with DescriptorSet
        // and Binding decorations and a StorageBuffer / Uniform storage
        // class.
        uint32_t ptr_id = inst->GetSingleWordInOperand(0);
        spvtools::opt::Instruction* ptr_def = du->GetDef(ptr_id);
        if (!ptr_def) return;

        // The OpAccessChain may be nested (chain of chains); follow
        // back through OpAccessChain / OpInBoundsAccessChain to the
        // root OpVariable.
        spvtools::opt::Instruction* root = ptr_def;
        while (root && (root->opcode() == spv::Op::OpAccessChain ||
                        root->opcode() == spv::Op::OpInBoundsAccessChain)) {
          uint32_t base_id = root->GetSingleWordInOperand(0);
          root = du->GetDef(base_id);
        }
        if (!root || root->opcode() != spv::Op::OpVariable) return;

        // Storage class is the result-type pointer's storage class,
        // but conveniently it's also the literal at operand 0 of the
        // OpVariable instruction's "in operands" — wait, OpVariable's
        // first in-operand is the storage class.
        uint32_t storage_class =
            root->GetSingleWordInOperand(0);
        if (storage_class != static_cast<uint32_t>(spv::StorageClass::StorageBuffer) &&
            storage_class != static_cast<uint32_t>(spv::StorageClass::Uniform)) {
          return;
        }

        loads_through_descriptor++;
      });
    }

    if (consumer()) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "[shim-spv] pass walked module: %d total OpLoad, %d through "
                    "StorageBuffer/Uniform descriptor",
                    loads_seen, loads_through_descriptor);
      spv_position_t pos = {};
      consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
    }

    __atomic_add_fetch(&shim_m5_spirv_loads_seen, loads_seen, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shim_m5_spirv_descriptor_loads_seen, loads_through_descriptor, __ATOMIC_RELAXED);

    // Phase A1 doesn't transform; report no change.
    return Status::SuccessWithoutChange;
  }
};

}  // namespace

extern "C" {

__attribute__((visibility("default")))
volatile int shim_m5_spirv_instrument_count = 0;
// Cumulative across all instrumented modules.
__attribute__((visibility("default")))
volatile int shim_m5_spirv_loads_seen = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_descriptor_loads_seen = 0;

__attribute__((visibility("default")))
int shim_spv_instrument(const uint32_t *in_code, size_t in_size_bytes,
                        uint32_t **out_code, size_t *out_size_bytes) {
  if (!in_code || !out_code || !out_size_bytes || in_size_bytes < 20) return 0;
  if (in_code[0] != 0x07230203u) return 0;

  const size_t in_words = in_size_bytes / sizeof(uint32_t);

  // Build the IRContext. Drop messages on the floor; the per-pass
  // consumer below picks up our own logs.
  auto consumer = [](spv_message_level_t /*level*/, const char * /*source*/,
                     const spv_position_t & /*pos*/, const char * /*msg*/) {};
  std::unique_ptr<spvtools::opt::IRContext> ctx = spvtools::BuildModule(
      SPV_ENV_VULKAN_1_3, consumer, in_code, in_words);
  if (!ctx) return 0;

  IdentifyDescriptorLoadsPass pass;
  pass.SetMessageConsumer(consumer);
  auto status = pass.Run(ctx.get());
  if (status == spvtools::opt::Pass::Status::Failure) return 0;

  std::vector<uint32_t> output;
  ctx->module()->ToBinary(&output, /*skip_nop=*/true);

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
