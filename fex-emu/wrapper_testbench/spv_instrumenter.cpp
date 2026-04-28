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
extern volatile int shim_m5_spirv_metadata_injected;
extern volatile int shim_m5_spirv_metadata_skipped_pre_1_3;
}

// Reserved (set, binding) for the side-channel metadata SSBO.
// set=7 sits at the top of the Mali-G720 maxBoundDescriptorSets=8
// range, leaving sets 0..6 free for the application. binding=0 has
// no in-app meaning at that set; A4 wires it. See plan
// project_spirv_instrumentation_plan_2026_04_28.md for the design
// choice (push-constants and hardcoded bounds were rejected).
static constexpr uint32_t kShimMetadataDescriptorSet = 7;
static constexpr uint32_t kShimMetadataBinding       = 0;

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

// Phase A2 pass: inject a metadata storage_buffer descriptor binding
// into the module. The binding is `MetadataBuf { uint sizes[]; }`
// at (set=7, binding=0). A3 will emit OpAccessChain into it for
// per-load bounds checks; A4 will populate it at runtime via
// vkUpdateDescriptorSets in the shim.
//
// Storage class & decoration: SPIR-V 1.3+ uses StorageBuffer +
// Block, no extension required. Pre-1.3 would need
// SPV_KHR_storage_buffer_storage_class or Uniform+BufferBlock; we
// don't support that today (the testbench's probe and DXVK 2.7+
// emit ≥ 1.3, per plan §A2). For pre-1.3 modules we skip injection
// and let the upstream spoof apply unchanged.
//
// Entry-point interface: SPIR-V 1.4 widened OpEntryPoint to require
// listing all interface variables (not just Input/Output). For ≥1.4
// modules we append the new variable to every OpEntryPoint.
class InjectMetadataBindingPass : public spvtools::opt::Pass {
 public:
  const char* name() const override { return "shim-m5-inject-metadata-binding"; }

  Status Process() override {
    using spv::Op;
    namespace opt = spvtools::opt;
    opt::IRContext* ctx = context();
    opt::Module* mod = ctx->module();

    // header_.version is the raw header word, e.g. 0x00010300 for 1.3.
    uint32_t version = mod->version();
    if (version < 0x00010300u) {
      __atomic_add_fetch(&shim_m5_spirv_metadata_skipped_pre_1_3, 1,
                         __ATOMIC_RELAXED);
      if (consumer()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "[shim-spv] skipping metadata injection: SPIR-V "
                      "version 0x%08x < 1.3 (pre-1.3 path not implemented)",
                      version);
        spv_position_t pos = {};
        consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
      }
      return Status::SuccessWithoutChange;
    }
    const bool needs_iface_extend = version >= 0x00010400u;

