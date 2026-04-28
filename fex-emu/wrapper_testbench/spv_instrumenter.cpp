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

#include "source/opt/basic_block.h"
#include "source/opt/build_module.h"
#include "source/opt/constants.h"
#include "source/opt/def_use_manager.h"
#include "source/opt/function.h"
#include "source/opt/instruction.h"
#include "source/opt/ir_context.h"
#include "source/opt/module.h"
#include "source/opt/pass.h"
#include "source/opt/type_manager.h"
#include "source/opt/types.h"
#include "spirv-tools/libspirv.hpp"

// Forward-declare the C-linkage counters here so the in-pass increments
// inside the anonymous namespace below bind to the same symbols as the
// extern "C" definitions further down.
extern "C" {
extern volatile int shim_m5_spirv_loads_seen;
extern volatile int shim_m5_spirv_descriptor_loads_seen;
extern volatile int shim_m5_spirv_metadata_injected;
extern volatile int shim_m5_spirv_metadata_skipped_pre_1_3;
extern volatile int shim_m5_spirv_loads_clamped;
extern volatile int shim_m5_spirv_loads_skipped_no_array;
extern volatile int shim_m5_spirv_image_ops_seen;
extern volatile int shim_m5_spirv_image_ops_clamped;
extern volatile int shim_m5_spirv_image_ops_skipped;
extern volatile int shim_m5_spirv_image_sample_ops_seen;
extern volatile int shim_m5_spirv_image_writes_seen;
extern volatile int shim_m5_spirv_image_writes_clamped;
extern volatile int shim_m5_spirv_image_writes_skipped;
}

// Metadata buffer logical layout: bucket = set * kShimMetadataMaxBindings + binding.
// 8 sets × 32 bindings = 256 entries × 4 bytes = 1024 bytes total. A4
// will allocate one device-lived SSBO of that size and write each
// bucket's element-count whenever the application binds something to
// the corresponding (set, binding). For now A3 just emits the OpLoad
// against this layout; the buffer doesn't yet exist at runtime, which
// means at draw time the wrapper would dereference an unbound binding
// — A3 is purely a SPIR-V change, not yet end-to-end.
static constexpr uint32_t kShimMetadataMaxBindings = 32;

// Reserved (set, binding) for the side-channel metadata SSBO.
// set=6 (NOT 7, even though Mali-G720 reports maxBoundDescriptorSets=8):
// the leegao wrapper has a latent off-by-one when setLayoutCount equals
// the reported max (8) — bisect 2026-04-28 found stack-canary corruption
// in caller frames downstream from vkCreatePipelineLayout(setLayoutCount=8).
// Capping at 7 sets total (slot 6 = ours, 0..5 = app) is the workaround.
// Push-constants and hardcoded bounds were rejected; see plan
// project_spirv_instrumentation_plan_2026_04_28.md.
static constexpr uint32_t kShimMetadataDescriptorSet = 6;
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

// Helper: extract DescriptorSet + Binding decorations for a given
// variable result-id by linear scan of OpDecorate annotations.
// Returns false if either is missing.
static bool ExtractSetBinding(spvtools::opt::Module* mod, uint32_t var_id,
                              uint32_t* out_set, uint32_t* out_binding) {
  using spv::Op;
  bool have_set = false, have_binding = false;
  for (auto& a : mod->annotations()) {
    if (a.opcode() != Op::OpDecorate || a.NumInOperands() < 3) continue;
    if (a.GetSingleWordInOperand(0) != var_id) continue;
    uint32_t deco = a.GetSingleWordInOperand(1);
    if (deco == 34 /*DescriptorSet*/) {
      *out_set = a.GetSingleWordInOperand(2);
      have_set = true;
    } else if (deco == 33 /*Binding*/) {
      *out_binding = a.GetSingleWordInOperand(2);
      have_binding = true;
    }
  }
  return have_set && have_binding;
}

// Helper: locate the variable injected by InjectMetadataBindingPass —
// the OpVariable with DescriptorSet=kShimMetadataDescriptorSet and
// Binding=kShimMetadataBinding.
static uint32_t FindMetadataVarId(spvtools::opt::Module* mod) {
  using spv::Op;
  for (auto& g : mod->types_values()) {
    if (g.opcode() != Op::OpVariable) continue;
    uint32_t set = 0, binding = 0;
    if (!ExtractSetBinding(mod, g.result_id(), &set, &binding)) continue;
    if (set == kShimMetadataDescriptorSet && binding == kShimMetadataBinding) {
      return g.result_id();
    }
  }
  return 0;
}

static bool HasCapability(spvtools::opt::IRContext* ctx, spv::Capability cap) {
  for (auto& c : ctx->capabilities()) {
    if (c.opcode() == spv::Op::OpCapability &&
        c.GetSingleWordInOperand(0) == static_cast<uint32_t>(cap)) {
      return true;
    }
  }
  return false;
}

static void EnsureCapability(spvtools::opt::IRContext* ctx, spv::Capability cap) {
  if (HasCapability(ctx, cap)) return;
  ctx->AddCapability(std::make_unique<spvtools::opt::Instruction>(
      ctx, spv::Op::OpCapability, 0, 0,
      std::initializer_list<spvtools::opt::Operand>{
          {SPV_OPERAND_TYPE_CAPABILITY, {static_cast<uint32_t>(cap)}}}));
}

