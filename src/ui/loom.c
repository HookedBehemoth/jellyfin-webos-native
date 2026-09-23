#include "loom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

loom_rect loom_intersect(loom_rect a, loom_rect b)
{
    const float x0 = maxf(a.x, b.x);
    const float y0 = maxf(a.y, b.y);
    const float x1 = minf(a.x + a.w, b.x + b.w);
    const float y1 = minf(a.y + a.h, b.y + b.h);
    return (loom_rect){x0, y0, maxf(0, x1 - x0), maxf(0, y1 - y0)};
}

loom_rect loom_inset(loom_rect r, float n)
{
    return (loom_rect){r.x + n, r.y + n, maxf(0, r.w - n * 2), maxf(0, r.h - n * 2)};
}

void loom_init(loom_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }

void loom_destroy(loom_context *ctx)
{
    free(ctx->commands);
    memset(ctx, 0, sizeof(*ctx));
}

void loom_begin(loom_context *ctx, float width, float height)
{
    ctx->count = 0;
    ctx->mask = (loom_mask){0};
    ctx->viewport = (loom_rect){0, 0, width, height};
}

/* Returns NULL when the command would not be visible, so callers need no other check. */
static loom_command *append(loom_context *ctx, loom_rect rect, const loom_rect *clip)
{
    const loom_rect effective = loom_intersect(ctx->viewport, clip != NULL ? *clip : ctx->viewport);
    if (effective.w <= 0 || effective.h <= 0 || rect.w <= 0 || rect.h <= 0)
        return NULL;
    if (ctx->count == ctx->capacity) {
        const size_t capacity = ctx->capacity ? ctx->capacity * 2 : 256;
        loom_command *grown = realloc(ctx->commands, capacity * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        ctx->commands = grown;
        ctx->capacity = capacity;
    }
    loom_command *command = &ctx->commands[ctx->count++];
    memset(command, 0, sizeof(*command));
    command->rect = rect;
    command->clip = effective;
    command->mask = ctx->mask;
    return command;
}

void loom_fill(loom_context *ctx, loom_rect rect, const loom_rect *clip, const loom_color color,
               float radius)
{
    loom_command *c = append(ctx, rect, clip);
    if (c == NULL)
        return;
    c->kind = LOOM_RECTANGLE;
    memcpy(c->rectangle.color, color, sizeof(loom_color));
    c->rectangle.radius = radius;
}

void loom_stroke(loom_context *ctx, loom_rect rect, const loom_rect *clip, const loom_color color,
                 float width, float radius)
{
    loom_command *c = append(ctx, rect, clip);
    if (c == NULL)
        return;
    c->kind = LOOM_BORDER;
    memcpy(c->border.color, color, sizeof(loom_color));
    c->border.width = width;
    c->border.radius = radius;
}

void loom_label(loom_context *ctx, loom_rect rect, const loom_rect *clip, const char *contents,
                const loom_color color, float size)
{
    loom_command *c = append(ctx, rect, clip);
    if (c == NULL)
        return;
    c->kind = LOOM_TEXT;
    c->text.contents = contents;
    memcpy(c->text.color, color, sizeof(loom_color));
    c->text.size = size;
}

void loom_textured(loom_context *ctx, loom_rect rect, const loom_rect *clip, uint32_t texture,
                   const float uv[4], const loom_color tint, float radius)
{
    loom_command *c = append(ctx, rect, clip);
    if (c == NULL)
        return;
    c->kind = LOOM_IMAGE;
    memcpy(c->image.uv, uv, sizeof(c->image.uv));
    memcpy(c->image.tint, tint, sizeof(loom_color));
    c->image.radius = radius;
    c->image.texture = texture;
}

void loom_image(loom_context *ctx, loom_rect rect, const loom_rect *clip, const float uv[4],
                const loom_color tint, float radius)
{
    loom_textured(ctx, rect, clip, 0, uv, tint, radius);
}

void loom_fade(loom_context *ctx, loom_axis axis, float start, float end,
               float scroll, float max_scroll, float width) {
  const int low = axis == LOOM_VERTICAL ? LOOM_FADE_TOP : LOOM_FADE_LEFT;
  ctx->mask.edge[low] = start - width + minf(maxf(scroll, 0), width);
  ctx->mask.edge[low + 1] =
      end + width - minf(maxf(max_scroll - scroll, 0), width);
  ctx->mask.width[low] = ctx->mask.width[low + 1] = width;
}

/* ------------------------------------------------------------------ layout */

loom_stack loom_stack_init(loom_rect rect, loom_axis axis, float padding, float gap)
{
    const loom_rect inner = loom_inset(rect, padding);
    return (loom_stack){inner, axis, gap, axis == LOOM_HORIZONTAL ? inner.x : inner.y};
}

loom_rect loom_stack_take(loom_stack *stack, float extent)
{
    loom_rect out;
    if (stack->axis == LOOM_HORIZONTAL)
        out = (loom_rect){stack->cursor, stack->rect.y, extent, stack->rect.h};
    else
        out = (loom_rect){stack->rect.x, stack->cursor, stack->rect.w, extent};
    stack->cursor += extent + stack->gap;
    return out;
}

loom_virtual_list loom_virtual_list_init(loom_rect viewport, size_t count, float item_height,
                                         float requested_scroll)
{
    const float content = (float)count * item_height;
    const float max_scroll = maxf(0, content - viewport.h);
    float scroll = requested_scroll;
    if (scroll < 0)
        scroll = 0;
    if (scroll > max_scroll)
        scroll = max_scroll;
    const size_t first_candidate = (size_t)floorf(scroll / item_height);
    const size_t first = first_candidate < count ? first_candidate : count;
    const size_t visible = (size_t)ceilf(viewport.h / item_height);
    size_t last = first + visible + 1;
    if (last > count)
        last = count;
    return (loom_virtual_list){viewport, count, item_height, scroll, first, last};
}

loom_rect loom_virtual_list_item(const loom_virtual_list *list, size_t index)
{
    return (loom_rect){list->viewport.x,
                       list->viewport.y + (float)index * list->item_height - list->scroll,
                       list->viewport.w, list->item_height};
}

float loom_virtual_list_max_scroll(const loom_virtual_list *list)
{
    return maxf(0, (float)list->count * list->item_height - list->viewport.h);
}

float loom_virtual_list_reveal(const loom_virtual_list *list, size_t index,
                               float margin) {
  const float top = (float)index * list->item_height - margin;
  const float bottom = top + list->item_height + margin * 2;
  if (top < list->scroll)
    return top;
  if (bottom > list->scroll + list->viewport.h)
    return bottom - list->viewport.h;
  return list->scroll;
}
