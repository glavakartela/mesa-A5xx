/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal, isolated compute path for a5xx (Adreno 5xx) GPUs.
 *
 * The a5xx command stream is programmed with CP_LOAD_STATE4 and the HLSQ_CS_*
 * register block, while Turnip's regular compute path is written against the
 * A6XX+ register set (CP_LOAD_STATE6, SP_CS_* A6xx layout, memory descriptors).
 * Keep this file self-contained and only reachable through the runtime gen5
 * checks so that A6XX+ behavior is unchanged.
 *
 * Current MVP limitations (deliberate, to be lifted step by step):
 *  - storage buffers only, no UBOs/images/push constants,
 *  - storage buffers must live in descriptor set 0; the hardware slot is the
 *    Vulkan binding number (bindful isam/ldgb model of a5xx),
 *  - one descriptor per binding (no arrays),
 *  - get_ssbo_size() currently returns 0 on a5xx.
 */

#include "tu5_compute.h"

#include "tu_cmd_buffer.h"
#include "tu_shader.h"

#include "ir3/ir3_shader.h"
#include "tu5_regs.h"
#include "tu5_gfx_regs.h"

/* Compact storage-buffer descriptor written by
 * write_buffer_descriptor_addr() when info->chip == 5:
 *   dword 0/1: 64-bit address
 *   dword 2:   size in dwords
 *   dword 3:   reserved
 */
#define TU5_SSBO_DESC_DWORDS 4

void
tu5_emit_cs_config(struct tu_cs *cs,
                   const struct ir3_shader_variant *v,
                   const struct tu_pvtmem_config *pvtmem,
                   uint64_t binary_iova)
{
   const struct ir3_info *info = &v->info;
   enum tu5_threadsize thrsz =
      info->double_threadsize ? TU5_FOUR_QUADS : TU5_TWO_QUADS;
   /* Do not preload compute shaders into the shared internal instruction
    * store: a preloaded graphics pipeline leaves its VS/FS there, and a
    * subsequent preloaded compute dispatch was observed to read 0 instead
    * of the SSBO values.  External fetch works reliably; keep it.
    */
   unsigned instrlen = 0;
   (void)v->instrlen;

   tu_cs_emit_pkt4(cs, TU5_REG_SP_SP_CNTL, 1);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_CONTROL_0_REG, 1);
   tu_cs_emit(cs, TU5_HLSQ_CONTROL_0_REG_FSTHREADSIZE(TU5_TWO_QUADS) |
                     TU5_HLSQ_CONTROL_0_REG_CSTHREADSIZE(thrsz) |
                     0x00000880 /* XXX: unknown bits, as in Gallium */);

   tu_cs_emit_pkt4(cs, TU5_REG_SP_CS_CTRL_REG0, 1);
   tu_cs_emit(cs, TU5_SP_CS_CTRL_REG0_THREADSIZE(thrsz) |
                     TU5_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(info->max_half_reg + 1) |
                     TU5_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(info->max_reg + 1) |
                     TU5_SP_CS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(v)) |
                     COND(instrlen != 0, TU5_SP_CS_CTRL_REG0_BUFFER) |
                     0x2 /* XXX: unknown bit, as in Gallium */);

   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_CS_CONFIG, 1);
   tu_cs_emit(cs, TU5_HLSQ_CS_CONFIG_CONSTOBJECTOFFSET(0) |
                     TU5_HLSQ_CS_CONFIG_SHADEROBJOFFSET(0) |
                     TU5_HLSQ_CS_CONFIG_ENABLED);

   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_CS_CNTL, 1);
   tu_cs_emit(cs, TU5_HLSQ_CS_CNTL_INSTRLEN(instrlen) |
                     COND(v->has_ssbo, TU5_HLSQ_CS_CNTL_SSBO_ENABLE));

   tu_cs_emit_pkt4(cs, TU5_REG_SP_CS_CONFIG, 1);
   tu_cs_emit(cs, TU5_SP_CS_CONFIG_CONSTOBJECTOFFSET(0) |
                     TU5_SP_CS_CONFIG_SHADEROBJOFFSET(0) |
                     TU5_SP_CS_CONFIG_ENABLED);

   assert(v->constlen % 4 == 0);
   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_CS_CONSTLEN, 2);
   tu_cs_emit(cs, v->constlen / 4);
   tu_cs_emit(cs, instrlen);

   /* Shader object: _OBJ_START (64-bit), PVT_MEM_PARAM, PVT_MEM_ADDR (64-bit),
    * PVT_MEM_SIZE.
    */
   tu_cs_emit_pkt4(cs, TU5_REG_SP_CS_OBJ_START, 6);
   tu_cs_emit_qw(cs, binary_iova);
   tu_cs_emit(cs,
      tu5_sp_cs_pvt_mem_param_memsizeperitem(pvtmem->per_fiber_size) |
      tu5_sp_cs_pvt_mem_param_hwstackoffset(pvtmem->per_sp_size));
   tu_cs_emit_qw(cs, pvtmem->iova);
   tu_cs_emit(cs,
      tu5_sp_cs_pvt_mem_size_totalpvtmemsize(pvtmem->per_sp_size));

   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_UPDATE_CNTL, 1);
   tu_cs_emit(cs, 0x01f00000);

   uint32_t local_invocation_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
   uint32_t work_group_id =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);

   tu_cs_emit_pkt4(cs, TU5_REG_HLSQ_CS_CNTL_0, 2);
   tu_cs_emit(cs, TU5_HLSQ_CS_CNTL_0_WGIDCONSTID(work_group_id) |
                     TU5_HLSQ_CS_CNTL_0_UNK0(regid(63, 0)) |
                     TU5_HLSQ_CS_CNTL_0_UNK1(regid(63, 0)) |
                     TU5_HLSQ_CS_CNTL_0_LOCALIDREGID(local_invocation_id));
   tu_cs_emit(cs, TU5_HLSQ_CS_CNTL_1_WORKITEMRASTORDER_LINEAR);

   if (instrlen > 0) {
      /* Preload the shader into the instruction cache. */
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                     CP_LOAD_STATE4_0_NUM_UNIT(instrlen));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER) |
                     CP_LOAD_STATE4_1_EXT_SRC_ADDR((uint32_t)binary_iova));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(binary_iova >> 32));
   }
}