struct IntCoordType {
  uint32_t type_id = 0;
  uint32_t scalar_type_id = 0;
  uint32_t component_count = 0;
  bool is_signed = false;
};

static bool GetIntCoordType(spvtools::opt::analysis::DefUseManager* du,
                            uint32_t type_id, IntCoordType* out) {
  using spv::Op;
  spvtools::opt::Instruction* type = du->GetDef(type_id);
  if (!type) return false;

  uint32_t scalar_type_id = type_id;
  uint32_t component_count = 1;
  if (type->opcode() == Op::OpTypeVector) {
    scalar_type_id = type->GetSingleWordInOperand(0);
    component_count = type->GetSingleWordInOperand(1);
    type = du->GetDef(scalar_type_id);
    if (!type) return false;
  }

  if (type->opcode() != Op::OpTypeInt || type->NumInOperands() < 2) return false;
  if (type->GetSingleWordInOperand(0) != 32) return false;

  out->type_id = type_id;
  out->scalar_type_id = scalar_type_id;
  out->component_count = component_count;
  out->is_signed = type->GetSingleWordInOperand(1) != 0;
  return true;
}

static uint32_t ImageCoordComponentCount(spvtools::opt::Instruction* image_type) {
  if (!image_type || image_type->opcode() != spv::Op::OpTypeImage ||
      image_type->NumInOperands() < 6) {
    return 0;
  }

  uint32_t dims = 0;
  switch (static_cast<spv::Dim>(image_type->GetSingleWordInOperand(1))) {
  case spv::Dim::Dim1D: dims = 1; break;
  case spv::Dim::Dim2D: dims = 2; break;
  case spv::Dim::Dim3D: dims = 3; break;
  case spv::Dim::Rect: dims = 2; break;
  case spv::Dim::Buffer: dims = 1; break;
  case spv::Dim::SubpassData: dims = 2; break;
  default: return 0;
  }

  const uint32_t arrayed = image_type->GetSingleWordInOperand(3);
  if (arrayed) dims++;
  return dims;
}

static uint32_t ResolveImageTypeId(spvtools::opt::analysis::DefUseManager* du,
                                   uint32_t image_value_id) {
  spvtools::opt::Instruction* image_def = du->GetDef(image_value_id);
  if (!image_def) return 0;
  uint32_t type_id = image_def->type_id();
  spvtools::opt::Instruction* type = du->GetDef(type_id);
  if (!type) return 0;
  if (type->opcode() == spv::Op::OpTypeSampledImage) {
    return type->GetSingleWordInOperand(0);
  }
  if (type->opcode() == spv::Op::OpTypeImage) {
    return type_id;
  }
  return 0;
}

static bool IsNormalizedImageSampleOp(spv::Op op) {
  switch (op) {
  case spv::Op::OpImageSampleImplicitLod:
  case spv::Op::OpImageSampleExplicitLod:
  case spv::Op::OpImageSampleDrefImplicitLod:
  case spv::Op::OpImageSampleDrefExplicitLod:
  case spv::Op::OpImageSampleProjImplicitLod:
  case spv::Op::OpImageSampleProjExplicitLod:
  case spv::Op::OpImageSampleProjDrefImplicitLod:
  case spv::Op::OpImageSampleProjDrefExplicitLod:
    return true;
  default:
    return false;
  }
}

// Phase A3 pass: rewrite each descriptor-mediated OpLoad whose pointer
// is a single OpAccessChain with ≥ 2 indices into a clamped form:
//
//   %ac      = OpAccessChain %_ptr_StorageBuffer_uint %metadata
//                  %const_uint_0 %const_uint_<bucket>
//   %count   = OpLoad %uint %ac
//   %cond    = OpULessThan %bool %array_idx %count
//   %orig    = OpLoad %T %ptr            (left in place; consumers redirected)
//   %sel     = OpSelect %T %cond %orig %null_T
//   ; uses of %orig redirected to %sel
//
// The "array index" picked is the LAST in-operand of the AccessChain
// (DXVK-typical pattern: var.field0.array[idx] → AC has 2 indices,
// last = idx). For accesses that have only 1 index (struct field
// only, no array) we skip — there's nothing to bound. This is the
// minimum needed for test_mali_oob_ssbo_probe; bindless / nested
// chains / NonUniformEXT / OpInBoundsAccessChain are out of scope
// for A3 and pass through unchanged.
class BoundsCheckDescriptorLoadsPass : public spvtools::opt::Pass {
 public:
  const char* name() const override { return "shim-m5-bounds-check-descriptor-loads"; }

