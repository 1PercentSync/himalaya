#version 450

/**
 * @file triangle.vert
 * @brief Hardcoded triangle vertex shader.
 *
 * Positions and colors are stored in constant arrays indexed by gl_VertexIndex.
 * No vertex buffer is needed — draw with vkCmdDraw(3, 1, 0, 0).
 */

layout(location = 0) out vec3 frag_color;

void main() {
    // Triangle vertices in NDC (clip space with w=1)
    const vec2 positions[3] = vec2[](
        vec2( 0.0, -0.5),   // top
        vec2( 0.5,  0.5),   // bottom-right
        vec2(-0.5,  0.5)    // bottom-left
    );

    // Per-vertex colors (red, green, blue)
    const vec3 colors[3] = vec3[](
        vec3(1.0, 0.0, 0.0),
        vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    frag_color = colors[gl_VertexIndex];
}
