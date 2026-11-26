/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal a5xx graphics emit path.
 *
 * The a5xx command stream uses CP_LOAD_STATE4 + the A5XX HLSQ/SP/VPC/RB
 * register blocks. Most of this is a trimmed port of the freedreno Gallium
 * fd5_program_emit()/fd5_emit_state() sequences, enough for a first
 * sysmem-rendered triangle:
 *   - VS/FS with no inputs other than gl_VertexIndex and no uniforms,
 *   - one linear (VK_IMAGE_TILING_LINEAR) color attachment,
 *   - no depth/stencil, no blending, no MSAA.
 */

#include "tu5_graphics.h"

#include <stdlib.h>
#include <string.h>

#include "tu_buffer.h"
#include "tu_cmd_buffer.h"
#include "tu_knl.h"
#include "tu_image.h"
#include "tu_sampler.h"
#include "tu_shader.h"

#include "ir3/ir3_shader.h"
#include "util/half_float.h"
#include "util/u_math.h"
#include "tu5_gfx_regs.h"
#include "tu5_regs.h"

static bool
tu5_gfx_no_flush(void)
{
   static int v = -1;
   if (v < 0)
      v = getenv("TU5_GFX_NO_FLUSH") != NULL;
   return v;
}

static bool
tu5_gfx_no_invalidate(void)
{
   static int v = -1;
   if (v < 0)
      v = getenv("TU5_GFX_NO_INVALIDATE") != NULL;
   return v;
}

static bool
tu5_gfx_no_preload(void)
{
   static int v = -1;
   if (v < 0)
      v = getenv("TU5_GFX_NO_PRELOAD") != NULL;
   return v;
}

static bool
tu5_gfx_debug(void)
{
   static int debug = -1;
   if (debug < 0)
      debug = getenv("TU5_GFX_DEBUG") != NULL;
   return debug;
}

static void
tu5_emit_reg(struct tu_cs *cs, uint32_t reg, uint32_t value)
{
   tu_cs_emit_pkt4(cs, reg, 1);
   tu_cs_emit(cs, value);
}

struct tu5_stage {
   const struct ir3_shader_variant *v;
   const struct ir3_info *info;
   uint8_t constoff; /* units of 4 vec4 */
   uint8_t constlen;
   uint8_t instroff; /* units of 16 instructions */
   uint8_t instrlen;
   uint64_t iova;
};

static uint32_t
tu5_format(VkFormat format, bool *srgb, bool *sint, bool *uint_format)
{
   *srgb = false;
   *sint = false;
   *uint_format = false;

   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      return A5EN_RB5_R8G8B8A8_UNORM;
   case VK_FORMAT_R8G8B8A8_SRGB:
      *srgb = true;
      return A5EN_RB5_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_UNORM:
      return A5EN_RB5_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_SRGB:
      *srgb = true;
      return A5EN_RB5_R8G8B8A8_UNORM;
   case VK_FORMAT_R8G8B8A8_SNORM:
      return A5EN_RB5_R8G8B8A8_SNORM;
   case VK_FORMAT_R8G8B8A8_UINT:
      *uint_format = true;
      return A5EN_RB5_R8G8B8A8_UINT;
   case VK_FORMAT_R8G8B8A8_SINT:
      *sint = true;
      return A5EN_RB5_R8G8B8A8_SINT;
   case VK_FORMAT_R8_UNORM:
      return A5EN_RB5_R8_UNORM;
   case VK_FORMAT_R8G8_UNORM:
      return A5EN_RB5_R8G8_UNORM;
   case VK_FORMAT_R16_UNORM:
      return A5EN_RB5_R16_UNORM;
   case VK_FORMAT_R16G16_UNORM:
      return A5EN_RB5_R16G16_UNORM;
   case VK_FORMAT_R32_UINT:
      *uint_format = true;
      return A5EN_RB5_R32_UINT;
   case VK_FORMAT_R32_SINT:
      *sint = true;
      return A5EN_RB5_R32_SINT;
   case VK_FORMAT_R32_SFLOAT:
      return A5EN_RB5_R32_FLOAT;
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return A5EN_RB5_R10G10B10A2_UNORM;
   default:
      return 0;
   }
}

static enum a3xx_color_swap
tu5_format_swap(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB:
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
      return WXYZ;
   default:
      return WZYX;
   }
}

static void
tu5_emit_shader_obj(struct tu_cs *cs, uint32_t obj_reg,
                    uint64_t iova)
{
   /* SP_{VS,FS}_OBJ_START: binary address, PVT_MEM_PARAM, PVT_MEM_ADDR, size.
    * Simple VS/FS have no private memory.
    */
   tu_cs_emit_pkt4(cs, obj_reg, 6);
   tu_cs_emit_qw(cs, iova);
   tu_cs_emit(cs, 0);
   tu_cs_emit_qw(cs, 0);
   tu_cs_emit(cs, 0);
}

static void
tu5_emit_shader_bin(struct tu_cs *cs, enum a4xx_state_block sb,
                    uint8_t instrlen, uint64_t iova)
{
   if (!instrlen)
      return;

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                  CP_LOAD_STATE4_0_NUM_UNIT(instrlen));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER) |
                  CP_LOAD_STATE4_1_EXT_SRC_ADDR((uint32_t)iova));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(iova >> 32));
}

static void
tu5_emit_immediates(struct tu_cs *cs, const struct ir3_shader_variant *v,
                    enum a4xx_state_block sb)
{
   const struct ir3_const_state *const_state = v->const_state;
   if (!const_state || !v->imm_state.count)
      return;

   uint32_t base = const_state->allocs.max_const_offset_vec4;
   int size = DIV_ROUND_UP(v->imm_state.count, 4);
   size = MIN2(size + base, v->constlen) - base;
   if (size <= 0)
      return;

   tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + size * 4);
   tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(base) |
                  CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                  CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                  CP_LOAD_STATE4_0_NUM_UNIT(size));
   tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                  CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   for (int i = 0; i < size * 4; i++)
      tu_cs_emit(cs, i < (int)v->imm_state.count ? v->imm_state.values[i] : 0);
}

static void
tu5_emit_constant_data(struct tu_cs *cs, const struct tu_shader *shader,
                       enum a4xx_state_block sb)
{
   const struct ir3_shader_variant *v = shader->variant;
   const struct ir3_const_state *const_state = v->const_state;
   if (!const_state || !v->constant_data_size)
      return;

   const struct ir3_ubo_analysis_state *state = &const_state->ubo_state;

   for (unsigned i = 0; i < state->num_enabled; i++) {
      unsigned ubo = state->range[i].ubo.block;
      if (ubo != const_state->consts_ubo.idx)
         continue;

      uint32_t size = state->range[i].end - state->range[i].start;
      if (16 * v->constlen <= state->range[i].offset)
         continue;
      size = MIN2(size, (16 * v->constlen) - state->range[i].offset);
      if (!size)
         continue;

      uint32_t dst_off = state->range[i].offset / 16;
      uint32_t num_unit = size / 16;
      uint64_t addr = shader->binary_iova + v->info.constant_data_offset +
                      state->range[i].start;

      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(dst_off) |
                     CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                     CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                     CP_LOAD_STATE4_0_NUM_UNIT(num_unit));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                     CP_LOAD_STATE4_1_EXT_SRC_ADDR((uint32_t)addr));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(addr >> 32));
   }
}

