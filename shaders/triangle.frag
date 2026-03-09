#version 460

/**
 * @file triangle.frag
 * @brief Triangle fragment shader — samples a bindless texture and
 *        multiplies with the interpolated vertex color.
 *
 * Uses a hardcoded bindless index (0 = default white texture) to
 * verify the bindless texture pipeline is functional.
 */

layout(set = 1, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

void main() {
    // Hardcoded bindless index 0 = default white texture (1,1,1,1).
    // White * vertex_color = vertex_color, proving the bindless path works.
    vec4 tex_color = texture(textures[0], frag_uv);
    out_color = tex_color * vec4(frag_color, 1.0);
}