  Status Process() override {
    using spv::Op;
    namespace opt = spvtools::opt;
    opt::IRContext* ctx = context();
    opt::Module* mod = ctx->module();

    // Pre-1.3: A2 didn't inject metadata, so there's no buffer to
    // index against. Skip silently.
    if (mod->version() < 0x00010300u) return Status::SuccessWithoutChange;

    const uint32_t metadata_var_id = FindMetadataVarId(mod);
    if (metadata_var_id == 0) return Status::SuccessWithoutChange;

    opt::analysis::DefUseManager* du = ctx->get_def_use_mgr();
    opt::analysis::TypeManager* tm = ctx->get_type_mgr();
    opt::analysis::ConstantManager* cm = ctx->get_constant_mgr();

    const uint32_t uint_id = tm->GetUIntTypeId();
    const uint32_t bool_id = tm->GetBoolTypeId();
    const uint32_t ptr_uint_sb_id = tm->FindPointerToType(
        uint_id, spv::StorageClass::StorageBuffer);
    const uint32_t const_uint_0 = cm->GetUIntConstId(0);

    // Snapshot candidate loads first; we'll mutate the IR after.
    struct LoadCandidate {
      opt::Instruction* load;
      uint32_t array_idx_id;
      uint32_t bucket;
    };
    std::vector<LoadCandidate> candidates;
    int loads_skipped_no_array = 0;

    for (auto& fn : *mod) {
      fn.ForEachInst([&](opt::Instruction* inst) {
        if (inst->opcode() != Op::OpLoad) return;
        uint32_t ptr_id = inst->GetSingleWordInOperand(0);
        opt::Instruction* ptr_def = du->GetDef(ptr_id);
        if (!ptr_def) return;
        // Only rewrite plain OpAccessChain; OpInBoundsAccessChain is
        // an application-level invariant we won't touch.
        if (ptr_def->opcode() != Op::OpAccessChain) return;
        opt::Instruction* ac = ptr_def;

        // Find the descriptor variable at the root of the chain.
        opt::Instruction* root = du->GetDef(ac->GetSingleWordInOperand(0));
        while (root && (root->opcode() == Op::OpAccessChain ||
                        root->opcode() == Op::OpInBoundsAccessChain)) {
          root = du->GetDef(root->GetSingleWordInOperand(0));
        }
        if (!root || root->opcode() != Op::OpVariable) return;

        uint32_t sc = root->GetSingleWordInOperand(0);
        if (sc != static_cast<uint32_t>(spv::StorageClass::StorageBuffer) &&
            sc != static_cast<uint32_t>(spv::StorageClass::Uniform)) {
          return;
        }
        if (root->result_id() == metadata_var_id) return;

        uint32_t set = 0, binding = 0;
        if (!ExtractSetBinding(mod, root->result_id(), &set, &binding)) return;
        if (set >= 8 || binding >= kShimMetadataMaxBindings) return;

        // AccessChain operands: base, idx0, idx1, ...  We need at
        // least 2 indices (struct field + array element).
        uint32_t num_indices = ac->NumInOperands() - 1;
        if (num_indices < 2) {
          loads_skipped_no_array++;
          return;
        }
        uint32_t array_idx_id = ac->GetSingleWordInOperand(num_indices);
        uint32_t bucket = set * kShimMetadataMaxBindings + binding;

        candidates.push_back({inst, array_idx_id, bucket});
      });
    }

    if (candidates.empty()) {
      __atomic_add_fetch(&shim_m5_spirv_loads_skipped_no_array,
                         loads_skipped_no_array, __ATOMIC_RELAXED);
      return Status::SuccessWithoutChange;
    }

    // Sanity: required types must already be in the module. uint and
    // const-zero are nearly always present in any non-trivial shader;
    // bool and the StorageBuffer-uint pointer might not be. The
    // helpers above already lazily create them.
    if (!uint_id || !bool_id || !ptr_uint_sb_id || !const_uint_0) {
      // Should never happen — the helpers add types/consts on demand.
      return Status::Failure;
    }

    int clamped = 0;
    for (auto& c : candidates) {
      opt::Instruction* load = c.load;
      const uint32_t loaded_type = load->type_id();
      const uint32_t orig_load_id = load->result_id();

      uint32_t bucket_const = cm->GetUIntConstId(c.bucket);

      // OpAccessChain into metadata[0][bucket]:
      uint32_t ac_id = ctx->TakeNextId();
      auto ac_inst = std::make_unique<opt::Instruction>(
          ctx, Op::OpAccessChain, ptr_uint_sb_id, ac_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {metadata_var_id}},
              {SPV_OPERAND_TYPE_ID, {const_uint_0}},
              {SPV_OPERAND_TYPE_ID, {bucket_const}}});

