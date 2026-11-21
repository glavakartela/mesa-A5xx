/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 * Subset of the a5xx register definitions needed by the isolated compute
 * path. These values are copied from the generated a5xx.xml.h; the generated
 * header cannot be included here because its enumerations (perf counters,
 * workitem raster order, ...) collide with the a6xx headers that Turnip
 * already includes.
 *
 * Keep in sync with src/freedreno/registers/adreno/a5xx.xml.
 */

#ifndef TU5_REGS_H
#define TU5_REGS_H

#include <assert.h>
#include <stdint.h>

enum tu5_threadsize {
   TU5_TWO_QUADS = 0,
   TU5_FOUR_QUADS = 1,
};

#define TU5_REG_SP_SP_CNTL                  0x0000e580
#define TU5_REG_SP_CS_CONFIG                0x0000e589
#define TU5_REG_SP_CS_CTRL_REG0             0x0000e5f0
#define TU5_REG_SP_CS_OBJ_START             0x0000e5f3
#define TU5_REG_SP_CS_PVT_MEM_PARAM         0x0000e5f5
#define TU5_REG_SP_CS_PVT_MEM_ADDR          0x0000e5f6
#define TU5_REG_SP_CS_PVT_MEM_SIZE          0x0000e5f8

#define TU5_REG_HLSQ_CONTROL_0_REG          0x0000e784
#define TU5_REG_HLSQ_UPDATE_CNTL            0x0000e78a
#define TU5_REG_HLSQ_CS_CONFIG              0x0000e790
#define TU5_REG_HLSQ_CS_CNTL                0x0000e796
#define TU5_REG_HLSQ_CS_NDRANGE_0           0x0000e7b0
#define TU5_REG_HLSQ_CS_NDRANGE_1           0x0000e7b1
#define TU5_REG_HLSQ_CS_NDRANGE_2           0x0000e7b2
#define TU5_REG_HLSQ_CS_NDRANGE_3           0x0000e7b3
#define TU5_REG_HLSQ_CS_NDRANGE_4           0x0000e7b4
#define TU5_REG_HLSQ_CS_NDRANGE_5           0x0000e7b5
#define TU5_REG_HLSQ_CS_NDRANGE_6           0x0000e7b6
#define TU5_REG_HLSQ_CS_CNTL_0              0x0000e7b7
#define TU5_REG_HLSQ_CS_CNTL_1              0x0000e7b8
#define TU5_REG_HLSQ_CS_KERNEL_GROUP_X      0x0000e7b9
#define TU5_REG_HLSQ_CS_CONSTLEN            0x0000e7dc

#define TU5_SP_CS_CONFIG_ENABLED                       0x00000001
#define TU5_SP_CS_CONFIG_CONSTOBJECTOFFSET(val)        (((val) << 1) & 0x000000fe)
#define TU5_SP_CS_CONFIG_SHADEROBJOFFSET(val)          (((val) << 8) & 0x00007f00)

#define TU5_SP_CS_CTRL_REG0_BUFFER                     0x00000004
#define TU5_SP_CS_CTRL_REG0_THREADSIZE(val)            (((val) << 3) & 0x00000008)
#define TU5_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(val)      (((val) << 4) & 0x000003f0)
#define TU5_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(val)      (((val) << 10) & 0x0000fc00)
#define TU5_SP_CS_CTRL_REG0_BRANCHSTACK(val)           (((val) << 24) & 0xff000000)

static inline uint32_t
tu5_sp_cs_pvt_mem_param_memsizeperitem(uint32_t val)
{
   assert(!(val & 0x1ff));
   return (val >> 9) & 0x000000ff;
}

static inline uint32_t
tu5_sp_cs_pvt_mem_param_hwstackoffset(uint32_t val)
{
   assert(!(val & 0x7ff));
   return ((val >> 11) << 8) & 0x00ffff00;
}

static inline uint32_t
tu5_sp_cs_pvt_mem_size_totalpvtmemsize(uint32_t val)
{
   assert(!(val & 0xfff));
   return (val >> 12) & 0x0003ffff;
}

#define TU5_HLSQ_CONTROL_0_REG_FSTHREADSIZE(val)       (((val) << 0) & 0x00000001)
#define TU5_HLSQ_CONTROL_0_REG_CSTHREADSIZE(val)       (((val) << 2) & 0x00000004)

#define TU5_HLSQ_CS_CONFIG_ENABLED                     0x00000001
#define TU5_HLSQ_CS_CONFIG_CONSTOBJECTOFFSET(val)      (((val) << 1) & 0x000000fe)
#define TU5_HLSQ_CS_CONFIG_SHADEROBJOFFSET(val)        (((val) << 8) & 0x00007f00)

#define TU5_HLSQ_CS_CNTL_SSBO_ENABLE                   0x00000001
#define TU5_HLSQ_CS_CNTL_INSTRLEN(val)                 (((val) << 1) & 0xfffffffe)

#define TU5_HLSQ_CS_NDRANGE_0_KERNELDIM(val)           (((val) << 0) & 0x00000003)
#define TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEX(val)          (((val) << 2) & 0x00000ffc)
#define TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEY(val)          (((val) << 12) & 0x003ff000)
#define TU5_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(val)          (((val) << 22) & 0xffc00000)
#define TU5_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(val)        (val)
#define TU5_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(val)        (val)
#define TU5_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(val)        (val)

#define TU5_HLSQ_CS_CNTL_0_WGIDCONSTID(val)            (((val) << 0) & 0x000000ff)
#define TU5_HLSQ_CS_CNTL_0_UNK0(val)                   (((val) << 8) & 0x0000ff00)
#define TU5_HLSQ_CS_CNTL_0_UNK1(val)                   (((val) << 16) & 0x00ff0000)
#define TU5_HLSQ_CS_CNTL_0_LOCALIDREGID(val)           (((val) << 24) & 0xff000000)

#define TU5_HLSQ_CS_CNTL_1_WORKITEMRASTORDER_LINEAR    0x00000000

#define TU5_SSBO_1_0_WIDTH(val)                        (((val) << 16) & 0xffff0000)
#define TU5_SSBO_1_1_HEIGHT(val)                       (((val) << 0) & 0x0000ffff)

#endif /* TU5_REGS_H */
