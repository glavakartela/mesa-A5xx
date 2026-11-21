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

struct tu_cmd_buffer;

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

/* Apply the render-pass loadOp=CLEAR for color attachments. a5xx sysmem
 * rendering has no GMEM, so this is done with a linear memory fill.
 */
void tu5_clear_attachments(struct tu_cmd_buffer *cmd);

#endif /* TU5_GRAPHICS_H */