static void
tu5_emit_program(struct tu_cmd_buffer *cmd,
                 const struct tu_shader *vs_shader,
                 const struct tu_shader *fs_shader,
                 const struct ir3_shader_linkage *linkage,
                 uint8_t pos_loc, uint8_t psize_loc)
{
   struct tu_cs *cs = &cmd->cs;
   const struct ir3_shader_variant *vs = vs_shader->variant;
   const struct ir3_shader_variant *fs = fs_shader->variant;

   struct tu5_stage s[2] = {
      [0] = {
         .v = vs,
         .info = &vs->info,
         .iova = vs_shader->binary_iova,
      },
      [1] = {
         .v = fs,
         .info = &fs->info,
         .iova = fs_shader->binary_iova,
      },
   };

   for (unsigned i = 0; i < 2; i++) {
      assert(s[i].v->constlen % 4 == 0);
      s[i].constlen = s[i].v->constlen / 4;
      s[i].instrlen = s[i].v->instrlen;
   }

   /* Both programs must fit in the 64-unit internal instruction store.
    * Oversized shaders are fetched from memory instead.
    */
   if (tu5_gfx_no_preload()) {
      s[0].instrlen = s[1].instrlen = 0;
   } else if ((unsigned)s[0].instrlen + s[1].instrlen > 64) {
      if (s[1].instrlen < 64)
         s[0].instrlen = 0;
      else if (s[0].instrlen < 64)
         s[1].instrlen = 0;
      else
         s[0].instrlen = s[1].instrlen = 0;
   }

   s[0].constoff = 0;
   s[1].constoff = s[0].constlen;
   s[0].instroff = 0;
   s[1].instroff = 64 - s[1].instrlen;

   /* a5xx requires FOUR_QUADS when the FS uses half registers (for example
    * an f16 texture sample); TWO_QUADS with half regs hangs the texture unit.
    */
   enum a3xx_threadsize fssz =
      (s[1].info->double_threadsize || s[1].info->max_half_reg >= 0) ?
      FOUR_QUADS : TWO_QUADS;

   uint32_t pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
   uint32_t psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
   uint32_t vertex_regid =
      ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE);
   uint32_t instance_regid =
      ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);

   uint32_t color_regid[8];
   if (fs->color0_mrt) {
      color_regid[0] = color_regid[1] = color_regid[2] = color_regid[3] =
      color_regid[4] = color_regid[5] = color_regid[6] = color_regid[7] =
         ir3_find_output_regid(fs, FRAG_RESULT_COLOR);
   } else {
      color_regid[0] = ir3_find_output_regid(fs, FRAG_RESULT_DATA0);
      color_regid[1] = ir3_find_output_regid(fs, FRAG_RESULT_DATA1);
      color_regid[2] = ir3_find_output_regid(fs, FRAG_RESULT_DATA2);
      color_regid[3] = ir3_find_output_regid(fs, FRAG_RESULT_DATA3);
      color_regid[4] = ir3_find_output_regid(fs, FRAG_RESULT_DATA4);
      color_regid[5] = ir3_find_output_regid(fs, FRAG_RESULT_DATA5);
      color_regid[6] = ir3_find_output_regid(fs, FRAG_RESULT_DATA6);
      color_regid[7] = ir3_find_output_regid(fs, FRAG_RESULT_DATA7);
   }

   uint32_t samp_id_regid =
      ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
   uint32_t samp_mask_regid =
      ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
   uint32_t face_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
   uint32_t coord_regid = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
   /* Keep the invalid regid for unused sysvals: 0 would mean r0 and the
    * hardware would write several sysvals into the same register, clobbering
    * the barycentric IJ values.
    */
   uint32_t zwcoord_regid = VALIDREG(coord_regid) ? coord_regid + 2
                                                  : regid(63, 0);
   uint32_t ij_regid[IJ_COUNT];
   for (unsigned i = 0; i < ARRAY_SIZE(ij_regid); i++)
      ij_regid[i] = ir3_find_sysval_regid(
         fs, SYSTEM_VALUE_BARYCENTRIC_PERSP_PIXEL + i);

   /* HLSQ/SP program configuration. */
   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CONFIG, 5);
   tu_cs_emit(cs, A5XX_HLSQ_VS_CONFIG_CONSTOBJECTOFFSET(s[0].constoff) |
                     A5XX_HLSQ_VS_CONFIG_SHADEROBJOFFSET(s[0].instroff) |
                     A5XX_HLSQ_VS_CONFIG_ENABLED);
   tu_cs_emit(cs, A5XX_HLSQ_FS_CONFIG_CONSTOBJECTOFFSET(s[1].constoff) |
                     A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET(s[1].instroff) |
                     A5XX_HLSQ_FS_CONFIG_ENABLED);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CNTL, 5);
   tu_cs_emit(cs, A5XX_HLSQ_VS_CNTL_INSTRLEN(s[0].instrlen) |
                  COND(vs->has_ssbo, A5XX_HLSQ_VS_CNTL_SSBO_ENABLE));
   tu_cs_emit(cs, A5XX_HLSQ_FS_CNTL_INSTRLEN(s[1].instrlen) |
                  COND(fs->has_ssbo, A5XX_HLSQ_FS_CNTL_SSBO_ENABLE));
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_VS_CONFIG, 5);
   tu_cs_emit(cs, A5XX_SP_VS_CONFIG_CONSTOBJECTOFFSET(s[0].constoff) |
                     A5XX_SP_VS_CONFIG_SHADEROBJOFFSET(s[0].instroff) |
                     A5XX_SP_VS_CONFIG_ENABLED);
   tu_cs_emit(cs, A5XX_SP_FS_CONFIG_CONSTOBJECTOFFSET(s[1].constoff) |
                     A5XX_SP_FS_CONFIG_SHADEROBJOFFSET(s[1].instroff) |
                     A5XX_SP_FS_CONFIG_ENABLED);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_VS_CONSTLEN, 2);
   tu_cs_emit(cs, s[0].constlen);
   tu_cs_emit(cs, s[0].instrlen);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_FS_CONSTLEN, 2);
   tu_cs_emit(cs, s[1].constlen);
   tu_cs_emit(cs, s[1].instrlen);

   tu5_emit_reg(cs, REG_A5XX_SP_VS_CTRL_REG0,
      A5XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(
         MAX2(s[0].info->max_half_reg + 1, 1)) |
      A5XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(s[0].info->max_reg + 1) |
      COND(s[0].instrlen != 0, A5XX_SP_VS_CTRL_REG0_BUFFER) |
      0x2 |
      A5XX_SP_VS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(vs)) |
      COND(vs->need_pixlod, A5XX_SP_VS_CTRL_REG0_PIXLODENABLE));

   /* a5xx appends gl_Position (and gl_PointSize) after the linked varyings. */
   uint8_t pos_loc_final = pos_loc;
   uint8_t psize_loc_final = psize_loc;
   (void)pos_loc_final;
   (void)psize_loc_final;

   /* fd5 computes this mask before appending POS/PSIZ to the linkage. */
   uint32_t varmask[4] = {
      linkage->varmask[0], linkage->varmask[1],
      linkage->varmask[2], linkage->varmask[3],
   };
   if (pos_loc != 0xff) {
      for (unsigned i = 0; i < 4; i++)
         varmask[(pos_loc + i) / 32] &= ~(1u << ((pos_loc + i) % 32));
   }
   if (psize_loc != 0xff)
      varmask[psize_loc / 32] &= ~(1u << (psize_loc % 32));

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VAR_DISABLE(0), 4);
   for (unsigned i = 0; i < 4; i++)
      tu_cs_emit(cs, ~varmask[i]);

   for (unsigned i = 0, j = 0; i < 16 && j < linkage->cnt; i++) {
      uint32_t reg = A5XX_SP_VS_OUT_REG_A_REGID(linkage->var[j].regid) |
                     A5XX_SP_VS_OUT_REG_A_COMPMASK(linkage->var[j].compmask);
      j++;
      reg |= A5XX_SP_VS_OUT_REG_B_REGID(linkage->var[j].regid) |
             A5XX_SP_VS_OUT_REG_B_COMPMASK(linkage->var[j].compmask);
      j++;
      tu5_emit_reg(cs, REG_A5XX_SP_VS_OUT_REG(i), reg);
   }

   for (unsigned i = 0, j = 0; i < 8 && j < linkage->cnt; i++) {
      uint32_t reg = A5XX_SP_VS_VPC_DST_REG_OUTLOC0(linkage->var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC1(linkage->var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC2(linkage->var[j++].loc);
      reg |= A5XX_SP_VS_VPC_DST_REG_OUTLOC3(linkage->var[j++].loc);
      tu5_emit_reg(cs, REG_A5XX_SP_VS_VPC_DST_REG(i), reg);
   }

   tu5_emit_shader_obj(cs, REG_A5XX_SP_VS_OBJ_START, s[0].iova);
   tu5_emit_shader_bin(cs, SB4_VS_SHADER, s[0].instrlen, s[0].iova);

   tu5_emit_reg(cs, REG_A5XX_PC_PRIM_VTX_CNTL,
                COND(vs->writes_psize, A5XX_PC_PRIM_VTX_CNTL_PSIZE));

   tu5_emit_reg(cs, REG_A5XX_SP_PRIMITIVE_CNTL,
                A5XX_SP_PRIMITIVE_CNTL_VSOUT(linkage->cnt));

   bool force_varying = getenv("TU5_GFX_FORCE_VARYING") != NULL;
   tu5_emit_reg(cs, REG_A5XX_VPC_CNTL_0,
                A5XX_VPC_CNTL_0_STRIDE_IN_VPC(linkage->max_loc) |
                COND(force_varying || fs->total_in > 0,
                     A5XX_VPC_CNTL_0_VARYING) |
                0x10000);

   tu5_emit_shader_obj(cs, REG_A5XX_SP_FS_OBJ_START, s[1].iova);

   tu_cs_emit_pkt4(cs, REG_A5XX_HLSQ_CONTROL_0_REG, 5);
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(fssz) |
                  A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(TWO_QUADS) |
                  0x00000880);
   tu_cs_emit(cs, A5XX_HLSQ_CONTROL_1_REG_PRIMALLOCTHRESHOLD(63));
   /* These HLSQ fields carry raw register ids, not booleans. Unused sysvals
    * must be encoded as r63.x (0xfc), as fd5 does, not as 0 (= r0).
    */
   tu_cs_emit(cs,
      A5XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
      A5XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
      A5XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(samp_mask_regid) |
      A5XX_HLSQ_CONTROL_2_REG_CENTERRHW(ij_regid[IJ_PERSP_CENTER_RHW]));
   tu_cs_emit(cs,
      A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(ij_regid[IJ_PERSP_PIXEL]) |
      A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(ij_regid[IJ_LINEAR_PIXEL]) |
      A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(ij_regid[IJ_PERSP_CENTROID]) |
      A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(ij_regid[IJ_LINEAR_CENTROID]));
   tu_cs_emit(cs,
      A5XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
      A5XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
      A5XX_HLSQ_CONTROL_4_REG_IJ_PERSP_SAMPLE(ij_regid[IJ_PERSP_SAMPLE]) |
      A5XX_HLSQ_CONTROL_4_REG_IJ_LINEAR_SAMPLE(ij_regid[IJ_LINEAR_SAMPLE]));

   tu5_emit_reg(cs, REG_A5XX_SP_FS_CTRL_REG0,
      COND(getenv("TU5_GFX_FORCE_VARYING") || fs->total_in > 0,
           A5XX_SP_FS_CTRL_REG0_VARYING) |
      0x40002 |
      COND(s[1].instrlen != 0, A5XX_SP_FS_CTRL_REG0_BUFFER) |
      A5XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
      A5XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(
         MAX2(s[1].info->max_half_reg + 1, 1)) |
      A5XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(s[1].info->max_reg + 1) |
      A5XX_SP_FS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(fs)) |
      COND(fs->need_pixlod, A5XX_SP_FS_CTRL_REG0_PIXLODENABLE));

   if (tu5_gfx_debug()) {
      fprintf(stderr, "  iova vs=%" PRIx64 " fs=%" PRIx64 "\n",
              vs_shader->binary_iova, fs_shader->binary_iova);
      fprintf(stderr, "  vscntl=0x%x fsctrl=0x%x hlsq0=0x%x\n",
         A5XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(
            MAX2(s[0].info->max_half_reg + 1, 1)) |
         A5XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(s[0].info->max_reg + 1) |
         COND(s[0].instrlen != 0, A5XX_SP_VS_CTRL_REG0_BUFFER) | 0x2 |
         A5XX_SP_VS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(vs)) |
         COND(vs->need_pixlod, A5XX_SP_VS_CTRL_REG0_PIXLODENABLE),
         COND(fs->total_in > 0, A5XX_SP_FS_CTRL_REG0_VARYING) | 0x40002 |
         COND(s[1].instrlen != 0, A5XX_SP_FS_CTRL_REG0_BUFFER) |
         A5XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
         A5XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(
            MAX2(s[1].info->max_half_reg + 1, 1)) |
         A5XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(s[1].info->max_reg + 1) |
         A5XX_SP_FS_CTRL_REG0_BRANCHSTACK(ir3_shader_branchstack_hw(fs)) |
         COND(fs->need_pixlod, A5XX_SP_FS_CTRL_REG0_PIXLODENABLE),
         A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(fssz) |
            A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(TWO_QUADS) | 0x880);
   }
   tu5_emit_reg(cs, REG_A5XX_HLSQ_UPDATE_CNTL, 0x020fffff);
   tu5_emit_reg(cs, REG_A5XX_VPC_GS_SIV_CNTL, 0x0000ffff);
   tu5_emit_reg(cs, REG_A5XX_SP_SP_CNTL, 0x00000010);

   uint32_t gras_cntl =
      CONDREG(ij_regid[IJ_PERSP_PIXEL], A5XX_GRAS_CNTL_IJ_PERSP_PIXEL) |
      CONDREG(ij_regid[IJ_PERSP_CENTROID], A5XX_GRAS_CNTL_IJ_PERSP_CENTROID) |
      CONDREG(ij_regid[IJ_PERSP_SAMPLE], A5XX_GRAS_CNTL_IJ_PERSP_SAMPLE) |
      CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
      CONDREG(ij_regid[IJ_LINEAR_CENTROID], A5XX_GRAS_CNTL_IJ_LINEAR_CENTROID) |
      CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A5XX_GRAS_CNTL_IJ_LINEAR_SAMPLE) |
      COND(fs->fragcoord_compmask != 0,
           A5XX_GRAS_CNTL_COORD_MASK(fs->fragcoord_compmask) |
              A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL) |
      COND(fs->frag_face, A5XX_GRAS_CNTL_IJ_LINEAR_PIXEL);
   tu5_emit_reg(cs, REG_A5XX_GRAS_CNTL, gras_cntl);

   uint32_t rb_rc0 =
      CONDREG(ij_regid[IJ_PERSP_PIXEL], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_PIXEL) |
      CONDREG(ij_regid[IJ_PERSP_CENTROID], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_CENTROID) |
      CONDREG(ij_regid[IJ_PERSP_SAMPLE], A5XX_RB_RENDER_CONTROL0_IJ_PERSP_SAMPLE) |
      CONDREG(ij_regid[IJ_LINEAR_PIXEL], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
      CONDREG(ij_regid[IJ_LINEAR_CENTROID], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_CENTROID) |
      CONDREG(ij_regid[IJ_LINEAR_SAMPLE], A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_SAMPLE) |
      COND(fs->fragcoord_compmask != 0,
           A5XX_RB_RENDER_CONTROL0_COORD_MASK(fs->fragcoord_compmask) |
              A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL) |
      COND(fs->frag_face, A5XX_RB_RENDER_CONTROL0_IJ_LINEAR_PIXEL);
   uint32_t rb_rc1 =
      CONDREG(samp_mask_regid, A5XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
      COND(fs->frag_face, A5XX_RB_RENDER_CONTROL1_FACENESS) |
      CONDREG(samp_id_regid, A5XX_RB_RENDER_CONTROL1_SAMPLEID) |
      CONDREG(ij_regid[IJ_PERSP_CENTER_RHW], A5XX_RB_RENDER_CONTROL1_CENTERRHW) |
      COND(fs->sample_shading, A5XX_RB_RENDER_CONTROL1_SAMPLEMODE(3));
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RENDER_CONTROL0, 2);
   tu_cs_emit(cs, rb_rc0);
   tu_cs_emit(cs, rb_rc1);

   tu_cs_emit_pkt4(cs, REG_A5XX_SP_FS_OUTPUT_REG(0), 8);
   for (unsigned i = 0; i < 8; i++) {
      tu_cs_emit(cs, A5XX_SP_FS_OUTPUT_REG_REGID(color_regid[i]) |
                     COND(color_regid[i] & HALF_REG_ID,
                          A5XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
   }

   tu5_emit_reg(cs, REG_A5XX_VPC_PACK,
                A5XX_VPC_PACK_NUMNONPOSVAR(pos_loc) |
                A5XX_VPC_PACK_PSIZELOC(psize_loc));

   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_INTERP_MODE(0), 8);
   for (unsigned i = 0; i < 8; i++)
      tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_VPC_VARYING_PS_REPL_MODE(0), 8);
   for (unsigned i = 0; i < 8; i++)
      tu_cs_emit(cs, 0);

   if (tu5_gfx_debug())
      fprintf(stderr, "  clip_mask=0x%x cull_mask=0x%x\n",
              vs->clip_mask, vs->cull_mask);
   tu5_emit_reg(cs, REG_A5XX_GRAS_VS_CL_CNTL,
                A5XX_GRAS_VS_CL_CNTL_CLIP_MASK(vs->clip_mask) |
                A5XX_GRAS_VS_CL_CNTL_CULL_MASK(vs->cull_mask));
   tu5_emit_reg(cs, REG_A5XX_VPC_CLIP_CNTL,
                A5XX_VPC_CLIP_CNTL_CLIP_MASK(vs->clip_mask | vs->cull_mask) |
                A5XX_VPC_CLIP_CNTL_CLIP_DIST_03_LOC(0xff) |
                A5XX_VPC_CLIP_CNTL_CLIP_DIST_47_LOC(0xff));
   tu5_emit_reg(cs, REG_A5XX_PC_CLIP_CNTL,
                A5XX_PC_CLIP_CNTL_CLIP_MASK(vs->clip_mask));

   tu5_emit_shader_bin(cs, SB4_FS_SHADER, s[1].instrlen, s[1].iova);

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_CONTROL_1, 5);
   tu_cs_emit(cs, A5XX_VFD_CONTROL_1_REGID4VTX(vertex_regid) |
                  A5XX_VFD_CONTROL_1_REGID4INST(instance_regid) |
                  0xfc0000);
   tu_cs_emit(cs, 0x0000fcfc);
   tu_cs_emit(cs, 0x0000fcfc);
   tu_cs_emit(cs, 0x000000fc);
   tu_cs_emit(cs, 0x00000000);

   tu5_emit_immediates(cs, vs, SB4_VS_SHADER);
   tu5_emit_immediates(cs, fs, SB4_FS_SHADER);
   tu5_emit_constant_data(cs, vs_shader, SB4_VS_SHADER);
   tu5_emit_constant_data(cs, fs_shader, SB4_FS_SHADER);
}