/* a5xx has no CP_EVENT_WRITE7-based cache flush: wait for outstanding writes
 * and the CS itself with CACHE_FLUSH_TS + WAIT_FOR_IDLE.
 */
static void
tu5_emit_flush(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   tu_cs_emit_qw(cs, global_iova(cmd, seqno_dummy));
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);
}

static void
tu5_emit_driver_params(struct tu_cmd_buffer *cmd,
                       const struct ir3_shader_variant *v,
                       const struct tu_dispatch_info *info)
{
   const struct ir3_const_state *const_state = v->const_state;

   if (!ir3_const_can_upload(&const_state->allocs,
                             IR3_CONST_ALLOC_DRIVER_PARAMS, v->constlen))
      return;

   uint32_t offset =
      const_state->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
   uint32_t num_consts =
      MIN2(const_state->num_driver_params, (v->constlen - offset) * 4);
   if (!num_consts)
      return;

   unsigned subgroup_size = v->info.subgroup_size;
   struct ir3_driver_params_cs dp = {
      .num_work_groups_x = info->blocks[0],
      .num_work_groups_y = info->blocks[1],
      .num_work_groups_z = info->blocks[2],
      .work_dim = 0,
      .base_group_x = info->offsets[0],
      .base_group_y = info->offsets[1],
      .base_group_z = info->offsets[2],
      .subgroup_size = subgroup_size,
      .subgroup_id_shift = util_logbase2(subgroup_size),
   };

   assert(num_consts <= sizeof(dp) / sizeof(uint32_t));

   unsigned size_vec4 = DIV_ROUND_UP(num_consts, 4);

   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + size_vec4 * 4);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(offset) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                            CP_LOAD_STATE4_0_NUM_UNIT(size_vec4));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   const uint32_t *src = (const uint32_t *)&dp;
   for (unsigned i = 0; i < size_vec4 * 4; i++)
      tu_cs_emit(&cmd->cs, i < num_consts ? src[i] : 0);
}