      // OpLoad %uint %ac → count
      uint32_t count_id = ctx->TakeNextId();
      auto count_inst = std::make_unique<opt::Instruction>(
          ctx, Op::OpLoad, uint_id, count_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {ac_id}}});

      // OpULessThan %bool %array_idx_id %count → cond
      uint32_t cond_id = ctx->TakeNextId();
      auto cond_inst = std::make_unique<opt::Instruction>(
          ctx, Op::OpULessThan, bool_id, cond_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {c.array_idx_id}},
              {SPV_OPERAND_TYPE_ID, {count_id}}});

      // OpConstantNull %loaded_type → null
      opt::analysis::Type* loaded_t = tm->GetType(loaded_type);
      if (!loaded_t) continue;
      uint32_t null_id = cm->GetNullConstId(loaded_t);
      if (!null_id) continue;

      // OpSelect %loaded_type %cond %orig_load %null → sel
      // Built with raw `new` because IntrusiveNodeBase::InsertAfter
      // (the only InsertAfter Instruction inherits) takes a position
      // pointer and the list takes ownership of the inserted node.
      // Pattern matches invocation_interlock_placement_pass.cpp:210.
      uint32_t sel_id = ctx->TakeNextId();
      opt::Instruction* sel_inst = new opt::Instruction(
          ctx, Op::OpSelect, loaded_type, sel_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {cond_id}},
              {SPV_OPERAND_TYPE_ID, {orig_load_id}},
              {SPV_OPERAND_TYPE_ID, {null_id}}});

      // Capture existing users of the original load BEFORE adding
      // the OpSelect (so the OpSelect's own use of orig_load_id
      // isn't included in the redirect set).
      std::vector<std::pair<opt::Instruction*, uint32_t>> uses_to_redirect;
      du->ForEachUse(orig_load_id,
                     [&](opt::Instruction* user, uint32_t op_idx) {
                       uses_to_redirect.push_back({user, op_idx});
                     });

      // Insert prelude before the load (InsertBefore takes unique_ptr).
      opt::Instruction* ac_raw = load->InsertBefore(std::move(ac_inst));
      opt::Instruction* count_raw = load->InsertBefore(std::move(count_inst));
      opt::Instruction* cond_raw = load->InsertBefore(std::move(cond_inst));
      // Insert OpSelect after the load.
      sel_inst->InsertAfter(load);
      du->AnalyzeInstDefUse(ac_raw);
      du->AnalyzeInstDefUse(count_raw);
      du->AnalyzeInstDefUse(cond_raw);
      du->AnalyzeInstDefUse(sel_inst);

      // Redirect all prior consumers of orig_load_id to sel_id.
      for (auto& u : uses_to_redirect) {
        opt::Instruction* user = u.first;
        uint32_t op_idx = u.second;
        // op_idx is a TOTAL operand index (counting type/result),
        // matching the storage layout SPIRV-Tools uses internally.
        // Rewrite that operand's id.
        opt::Operand& o = *(user->begin() + op_idx);
        if (o.words.size() == 1 && o.words[0] == orig_load_id) {
          o.words[0] = sel_id;
        }
        du->AnalyzeInstUse(user);
      }

      clamped++;
    }

    if (consumer()) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "[shim-spv] A3: clamped %d descriptor load(s); "
                    "skipped_no_array=%d (struct-field-only chains)",
                    clamped, loads_skipped_no_array);
      spv_position_t pos = {};
      consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
    }

    __atomic_add_fetch(&shim_m5_spirv_loads_clamped, clamped, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shim_m5_spirv_loads_skipped_no_array,
                       loads_skipped_no_array, __ATOMIC_RELAXED);
    return clamped > 0 ? Status::SuccessWithChange
                       : Status::SuccessWithoutChange;
  }
};

// Phase A5: robustImageAccess2 for integer-coordinate image reads/fetches.
// For OpImageRead / OpImageFetch on 1D/2D/3D-style images, emit:
//
//   %size = OpImageQuerySize[ Lod ] %coord_type %image [ %lod ]
//   %ok   = all(coord >= 0 && coord < size)  ; signed coords
//        or all(coord < size)                 ; unsigned coords
//   %orig = OpImageRead/Fetch %T %image %coord ...
//   %sel  = OpSelect %T %ok %orig %null_T
//
// This deliberately does not allocate or bind any per-image state: image
// extents come from the image object itself, and zero values use
// OpConstantNull of the image result type. Normalized OpImageSample* ops
// are explicitly counted but not transformed: Vulkan says sampling
// coordinates outside descriptor dimensions are defined by the sampler
// wrapping operation, so forcing zero here would break repeat/mirror/etc.
// Image writes, sparse residency, and format-specific non-zero defaults
// are still out of scope.
class BoundsCheckImageReadsPass : public spvtools::opt::Pass {
 public:
  const char* name() const override { return "shim-m5-bounds-check-image-reads"; }