static void
tu5_event_write(struct tu_cs *cs, enum vgt_event_type event)
{
   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 1);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(event));
}

static void
tu5_ccu_flush_color(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;
   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(PC_CCU_FLUSH_COLOR_TS));
   tu_cs_emit_qw(cs, global_iova(cmd, seqno_dummy));
   tu_cs_emit(cs, 0);
}

static bool
tu5_has_depth_attachment(struct tu_cmd_buffer *cmd)
{
   const struct tu_subpass *subpass = cmd->state.subpass;

   return subpass && cmd->state.attachments &&
          subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED &&
          cmd->state.attachments[
             subpass->depth_stencil_attachment.attachment] != NULL;
}

static void
tu5_ccu_flush_depth(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;
   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(PC_CCU_FLUSH_DEPTH_TS));
   tu_cs_emit_qw(cs, global_iova(cmd, seqno_dummy));
   tu_cs_emit(cs, 0);
}

static void
tu5_emit_vs_driver_params(struct tu_cmd_buffer *cmd,
                          const struct tu_shader *vs_shader,
                          const struct tu5_draw_info *info)
{
   const struct ir3_shader_variant *v = vs_shader->variant;
   if (!v->need_driver_params)
      return;

   const struct ir3_const_state *const_state = v->const_state;
   const struct ir3_const_allocations *allocs = &const_state->allocs;
   if (!ir3_const_can_upload(allocs, IR3_CONST_ALLOC_DRIVER_PARAMS,
                             v->constlen))
      return;

   uint32_t offset = allocs->consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
   uint32_t avail = (v->constlen - offset) * 4;
   uint32_t size = MIN2(const_state->num_driver_params, avail);
   size = align(size, 4);
   if (size > avail)
      size = avail;
   if (!size)
      return;

   if (getenv("TU5_GFX_DEBUG"))
      fprintf(stderr, "tu5 vs params: offset=%u vec4, size=%u dwords, struct=%zu dwords, constlen=%u\n",
              offset, size, sizeof(struct ir3_driver_params_vs) / 4, v->constlen);

   struct ir3_driver_params_vs p = {
      .draw_id = 0,
      .vtxid_base = info->indexed ? (uint32_t)info->vertex_offset
                                  : info->first_vertex,
      .instid_base = info->first_instance,
      .vtxcnt_max = 0,
      .is_indexed_draw = info->indexed ? ~0u : 0,
   };

   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + size);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(offset) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_VS_SHADER) |
                            CP_LOAD_STATE4_0_NUM_UNIT(size / 4));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

   const uint32_t *dwords = (const uint32_t *)&p;
   for (unsigned i = 0; i < size; i++)
      tu_cs_emit(&cmd->cs, i < sizeof(p) / 4 ? dwords[i] : 0);
}

static bool
tu5_gfx_clear(void)
{
   static int v = -1;
   if (v < 0)
      v = getenv("TU5_GFX_CLEAR") != NULL;
   return v;
}

static void
tu5_emit_clear(struct tu_cmd_buffer *cmd, uint32_t color)
{
   struct tu_cs *cs = &cmd->cs;

   tu5_emit_reg(cs, REG_A5XX_RB_BLIT_CNTL,
                A5XX_RB_BLIT_CNTL_BUF(A5EN_BLIT_MRT0));
   tu5_emit_reg(cs, REG_A5XX_RB_CLEAR_CNTL,
                A5XX_RB_CLEAR_CNTL_FAST_CLEAR |
                A5XX_RB_CLEAR_CNTL_MASK(0xf));
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_CLEAR_COLOR_DW0, 4);
   tu_cs_emit(cs, color);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CCU_RESOLVE));
   tu_cs_emit_qw(cs, global_iova(cmd, seqno_dummy));
   tu_cs_emit(cs, 0);

   tu5_emit_reg(cs, REG_A5XX_RB_CLEAR_CNTL, 0);
}

static uint32_t
tu5_vfmt(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R32G32_SFLOAT:
      return A5EN_VFMT5_32_32_FLOAT;
   case VK_FORMAT_R32G32B32_SFLOAT:
      return A5EN_VFMT5_32_32_32_FLOAT;
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      return A5EN_VFMT5_32_32_32_32_FLOAT;
   case VK_FORMAT_R32_SFLOAT:
      return A5EN_VFMT5_32_FLOAT;
   case VK_FORMAT_R16G16_SFLOAT:
      return A5EN_VFMT5_16_16_FLOAT;
   case VK_FORMAT_R8G8B8A8_UNORM:
      return A5EN_VFMT5_8_8_8_8_UNORM;
   default:
      return 0;
   }
}

static void
tu5_emit_vertex_inputs(struct tu_cmd_buffer *cmd,
                       const struct tu_shader *vs_shader)
{
   struct tu_cs *cs = &cmd->cs;
   const struct ir3_shader_variant *vs = vs_shader->variant;
   const struct vk_vertex_input_state *vi = &cmd->state.vi;

   if (tu5_gfx_debug()) {
      fprintf(stderr, "  VS inputs: count=%u attributes_valid=0x%x bindings_valid=0x%x\n",
              vs->inputs_count, vi->attributes_valid, vi->bindings_valid);
      for (unsigned k = 0; k < vs->inputs_count; k++)
         fprintf(stderr, "    in[%u]: slot=%u sysval=%d compmask=0x%x regid=0x%x\n",
                 k, vs->inputs[k].slot, vs->inputs[k].sysval,
                 vs->inputs[k].compmask, vs->inputs[k].regid);
   }

   unsigned slot = 0;
   for (unsigned i = 0; i < vs->inputs_count && slot < 16; i++) {
      if (vs->inputs[i].sysval || !vs->inputs[i].compmask)
         continue;

      if (vs->inputs[i].slot < VERT_ATTRIB_GENERIC0)
         continue;
      unsigned location = vs->inputs[i].slot - VERT_ATTRIB_GENERIC0;
      if (location >= MESA_VK_MAX_VERTEX_ATTRIBUTES ||
          !(vi->attributes_valid & (1u << location)))
         continue;

      const struct vk_vertex_attribute_state *attr = &vi->attributes[location];
      if (attr->binding >= MESA_VK_MAX_VERTEX_BINDINGS ||
          !(vi->bindings_valid & (1u << attr->binding)))
         continue;

      const struct vk_vertex_binding_state *binding =
         &vi->bindings[attr->binding];
      uint64_t base = cmd->state.vb[attr->binding].base + attr->offset;
      uint32_t size = cmd->state.vb[attr->binding].size > attr->offset ?
         cmd->state.vb[attr->binding].size - attr->offset : 0;
      uint32_t fmt = tu5_vfmt(attr->format);
      if (!fmt)
         continue;

      tu_cs_emit_pkt4(cs, REG_A5XX_VFD_FETCH(slot), 4);
      tu_cs_emit_qw(cs, base);
      tu_cs_emit(cs, size);
      tu_cs_emit(cs, binding->stride);

      tu_cs_emit_pkt4(cs, REG_A5XX_VFD_DECODE(slot), 2);
      tu_cs_emit(cs, A5XX_VFD_DECODE_INSTR_IDX(slot) |
                     A5XX_VFD_DECODE_INSTR_FORMAT(fmt) |
                     A5XX_VFD_DECODE_INSTR_SWAP(WZYX) |
                     A5XX_VFD_DECODE_INSTR_UNK30 |
                     A5XX_VFD_DECODE_INSTR_FLOAT);
      tu_cs_emit(cs, MAX2(binding->divisor, 1));

      if (tu5_gfx_debug()) {
         fprintf(stderr, "  VFD slot %u: loc=%u base=%" PRIx64 " size=%u stride=%u fmt=%u regid=0x%x\n",
                 slot, location, base, size, binding->stride, fmt,
                 vs->inputs[i].regid);
      }

      tu5_emit_reg(cs, REG_A5XX_VFD_DEST_CNTL(slot),
         A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(vs->inputs[i].compmask) |
         A5XX_VFD_DEST_CNTL_INSTR_REGID(vs->inputs[i].regid));
      slot++;
   }

   tu5_emit_reg(cs, REG_A5XX_VFD_CONTROL_0,
                A5XX_VFD_CONTROL_0_VTXCNT(slot));
}

static void
tu5_emit_gfx_ssbo_state(struct tu_cmd_buffer *cmd,
                        const struct tu_shader *shader,
                        enum a4xx_state_block shader_sb)
{
   if (!shader->num_ssbos)
      return;

   if (getenv("TU5_GFX_DEBUG"))
      fprintf(stderr, "tu5 ssbo state: num_ssbos=%u\n", shader->num_ssbos);

   struct tu_descriptor_state *ds =
      tu_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
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

   tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + 2 * count);
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                            CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_SSBO) |
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
                            CP_LOAD_STATE4_0_STATE_BLOCK(SB4_SSBO) |
                            CP_LOAD_STATE4_0_NUM_UNIT(count));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_UBO) |
                            CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
   tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
   for (unsigned i = 0; i < count; i++)
      tu_cs_emit_qw(&cmd->cs, addrs[i]);

   const struct ir3_shader_variant *v = shader->variant;
   if (ir3_const_can_upload(&v->const_state->allocs,
                            IR3_CONST_ALLOC_SSBO_SIZES, v->constlen)) {
      uint32_t off =
         v->const_state->allocs.consts[IR3_CONST_ALLOC_SSBO_SIZES].offset_vec4;
      unsigned units = DIV_ROUND_UP(TU5_MAX_SSBOS, 4);

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + units * 4);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(off) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(shader_sb) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      for (unsigned i = 0; i < units * 4; i++)
         tu_cs_emit(&cmd->cs, i < TU5_MAX_SSBOS ? sizes[i] * 4 : 0);
   }
}