static void
tu5_emit_ssbo_state(struct tu_cmd_buffer *cmd)
{
   struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
   struct tu_descriptor_state *ds =
      tu_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);

   if (!shader->num_ssbos)
      return;

   assert(shader->num_ssbos <= TU5_MAX_SSBOS);

   uint64_t addrs[TU5_MAX_SSBOS] = {};
   uint32_t sizes[TU5_MAX_SSBOS] = {};
   unsigned max_binding = 0;

   for (unsigned i = 0; i < shader->num_ssbos; i++) {
      unsigned set = shader->ssbo_map[i].set;
      unsigned binding = shader->ssbo_map[i].binding;

      max_binding = MAX2(max_binding, binding);
      const struct tu_descriptor_set *dset =
         set < MAX_SETS ? ds->sets[set] : NULL;

      if (!dset || binding >= dset->layout->binding_count || !dset->mapped_ptr)
         continue;

      const struct tu_descriptor_set_binding_layout *bl =
         &dset->layout->binding[binding];
      if (bl->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
         continue;

      unsigned array_size = MAX2(bl->array_size, 1);
      for (unsigned e = 0; e < array_size && binding + e < TU5_MAX_SSBOS; e++) {
         const uint32_t *desc = dset->mapped_ptr + bl->offset / 4 +
                                e * bl->size / 4;
         addrs[binding + e] = desc[0] | ((uint64_t)desc[1] << 32);
         sizes[binding + e] = desc[2];
         max_binding = MAX2(max_binding, binding + e);
      }
   }

   unsigned count = max_binding + 1;

   /* Sizes live in the "constants" state type, addresses in the "UBO" state
    * type; both are two dwords per buffer, matching Gallium's a5xx emit_ssbos.
    */
   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + 2 * count);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) |
                            CP_LOAD_STATE4_0_NUM_UNIT(count));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   for (unsigned i = 0; i < count; i++) {
      tu_cs_emit(&cmd->cs, TU5_SSBO_1_0_WIDTH(sizes[i] & 0xffff));
      tu_cs_emit(&cmd->cs, TU5_SSBO_1_1_HEIGHT(sizes[i] >> 16));
   }

   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + 2 * count);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) |
                            CP_LOAD_STATE4_0_NUM_UNIT(count));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_UBO) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   for (unsigned i = 0; i < count; i++)
      tu_cs_emit_qw(&cmd->cs, addrs[i]);
   /* a5xx get_ssbo_size() is lowered to a const load: upload per-slot sizes
    * (in bytes) into the reserved const range.
    */
   const struct ir3_shader_variant *v = shader->variant;
   if (ir3_const_can_upload(&v->const_state->allocs,
                            IR3_CONST_ALLOC_SSBO_SIZES, v->constlen)) {
      uint32_t off =
         v->const_state->allocs.consts[IR3_CONST_ALLOC_SSBO_SIZES].offset_vec4;
      unsigned units = DIV_ROUND_UP(TU5_MAX_SSBOS, 4);

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + units * 4);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(off) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      for (unsigned i = 0; i < units * 4; i++)
         tu_cs_emit(&cmd->cs, i < TU5_MAX_SSBOS ? sizes[i] * 4 : 0);
   }
}

static void
tu5_emit_push_consts(struct tu_cmd_buffer *cmd,
                     const struct ir3_shader_variant *v)
{
   const struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
   const struct tu_push_constant_range *pc = &shader->const_state.push_consts;

   if (pc->type != IR3_PUSH_CONSTS_PER_STAGE || pc->dwords == 0)
      return;

   unsigned units = DIV_ROUND_UP(pc->dwords, 4);

   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + units * 4);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(pc->lo_dwords / 4) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                            CP_LOAD_STATE4_0_NUM_UNIT(units));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   for (unsigned i = 0; i < units * 4; i++)
      tu_cs_emit(&cmd->cs,
                 i < pc->dwords ? cmd->push_constants[i] : 0);
}

static uint64_t
tu5_get_ubo_addr(struct tu_cmd_buffer *cmd, unsigned binding)
{
   struct tu_descriptor_state *ds =
      tu_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_COMPUTE);
   struct tu_descriptor_set *dset = ds->sets[0];

   if (!dset || binding >= dset->layout->binding_count || !dset->mapped_ptr)
      return 0;

   const struct tu_descriptor_set_binding_layout *bl =
      &dset->layout->binding[binding];
   if (bl->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
      return 0;

   const uint32_t *desc = dset->mapped_ptr + bl->offset / 4;
   return desc[0] | ((uint64_t)(desc[1] & 0x1ffff) << 32);
}

