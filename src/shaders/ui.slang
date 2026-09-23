// Instanced UI batch: rectangles, border rings, images and glyphs.
//
// ONE sampler, deliberately. There is no bindless texture support here, so the
// renderer batches until the bound texture changes and then flushes; a sampler
// per texture would cap the UI at however many slots the shader declared.
// Glyphs are 8-bit coverage in .r, images are colour in .rgb.
//
// FIVE fragment entry points, one per kind of instance, instead of one shader
// branching on a per-instance mode. The renderer already flushes on a binding
// change, so the program is just another part of that state and costs nothing
// extra to switch. Measured on Mali-G52 at 1080p: the branchy version spent
// ~1 ms a frame on control flow, and made every plain fill -- including the
// full-screen background -- pay for rounded-corner code it never used.
//
// Clipping happens in the VERTEX shader, by shrinking the quad to the
// intersection of its rect and its clip rect and moving the UVs with it. The
// obvious version -- pass the clip rect through and `discard` outside it --
// cost ~1.5 ms: `discard` disables early-ZS and Forward Pixel Kill, so every
// overdrawn fragment runs the whole shader. Shrinking the geometry instead
// means fully clipped instances (most of a virtual list) cost nothing.

uniform float2 viewport;
Sampler2D atlas;

struct VSIn {
    float2 corner : POSITION;
    float4 rect : RECT;
    float4 uv : TEXCOORD0;
    float4 color : COLOR;
    float4 clip : CLIP; // x0, y0, x1, y1
    float4 shape : SHAPE; // radius, border width, unused, unused
    // Edge fade, per top/bottom/left/right: the distance into it, in fade widths, is
    // position * scale - offset with the position mirrored for bottom and right.
    float4 maskScale : MASKSCALE;
    float4 maskOffset : MASKOFFSET;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
    float2 local : TEXCOORD1;
    float2 size : TEXCOORD2;
    nointerpolation float4 shape : TEXCOORD3;
    float4 mask : TEXCOORD4;
};

[shader("vertex")]
VSOut vsUi(VSIn v) {
    // Clip by geometry. A fully clipped instance collapses to zero area, which
    // the rasteriser drops for free.
    float2 lo = max(v.rect.xy, v.clip.xy);
    float2 hi = max(lo, min(v.rect.xy + v.rect.zw, v.clip.zw));
    float2 pixel = lerp(lo, hi, v.corner);

    // Where that landed inside the original rect, so UVs and the rounded-corner
    // distance still refer to the whole shape.
    float2 t = (pixel - v.rect.xy) / max(v.rect.zw, float2(1e-6, 1e-6));

    VSOut o;
    o.pos = float4(pixel.x / viewport.x * 2.0 - 1.0,
                   1.0 - pixel.y / viewport.y * 2.0, 0.0, 1.0);
    o.uv = lerp(v.uv.xy, v.uv.zw, t);
    o.color = v.color;
    o.local = pixel - v.rect.xy;
    o.size = v.rect.zw;
    o.shape = v.shape;
    // Linear in the position, so it interpolates exactly; the clamp keeps it in range
    // however far the quad reaches past the fade.
    o.mask = clamp(float4(pixel.y, -pixel.y, pixel.x, -pixel.x) * v.maskScale - v.maskOffset,
                   -2.0, 2.0);
    return o;
}

float masked(VSOut i, float alpha) {
    float4 m = i.mask;
    return alpha * smoothstep(0.0, 1.0, min(min(m.x, m.y), min(m.z, m.w)));
}

float clampRadius(float2 size, float radius) {
    return min(radius, min(size.x, size.y) * 0.5);
}

float roundedDistance(float2 p, float2 size, float r) {
    float2 q = abs(p - size * 0.5) - (size * 0.5 - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

/// True when this fragment is more than `margin` from both edges, so no corner
/// can reach it. The corners are a few hundred pixels of a panel that is half a
/// megapixel; this test is four instructions and skips a `length()` for all of
/// the rest, which was worth 2 ms a frame on its own.
bool interior(float2 local, float2 size, float margin) {
    float2 d = min(local, size - local);
    return d.x >= margin && d.y >= margin;
}

[shader("fragment")]
float4 fsFill(VSOut i) : SV_Target {
    return float4(i.color.rgb, masked(i, i.color.a));
}

[shader("fragment")]
float4 fsRound(VSOut i) : SV_Target {
    float r = clampRadius(i.size, i.shape.x);
    if (interior(i.local, i.size, r)) return float4(i.color.rgb, masked(i, i.color.a));
    float alpha = 1.0 - smoothstep(0.0, 1.0, roundedDistance(i.local, i.size, r));
    return float4(i.color.rgb, masked(i, i.color.a * alpha));
}

[shader("fragment")]
float4 fsBorder(VSOut i) : SV_Target {
    float b = max(i.shape.y, 1.0);
    float r = clampRadius(i.size, i.shape.x);
    // Everything inside the ring is transparent, which is most of the quad.
    if (interior(i.local, i.size, max(r, b) + 1.0)) return float4(0.0, 0.0, 0.0, 0.0);

    float alpha = 1.0 - smoothstep(0.0, 1.0, roundedDistance(i.local, i.size, r));
    float2 inner_size = i.size - b * 2.0;
    if (inner_size.x > 0.0 && inner_size.y > 0.0) {
        float inner = roundedDistance(i.local - b, inner_size, max(0.0, r - b));
        alpha *= smoothstep(-1.0, 1.0, inner);
    }
    return float4(i.color.rgb, masked(i, i.color.a * alpha));
}

[shader("fragment")]
float4 fsGlyph(VSOut i) : SV_Target {
    // Rasterised coverage, so the glyph is the texel: no distance field to
    // decode and no screen-space derivative to take.
    return float4(i.color.rgb, masked(i, i.color.a * atlas.Sample(i.uv).r));
}

[shader("fragment")]
float4 fsImage(VSOut i) : SV_Target {
    float r = clampRadius(i.size, i.shape.x);
    float alpha = 1.0;
    if (!interior(i.local, i.size, r))
        alpha = 1.0 - smoothstep(0.0, 1.0, roundedDistance(i.local, i.size, r));
    return atlas.Sample(i.uv) * float4(i.color.rgb, masked(i, i.color.a * alpha));
}

// A texture composited premultiplied, which is how blending overlapping layers comes out
// right and what filters correctly. Dividing it back out here is one reciprocal per
// fragment; on the CPU it was a divide per pixel that nothing vectorises.
[shader("fragment")]
float4 fsImagePremultiplied(VSOut i) : SV_Target {
    float4 t = atlas.Sample(i.uv);
    return float4(t.rgb / max(t.a, 1.0 / 255.0), t.a) * float4(i.color.rgb, masked(i, i.color.a));
}
