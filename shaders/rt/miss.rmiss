#version 460

/**
 * @file miss.rmiss
 * @brief Path tracing environment miss shader — samples IBL cubemap.
 *
 * When a ray misses all geometry, samples the skybox cubemap with IBL
 * rotation applied. Writes environment radiance to PrimaryPayload and
 * signals path termination via hit_distance = -1.
 */

#define HIMALAYA_RT
#include "common/bindings.glsl"
#include "rt/pt_common.glsl"
#include "common/transform.glsl"

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;

void main() {
    // Apply IBL Y-axis rotation to the ray direction
    vec3 dir = rotate_y(gl_WorldRayDirectionEXT,
                        global.ibl_rotation_sin,
                        global.ibl_rotation_cos);

    // Sample skybox cubemap
    vec3 env_color = texture(cubemaps[nonuniformEXT(global.skybox_cubemap_index)], dir).rgb
                     * global.ibl_intensity;

    payload.color = env_color;
    payload.hit_distance = -1.0; // signal path termination
}
