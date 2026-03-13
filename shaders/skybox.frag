#version 460

/**
 * Skybox fragment shader — cubemap sampling with IBL rotation.
 *
 * Normalizes the interpolated world direction, applies horizontal
 * rotation (rotate_y), then samples the skybox cubemap from the
 * bindless array.
 */

#include "common/bindings.glsl"

layout(location = 0) in vec3 in_world_dir;

layout(location = 0) out vec4 out_color;

/** Rotate a direction around the Y axis by angle (sin, cos). */
vec3 rotate_y(vec3 d, float s, float c) {
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

void main() {
    vec3 dir = normalize(in_world_dir);

    // IBL horizontal rotation (identity until ibl_rotation_sin/cos are wired)
    dir = rotate_y(dir, 0.0, 1.0);

    vec3 color = texture(cubemaps[global.skybox_cubemap_index], dir).rgb;
    out_color = vec4(color, 1.0);
}
