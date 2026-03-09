#version 460

/**
 * @file triangle.vert
 * @brief Triangle vertex shader with camera view-projection transform.
 *
 * Reads position and color from vertex buffer, transforms position
 * by the view-projection matrix from GlobalUBO.
 */

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inv_view_projection;
    vec4 camera_position_and_exposure;
    vec2 screen_size;
    float time;
} global;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = global.view_projection * vec4(in_position, 1.0);
    frag_color = in_color;
}