  Status Process() override {
    using spv::Op;
    namespace opt = spvtools::opt;
    opt::IRContext* ctx = context();
    opt::Module* mod = ctx->module();

    opt::analysis::DefUseManager* du = ctx->get_def_use_mgr();
    opt::analysis::TypeManager* tm = ctx->get_type_mgr();
    opt::analysis::ConstantManager* cm = ctx->get_constant_mgr();

    const uint32_t bool_id = tm->GetBoolTypeId();
    if (!bool_id) return Status::Failure;

    struct Candidate {
      opt::Instruction* image_read;
      uint32_t image_id;
      uint32_t coord_id;
      uint32_t lod_id;
      IntCoordType coord_type;
    };
    std::vector<Candidate> candidates;
    int seen = 0;
    int skipped = 0;
    int normalized_samples = 0;

    for (auto& fn : *mod) {
      fn.ForEachInst([&](opt::Instruction* inst) {
        if (IsNormalizedImageSampleOp(inst->opcode())) {
          normalized_samples++;
          return;
        }
        if (inst->opcode() != Op::OpImageRead &&
            inst->opcode() != Op::OpImageFetch) return;
        seen++;
        if (inst->NumInOperands() < 2) { skipped++; return; }

        const uint32_t image_id = inst->GetSingleWordInOperand(0);
        const uint32_t coord_id = inst->GetSingleWordInOperand(1);
        uint32_t lod_id = 0;
        if (inst->opcode() == Op::OpImageFetch && inst->NumInOperands() >= 4) {
          const uint32_t image_operands = inst->GetSingleWordInOperand(2);
          if (image_operands & uint32_t(spv::ImageOperandsMask::Lod)) {
            lod_id = inst->GetSingleWordInOperand(3);
          }
        }
        opt::Instruction* coord_def = du->GetDef(coord_id);
        if (!coord_def) { skipped++; return; }

        IntCoordType coord_type;
        if (!GetIntCoordType(du, coord_def->type_id(), &coord_type)) {
          skipped++;
          return;
        }

        const uint32_t image_type_id = ResolveImageTypeId(du, image_id);
        opt::Instruction* image_type = du->GetDef(image_type_id);
        const uint32_t needed_components = ImageCoordComponentCount(image_type);
        if (needed_components == 0 || needed_components != coord_type.component_count) {
          skipped++;
          return;
        }

        candidates.push_back({inst, image_id, coord_id, lod_id, coord_type});
      });
    }

    __atomic_add_fetch(&shim_m5_spirv_image_ops_seen, seen, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shim_m5_spirv_image_sample_ops_seen, normalized_samples,
                       __ATOMIC_RELAXED);
    if (candidates.empty()) {
      __atomic_add_fetch(&shim_m5_spirv_image_ops_skipped, skipped, __ATOMIC_RELAXED);
      return Status::SuccessWithoutChange;
    }

    EnsureCapability(ctx, spv::Capability::ImageQuery);

    int clamped = 0;
    for (const Candidate& c : candidates) {
      opt::Instruction* read = c.image_read;
      const uint32_t result_type = read->type_id();
      const uint32_t orig_read_id = read->result_id();

      opt::analysis::Type* result_t = tm->GetType(result_type);
      opt::analysis::Type* scalar_coord_t = tm->GetType(c.coord_type.scalar_type_id);
      if (!result_t || !scalar_coord_t) { skipped++; continue; }
      const uint32_t null_result_id = cm->GetNullConstId(result_t);
      const uint32_t zero_coord_id = cm->GetNullConstId(scalar_coord_t);
      if (!null_result_id || !zero_coord_id) { skipped++; continue; }

      const uint32_t size_id = ctx->TakeNextId();
      std::unique_ptr<opt::Instruction> size_inst;
      if (c.lod_id != 0) {
        size_inst = std::make_unique<opt::Instruction>(
            ctx, Op::OpImageQuerySizeLod, c.coord_type.type_id, size_id,
            std::initializer_list<opt::Operand>{
                {SPV_OPERAND_TYPE_ID, {c.image_id}},
                {SPV_OPERAND_TYPE_ID, {c.lod_id}}});
      } else {
        size_inst = std::make_unique<opt::Instruction>(
            ctx, Op::OpImageQuerySize, c.coord_type.type_id, size_id,
            std::initializer_list<opt::Operand>{
                {SPV_OPERAND_TYPE_ID, {c.image_id}}});
      }

      std::vector<std::unique_ptr<opt::Instruction>> prelude;
      prelude.push_back(std::move(size_inst));

      uint32_t combined_cond_id = 0;
      for (uint32_t i = 0; i < c.coord_type.component_count; i++) {
        uint32_t coord_comp_id = c.coord_id;
        uint32_t size_comp_id = size_id;

        if (c.coord_type.component_count > 1) {
          coord_comp_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpCompositeExtract, c.coord_type.scalar_type_id,
              coord_comp_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {c.coord_id}},
                  {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i}}}));

          size_comp_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpCompositeExtract, c.coord_type.scalar_type_id,
              size_comp_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {size_id}},
                  {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i}}}));
        }

        uint32_t comp_ok_id = 0;
        if (c.coord_type.is_signed) {
          const uint32_t ge_zero_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpSGreaterThanEqual, bool_id, ge_zero_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {zero_coord_id}}}));

          const uint32_t lt_size_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpSLessThan, bool_id, lt_size_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {size_comp_id}}}));

          comp_ok_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpLogicalAnd, bool_id, comp_ok_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {ge_zero_id}},
                  {SPV_OPERAND_TYPE_ID, {lt_size_id}}}));
        } else {
          comp_ok_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpULessThan, bool_id, comp_ok_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {size_comp_id}}}));
        }

        if (combined_cond_id == 0) {
          combined_cond_id = comp_ok_id;
        } else {
          const uint32_t and_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpLogicalAnd, bool_id, and_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {combined_cond_id}},
                  {SPV_OPERAND_TYPE_ID, {comp_ok_id}}}));
          combined_cond_id = and_id;
        }
      }

      if (combined_cond_id == 0) { skipped++; continue; }

      // Capture existing users before adding the OpSelect, so the select's
      // own use of the original image-read id is not redirected.
      std::vector<std::pair<opt::Instruction*, uint32_t>> uses_to_redirect;
      du->ForEachUse(orig_read_id,
                     [&](opt::Instruction* user, uint32_t op_idx) {
                       uses_to_redirect.push_back({user, op_idx});
                     });

      for (auto& inst : prelude) {
        opt::Instruction* raw = read->InsertBefore(std::move(inst));
        du->AnalyzeInstDefUse(raw);
      }

      const uint32_t sel_id = ctx->TakeNextId();
      opt::Instruction* sel_inst = new opt::Instruction(
          ctx, Op::OpSelect, result_type, sel_id,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {combined_cond_id}},
              {SPV_OPERAND_TYPE_ID, {orig_read_id}},
              {SPV_OPERAND_TYPE_ID, {null_result_id}}});
      sel_inst->InsertAfter(read);
      du->AnalyzeInstDefUse(sel_inst);

      for (auto& u : uses_to_redirect) {
        opt::Instruction* user = u.first;
        uint32_t op_idx = u.second;
        opt::Operand& o = *(user->begin() + op_idx);
        if (o.words.size() == 1 && o.words[0] == orig_read_id) {
          o.words[0] = sel_id;
        }
        du->AnalyzeInstUse(user);
      }

      clamped++;
    }

    if (consumer()) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "[shim-spv] A5: clamped %d image read/fetch op(s); seen=%d skipped=%d normalized_samples=%d",
                    clamped, seen, skipped, normalized_samples);
      spv_position_t pos = {};
      consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
    }

    __atomic_add_fetch(&shim_m5_spirv_image_ops_clamped, clamped, __ATOMIC_RELAXED);
    __atomic_add_fetch(&shim_m5_spirv_image_ops_skipped, skipped, __ATOMIC_RELAXED);
    return clamped > 0 ? Status::SuccessWithChange
                       : Status::SuccessWithoutChange;
  }
};