    // Reuse OpTypeInt 32 0 if the module already has it; almost every
    // shader does, but emit one if not.
    uint32_t uint_id = 0;
    for (auto& t : mod->types_values()) {
      if (t.opcode() == Op::OpTypeInt &&
          t.NumInOperands() == 2 &&
          t.GetSingleWordInOperand(0) == 32 &&
          t.GetSingleWordInOperand(1) == 0) {
        uint_id = t.result_id();
        break;
      }
    }
    if (uint_id == 0) {
      uint_id = ctx->TakeNextId();
      mod->AddType(std::make_unique<opt::Instruction>(
          ctx, Op::OpTypeInt, 0, uint_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_LITERAL_INTEGER, {32}},
              {SPV_OPERAND_TYPE_LITERAL_INTEGER, {0}}}));
    }

    const uint32_t arr_id = ctx->TakeNextId();
    mod->AddType(std::make_unique<opt::Instruction>(
        ctx, Op::OpTypeRuntimeArray, 0, arr_id,
        std::initializer_list<opt::Operand>{
            {SPV_OPERAND_TYPE_ID, {uint_id}}}));

    const uint32_t struct_id = ctx->TakeNextId();
    mod->AddType(std::make_unique<opt::Instruction>(
        ctx, Op::OpTypeStruct, 0, struct_id,
        std::initializer_list<opt::Operand>{
            {SPV_OPERAND_TYPE_ID, {arr_id}}}));

    const uint32_t ptr_id = ctx->TakeNextId();
    mod->AddType(std::make_unique<opt::Instruction>(
        ctx, Op::OpTypePointer, 0, ptr_id,
        std::initializer_list<opt::Operand>{
            {SPV_OPERAND_TYPE_STORAGE_CLASS,
             {static_cast<uint32_t>(spv::StorageClass::StorageBuffer)}},
            {SPV_OPERAND_TYPE_ID, {struct_id}}}));

    const uint32_t var_id = ctx->TakeNextId();
    mod->AddGlobalValue(std::make_unique<opt::Instruction>(
        ctx, Op::OpVariable, ptr_id, var_id,
        std::initializer_list<opt::Operand>{
            {SPV_OPERAND_TYPE_STORAGE_CLASS,
             {static_cast<uint32_t>(spv::StorageClass::StorageBuffer)}}}));

    auto add_dec_lit = [&](uint32_t target, spv::Decoration d, uint32_t lit) {
      mod->AddAnnotationInst(std::make_unique<opt::Instruction>(
          ctx, Op::OpDecorate, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {target}},
              {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(d)}},
              {SPV_OPERAND_TYPE_LITERAL_INTEGER, {lit}}}));
    };
    auto add_dec_nolit = [&](uint32_t target, spv::Decoration d) {
      mod->AddAnnotationInst(std::make_unique<opt::Instruction>(
          ctx, Op::OpDecorate, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {target}},
              {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(d)}}}));
    };
    auto add_member_dec_lit = [&](uint32_t target, uint32_t member,
                                  spv::Decoration d, uint32_t lit) {
      mod->AddAnnotationInst(std::make_unique<opt::Instruction>(
          ctx, Op::OpMemberDecorate, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {target}},
              {SPV_OPERAND_TYPE_LITERAL_INTEGER, {member}},
              {SPV_OPERAND_TYPE_DECORATION, {static_cast<uint32_t>(d)}},
              {SPV_OPERAND_TYPE_LITERAL_INTEGER, {lit}}}));
    };

    add_dec_lit(arr_id, spv::Decoration::ArrayStride, 4);
    add_member_dec_lit(struct_id, 0, spv::Decoration::Offset, 0);
    add_dec_nolit(struct_id, spv::Decoration::Block);
    add_dec_lit(var_id, spv::Decoration::DescriptorSet,
                kShimMetadataDescriptorSet);
    add_dec_lit(var_id, spv::Decoration::Binding, kShimMetadataBinding);
    add_dec_nolit(var_id, spv::Decoration::NonWritable);

    if (needs_iface_extend) {
      for (auto& ep : mod->entry_points()) {
        ep.AddOperand({SPV_OPERAND_TYPE_ID, {var_id}});
      }
    }

    if (consumer()) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "[shim-spv] injected metadata SSBO at set=%u binding=%u "
                    "(var=%u struct=%u arr=%u, %sextended OpEntryPoint)",
                    kShimMetadataDescriptorSet, kShimMetadataBinding,
                    var_id, struct_id, arr_id,
                    needs_iface_extend ? "" : "did not ");
      spv_position_t pos = {};
      consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
    }

    __atomic_add_fetch(&shim_m5_spirv_metadata_injected, 1, __ATOMIC_RELAXED);
    return Status::SuccessWithChange;
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
volatile int shim_m5_spirv_metadata_injected = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_metadata_skipped_pre_1_3 = 0;

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

  IdentifyDescriptorLoadsPass a1;
  a1.SetMessageConsumer(consumer);
  if (a1.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;

  InjectMetadataBindingPass a2;
  a2.SetMessageConsumer(consumer);
  if (a2.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;

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

// Run spirv-val (Vulkan 1.3 env) over an arbitrary SPIR-V buffer.
// Returns 1 if valid, 0 otherwise. On invalid, the diagnostic
// message is copied into err_buf (truncated to err_buf_sz-1).
__attribute__((visibility("default")))
int shim_spv_validate(const uint32_t *code, size_t size_bytes,
                      char *err_buf, size_t err_buf_sz) {
  if (err_buf && err_buf_sz) err_buf[0] = '\0';
  if (!code || size_bytes < 20) return 0;
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
  std::string captured;
  tools.SetMessageConsumer([&](spv_message_level_t level, const char * /*src*/,
                               const spv_position_t & /*pos*/,
                               const char *msg) {
    if (level <= SPV_MSG_ERROR && captured.empty() && msg) captured = msg;
  });
  bool ok = tools.Validate(code, size_bytes / sizeof(uint32_t));
  if (!ok && err_buf && err_buf_sz && !captured.empty()) {
    std::strncpy(err_buf, captured.c_str(), err_buf_sz - 1);
    err_buf[err_buf_sz - 1] = '\0';
  }
  return ok ? 1 : 0;
}

}  // extern "C"
