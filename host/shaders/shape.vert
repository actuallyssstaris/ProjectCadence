#version 450

// one instance = one shape, the quad is built from gl_VertexIndex
layout(location = 0) in vec4 i_rect;    // x, y, w, h  (centre and half size, pixels)
layout(location = 1) in vec2 i_rot_z;   // rotation, depth
layout(location = 2) in uvec2 i_misc;   // colour rgba8, kind
layout(location = 3) in vec4 i_p;

layout(push_constant) uniform PC {
    vec2 screen;
} pc;

layout(location = 0) out vec2 v_local;      // pixels from the shape centre (unrotated)
layout(location = 1) flat out vec4 v_colour;
layout(location = 2) flat out uint v_kind;
layout(location = 3) flat out vec4 v_p;
layout(location = 4) flat out vec2 v_half;
layout(location = 5) out vec2 v_uv;

const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main()
{
    vec2 c = corners[gl_VertexIndex];
    uint kind = i_misc.y;

    vec2 centre = i_rect.xy;
    vec2 half_size = i_rect.zw;
    float rot = i_rot_z.x;

    // capsule: x,y -> p.xy, radius in w. Turn it into a rotated box around the segment.
    if (kind == 4u) {
        vec2 a = i_rect.xy;
        vec2 b = i_p.xy;
        vec2 d = b - a;
        float len = length(d);
        centre = (a + b) * 0.5;
        half_size = vec2(len * 0.5 + i_rect.z + 1.5, i_rect.z + 1.5);
        rot = len > 0.0 ? atan(d.y, d.x) : 0.0;
    }

    vec2 local = c * half_size;
    float s = sin(rot), co = cos(rot);
    vec2 world = centre + vec2(local.x * co - local.y * s, local.x * s + local.y * co);

    gl_Position = vec4(world / pc.screen * 2.0 - 1.0, i_rot_z.y, 1.0);

    v_local = local;
    v_colour = unpackUnorm4x8(i_misc.x);
    v_kind = kind;
    v_p = i_p;
    v_half = half_size;
    // uv rect for textured kinds lives in p
    v_uv = mix(i_p.xy, i_p.zw, c * 0.5 + 0.5);
}