// Phase A5 image-write slice: robustImageAccess2 for OpImageWrite. The
// write opcode produces no result, so A3/A5-read's OpSelect-zero pattern
// does not apply — we must wrap the write in structured control flow so
// out-of-extent invocations skip the OpImageWrite entirely. For each
// candidate write the pass synthesizes:
//
//   %size = OpImageQuerySize %coord_type %image
//   %ok_x = OpULessThan %bool %coord_x %size_x   ; OpSGreaterThanEqual+
//                                                  OpSLessThan for signed
//                                                  coords
//   ... per-component AND-reduction → %in_bounds ...
//   OpSelectionMerge %merge None
//   OpBranchConditional %in_bounds %do_write %merge
//   %do_write = OpLabel
//     OpImageWrite %image %coord %texel ...
//     OpBranch %merge
//   %merge = OpLabel
//
// Block split: the original block is partitioned at the OpImageWrite into
// header (pre-write instructions), do_write (the write alone), and merge
// (post-write instructions and the original terminator). Skip rule:
// candidates whose containing block has OpSelectionMerge or OpLoopMerge
// (always after the OpImageWrite by spec, since both must precede the
// terminator) are passed through untouched — splitting would migrate the
// structured-merge declaration into the new merge_bb, which would change
// which block is the construct/loop header and is structurally fragile in
// loops. Counted in shim_m5_spirv_image_writes_skipped.
class BoundsCheckImageWritesPass : public spvtools::opt::Pass {
 public:
  const char* name() const override { return "shim-m5-bounds-check-image-writes"; }

