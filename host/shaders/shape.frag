#version 450

layout(location = 0) in vec2 v_local;
layout(location = 1) flat in vec4 v_colour;
layout(location = 2) flat in uint v_kind;
layout(location = 3) flat in vec4 v_p;
layout(location = 4) flat in vec2 v_half;
layout(location = 5) in vec2 v_uv;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 o_colour;

// kinds, keep in sync with gfx.h
const uint K_RECT = 0u;
const uint K_TEX = 1u;
const uint K_DISC = 2u;
const uint K_RING = 3u;
const uint K_CAPSULE = 4u;
const uint K_HITCIRCLE = 5u;
const uint K_ARROW = 6u;
const uint K_GLYPH = 7u;

float aa(float d)
{
    // d is signed distance in pixels, negative inside
    return clamp(0.5 - d, 0.0, 1.0);
}

void main()
{
    vec4 c = v_colour;
    float r = v_half.x;
    float dist = length(v_local);

    if (v_kind == K_RECT) {
        // nothing to do
    } else if (v_kind == K_TEX) {
        c *= texture(tex, v_uv);
    } else if (v_kind == K_GLYPH) {
        c.a *= texture(tex, v_uv).r;
    } else if (v_kind == K_DISC) {
        c.a *= aa(dist - r);
    } else if (v_kind == K_RING) {
        float t = v_p.x * 0.5;
        c.a *= aa(abs(dist - (r - t)) - t);
    } else if (v_kind == K_CAPSULE) {
        // v_half.y - 1.5 is the radius, segment runs along local x
        float rad = v_half.y - 1.5;
        float hl = v_half.x - v_half.y;
        vec2 q = vec2(max(abs(v_local.x) - hl, 0.0), v_local.y);
        float d = length(q) - rad;
        c.a *= aa(d);
        // p.z = 1 asks for a soft centre highlight (slider body interior)
        if (v_p.z > 0.0) {
            float k = clamp(length(q) / rad, 0.0, 1.0);
            c.rgb = mix(c.rgb * 1.25, c.rgb * 0.7, k);
        }
    } else if (v_kind == K_HITCIRCLE) {
        // combo coloured disc, darker towards the edge, white border
        float border = r * 0.12;
        float k = dist / r;
        vec3 body = mix(c.rgb, c.rgb * 0.55, smoothstep(0.2, 0.9, k));
        vec3 col = mix(body, vec3(1.0), smoothstep(-1.0, 1.0, dist - (r - border)));
        c = vec4(col, c.a * aa(dist - r));
    } else if (v_kind == K_ARROW) {
        // chevron pointing along +x
        vec2 p = v_local / r;
        float d1 = abs(p.y) * 0.9 + p.x * 0.8 - 0.45;
        float d2 = abs(p.y) * 0.9 + p.x * 0.8 - 0.05;
        float inner = max(d1, -d2);
        c.a *= aa(inner * r);
    }

    if (c.a < 0.004)
        discard;

    o_colour = c;
}
