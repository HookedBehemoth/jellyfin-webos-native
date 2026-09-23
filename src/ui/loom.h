/*
 * Small TV-facing subset of loom.
 *
 * This keeps a useful boundary: layout emits backend-neutral draw commands and the
 * renderer knows nothing about widgets. The TV does not need custom 3D commands,
 * clipboard, drag and drop, right-click state, or retained desktop-window machinery. An
 * image is a texture plus a UV rectangle, so art from one shared atlas stays in the same
 * instanced batch and art loaded at runtime costs only a binding change.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { float x, y, w, h; } loom_rect;
typedef uint8_t loom_color[4];

static inline bool loom_contains(loom_rect r, float px, float py)
{
    return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}
loom_rect loom_intersect(loom_rect a, loom_rect b);
loom_rect loom_inset(loom_rect r, float n);

typedef enum { LOOM_FADE_TOP, LOOM_FADE_BOTTOM, LOOM_FADE_LEFT, LOOM_FADE_RIGHT } loom_fade_edge;

typedef enum {
  LOOM_RECTANGLE,
  LOOM_BORDER,
  LOOM_TEXT,
  LOOM_IMAGE
} loom_command_kind;

/* Per edge, content is transparent at `edge` and reaches full opacity `width`
 * further in. A width of 0 leaves that edge alone. */
typedef struct {
  float edge[4];
  float width[4];
} loom_mask;

typedef struct {
    loom_rect rect;
    loom_rect clip;
    loom_mask mask;
    loom_command_kind kind;
    union {
        struct { loom_color color; float radius; } rectangle;
        struct { loom_color color; float width, radius; } border;
        /* A command holds the string, not a copy of it: whatever it points at must stay
         * alive until the frame has been rendered. Passing a struct *by value* to a draw
         * helper and labelling one of its fields makes the text point into that helper's
         * stack frame, which is gone by then - so those are passed by pointer. */
        struct { const char *contents; loom_color color; float size; } text;
        struct {
            float uv[4];
            loom_color tint;
            float radius;
            /* Which texture to sample. 0 means the renderer's default media texture;
             * anything else is a texture the application made, and each distinct one costs
             * a draw call, because this GPU has no bindless textures. */
            uint32_t texture;
            /* The texture's colour is already multiplied by its alpha. */
            bool premultiplied;
        } image;
    };
} loom_command;

/* One frame's command list. */
typedef struct {
    loom_command *commands;
    size_t count, capacity;
    loom_rect viewport;
    /* Applied to every command appended while it is set; save and restore it by
     * value. */
    loom_mask mask;
} loom_context;

void loom_init(loom_context *ctx);
void loom_destroy(loom_context *ctx);
void loom_begin(loom_context *ctx, float width, float height);

/* `clip` of NULL means the viewport. */
void loom_fill(loom_context *ctx, loom_rect rect, const loom_rect *clip, const loom_color color, float radius);
void loom_stroke(loom_context *ctx, loom_rect rect, const loom_rect *clip, const loom_color color, float width, float radius);
void loom_label(loom_context *ctx, loom_rect rect, const loom_rect *clip, const char *contents, const loom_color color, float size);
void loom_image(loom_context *ctx, loom_rect rect, const loom_rect *clip, const float uv[4], const loom_color tint, float radius);
/* Same, from a texture the application owns rather than the default one. */
void loom_textured(loom_context *ctx, loom_rect rect, const loom_rect *clip,
                   uint32_t texture, const float uv[4], const loom_color tint,
                   float radius);

/* A texture holding premultiplied colour, drawn without rounded corners. */
void loom_premultiplied(loom_context *ctx, loom_rect rect,
                        const loom_rect *clip, uint32_t texture,
                        const float uv[4], const loom_color tint);

/* Cursor layout: the retained loom flex machinery reduced to what a TV screen uses most,
 * ordered rows and columns with padding and a gap. */
typedef enum { LOOM_HORIZONTAL, LOOM_VERTICAL } loom_axis;

typedef struct {
    loom_rect rect;
    loom_axis axis;
    float gap;
    float cursor;
} loom_stack;

/* Fades whatever is drawn next out towards both ends of a list spanning [start,
 * end] on `axis`, so partly offscreen items recede into whatever is behind
 * them. Each end's fade grows with how far the list can still scroll that way,
 * up to `width`. */
void loom_fade(loom_context *ctx, loom_axis axis, float start, float end,
               float scroll, float max_scroll, float width);

loom_stack loom_stack_init(loom_rect rect, loom_axis axis, float padding, float gap);
loom_rect loom_stack_take(loom_stack *stack, float extent);

/* Geometry-only virtual list. Only [first, last) is declared and rendered; item
 * coordinates stay stable in the full logical list. */
typedef struct {
    loom_rect viewport;
    size_t count;
    float item_height;
    float scroll;
    size_t first, last;
} loom_virtual_list;

loom_virtual_list loom_virtual_list_init(loom_rect viewport, size_t count, float item_height,
                                         float requested_scroll);
loom_rect loom_virtual_list_item(const loom_virtual_list *list, size_t index);
float loom_virtual_list_max_scroll(const loom_virtual_list *list);
/* The scroll that shows `index` at least `margin` clear of either edge. */
float loom_virtual_list_reveal(const loom_virtual_list *list, size_t index,
                               float margin);