  Status Process() override {
    using spv::Op;
    namespace opt = spvtools::opt;
    opt::IRContext* ctx = context();
    opt::Module* mod = ctx->module();

    opt::analysis::DefUseManager* du = ctx->get_def_use_mgr();
    opt::analysis::TypeManager* tm = ctx->get_type_mgr();
    opt::analysis::ConstantManager* cm = ctx->get_constant_mgr();

    const uint32_t bool_id = tm->GetBoolTypeId();
    if (!bool_id) return Status::Failure;

    // Collect candidates first (raw Instruction* + extracted operands +
    // resolved coord type) so the rewrite loop below can mutate the IR
    // freely. Each candidate's containing block is looked up dynamically
    // at rewrite time via ctx->get_instr_block — earlier splits in the
    // same loop are fine because SplitBasicBlock keeps that mapping
    // current.
    struct Candidate {
      opt::Instruction* image_write;
      uint32_t image_id;
      uint32_t coord_id;
      IntCoordType coord_type;
    };
    std::vector<Candidate> candidates;
    int seen = 0;
    int skipped = 0;

    for (auto& fn : *mod) {
      for (auto& bb : fn) {
        // Pre-scan the block for structured merges. If we find one,
        // every OpImageWrite in the block is unsafe to split (the merge
        // instruction is by spec right before the terminator and would
        // migrate into our merge_bb after the split).
        bool has_structured_merge = false;
        for (auto& inst : bb) {
          if (inst.opcode() == Op::OpSelectionMerge ||
              inst.opcode() == Op::OpLoopMerge) {
            has_structured_merge = true;
            break;
          }
        }

        for (auto& inst : bb) {
          if (inst.opcode() != Op::OpImageWrite) continue;
          seen++;
          if (has_structured_merge) { skipped++; continue; }
          if (inst.NumInOperands() < 3) { skipped++; continue; }

          // OpImageWrite layout: image, coord, texel, [image_operands_mask,
          // operand_args...]. Glslc emits ZeroExtend (or SignExtend) for
          // every storage-image write, so an ImageOperands word is the
          // norm — accept it if the mask only carries
          // texel-encoding/coherency/temporal flags. Reject masks that
          // carry Bias / Lod / Grad / *Offset* / Sample / MinLod /
          // Offsets — those change the indexing semantics and would
          // require a different size-query than OpImageQuerySize.
          if (inst.NumInOperands() > 3) {
            const uint32_t mask = inst.GetSingleWordInOperand(3);
            constexpr uint32_t kIndexingMask =
                uint32_t(spv::ImageOperandsMask::Bias) |
                uint32_t(spv::ImageOperandsMask::Lod) |
                uint32_t(spv::ImageOperandsMask::Grad) |
                uint32_t(spv::ImageOperandsMask::ConstOffset) |
                uint32_t(spv::ImageOperandsMask::Offset) |
                uint32_t(spv::ImageOperandsMask::ConstOffsets) |
                uint32_t(spv::ImageOperandsMask::Sample) |
                uint32_t(spv::ImageOperandsMask::MinLod) |
                uint32_t(spv::ImageOperandsMask::Offsets);
            if (mask & kIndexingMask) { skipped++; continue; }
          }

          const uint32_t image_id = inst.GetSingleWordInOperand(0);
          const uint32_t coord_id = inst.GetSingleWordInOperand(1);

          opt::Instruction* coord_def = du->GetDef(coord_id);
          if (!coord_def) { skipped++; continue; }

          IntCoordType coord_type;
          if (!GetIntCoordType(du, coord_def->type_id(), &coord_type)) {
            skipped++;
            continue;
          }

          const uint32_t image_type_id = ResolveImageTypeId(du, image_id);
          opt::Instruction* image_type = du->GetDef(image_type_id);
          const uint32_t needed_components = ImageCoordComponentCount(image_type);
          if (needed_components == 0 ||
              needed_components != coord_type.component_count) {
            skipped++;
            continue;
          }

          candidates.push_back({&inst, image_id, coord_id, coord_type});
        }
      }
    }

    __atomic_add_fetch(&shim_m5_spirv_image_writes_seen, seen, __ATOMIC_RELAXED);
    if (candidates.empty()) {
      __atomic_add_fetch(&shim_m5_spirv_image_writes_skipped, skipped,
                         __ATOMIC_RELAXED);
      return Status::SuccessWithoutChange;
    }

    EnsureCapability(ctx, spv::Capability::ImageQuery);

    int clamped = 0;
    for (const Candidate& c : candidates) {
      opt::Instruction* write = c.image_write;
      opt::analysis::Type* scalar_coord_t = tm->GetType(c.coord_type.scalar_type_id);
      if (!scalar_coord_t) { skipped++; continue; }
      const uint32_t zero_coord_id = cm->GetNullConstId(scalar_coord_t);
      if (!zero_coord_id) { skipped++; continue; }

      opt::BasicBlock* old_bb = ctx->get_instr_block(write);
      if (!old_bb) { skipped++; continue; }

      // Locate the OpImageWrite's iterator within its (current) block.
      auto write_iter = old_bb->begin();
      while (write_iter != old_bb->end() && &*write_iter != write) ++write_iter;
      if (write_iter == old_bb->end()) { skipped++; continue; }

      // Build the bounds-check instructions but defer insertion until
      // after the splits — we need the merge label to be allocated before
      // we add OpSelectionMerge to old_bb.
      const uint32_t size_id = ctx->TakeNextId();
      std::unique_ptr<opt::Instruction> size_inst =
          std::make_unique<opt::Instruction>(
              ctx, Op::OpImageQuerySize, c.coord_type.type_id, size_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {c.image_id}}});

      std::vector<std::unique_ptr<opt::Instruction>> prelude;
      prelude.push_back(std::move(size_inst));