static uint64_t
tu5_get_gfx_ubo_addr(struct tu_cmd_buffer *cmd, unsigned binding)
{
   struct tu_descriptor_state *ds =
      tu_get_descriptors_state(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS);
   struct tu_descriptor_set *dset = ds->sets[0];

   if (!dset || binding >= dset->layout->binding_count || !dset->mapped_ptr)
      return 0;

   const struct tu_descriptor_set_binding_layout *bl =
      &dset->layout->binding[binding];
   if (bl->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
       bl->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
      return 0;

   const uint32_t *desc = dset->mapped_ptr + bl->offset / 4;
   uint64_t addr = desc[0] | ((uint64_t)(desc[1] & 0x1ffff) << 32);
   uint32_t dyn = 0;
   if (bl->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
       dset->dynamic_descriptors) {
      dyn = dset->dynamic_descriptors[bl->dynamic_offset_offset / 4];
      addr += dyn;
   }
   if (getenv("TU5_GFX_DEBUG")) {
      fprintf(stderr, "tu5 ubo binding=%u type=%u addr=%llx size=%u dyn=%u\n",
              binding, bl->type, (unsigned long long)addr, desc[2], dyn);
   }
   return addr;
}

static void
tu5_emit_gfx_ubo_state(struct tu_cmd_buffer *cmd,
                       struct tu_shader *shader,
                       enum a4xx_state_block shader_sb)
{
   if (!shader || !shader->num_ubos)
      return;

   const struct ir3_shader_variant *v = shader->variant;
   const struct ir3_const_allocations *allocs = &v->const_state->allocs;

   if (getenv("TU5_GFX_DEBUG")) {
      fprintf(stderr, "tu5 ubo state: stage=%u num_ubos=%u constlen=%u base=%u can=%d\n",
              v->type, shader->num_ubos, v->constlen,
              allocs->consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4 * 4,
              ir3_const_can_upload(allocs, IR3_CONST_ALLOC_UBO_PTRS, v->constlen));
      fprintf(stderr, "  driver params at vec4 %u, ssbo sizes at vec4 %u\n",
              allocs->consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4,
              allocs->consts[IR3_CONST_ALLOC_SSBO_SIZES].offset_vec4);
   }

   /* a5xx UBO loads are lowered to global loads through pointers stored in
    * const state. Without uploading them the shader dereferences garbage
    * (usually 0) and the GPU hangs.
    */
   if (ir3_const_can_upload(allocs, IR3_CONST_ALLOC_UBO_PTRS, v->constlen)) {
      uint32_t off = allocs->consts[IR3_CONST_ALLOC_UBO_PTRS].offset_vec4;
      unsigned slots = MAX2((unsigned)shader->num_ubos,
                            v->const_state->num_ubos);
      unsigned units = DIV_ROUND_UP(slots * 2, 4);

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3 + units * 4);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(off) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(shader_sb) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));

      for (unsigned i = 0; i < units * 4; i++) {
         unsigned slot = i / 2;
         uint32_t addr_lo = 0, addr_hi = 0;
         if (slot < shader->num_ubos) {
            uint64_t addr =
               tu5_get_gfx_ubo_addr(cmd, shader->ubo_map[slot].binding);
            addr_lo = addr;
            addr_hi = addr >> 32;
         }
         tu_cs_emit(&cmd->cs, (i & 1) ? addr_hi : addr_lo);
      }
   }

   const struct ir3_ubo_analysis_state *state = &v->const_state->ubo_state;
   for (unsigned i = 0; i < state->num_enabled; i++) {
      const struct ir3_ubo_range *r = &state->range[i];
      uint32_t size = r->end - r->start;
      if (!size || !ir3_const_can_upload(allocs, IR3_CONST_ALLOC_UBO_RANGES,
                                         v->constlen))
         continue;

      uint64_t addr = tu5_get_gfx_ubo_addr(cmd, r->ubo.block);
      if (!addr)
         continue;

      unsigned units = size / 16;
      if (!units)
         continue;

      tu_cs_emit_pkt7(&cmd->cs, CP_LOAD_STATE4, 3);
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_0_DST_OFF(r->offset / 16) |
                               CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
                               CP_LOAD_STATE4_0_STATE_BLOCK(shader_sb) |
                               CP_LOAD_STATE4_0_NUM_UNIT(units));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                               CP_LOAD_STATE4_1_EXT_SRC_ADDR((uint32_t)(addr + r->start)));
      tu_cs_emit(&cmd->cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI((addr + r->start) >> 32));
   }
}

static void
tu5_emit_global_state(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;

   /* Sysmem rendering needs the CP render mode set to BYPASS; RB_CNTL_BYPASS
    * alone is not enough for shader (MRT) writes.
    */
   tu_cs_emit_pkt7(cs, CP_SET_RENDER_MODE, 5);
   tu_cs_emit(cs, CP_SET_RENDER_MODE_0_MODE(BYPASS));
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   /* UCHE is physically indexed and survives process switches, while BOs of
    * different processes can alias in physical memory. Without this, a new
    * process can read stale vertex/index/shader data from a previous one.
    */
   tu_cs_emit_pkt4(cs, REG_A5XX_UCHE_CACHE_INVALIDATE_MIN, 5);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0x12);
   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);

   /* The kernel already programs RB/TPL1 bank bits and the DBG_ECO
    * workarounds in a5xx_hw_init(). Only the mode registers that are not
    * part of the kernel init are needed here.
    */
   tu5_emit_reg(cs, REG_A5XX_PC_MODE_CNTL, 0x1f);
   tu5_emit_reg(cs, REG_A5XX_SP_MODE_CNTL, 0x1c);
   tu5_emit_reg(cs, REG_A5XX_HLSQ_MODE_CNTL, 1);
   tu5_emit_reg(cs, REG_A5XX_VFD_MODE_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_VPC_MODE_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_SP_VS_CONFIG_MAX_CONST, 0);
   tu5_emit_reg(cs, REG_A5XX_SP_FS_CONFIG_MAX_CONST, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_SC_BIN_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_CONSERVATIVE_RAS_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_VPC_FS_PRIMITIVEID_CNTL, 0xff);

   /* The kernel only programs the highest-bank-bit field; fd5 also sets the
    * TPL1 mode flags. Texturing needs the full value.
    */
   uint32_t hbb = cmd->device->physical_device->info->highest_bank_bit;
   if (hbb >= 13 && hbb <= 16) {
      tu5_emit_reg(cs, REG_A5XX_TPL1_MODE_CNTL,
                   0x444 | A5XX_TPL1_MODE_CNTL_HIGHESTBANKBIT(hbb - 13));
      tu5_emit_reg(cs, REG_A5XX_RB_MODE_CNTL,
                   0x40 | A5XX_RB_MODE_CNTL_HIGHESTBANKBIT(hbb - 13));
   }
}

/* A5xx sysmem rendering has no GMEM, so RB_CLEAR_CNTL cannot be used to
 * clear attachments (doing so hangs the GPU). The images created by this
 * driver are linear, so a render-pass clear can be done with a plain
 * CP_MEM_WRITE fill of the attachment memory.
 */
static bool
tu5_clear_color_dword(VkFormat format, const VkClearColorValue *color,
                      uint32_t *out)
{
   float rgba[4] = {
      color->float32[0], color->float32[1],
      color->float32[2], color->float32[3],
   };
   bool srgb = false, bgra = false;

   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      break;
   case VK_FORMAT_R8G8B8A8_SRGB:
      srgb = true;
      break;
   case VK_FORMAT_B8G8R8A8_UNORM:
      bgra = true;
      break;
   case VK_FORMAT_B8G8R8A8_SRGB:
      bgra = true;
      srgb = true;
      break;
   case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
      /* A2R10G10B10: A in bits 31:30, R 29:20, G 19:10, B 9:0. */
      uint32_t r = (uint32_t)(CLAMP(rgba[0], 0.0f, 1.0f) * 1023.0f + 0.5f);
      uint32_t g = (uint32_t)(CLAMP(rgba[1], 0.0f, 1.0f) * 1023.0f + 0.5f);
      uint32_t b = (uint32_t)(CLAMP(rgba[2], 0.0f, 1.0f) * 1023.0f + 0.5f);
      uint32_t a = (uint32_t)(CLAMP(rgba[3], 0.0f, 1.0f) * 3.0f + 0.5f);
      *out = b | (g << 10) | (r << 20) | (a << 30);
      return true;
   }
   default:
      return false;
   }

   uint32_t c[4];
   for (unsigned i = 0; i < 4; i++) {
      float v = CLAMP(rgba[i], 0.0f, 1.0f);
      if (srgb)
         v = (v <= 0.0031308f) ? v * 12.92f
                               : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
      c[i] = (uint32_t)(v * 255.0f + 0.5f);
   }

   if (bgra)
      *out = c[2] | (c[1] << 8) | (c[0] << 16) | (c[3] << 24);
   else
      *out = c[0] | (c[1] << 8) | (c[2] << 16) | (c[3] << 24);
   return true;
}

static void
tu5_fill_linear(struct tu_cs *cs, uint64_t addr, uint64_t size, uint32_t value)
{
   /* CP_MEM_WRITE's packet length field is 14 bits including the address. */
   const uint64_t chunk_size = 16380 * 4;

   for (uint64_t offset = 0; offset < size; offset += chunk_size) {
      uint64_t chunk = MIN2(size - offset, chunk_size);
      unsigned dwords = (chunk + 3) / 4;

      tu_cs_emit_pkt7(cs, CP_MEM_WRITE, 2 + dwords);
      tu_cs_emit_qw(cs, addr + offset);
      for (unsigned i = 0; i < dwords; i++)
         tu_cs_emit(cs, value);
   }

   tu_cs_emit_pkt7(cs, CP_WAIT_MEM_WRITES, 0);
}

static bool
tu5_clear_depth_dword(VkFormat format, float depth, uint32_t stencil,
                      bool clear_stencil, uint32_t *out)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM: {
      /* Use double: at 2^24 a float cannot represent the .5 added for
       * rounding, and the result would overflow into the neighbouring bits.
       */
      uint32_t v = (uint32_t)((double)CLAMP(depth, 0.0f, 1.0f) * 65535.0 + 0.5);
      v = MIN2(v, 0xffffu);
      *out = v | (v << 16);
      return true;
   }
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D24_UNORM_S8_UINT: {
      uint32_t v =
         (uint32_t)((double)CLAMP(depth, 0.0f, 1.0f) * 16777215.0 + 0.5);
      v = MIN2(v, 0xffffffu);
      if (clear_stencil)
         v |= (stencil & 0xff) << 24;
      *out = v;
      return true;
   }
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT: {
      union {
         float f;
         uint32_t u;
      } v = { depth };
      *out = v.u;
      return true;
   }
   default:
      return false;
   }
}

static uint64_t
tu5_ds_clear_size(const struct fdl6_view *view, uint64_t height)
{
   uint64_t linear = (uint64_t)view->pitch * height;
   return view->layer_size > linear ? view->layer_size : linear;
}

static void
tu5_clear_ds_attachment(struct tu_cmd_buffer *cmd, unsigned idx,
                        const struct tu_render_pass_attachment *att,
                        const struct tu_image_view *iview)
{
   VkFormat format = att->format;
   bool clear_depth = att->clear_mask & VK_IMAGE_ASPECT_DEPTH_BIT;
   bool clear_stencil = att->clear_mask & VK_IMAGE_ASPECT_STENCIL_BIT;
   /* For S8 the common code stores the stencil clear in the COLOR bit. */
   if (format == VK_FORMAT_S8_UINT)
      clear_stencil = att->clear_mask & VK_IMAGE_ASPECT_COLOR_BIT;
   bool d24s8 = format == VK_FORMAT_D24_UNORM_S8_UINT;
   float depth = cmd->state.clear_values[idx].depthStencil.depth;
   uint32_t stencil = cmd->state.clear_values[idx].depthStencil.stencil;
   uint64_t height = iview->image->vk.extent.height;
   const struct fdl6_view *dview =
      iview->view_depth ? iview->view_depth : &iview->view;

   if (d24s8) {
      /* Depth and stencil share a dword. A raw fill can only clear both at
       * once; preserving one of them would need a read-modify-write. If the
       * other aspect is not loaded (DONT_CARE) overwriting it is fine.
       */
      if (clear_depth && clear_stencil) {
         uint32_t value;
         if (tu5_clear_depth_dword(format, depth, stencil, true, &value)) {
            if (tu5_gfx_debug())
               fprintf(stderr, "  clear: D24S8 addr=%" PRIx64 " pitch=%u"
                       " h=%" PRIu64 " val=%08x\n", dview->base_addr,
                       dview->pitch, height, value);
            tu5_fill_linear(&cmd->cs, dview->base_addr,
                            tu5_ds_clear_size(dview, height), value);
         }
      } else if (clear_depth || clear_stencil) {
         if (!att->load) {
            uint32_t value;
            if (tu5_clear_depth_dword(format, depth, stencil,
                                      clear_stencil, &value))
               tu5_fill_linear(&cmd->cs, dview->base_addr,
                               tu5_ds_clear_size(dview, height), value);
         } else if (tu5_gfx_debug()) {
            fprintf(stderr, "  clear: D24S8 %s-only clear would clobber the "
                            "other aspect, skipped\n",
                    clear_depth ? "depth" : "stencil");
         }
      }
      return;
   }

   if (tu5_gfx_debug())
      fprintf(stderr, "  clear ds: vkfmt=%d depth=%d stencil=%d value=%f base=%" PRIx64 " pitch=%u layer=%u\n",
              format, clear_depth, clear_stencil, depth, dview->base_addr,
              dview->pitch, dview->layer_size);

   if (clear_depth) {
      uint32_t value;
      if (tu5_clear_depth_dword(format, depth, 0, false, &value))
         tu5_fill_linear(&cmd->cs, dview->base_addr,
                         tu5_ds_clear_size(dview, height), value);
      else if (tu5_gfx_debug())
         fprintf(stderr, "  clear: unsupported depth format %d, skipped\n",
                 format);
   }

   if (clear_stencil) {
      const struct fdl6_view *sview =
         iview->view_stencil ? iview->view_stencil : &iview->view;
      if (format != VK_FORMAT_D32_SFLOAT_S8_UINT &&
          format != VK_FORMAT_S8_UINT) {
         if (tu5_gfx_debug())
            fprintf(stderr, "  clear: unsupported stencil format %d, skipped\n",
                    format);
      } else {
         uint32_t value = (stencil & 0xff) * 0x01010101u;
         tu5_fill_linear(&cmd->cs, sview->base_addr,
                         tu5_ds_clear_size(sview, height), value);
      }
   }
}