static void
tu5_emit_ubo_state(struct tu_cmd_buffer *cmd)
{
   struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
   const struct ir3_shader_variant *v = shader->variant;

   if (!shader->num_ubos)
      return;

   const struct ir3_const_allocations *allocs = &v->const_state->allocs;

   /* Full UBO loads go through pointers in const state (gen < 6). */
   if (ir3_const_can_upload(allocs, IR3_CONST_ALLOC_UBO_PTRS, v->constlen)) {
      uint32_t off = allocs->consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4;
      unsigned slots = MAX2((unsigned)shader->num_ubos, v->const_state->num_ubos);
      unsigned units = DIV_ROUND_UP(slots * 2, 4);

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + units * 4);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(off) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

      for (unsigned i = 0; i < units * 4; i++) {
         unsigned slot = i / 2;
         unsigned addr_lo = 0, addr_hi = 0;
         if (slot < shader->num_ubos) {
            uint64_t addr = tu5_get_ubo_addr(cmd, shader->ubo_map[slot].binding);
            addr_lo = addr;
            addr_hi = addr >> 32;
         }
         tu_cs_emit(&cmd->cs, (i & 1) ? addr_hi : addr_lo);
      }
   }

   /* Static subranges are pushed directly into const state. */
   const struct ir3_ubo_analysis_state *state = &v->const_state->ubo_state;
   for (unsigned i = 0; i < state->num_enabled; i++) {
      const struct ir3_ubo_range *r = &state->range[i];
      uint32_t size = r->end - r->start;
      if (!size || !ir3_const_can_upload(allocs, IR3_CONST_ALLOC_UBO_RANGES,
                                         v->constlen))
         continue;

      uint64_t addr = tu5_get_ubo_addr(cmd, r->ubo.block);
      if (!addr)
         continue;

      unsigned units = size / 16;
      if (!units)
         continue;

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(r->offset / 16) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR((uint32_t)(addr + r->start)));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI((addr + r->start) >> 32));
   }
}

void
tu5_dispatch(struct tu_cmd_buffer *cmd, const struct tu_dispatch_info *info)
{
   if (!info->indirect &&
       (info->blocks[0] == 0 || info->blocks[1] == 0 || info->blocks[2] == 0))
      return;

   struct tu_shader *shader = cmd->state.shaders[MESA_SHADER_COMPUTE];
   if (!shader)
      return;

   const struct ir3_shader_variant *v = shader->variant;

   /* UCHE is physically indexed and survives process switches; BOs of
    * different processes can alias in physical memory. Invalidate it before
    * the dispatch so we never read stale shader/descriptor data.
    */
   tu_cs_emit_pkt4(&cmd->cs, REG_A5XX_UCHE_CACHE_INVALIDATE_MIN, 5);
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs, 0x12);
   tu_cs_emit_pkt7(&cmd->cs, CP_WAIT_FOR_IDLE, 0);

   tu5_emit_flush(cmd);
   tu5_emit_driver_params(cmd, v, info);
   tu5_emit_push_consts(cmd, v);
   tu5_emit_ssbo_state(cmd);
   tu5_emit_ubo_state(cmd);
   tu5_emit_ubo_state(cmd);

   const uint16_t *local_size = v->local_size;
   const uint32_t *num_groups = info->blocks;

   tu_cs_emit_pkt4(&cmd->cs, TU5_REG_HLSQ_CS_NDRANGE_0, 7);
   tu_cs_emit(&cmd->cs,
      TU5_HLSQ_CS_NDRANGE_0_KERNELDIM(3) |
      TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEX(local_size[0] - 1) |
      TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEY(local_size[1] - 1) |
      TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(local_size[2] - 1));
   tu_cs_emit(&cmd->cs,
      TU5_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(local_size[0] * num_groups[0]));
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs,
      TU5_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(local_size[1] * num_groups[1]));
   tu_cs_emit(&cmd->cs, 0);
   tu_cs_emit(&cmd->cs,
      TU5_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(local_size[2] * num_groups[2]));
   tu_cs_emit(&cmd->cs, 0);

   tu_cs_emit_pkt4(&cmd->cs, TU5_REG_HLSQ_CS_KERNEL_GROUP_X, 3);
   tu_cs_emit(&cmd->cs, 1);
   tu_cs_emit(&cmd->cs, 1);
   tu_cs_emit(&cmd->cs, 1);

   if (info->indirect) {
      tu_cs_emit_pkt7(&cmd->cs, CP_EXEC_CS_INDIRECT, 4);
      tu_cs_emit(&cmd->cs, 0);
      tu_cs_emit_qw(&cmd->cs, info->indirect);
      tu_cs_emit(&cmd->cs,
         A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEX(local_size[0] - 1) |
         A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEY(local_size[1] - 1) |
         A5XX_CP_EXEC_CS_INDIRECT_3_LOCALSIZEZ(local_size[2] - 1));
   } else {
      tu_cs_emit_pkt7(&cmd->cs, CP_EXEC_CS, 4);
      tu_cs_emit(&cmd->cs, 0);
      tu_cs_emit(&cmd->cs, CP_EXEC_CS_1_NGROUPS_X(num_groups[0]));
      tu_cs_emit(&cmd->cs, CP_EXEC_CS_2_NGROUPS_Y(num_groups[1]));
      tu_cs_emit(&cmd->cs, CP_EXEC_CS_3_NGROUPS_Z(num_groups[2]));
   }

   /* Make the CS writes visible before the host reads the result. */
   tu5_emit_flush(cmd);

   cmd->state.total_dispatches++;
}