      uint32_t combined_cond_id = 0;
      for (uint32_t i = 0; i < c.coord_type.component_count; i++) {
        uint32_t coord_comp_id = c.coord_id;
        uint32_t size_comp_id = size_id;

        if (c.coord_type.component_count > 1) {
          coord_comp_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpCompositeExtract, c.coord_type.scalar_type_id,
              coord_comp_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {c.coord_id}},
                  {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i}}}));

          size_comp_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpCompositeExtract, c.coord_type.scalar_type_id,
              size_comp_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {size_id}},
                  {SPV_OPERAND_TYPE_LITERAL_INTEGER, {i}}}));
        }

        uint32_t comp_ok_id = 0;
        if (c.coord_type.is_signed) {
          const uint32_t ge_zero_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpSGreaterThanEqual, bool_id, ge_zero_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {zero_coord_id}}}));

          const uint32_t lt_size_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpSLessThan, bool_id, lt_size_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {size_comp_id}}}));

          comp_ok_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpLogicalAnd, bool_id, comp_ok_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {ge_zero_id}},
                  {SPV_OPERAND_TYPE_ID, {lt_size_id}}}));
        } else {
          comp_ok_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpULessThan, bool_id, comp_ok_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {coord_comp_id}},
                  {SPV_OPERAND_TYPE_ID, {size_comp_id}}}));
        }

        if (combined_cond_id == 0) {
          combined_cond_id = comp_ok_id;
        } else {
          const uint32_t and_id = ctx->TakeNextId();
          prelude.push_back(std::make_unique<opt::Instruction>(
              ctx, Op::OpLogicalAnd, bool_id, and_id,
              std::initializer_list<opt::Operand>{
                  {SPV_OPERAND_TYPE_ID, {combined_cond_id}},
                  {SPV_OPERAND_TYPE_ID, {comp_ok_id}}}));
          combined_cond_id = and_id;
        }
      }

      if (combined_cond_id == 0) { skipped++; continue; }

      // Allocate labels for do_write_bb and merge_bb. The first split
      // moves [OpImageWrite, ..., terminator] to a new block "tail_bb"
      // (we name it do_write_bb here, but it still contains the original
      // post-write instructions until the second split factors them off).
      const uint32_t do_write_label_id = ctx->TakeNextId();
      const uint32_t merge_label_id = ctx->TakeNextId();

      opt::BasicBlock* do_write_bb =
          old_bb->SplitBasicBlock(ctx, do_write_label_id, write_iter);

      // After the first split, do_write_bb starts with the OpImageWrite.
      // Split again at the instruction immediately following it so
      // do_write_bb contains the write alone, and merge_bb gets the
      // original post-write instructions and the terminator.
      auto post_write_iter = do_write_bb->begin();
      ++post_write_iter;  // past the OpImageWrite
      opt::BasicBlock* merge_bb =
          do_write_bb->SplitBasicBlock(ctx, merge_label_id, post_write_iter);
      (void)merge_bb;  // referenced via merge_label_id

      // Emit the prelude + structured-merge + conditional branch as the
      // new terminator of old_bb. BasicBlock::AddInstruction returns
      // void, so grab the raw pointer before moving the unique_ptr.
      auto append_to = [&](opt::BasicBlock* bb,
                           std::unique_ptr<opt::Instruction> inst) {
        opt::Instruction* raw = inst.get();
        bb->AddInstruction(std::move(inst));
        du->AnalyzeInstDefUse(raw);
        ctx->set_instr_block(raw, bb);
      };

      for (auto& inst : prelude) {
        append_to(old_bb, std::move(inst));
      }
      append_to(old_bb, std::make_unique<opt::Instruction>(
          ctx, Op::OpSelectionMerge, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {merge_label_id}},
              {SPV_OPERAND_TYPE_SELECTION_CONTROL,
               {static_cast<uint32_t>(spv::SelectionControlMask::MaskNone)}}}));
      append_to(old_bb, std::make_unique<opt::Instruction>(
          ctx, Op::OpBranchConditional, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {combined_cond_id}},
              {SPV_OPERAND_TYPE_ID, {do_write_label_id}},
              {SPV_OPERAND_TYPE_ID, {merge_label_id}}}));

      // Terminate do_write_bb with OpBranch to merge_bb.
      append_to(do_write_bb, std::make_unique<opt::Instruction>(
          ctx, Op::OpBranch, 0, 0,
          std::initializer_list<opt::Operand>{
              {SPV_OPERAND_TYPE_ID, {merge_label_id}}}));

      clamped++;
    }

    if (consumer()) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "[shim-spv] A5W: clamped %d image-write op(s); seen=%d skipped=%d",
                    clamped, seen, skipped);
      spv_position_t pos = {};
      consumer()(SPV_MSG_INFO, "shim-spv", pos, buf);
    }

    __atomic_add_fetch(&shim_m5_spirv_image_writes_clamped, clamped,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&shim_m5_spirv_image_writes_skipped, skipped,
                       __ATOMIC_RELAXED);

    if (clamped > 0) {
      // CFG / dominator / structured-CFG analyses are stale after our
      // block splits. DefUse and InstrToBlockMapping have been kept
      // current via per-instruction updates and SplitBasicBlock's own
      // mapping fix-up. Invalidate the rest defensively; the driver
      // doesn't need them after this pass, but it's cheap insurance
      // against future passes being chained in.
      ctx->InvalidateAnalysesExceptFor(
          opt::IRContext::Analysis::kAnalysisDefUse |
          opt::IRContext::Analysis::kAnalysisInstrToBlockMapping);
    }

    return clamped > 0 ? Status::SuccessWithChange
                       : Status::SuccessWithoutChange;
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
volatile int shim_m5_spirv_loads_clamped = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_loads_skipped_no_array = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_ops_seen = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_ops_clamped = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_ops_skipped = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_sample_ops_seen = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_writes_seen = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_writes_clamped = 0;
__attribute__((visibility("default")))
volatile int shim_m5_spirv_image_writes_skipped = 0;

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

  // A2 always runs — declares an unused storage_buffer variable;
  // wrappers skip validation for unused-but-declared interface vars
  // (verified in the A2-only commit, which didn't regress any test).
  InjectMetadataBindingPass a2;
  a2.SetMessageConsumer(consumer);
  if (a2.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;

  // A3 is env-gated by SHIM_INSTRUMENT_ENABLE — the same gate that
  // turns on A4 (runtime metadata-buffer plumbing in shim_maintenance5.c).
  // The two phases must be ON together: A3 emits OpAccessChain references
  // to set=7 binding=0; A4 extends the pipeline layout and binds the
  // metadata buffer at slot 7. With only A3, vkCreateComputePipelines
  // rejects. With only A4, the layout has an unused slot 7 (harmless).
  const char *enable = std::getenv("SHIM_INSTRUMENT_ENABLE");
  if (enable && enable[0] == '1') {
    BoundsCheckDescriptorLoadsPass a3;
    a3.SetMessageConsumer(consumer);
    if (a3.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;

    BoundsCheckImageReadsPass a5;
    a5.SetMessageConsumer(consumer);
    if (a5.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;

    BoundsCheckImageWritesPass a5w;
    a5w.SetMessageConsumer(consumer);
    if (a5w.Run(ctx.get()) == spvtools::opt::Pass::Status::Failure) return 0;
  }

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