void
tu5_clear_attachments(struct tu_cmd_buffer *cmd)
{
   const struct tu_render_pass *pass = cmd->state.pass;

   if (!pass || !cmd->state.subpass)
      return;

   uint32_t ds_idx =
      cmd->state.subpass->depth_stencil_attachment.attachment;

   for (unsigned i = 0; i < MIN2(pass->user_attachment_count,
                                 pass->attachment_count); i++) {
      const struct tu_render_pass_attachment *att = &pass->attachments[i];
      const struct tu_image_view *iview = cmd->state.attachments[i];

      if (!iview)
         continue;

      if (i == ds_idx) {
         tu5_clear_ds_attachment(cmd, i, att, iview);
         continue;
      }

      if (!(att->clear_mask & VK_IMAGE_ASPECT_COLOR_BIT))
         continue;

      uint32_t value;
      if (!tu5_clear_color_dword(att->format, &cmd->state.clear_values[i].color,
                                 &value)) {
         if (tu5_gfx_debug())
            fprintf(stderr, "  clear: unsupported format %d, skipped\n",
                    att->format);
         continue;
      }

      uint32_t width = iview->image->vk.extent.width;
      uint32_t height = iview->image->vk.extent.height;
      (void)width;

      tu5_fill_linear(&cmd->cs, iview->view.base_addr,
                      (uint64_t)iview->view.pitch * height, value);

      if (tu5_gfx_debug())
         fprintf(stderr, "  clear: att=%u addr=0x%llx pitch=%u h=%u val=0x%08x\n",
                 i, (unsigned long long)iview->view.base_addr,
                 iview->view.pitch, height, value);
   }
}

static void
tu5_emit_mrt(struct tu_cmd_buffer *cmd,
             const struct tu_image_view *iviews[8], unsigned nr)
{
   struct tu_cs *cs = &cmd->cs;
   uint32_t components = 0;

   for (unsigned i = 0; i < 8; i++) {
      const struct tu_image_view *iview = i < nr ? iviews[i] : NULL;
      bool used = iview != NULL;
      bool srgb = false, sint = false, uint_format = false;
      uint32_t format = 0;
      enum a3xx_color_swap swap = WZYX;
      uint32_t pitch = 0, layer_size = 0;
      uint64_t base = 0;

      if (used) {
         VkFormat vk_format = iview->vk.format;
         format = tu5_format(vk_format, &srgb, &sint, &uint_format);
         swap = tu5_format_swap(vk_format);
         pitch = iview->view.pitch;
         layer_size = iview->view.layer_size;
         base = iview->view.base_addr;
         components |= 0xf << (4 * i);
      }

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_BUF_INFO(i), 5);
      tu_cs_emit(cs,
         A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(used ? format : 0) |
         A5XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(A5EN_TILE5_LINEAR) |
         A5XX_RB_MRT_BUF_INFO_COLOR_SWAP(used ? swap : WZYX) |
         COND(used && srgb, A5XX_RB_MRT_BUF_INFO_COLOR_SRGB));
      tu_cs_emit(cs, A5XX_RB_MRT_PITCH(used ? pitch : 0));
      tu_cs_emit(cs, A5XX_RB_MRT_ARRAY_PITCH(used ? layer_size : 0));
      tu_cs_emit_qw(cs, base);

      tu5_emit_reg(cs, REG_A5XX_SP_FS_MRT_REG(i),
         A5XX_SP_FS_MRT_REG_COLOR_FORMAT(used ? format : 0) |
         COND(used && sint, A5XX_SP_FS_MRT_REG_COLOR_SINT) |
         COND(used && uint_format, A5XX_SP_FS_MRT_REG_COLOR_UINT) |
         COND(used && srgb, A5XX_SP_FS_MRT_REG_COLOR_SRGB));

      tu_cs_emit_pkt4(cs, REG_A5XX_RB_MRT_FLAG_BUFFER(i), 4);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, A5XX_RB_MRT_FLAG_BUFFER_PITCH(0));
      tu_cs_emit(cs, A5XX_RB_MRT_FLAG_BUFFER_ARRAY_PITCH(0));
   }

   tu5_emit_reg(cs, REG_A5XX_RB_RENDER_COMPONENTS, components);
}

/* The A6xx MEMOBJ descriptor field layout overlaps A5xx TEX_CONST for the
 * fields we need (format, swizzle, size, pitch, type, base, depth). The
 * chip 5 images are linear, so force the tile mode. A5xx uses an explicit
 * base register instead of a bindless descriptor vector.
 */
void
tu5_texture_descriptor(const uint32_t *src, uint32_t *dst)
{
   dst[0] = (src[0] & ~0x3u) | A5EN_TILE5_LINEAR;
   dst[1] = src[1] & 0x3fffffffu;
   dst[2] = src[2];
   dst[3] = (src[3] & 0x3fffu) | (src[3] & 0x08000000u);
   dst[4] = src[4] & ~0x1fu;
   dst[5] = src[5] & 0x3fffffffu;
   for (unsigned i = 6; i < 12; i++)
      dst[i] = 0;
}

/* A5xx sampler words 0/1 match A6xx, word 2 only has BCOLOR_OFFSET instead
 * of the A6xx border-color selectors. */
static void
tu5_sampler_descriptor(const uint32_t *src, uint32_t *dst)
{
   dst[0] = src[0];
   dst[1] = src[1] & ~0x1u;
   /* fd5 uses a small non-zero LOD clamp when mipmapping is off so the HW
    * can still choose between min and mag filtering of level 0.
    */
   if (!(dst[1] & A5XX_TEX_SAMP_1_MAX_LOD__MASK))
      dst[1] |= A5XX_TEX_SAMP_1_MAX_LOD(0.125f);
   dst[2] = 0;
   dst[3] = 0;
}

void
tu5_emit_textures(struct tu_cmd_buffer *cmd, struct tu_shader *shader,
                  enum a4xx_state_block sb, VkPipelineBindPoint bind_point)
{
   if (!shader->num_textures && !shader->num_samplers)
      return;

   struct tu_descriptor_state *ds =
      tu_get_descriptors_state(cmd, bind_point);
   uint32_t samplers[TU5_MAX_SAMPLERS][4] = {};
   uint32_t textures[TU5_MAX_TEXTURES][12] = {};

   for (unsigned i = 0; i < shader->num_samplers; i++) {
      unsigned set = shader->samp_map[i].set;
      unsigned binding = shader->samp_map[i].binding;
      const struct tu_descriptor_set *dset =
         set < MAX_SETS ? ds->sets[set] : NULL;

      if (!dset || binding >= dset->layout->binding_count)
         continue;

      const struct tu_descriptor_set_binding_layout *bl =
         &dset->layout->binding[binding];
      const uint32_t *src = NULL;

      if (bl->immutable_samplers_offset) {
         const struct tu_sampler *immutable =
            tu_immutable_samplers(dset->layout, bl);
         if (immutable)
            src = immutable->descriptor;
      } else if (dset->mapped_ptr) {
         if (bl->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            src = dset->mapped_ptr + bl->offset / 4 + FDL6_TEX_CONST_DWORDS;
         else if (bl->type == VK_DESCRIPTOR_TYPE_SAMPLER)
            src = dset->mapped_ptr + bl->offset / 4;
      }

      if (src)
         tu5_sampler_descriptor(src, samplers[i]);
   }

   for (unsigned i = 0; i < shader->num_textures; i++) {
      unsigned set = shader->tex_map[i].set;
      unsigned binding = shader->tex_map[i].binding;
      const struct tu_descriptor_set *dset =
         set < MAX_SETS ? ds->sets[set] : NULL;

      if (!dset || binding >= dset->layout->binding_count ||
          !dset->mapped_ptr)
         continue;

      const struct tu_descriptor_set_binding_layout *bl =
         &dset->layout->binding[binding];
      if (bl->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
          bl->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
          bl->type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
         continue;

      tu5_texture_descriptor(dset->mapped_ptr + bl->offset / 4, textures[i]);
   }

   struct tu_cs *cs = &cmd->cs;

   if (getenv("TU5_GFX_DEBUG")) {
      fprintf(stderr, "tu5 tex: sb=%u samplers=%u textures=%u\n",
              sb, shader->num_samplers, shader->num_textures);
      for (unsigned i = 0; i < shader->num_samplers; i++)
         fprintf(stderr, "  samp[%u]=%08x %08x %08x %08x\n", i,
                 samplers[i][0], samplers[i][1], samplers[i][2], samplers[i][3]);
      for (unsigned i = 0; i < shader->num_textures; i++)
         fprintf(stderr, "  tex[%u]=%08x %08x %08x %08x %08x\n", i,
                 textures[i][0], textures[i][1], textures[i][2],
                 textures[i][3], textures[i][4]);
   }

   if (shader->num_samplers) {
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 4 * shader->num_samplers);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                       CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                       CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                       CP_LOAD_STATE4_0_NUM_UNIT(shader->num_samplers));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER) |
                       CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      for (unsigned i = 0; i < shader->num_samplers; i++)
         for (unsigned j = 0; j < 4; j++)
            tu_cs_emit(cs, samplers[i][j]);
   }

   if (shader->num_textures) {
      tu_cs_emit_pkt7(cs, CP_LOAD_STATE4, 3 + 12 * shader->num_textures);
      tu_cs_emit(cs, CP_LOAD_STATE4_0_DST_OFF(0) |
                       CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
                       CP_LOAD_STATE4_0_STATE_BLOCK(sb) |
                       CP_LOAD_STATE4_0_NUM_UNIT(shader->num_textures));
      tu_cs_emit(cs, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) |
                       CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
      tu_cs_emit(cs, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
      for (unsigned i = 0; i < shader->num_textures; i++)
         for (unsigned j = 0; j < 12; j++)
            tu_cs_emit(cs, textures[i][j]);
   }

   if (getenv("TU5_GFX_TEXWAIT"))
      tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);
}

static void
tu5_emit_viewport(struct tu_cmd_buffer *cmd, uint32_t width, uint32_t height)
{
   struct tu_cs *cs = &cmd->cs;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   VkViewport vp = {
      .x = 0.0f,
      .y = 0.0f,
      .width = (float)width,
      .height = (float)height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
   };
   if (dyn->vp.viewport_count > 0)
      vp = dyn->vp.viewports[0];

   VkRect2D scissor = { { 0, 0 }, { width, height } };
   if (dyn->vp.scissor_count > 0)
      scissor = dyn->vp.scissors[0];

   if (tu5_gfx_debug()) {
      fprintf(stderr, "  viewport: count=%u vp={%f,%f,%f,%f,%f,%f}\n",
              dyn->vp.viewport_count, vp.x, vp.y, vp.width, vp.height,
              vp.minDepth, vp.maxDepth);
      fprintf(stderr, "  scissor: count=%u rect={%d,%d,%u,%u}\n",
              dyn->vp.scissor_count, scissor.offset.x, scissor.offset.y,
              scissor.extent.width, scissor.extent.height);
   }

   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_CL_VPORT_XOFFSET_0, 6);
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_XOFFSET_0(vp.x + vp.width * 0.5f));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_XSCALE_0(vp.width * 0.5f));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_YOFFSET_0(vp.y + vp.height * 0.5f));
   /* Vulkan NDC has +y pointing down, so the window-space y is
    * y_offset + y_ndc * height/2 (a positive scale). The GL convention
    * (used by fd5) applies the opposite sign.
    */
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_YSCALE_0(vp.height * 0.5f));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_ZOFFSET_0(
      (vp.minDepth + vp.maxDepth) * 0.5f));
   tu_cs_emit(cs, A5XX_GRAS_CL_VPORT_ZSCALE_0(
      (vp.maxDepth - vp.minDepth) * 0.5f));

   uint32_t minx = MAX2(scissor.offset.x, 0);
   uint32_t miny = MAX2(scissor.offset.y, 0);
   uint32_t maxx = MIN2(scissor.offset.x + scissor.extent.width, width);
   uint32_t maxy = MIN2(scissor.offset.y + scissor.extent.height, height);
   if (maxx <= minx)
      maxx = minx + 1;
   if (maxy <= miny)
      maxy = miny + 1;

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(minx) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(miny));
   tu_cs_emit(cs, A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_X(maxx - 1) |
                  A5XX_GRAS_SC_VIEWPORT_SCISSOR_TL_0_Y(maxy - 1));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(minx) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(miny));
   tu_cs_emit(cs, A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_X(maxx - 1) |
                  A5XX_GRAS_SC_SCREEN_SCISSOR_TL_0_Y(maxy - 1));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_WINDOW_SCISSOR_TL, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_WINDOW_SCISSOR_TL_X(0) |
                  A5XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(0));
   tu_cs_emit(cs, A5XX_GRAS_SC_WINDOW_SCISSOR_BR_X(width - 1) |
                  A5XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(height - 1));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RESOLVE_CNTL_1, 2);
   tu_cs_emit(cs, A5XX_RB_RESOLVE_CNTL_1_X(0) |
                  A5XX_RB_RESOLVE_CNTL_1_Y(0));
   tu_cs_emit(cs, A5XX_RB_RESOLVE_CNTL_2_X(width - 1) |
                  A5XX_RB_RESOLVE_CNTL_2_Y(height - 1));
   tu5_emit_reg(cs, REG_A5XX_RB_WINDOW_OFFSET,
                A5XX_RB_WINDOW_OFFSET_X(0) | A5XX_RB_WINDOW_OFFSET_Y(0));
}

