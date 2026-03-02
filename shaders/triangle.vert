#version 460

/**
 * @file triangle.vert
 * @brief Triangle vertex shader reading from vertex buffer attributes.
 *
 * Vertex data (position, color) is supplied via a vertex buffer bound at
 * binding 0. The pipeline's vertex input state defines the attribute layout.
 */

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    frag_color = in_color;
}
