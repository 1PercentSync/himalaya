#version 460

/**
 * Skybox fragment shader — cubemap sampling.
 *
 * Normalizes the interpolated world direction from skybox.vert and
 * samples the environment cubemap via bindless index from GlobalUBO.
 * Output is linear HDR; tonemapping is handled by the downstream pass.
 */

#include "common/bindings.glsl"

layout(location = 0) in vec3 in_world_dir;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 dir = normalize(in_world_dir);
    out_color = texture(cubemaps[global.skybox_cubemap_index], dir);
}