static uint32_t
tu5_depth_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_D16_UNORM:
      return A5EN_DEPTH5_16;
   case VK_FORMAT_X8_D24_UNORM_PACK32:
   case VK_FORMAT_D24_UNORM_S8_UINT:
      return A5EN_DEPTH5_24_8;
   case VK_FORMAT_D32_SFLOAT:
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return A5EN_DEPTH5_32;
   default:
      return A5EN_DEPTH5_NONE;
   }
}

static bool
tu5_format_has_stencil(VkFormat format)
{
   return format == VK_FORMAT_D24_UNORM_S8_UINT ||
          format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
          format == VK_FORMAT_S8_UINT;
}

static uint32_t
tu5_stencil_face(const struct vk_stencil_test_face_state *face)
{
   return A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE |
          A5XX_RB_STENCIL_CONTROL_STENCIL_READ |
          A5XX_RB_STENCIL_CONTROL_FUNC(face->op.compare) |
          A5XX_RB_STENCIL_CONTROL_FAIL(face->op.fail) |
          A5XX_RB_STENCIL_CONTROL_ZPASS(face->op.pass) |
          A5XX_RB_STENCIL_CONTROL_ZFAIL(face->op.depth_fail);
}

static uint32_t
tu5_stencil_refmask(const struct vk_stencil_test_face_state *face)
{
   return A5XX_RB_STENCILREFMASK_STENCILREF(face->reference) |
          A5XX_RB_STENCILREFMASK_STENCILMASK(face->compare_mask) |
          A5XX_RB_STENCILREFMASK_STENCILWRITEMASK(face->write_mask);
}

/* Values match enum adreno_rb_blend_factor (adreno_common.xml). The a5xx
 * enum itself is not included here to avoid clashes with the a6xx headers.
 */
static uint32_t
tu5_blend_factor(VkBlendFactor factor)
{
   switch (factor) {
   case VK_BLEND_FACTOR_ZERO: return 0;
   case VK_BLEND_FACTOR_ONE: return 1;
   case VK_BLEND_FACTOR_SRC_COLOR: return 4;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 5;
   case VK_BLEND_FACTOR_SRC_ALPHA: return 6;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 7;
   case VK_BLEND_FACTOR_DST_COLOR: return 8;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return 9;
   case VK_BLEND_FACTOR_DST_ALPHA: return 10;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return 11;
   case VK_BLEND_FACTOR_CONSTANT_COLOR: return 12;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return 13;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA: return 14;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return 15;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return 16;
   case VK_BLEND_FACTOR_SRC1_COLOR: return 20;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return 21;
   case VK_BLEND_FACTOR_SRC1_ALPHA: return 22;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return 23;
   default: return 0;
   }
}

/* Values match enum a3xx_rb_blend_opcode. */
static uint32_t
tu5_blend_op(VkBlendOp op)
{
   switch (op) {
   case VK_BLEND_OP_ADD: return 0;              /* DST_PLUS_SRC */
   case VK_BLEND_OP_SUBTRACT: return 1;         /* SRC_MINUS_DST */
   case VK_BLEND_OP_REVERSE_SUBTRACT: return 2; /* DST_MINUS_SRC */
   case VK_BLEND_OP_MIN: return 3;
   case VK_BLEND_OP_MAX: return 4;
   default: return 0;
   }
}

static void
tu5_emit_zs_disabled(struct tu_cs *cs)
{
   tu5_emit_reg(cs, REG_A5XX_RB_DEPTH_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_RB_DEPTH_PLANE_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL, 0);
   tu5_emit_reg(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO,
                A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(A5EN_DEPTH5_NONE));
   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO,
                A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(A5EN_DEPTH5_NONE));
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu5_emit_reg(cs, REG_A5XX_RB_STENCIL_INFO, 0);
   tu5_emit_reg(cs, REG_A5XX_RB_STENCIL_CONTROL, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_BUFFER_BASE, 3);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, A5XX_GRAS_LRZ_BUFFER_PITCH(0));
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_LRZ_CNTL, 0);
}

/* Depth/stencil images are always tiled on a5xx (the hardware does not
 * support linear D/S), and RB_DEPTH_BUFFER_INFO has no tile-mode field, so
 * the implicit tiled layout is what the hardware expects here.
 */
static void
tu5_emit_zs(struct tu_cmd_buffer *cmd,
            const struct ir3_shader_variant *fs)
{
   struct tu_cs *cs = &cmd->cs;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;
   const struct tu_subpass *subpass = cmd->state.subpass;
   const struct tu_image_view *iview = NULL;

   if (subpass &&
       subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED) {
      iview = cmd->state.attachments[
         subpass->depth_stencil_attachment.attachment];
   }

   if (!iview) {
      tu5_emit_zs_disabled(cs);
      return;
   }

   VkFormat format = iview->vk.format;
   uint32_t depth_format = tu5_depth_format(format);
   const struct fdl6_view *dview =
      iview->view_depth ? iview->view_depth : &iview->view;
   bool has_stencil = tu5_format_has_stencil(format);
   bool separate_stencil =
      format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
      format == VK_FORMAT_S8_UINT;
   const struct fdl6_view *sview =
      has_stencil ? (iview->view_stencil ? iview->view_stencil : &iview->view)
                  : NULL;

   if (tu5_gfx_debug()) {
      fprintf(stderr,
              "  zs: fmt=%d depth_fmt=%u base=%" PRIx64 " pitch=%u layer=%u"
              " stencil=%d sep=%d test=%d write=%d cmp=%u\n",
              format, depth_format, dview->base_addr, dview->pitch,
              dview->layer_size, has_stencil, separate_stencil,
              dyn->ds.depth.test_enable, dyn->ds.depth.write_enable,
              dyn->ds.depth.compare_op);
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_BUFFER_INFO, 5);
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(depth_format));
   tu_cs_emit_qw(cs, dview->base_addr);
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_PITCH(dview->pitch));
   tu_cs_emit(cs, A5XX_RB_DEPTH_BUFFER_ARRAY_PITCH(dview->layer_size));

   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_DEPTH_BUFFER_INFO,
                A5XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(depth_format));

   tu_cs_emit_pkt4(cs, REG_A5XX_RB_DEPTH_FLAG_BUFFER_BASE, 3);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_BUFFER_BASE, 3);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, A5XX_GRAS_LRZ_BUFFER_PITCH(0));
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_LRZ_FAST_CLEAR_BUFFER_BASE, 2);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu5_emit_reg(cs, REG_A5XX_GRAS_LRZ_CNTL, 0);

   if (sview) {
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCIL_INFO, 5);
      tu_cs_emit(cs, separate_stencil ?
                   A5XX_RB_STENCIL_INFO_SEPARATE_STENCIL : 0);
      tu_cs_emit_qw(cs, sview->base_addr);
      tu_cs_emit(cs, A5XX_RB_STENCIL_PITCH(sview->pitch));
      tu_cs_emit(cs, A5XX_RB_STENCIL_ARRAY_PITCH(sview->layer_size));
   } else {
      tu5_emit_reg(cs, REG_A5XX_RB_STENCIL_INFO, 0);
   }

   uint32_t depth_cntl =
      A5XX_RB_DEPTH_CNTL_ZFUNC(dyn->ds.depth.compare_op);
   if (!getenv("TU5_GFX_NODEPTH")) {
      if (dyn->ds.depth.test_enable)
         depth_cntl |= A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE |
                       A5XX_RB_DEPTH_CNTL_Z_READ_ENABLE;
      if (dyn->ds.depth.write_enable)
         depth_cntl |= A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE;
   }
   tu5_emit_reg(cs, REG_A5XX_RB_DEPTH_CNTL, depth_cntl);

   bool fragz = fs->writes_pos;
   tu5_emit_reg(cs, REG_A5XX_RB_DEPTH_PLANE_CNTL,
                COND(fragz, A5XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));
   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL,
                COND(fragz, A5XX_GRAS_SU_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));

   if (dyn->ds.stencil.test_enable && sview) {
      tu5_emit_reg(cs, REG_A5XX_RB_STENCIL_CONTROL,
         tu5_stencil_face(&dyn->ds.stencil.front) |
         A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF |
         A5XX_RB_STENCIL_CONTROL_FUNC_BF(
            dyn->ds.stencil.back.op.compare) |
         A5XX_RB_STENCIL_CONTROL_FAIL_BF(
            dyn->ds.stencil.back.op.fail) |
         A5XX_RB_STENCIL_CONTROL_ZPASS_BF(
            dyn->ds.stencil.back.op.pass) |
         A5XX_RB_STENCIL_CONTROL_ZFAIL_BF(
            dyn->ds.stencil.back.op.depth_fail));
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
      tu_cs_emit(cs, tu5_stencil_refmask(&dyn->ds.stencil.front));
      tu_cs_emit(cs, tu5_stencil_refmask(&dyn->ds.stencil.back));
   } else {
      tu5_emit_reg(cs, REG_A5XX_RB_STENCIL_CONTROL, 0);
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_STENCILREFMASK, 2);
      tu_cs_emit(cs, 0);
      tu_cs_emit(cs, 0);
   }
}

