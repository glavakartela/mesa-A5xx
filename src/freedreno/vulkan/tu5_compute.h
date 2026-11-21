/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal, isolated compute path for a5xx (Adreno 5xx) GPUs.
 *
 * This is intentionally not wired into the A6XX+ template machinery: the
 * a5xx register set and command packets are different enough that reusing
 * the A6xx emitters would program the hardware incorrectly.
 */

#ifndef TU5_COMPUTE_H
#define TU5_COMPUTE_H

#include "tu_common.h"

struct ir3_shader_variant;
struct tu_cmd_buffer;
struct tu_dispatch_info;
struct tu_pvtmem_config;

/* Emit the A5xx compute program state (shader object + HLSQ/SP config). */
void
tu5_emit_cs_config(struct tu_cs *cs,
                   const struct ir3_shader_variant *v,
                   const struct tu_pvtmem_config *pvtmem,
                   uint64_t binary_iova);

/* Record an A5xx compute dispatch. Replaces the A6xx+ tu_dispatch(). */
void
tu5_dispatch(struct tu_cmd_buffer *cmd, const struct tu_dispatch_info *info);

#endif /* TU5_COMPUTE_H */
