#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * Skybox fragment shader — cubemap sampling with IBL rotation.
 *
 * Normalizes the interpolated world direction, applies horizontal
 * rotation (rotate_y), then samples the skybox cubemap from the
 * bindless array.
 */

#include "common/bindings.glsl"
#include "common/transform.glsl"

layout(location = 0) in vec3 in_world_dir;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 dir = normalize(in_world_dir);

    // IBL horizontal rotation
    dir = rotate_y(dir, global.ibl_rotation_sin, global.ibl_rotation_cos);

    vec3 color = texture(cubemaps[nonuniformEXT(global.skybox_cubemap_index)], dir).rgb;
    out_color = vec4(color, 1.0);
}