static void
tu5_emit_rasterizer(struct tu_cmd_buffer *cmd,
                    const struct tu_shader *vs_shader,
                    const struct tu_shader *fs_shader,
                    uint8_t max_loc)
{
   struct tu_cs *cs = &cmd->cs;
   const struct ir3_shader_variant *vs = vs_shader->variant;
   const struct ir3_shader_variant *fs = fs_shader->variant;
   const struct vk_dynamic_graphics_state *dyn =
      &cmd->vk.dynamic_graphics_state;

   uint32_t gras_su_cntl = A5XX_GRAS_SU_CNTL_LINEHALFWIDTH(0.5f);
   if (dyn->rs.cull_mode & VK_CULL_MODE_FRONT_BIT)
      gras_su_cntl |= A5XX_GRAS_SU_CNTL_CULL_FRONT;
   if (dyn->rs.cull_mode & VK_CULL_MODE_BACK_BIT)
      gras_su_cntl |= A5XX_GRAS_SU_CNTL_CULL_BACK;
   if (dyn->rs.front_face == VK_FRONT_FACE_CLOCKWISE)
      gras_su_cntl |= A5XX_GRAS_SU_CNTL_FRONT_CW;
   gras_su_cntl |= A5XX_GRAS_SU_CNTL_LINE_MODE(BRESENHAM);

   tu5_emit_reg(cs, REG_A5XX_GRAS_SU_CNTL, gras_su_cntl);
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_POINT_MINMAX, 2);
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_MINMAX_MIN(1.0f) |
                  A5XX_GRAS_SU_POINT_MINMAX_MAX(1.0f));
   tu_cs_emit(cs, A5XX_GRAS_SU_POINT_SIZE(1.0f));

   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SU_POLY_OFFSET_SCALE, 3);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);
   tu_cs_emit(cs, 0);

   /* POLYMODE describes how polygons are filled (polygonMode), not the
    * primitive topology - the draw packet carries the topology.
    */
   uint32_t ptype = PC_DRAW_TRIANGLES;
   switch (dyn->rs.polygon_mode) {
   case VK_POLYGON_MODE_LINE:
      ptype = PC_DRAW_LINES;
      break;
   case VK_POLYGON_MODE_POINT:
      ptype = PC_DRAW_POINTS;
      break;
   default:
      break;
   }
   tu5_emit_reg(cs, REG_A5XX_PC_RASTER_CNTL,
      A5XX_PC_RASTER_CNTL_POLYMODE_FRONT_PTYPE(ptype) |
      A5XX_PC_RASTER_CNTL_POLYMODE_BACK_PTYPE(ptype) |
      COND(dyn->rs.polygon_mode != VK_POLYGON_MODE_FILL,
           A5XX_PC_RASTER_CNTL_POLYMODE_ENABLE));

   tu5_emit_reg(cs, REG_A5XX_PC_PRIMITIVE_CNTL,
      A5XX_PC_PRIMITIVE_CNTL_PROVOKING_VTX_LAST |
      A5XX_PC_PRIMITIVE_CNTL_STRIDE_IN_VPC(max_loc));

   /* fd5 disables guardband clipping for points and lines: the clip codes
    * of a point whose centre is outside the guardband would otherwise cull
    * it entirely, even when the point itself overlaps the viewport.
    */
   bool point_or_line =
      dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
      dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
      dyn->ia.primitive_topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
   tu5_emit_reg(cs, REG_A5XX_GRAS_CL_CNTL,
                COND(point_or_line, A5XX_GRAS_CL_CNTL_VP_CLIP_CODE_IGNORE));
   tu5_emit_reg(cs, REG_A5XX_GRAS_CL_GUARDBAND_CLIP_ADJ, 0);

   tu5_emit_reg(cs, REG_A5XX_RB_ALPHA_CONTROL, 0);
   tu5_emit_zs(cmd, fs);

   uint32_t posz_regid = ir3_find_output_regid(fs, FRAG_RESULT_DEPTH);
   uint32_t smask_regid = ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK);
   bool writes_smask = fs->writes_smask;
   if (!writes_smask)
      smask_regid = regid(63, 0);

   unsigned nr = MIN2(cmd->state.subpass->color_count, 8u);

   tu5_emit_reg(cs, REG_A5XX_RB_FS_OUTPUT_CNTL,
      A5XX_RB_FS_OUTPUT_CNTL_MRT(nr) |
      COND(fs->writes_pos, A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_Z) |
      COND(writes_smask, A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_SAMPMASK));
   tu5_emit_reg(cs, REG_A5XX_SP_FS_OUTPUT_CNTL,
      A5XX_SP_FS_OUTPUT_CNTL_MRT(nr) |
      A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(posz_regid) |
      A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(smask_regid));



   uint32_t blend_mask = 0;

   for (unsigned i = 0; i < 8; i++) {
      const struct vk_color_blend_attachment_state *cba =
         &dyn->cb.attachments[i];
      bool used = i < nr;
      uint32_t mrt_control = used ?
         A5XX_RB_MRT_CONTROL_ROP_CODE(ROP_COPY) |
         A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(cba->write_mask) : 0;
      uint32_t blend_control = 0;

      if (used && cba->blend_enable) {
         blend_control =
            A5XX_RB_MRT_BLEND_CONTROL_RGB_SRC_FACTOR(
               tu5_blend_factor((VkBlendFactor)cba->src_color_blend_factor)) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_BLEND_OPCODE(
               tu5_blend_op(cba->color_blend_op)) |
            A5XX_RB_MRT_BLEND_CONTROL_RGB_DEST_FACTOR(
               tu5_blend_factor((VkBlendFactor)cba->dst_color_blend_factor)) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_SRC_FACTOR(
               tu5_blend_factor((VkBlendFactor)cba->src_alpha_blend_factor)) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_BLEND_OPCODE(
               tu5_blend_op(cba->alpha_blend_op)) |
            A5XX_RB_MRT_BLEND_CONTROL_ALPHA_DEST_FACTOR(
               tu5_blend_factor((VkBlendFactor)cba->dst_alpha_blend_factor));
         mrt_control |= A5XX_RB_MRT_CONTROL_BLEND |
                        A5XX_RB_MRT_CONTROL_BLEND2;
         blend_mask |= 1u << i;
      }

      tu5_emit_reg(cs, REG_A5XX_RB_MRT_CONTROL(i), mrt_control);
      tu5_emit_reg(cs, REG_A5XX_RB_MRT_BLEND_CONTROL(i), blend_control);
   }

   /* A zero sample mask means no covered samples, so the FS would never be
    * launched. Default to all 16 samples like the blob does.
    */
   uint32_t sample_mask = dyn->ms.sample_mask & 0xffff;
   if (!sample_mask)
      sample_mask = 0xffff;
   tu5_emit_reg(cs, REG_A5XX_SP_BLEND_CNTL,
                A5XX_SP_BLEND_CNTL_ENABLE_BLEND(blend_mask) |
                COND(blend_mask != 0, A5XX_SP_BLEND_CNTL_UNK8));
   tu5_emit_reg(cs, REG_A5XX_RB_BLEND_CNTL,
                A5XX_RB_BLEND_CNTL_ENABLE_BLEND(blend_mask) |
                A5XX_RB_BLEND_CNTL_SAMPLE_MASK(sample_mask));

   /* Blend constants: each channel has a float16/uint/sint word followed by
    * a float32 word. Reuse the RF/BL/AL register names for the second word,
    * exactly like fd5 does.
    */
   for (unsigned i = 0; i < 4; i++) {
      float c = dyn->cb.blend_constants[i];
      uint32_t packed =
         (i == 0 ? A5XX_RB_BLEND_RED_FLOAT(c) :
          i == 1 ? A5XX_RB_BLEND_GREEN_FLOAT(c) :
          i == 2 ? A5XX_RB_BLEND_BLUE_FLOAT(c) :
                   A5XX_RB_BLEND_ALPHA_FLOAT(c)) |
         (i == 0 ? A5XX_RB_BLEND_RED_UINT((uint32_t)(CLAMP(c, 0.f, 1.f) * 0xff)) :
          i == 1 ? A5XX_RB_BLEND_GREEN_UINT((uint32_t)(CLAMP(c, 0.f, 1.f) * 0xff)) :
          i == 2 ? A5XX_RB_BLEND_BLUE_UINT((uint32_t)(CLAMP(c, 0.f, 1.f) * 0xff)) :
                   A5XX_RB_BLEND_ALPHA_UINT((uint32_t)(CLAMP(c, 0.f, 1.f) * 0xff))) |
         (i == 0 ? A5XX_RB_BLEND_RED_SINT((uint32_t)(CLAMP(c, -1.f, 1.f) * 0x7f)) :
          i == 1 ? A5XX_RB_BLEND_GREEN_SINT((uint32_t)(CLAMP(c, -1.f, 1.f) * 0x7f)) :
          i == 2 ? A5XX_RB_BLEND_BLUE_SINT((uint32_t)(CLAMP(c, -1.f, 1.f) * 0x7f)) :
                   A5XX_RB_BLEND_ALPHA_SINT((uint32_t)(CLAMP(c, -1.f, 1.f) * 0x7f)));
      tu_cs_emit_pkt4(cs, REG_A5XX_RB_BLEND_RED + 2 * i, 2);
      tu_cs_emit(cs, packed);
      tu_cs_emit(cs, fui(c));
   }
}

static void
tu5_dump_cs(struct tu_cmd_buffer *cmd)
{
   struct tu_cs *cs = &cmd->cs;
   FILE *f = fopen("/tmp/tu5_cs.txt", "w");
   if (!f)
      return;

   uint32_t idx = 0;
   for (uint32_t e = 0; e < cs->entry_count; e++) {
      const struct tu_cs_entry *entry = &cs->entries[e];
      const uint32_t *p = (const uint32_t *)entry->bo->map + entry->offset / 4;
      for (uint32_t i = 0; i < entry->size / 4; i++)
         fprintf(f, "%u %08x\n", idx++, p[i]);
   }
   for (const uint32_t *p = cs->start; p < cs->cur; p++)
      fprintf(f, "%u %08x\n", idx++, *p);
   fclose(f);
}

