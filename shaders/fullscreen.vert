#version 460

/**
 * Fullscreen triangle — no vertex input.
 *
 * Generates a single triangle that covers the entire screen using
 * gl_VertexIndex (0, 1, 2). The triangle extends beyond the viewport
 * and is clipped to the screen by the rasterizer.
 *
 * Outputs UV coordinates in [0,1] range for fragment shader sampling.
 *
 * Usage: draw(3, 1) with no vertex buffer bound.
 */

layout(location = 0) out vec2 out_uv;

void main() {
    // Vertex 0: (-1, -1)  UV (0, 0)
    // Vertex 1: ( 3, -1)  UV (2, 0)
    // Vertex 2: (-1,  3)  UV (0, 2)
    out_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);
}
