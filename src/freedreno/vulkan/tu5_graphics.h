/*
 * Copyright © 2026 Mesa contributors
 * SPDX-License-Identifier: MIT
 *
 * Isolated a5xx graphics path. Everything is emitted at draw time into the
 * main command stream; no A6xx draw states are involved.
 */

#ifndef TU5_GRAPHICS_H
#define TU5_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

#include "tu_common.h"

struct tu_cmd_buffer;
struct tu_shader;

struct tu5_draw_info {
   bool indexed;
   uint32_t vertex_count;
   uint32_t instance_count;
   uint32_t first_vertex;
   uint32_t first_instance;
   uint32_t index_count;
   uint32_t first_index;
   int32_t vertex_offset;
};

void tu5_draw(struct tu_cmd_buffer *cmd, const struct tu5_draw_info *info);
void tu5_draw_indexed(struct tu_cmd_buffer *cmd, const struct tu5_draw_info *info);

/* Apply the render-pass loadOp=CLEAR for color attachments. a5xx sysmem
 * rendering has no GMEM, so this is done with a linear memory fill.
 */
void tu5_clear_attachments(struct tu_cmd_buffer *cmd);

/* Convert an A6xx TEX_CONST descriptor (16 dwords) into the A5xx 12-dword
 * layout used by texture/image state.
 */
void tu5_texture_descriptor(const uint32_t *src, uint32_t *dst);

/* Emit sampler and texture state.  Shared between the graphics path
 * (SB4_VS_TEX/SB4_FS_TEX) and the compute path (SB4_CS_TEX); the descriptor
 * state depends on the bind point.
 */
void tu5_emit_textures(struct tu_cmd_buffer *cmd, struct tu_shader *shader,
                       enum a4xx_state_block sb, VkPipelineBindPoint bind_point);

#endif /* TU5_GRAPHICS_H */