void
tu5_draw(struct tu_cmd_buffer *cmd, const struct tu5_draw_info *info)
{
   struct tu_cs *cs = &cmd->cs;
   struct tu_shader *vs_shader = cmd->state.shaders[MESA_SHADER_VERTEX];
   struct tu_shader *fs_shader = cmd->state.shaders[MESA_SHADER_FRAGMENT];
   const struct tu_framebuffer *fb = cmd->state.framebuffer;

   if (!vs_shader || !fs_shader || !fb || !cmd->state.attachments ||
       !cmd->state.attachments[0])
      return;

   const struct tu_subpass *subpass = cmd->state.subpass;
   const struct tu_image_view *iviews[8] = {};
   unsigned nr = 0;

   for (unsigned i = 0; subpass && i < MIN2(subpass->color_count, 8u); i++) {
      uint32_t att = subpass->color_attachments[i].attachment;
      if (att == VK_ATTACHMENT_UNUSED)
         continue;
      iviews[i] = cmd->state.attachments[att];
      if (iviews[i])
         nr = i + 1;
   }

   if (!nr)
      return;

   const struct tu_image_view *iview = iviews[0];
   if (tu5_gfx_debug()) {
      const struct ir3_shader_variant *vs_dbg = vs_shader->variant;
      const struct ir3_shader_variant *fs_dbg = fs_shader->variant;
      fprintf(stderr, "tu5 draw: %s count=%u instances=%u fb=%ux%u base=%" PRIx64
              " pitch=%u format=%d\n",
              info->indexed ? "indexed" : "array",
              info->indexed ? info->index_count : info->vertex_count,
              info->instance_count, fb->width, fb->height,
              iview->view.base_addr, iview->view.pitch,
              iview->vk.format);
      fprintf(stderr, "  VS: instrlen=%u constlen=%u sizedwords=%u pos_regid=%x vtxid_regid=%x\n",
              vs_dbg->instrlen, vs_dbg->constlen, vs_dbg->info.sizedwords,
              ir3_find_output_regid(vs_dbg, VARYING_SLOT_POS),
              ir3_find_sysval_regid(vs_dbg, SYSTEM_VALUE_VERTEX_ID_ZERO_BASE));
      fprintf(stderr, "  FS: instrlen=%u constlen=%u total_in=%u color0_regid=%x color_regid=%x\n",
              fs_dbg->instrlen, fs_dbg->constlen, fs_dbg->total_in,
              ir3_find_output_regid(fs_dbg, FRAG_RESULT_DATA0),
              ir3_find_output_regid(fs_dbg, FRAG_RESULT_COLOR));
      for (unsigned k = 0; k < fs_dbg->inputs_count; k++)
         fprintf(stderr, "    fs_in[%u]: slot=%u compmask=0x%x inloc=%u"
                         " flat=%d bary=%d\n", k, fs_dbg->inputs[k].slot,
                 fs_dbg->inputs[k].compmask, fs_dbg->inputs[k].inloc,
                 fs_dbg->inputs[k].flat, fs_dbg->inputs[k].bary);
   }

   struct ir3_shader_linkage linkage = {};
   ir3_link_shaders(&linkage, vs_shader->variant, fs_shader->variant, true);

   const struct ir3_shader_variant *vs = vs_shader->variant;
   uint32_t pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
   uint32_t psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
   uint8_t pos_loc = linkage.max_loc;
   uint8_t psize_loc = 0xff;
   if (pos_regid != INVALID_REG)
      ir3_link_add(&linkage, VARYING_SLOT_POS, pos_regid, 0xf, linkage.max_loc);
   if (psize_regid != INVALID_REG) {
      psize_loc = linkage.max_loc;
      ir3_link_add(&linkage, VARYING_SLOT_PSIZ, psize_regid, 0x1,
                   linkage.max_loc);
   }

   uint32_t num_sp_cores = cmd->device->physical_device->info->num_sp_cores;
   tu5_emit_global_state(cmd);
   if (!tu5_gfx_no_invalidate()) {
      tu5_event_write(cs, PC_CCU_INVALIDATE_COLOR);
      if (tu5_has_depth_attachment(cmd))
         tu5_event_write(cs, PC_CCU_INVALIDATE_DEPTH);
      tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);
   }
   tu5_emit_reg(cs, REG_A5XX_PC_POWER_CNTL, num_sp_cores - 1);
   tu5_emit_reg(cs, REG_A5XX_VFD_POWER_CNTL, num_sp_cores - 1);
   tu5_emit_reg(cs, REG_A5XX_RB_CCU_CNTL,
                A5XX_RB_CCU_CNTL_COLOR_OFFSET(32 * num_sp_cores));
   tu5_emit_reg(cs, REG_A5XX_RB_CNTL,
                A5XX_RB_CNTL_WIDTH(0) | A5XX_RB_CNTL_HEIGHT(0) |
                A5XX_RB_CNTL_BYPASS);

   tu_cs_emit_pkt4(cs, REG_A5XX_TPL1_TP_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_TPL1_TP_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   tu_cs_emit(cs, A5XX_TPL1_TP_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                  A5XX_TPL1_TP_DEST_MSAA_CNTL_MSAA_DISABLE);
   tu_cs_emit_pkt4(cs, REG_A5XX_RB_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_RB_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   tu_cs_emit(cs, A5XX_RB_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                  A5XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE);
   tu_cs_emit_pkt4(cs, REG_A5XX_GRAS_SC_RAS_MSAA_CNTL, 2);
   tu_cs_emit(cs, A5XX_GRAS_SC_RAS_MSAA_CNTL_SAMPLES(MSAA_ONE));
   tu_cs_emit(cs, A5XX_GRAS_SC_DEST_MSAA_CNTL_SAMPLES(MSAA_ONE) |
                  A5XX_GRAS_SC_DEST_MSAA_CNTL_MSAA_DISABLE);

   tu5_emit_mrt(cmd, iviews, nr);
   if (tu5_gfx_clear())
      tu5_emit_clear(cmd, 0xff0000ffu);
   tu5_emit_viewport(cmd, fb->width, fb->height);
   if (tu5_gfx_debug()) {
      fprintf(stderr, "  link: cnt=%u max_loc=%u pos_loc=%u psize_loc=%u varmask=%08x %08x\n",
              linkage.cnt, linkage.max_loc, pos_loc, psize_loc,
              linkage.varmask[0], linkage.varmask[1]);
      for (unsigned k = 0; k < linkage.cnt; k++)
         fprintf(stderr, "    var[%u]: slot=%u regid=0x%x compmask=0x%x loc=%u\n",
                 k, linkage.var[k].slot, linkage.var[k].regid,
                 linkage.var[k].compmask, linkage.var[k].loc);
   }
   tu5_emit_program(cmd, vs_shader, fs_shader, &linkage, pos_loc, psize_loc);
   tu5_emit_vs_driver_params(cmd, vs_shader, info);
   tu5_emit_gfx_ssbo_state(cmd, vs_shader, SB4_VS_SHADER);
   tu5_emit_gfx_ssbo_state(cmd, fs_shader, SB4_FS_SHADER);
   tu5_emit_gfx_ubo_state(cmd, vs_shader, SB4_VS_SHADER);
   tu5_emit_gfx_ubo_state(cmd, fs_shader, SB4_FS_SHADER);
   tu5_emit_textures(cmd, vs_shader, SB4_VS_TEX, VK_PIPELINE_BIND_POINT_GRAPHICS);
   tu5_emit_textures(cmd, fs_shader, SB4_FS_TEX, VK_PIPELINE_BIND_POINT_GRAPHICS);
   tu5_emit_reg(cs, REG_A5XX_TPL1_VS_TEX_COUNT, vs_shader->num_textures);
   tu5_emit_reg(cs, REG_A5XX_TPL1_FS_TEX_COUNT, fs_shader->num_textures);
   tu5_emit_rasterizer(cmd, vs_shader, fs_shader, linkage.max_loc);
   tu5_emit_vertex_inputs(cmd, vs_shader);

   tu5_emit_reg(cs, REG_A5XX_VPC_SO_OVERRIDE, 0);
   if (!getenv("TU5_GFX_NOVIS")) {
      tu_cs_emit_pkt7(cs, CP_SET_VISIBILITY_OVERRIDE, 1);
      tu_cs_emit(cs, 0x1);
      tu_cs_emit_pkt7(cs, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
      tu_cs_emit(cs, 0x0);
   }

   enum pc_di_primtype primtype = DI_PT_TRILIST;
   uint32_t topology = cmd->vk.dynamic_graphics_state.ia.primitive_topology;
   switch (topology) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      primtype = DI_PT_POINTLIST; break;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      primtype = DI_PT_LINELIST; break;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      primtype = DI_PT_LINESTRIP; break;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      primtype = DI_PT_TRILIST; break;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      primtype = DI_PT_TRISTRIP; break;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      primtype = DI_PT_TRIFAN; break;
   default:
      break;
   }

   tu_cs_emit_pkt4(cs, REG_A5XX_VFD_INDEX_OFFSET, 2);
   tu_cs_emit(cs, info->indexed ? (uint32_t)info->vertex_offset
                                : info->first_vertex);
   tu_cs_emit(cs, info->first_instance);

   tu5_emit_reg(cs, REG_A5XX_PC_RESTART_INDEX, 0xffffffff);

   uint32_t rb_rc = getenv("TU5_RB_RC") ? strtoul(getenv("TU5_RB_RC"), NULL, 0) : 0x8;
   uint32_t gras_sc = getenv("TU5_GRAS_SC") ? strtoul(getenv("TU5_GRAS_SC"), NULL, 0) : 0x8;
   tu5_emit_reg(cs, REG_A5XX_RB_RENDER_CNTL, rb_rc);
   tu5_emit_reg(cs, REG_A5XX_GRAS_SC_CNTL, gras_sc);

   enum pc_di_vis_cull_mode vismode = getenv("TU5_GFX_VIS1") ?
      USE_VISIBILITY : IGNORE_VISIBILITY;
   uint32_t initiator = CP_DRAW_INDX_OFFSET_0_PRIM_TYPE(primtype) |
                        CP_DRAW_INDX_OFFSET_0_VIS_CULL(vismode);

   if (info->indexed) {
      if (getenv("TU5_GFX_DEBUG"))
         fprintf(stderr, "tu5: indexed draw count=%u first=%u voff=%d inst=%u va=%" PRIx64 " max=%u size=%u\n",
                 info->index_count, info->first_index, info->vertex_offset,
                 info->instance_count, cmd->state.index_va,
                 cmd->state.max_index_count, cmd->state.index_size);
      enum a4xx_index_size index_size;
      switch (cmd->state.index_size) {
      case 1: index_size = INDEX4_SIZE_8_BIT; break;
      case 2: index_size = INDEX4_SIZE_16_BIT; break;
      default: index_size = INDEX4_SIZE_32_BIT; break;
      }
      initiator |= CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_DMA) |
                   CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(index_size);

      tu_cs_emit_pkt7(cs, CP_DRAW_INDX_OFFSET, 7);
      tu_cs_emit(cs, initiator);
      tu_cs_emit(cs, info->instance_count);
      tu_cs_emit(cs, info->index_count);
      tu_cs_emit(cs, info->first_index);
      tu_cs_emit_qw(cs, cmd->state.index_va);
      tu_cs_emit(cs, cmd->state.max_index_count);
   } else {
      initiator |= CP_DRAW_INDX_OFFSET_0_SOURCE_SELECT(DI_SRC_SEL_AUTO_INDEX) |
                   CP_DRAW_INDX_OFFSET_0_INDEX_SIZE(INDEX4_SIZE_32_BIT);
      tu_cs_emit_pkt7(cs, CP_DRAW_INDX_OFFSET, 3);
      tu_cs_emit(cs, initiator);
      tu_cs_emit(cs, info->instance_count);
      tu_cs_emit(cs, info->vertex_count);
   }

   if (getenv("TU5_GFX_POSTCLEAR"))
      tu5_emit_clear(cmd, 0x00ff00ffu);
   if (!tu5_gfx_no_flush()) {
      tu5_ccu_flush_color(cmd);
      if (tu5_has_depth_attachment(cmd))
         tu5_ccu_flush_depth(cmd);
   }

   /* Flush UCHE/CCU writes so CPU readback sees them (same sequence as the
    * a5xx compute path).
    */
   tu_cs_emit_pkt7(cs, CP_EVENT_WRITE, 4);
   tu_cs_emit(cs, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS));
   tu_cs_emit_qw(cs, global_iova(cmd, seqno_dummy));
   tu_cs_emit(cs, 0);
   tu_cs_emit_pkt7(cs, CP_WAIT_FOR_IDLE, 0);

   if (getenv("TU5_GFX_DUMPCS"))
      tu5_dump_cs(cmd);
}

/* a5xx hardware index fetch through CP_DRAW_INDX_OFFSET(DMA) does not work in
 * our sysmem/bypass path. Expand the indexed draw on the CPU: build a small
 * temporary vertex buffer with vertices in index order and issue a regular
 * non-indexed draw. This is enough for triangle-list style content (vkcube).
 */

void
tu5_draw_indexed(struct tu_cmd_buffer *cmd, const struct tu5_draw_info *info)
{
   struct tu5_draw_info local = *info;
   info = &local;

   if (!info->indexed || !cmd->state.index_buffer || info->instance_count != 1) {
      tu5_draw(cmd, info);
      return;
   }

   struct tu_buffer *ibuf = cmd->state.index_buffer;
   if (tu_bo_map(cmd->device, ibuf->bo, NULL) != VK_SUCCESS) {
      tu5_draw(cmd, info);
      return;
   }

   unsigned index_bytes;
   switch (cmd->state.index_size) {
   case INDEX4_SIZE_8_BIT:
      index_bytes = 1;
      break;
   case INDEX4_SIZE_16_BIT:
      index_bytes = 2;
      break;
   case INDEX4_SIZE_32_BIT:
      index_bytes = 4;
      break;
   default:
      tu5_draw(cmd, info);
      return;
   }

   if (cmd->state.index_va < ibuf->bo->iova ||
       cmd->state.index_va - ibuf->bo->iova >= ibuf->bo->size) {
      tu5_draw(cmd, info);
      return;
   }

   const uint8_t *idx = (const uint8_t *)ibuf->bo->map +
      (cmd->state.index_va - ibuf->bo->iova) +
      (size_t)info->first_index * index_bytes;

   struct tu_cs_memory tmp[MAX_VBS] = {};
   bool any = false;

   for (unsigned b = 0; b < cmd->state.max_vbs_bound; b++) {
      struct tu_buffer *vb = cmd->state.vb_buffer[b];
      if (!vb || !cmd->state.vb[b].size)
         continue;

      unsigned stride = cmd->state.vi.bindings[b].stride;
      if (!stride)
         continue;

      if (tu_bo_map(cmd->device, vb->bo, NULL) != VK_SUCCESS) {
         tu5_draw(cmd, info);
         return;
      }

      uint64_t size = (uint64_t)info->index_count * stride;
      if (!size || size > 4 * 1024 * 1024) {
         tu5_draw(cmd, info);
         return;
      }

      if (tu_cs_alloc(&cmd->sub_cs, size, 16, &tmp[b]) != VK_SUCCESS) {
         tu5_draw(cmd, info);
         return;
      }

      uint64_t vb_off = cmd->state.vb[b].base - vb->bo->iova;
      const uint8_t *src = (const uint8_t *)vb->bo->map + vb_off;

      for (unsigned i = 0; i < info->index_count; i++) {
         uint32_t index;
         if (index_bytes == 1)
            index = idx[i];
         else if (index_bytes == 2)
            index = ((const uint16_t *)idx)[i];
         else
            index = ((const uint32_t *)idx)[i];

         index += info->vertex_offset;
         if ((uint64_t)index * stride + stride > cmd->state.vb[b].size)
            continue;

         memcpy((uint8_t *)tmp[b].map + (uint64_t)i * stride,
                src + (uint64_t)index * stride, stride);
      }

      any = true;
   }

   if (!any) {
      if (getenv("TU5_GFX_DEBUG"))
         fprintf(stderr, "tu5 indexed: no expandable bindings (count=%u max_vbs=%u)\n",
                 info->index_count, cmd->state.max_vbs_bound);
      tu5_draw(cmd, info);
      return;
   }

   if (getenv("TU5_GFX_DEBUG")) {
      fprintf(stderr, "tu5 indexed expand: count=%u max_vbs=%u\n",
              info->index_count, cmd->state.max_vbs_bound);
      for (unsigned b = 0; b < cmd->state.max_vbs_bound; b++) {
         if (tmp[b].map)
            fprintf(stderr, "  binding %u: stride=%u size=%llu -> tmp %llx\n",
                    b, cmd->state.vi.bindings[b].stride,
                    (unsigned long long)cmd->state.vb[b].size,
                    (unsigned long long)tmp[b].iova);
      }
   }

   uint64_t old_base[MAX_VBS] = {};
   uint32_t old_size[MAX_VBS] = {};

   for (unsigned b = 0; b < cmd->state.max_vbs_bound; b++) {
      if (!tmp[b].map)
         continue;
      old_base[b] = cmd->state.vb[b].base;
      old_size[b] = cmd->state.vb[b].size;
      cmd->state.vb[b].base = tmp[b].iova;
      cmd->state.vb[b].size =
         (uint64_t)info->index_count * cmd->state.vi.bindings[b].stride;
   }

   struct tu5_draw_info nonindexed = *info;
   nonindexed.indexed = false;
   nonindexed.vertex_count = info->index_count;
   nonindexed.first_vertex = 0;
   nonindexed.vertex_offset = 0;
   tu5_draw(cmd, &nonindexed);

   for (unsigned b = 0; b < cmd->state.max_vbs_bound; b++) {
      if (!tmp[b].map)
         continue;
      cmd->state.vb[b].base = old_base[b];
      cmd->state.vb[b].size = old_size[b];
   }
}
