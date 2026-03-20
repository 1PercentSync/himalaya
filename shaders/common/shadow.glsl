/**
 * @file shadow.glsl
 * @brief CSM shadow sampling — cascade selection, shadow comparison, distance fade.
 *
 * Provides decomposed functions for the forward pass to assemble shadow
 * evaluation.  Intermediate results (cascade index, blend factor) remain
 * visible to callers for debug visualization.
 *
 * Depends on bindings.glsl being included first (GlobalUBO, rt_shadow_map).
 */

#ifndef SHADOW_GLSL
#define SHADOW_GLSL

/**
 * Select the cascade index for a given view-space depth.
 *
 * Step 3: always returns cascade 0 (single cascade).
 * Step 4 will implement PSSM-based selection with cascade_splits comparison.
 *
 * @param view_depth   Positive linear distance from camera.
 * @param blend_factor Output: 0.0 = no blending needed (Step 6 implements blend).
 * @return Cascade index [0, shadow_cascade_count).
 */
int select_cascade(float view_depth, out float blend_factor) {
    blend_factor = 0.0;
    return 0;
}

/**
 * Compute the world-space size of one shadow map texel for a given cascade.
 *
 * Extracts the orthographic projection scale from the cascade VP matrix.
 * Row 0 of the VP matrix encodes the clip-X-per-world-unit gradient; its
 * length gives the rate of change, inverted and scaled by texel_size
 * (1 / resolution) to yield world units per texel.
 *
 * @param cascade Cascade index.
 * @return World-space extent of one shadow texel.
 */
float cascade_texel_world_size(int cascade) {
    mat4 vp = global.cascade_view_proj[cascade];
    // Row 0: (col0_row0, col1_row0, col2_row0) = gradient of clip_x w.r.t. world xyz
    vec3 row0 = vec3(vp[0][0], vp[1][0], vp[2][0]);
    // clip range [-1,1] = 2 units covers (2 / ||row0||) world units,
    // divided by resolution (= 1/shadow_texel_size) gives per-texel size
    return 2.0 * global.shadow_texel_size / length(row0);
}

/**
 * Sample the shadow map with a single hardware comparison (hard shadow).
 *
 * Applies normal offset bias before projecting to light space: the sampling
 * position is pushed along the surface normal by an amount proportional to
 * the cascade's texel world size.  This complements the hardware depth bias
 * (constant + slope) set during shadow map rendering.
 *
 * @param world_pos    Fragment world-space position.
 * @param world_normal Fragment world-space shading normal (normalized).
 * @param cascade      Cascade index.
 * @return Shadow factor: 1.0 = fully lit, 0.0 = fully in shadow.
 */
float sample_shadow(vec3 world_pos, vec3 world_normal, int cascade) {
    // Normal offset: push along normal to reduce acne on surfaces
    // nearly parallel to the light direction
    float texel_ws = cascade_texel_world_size(cascade);
    vec3 offset_pos = world_pos + world_normal * global.shadow_normal_offset * texel_ws;

    // Project to light clip space (w = 1 for orthographic, divide is a no-op)
    vec4 light_clip = global.cascade_view_proj[cascade] * vec4(offset_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;

    // NDC [-1,1] -> UV [0,1]
    vec2 shadow_uv = light_ndc.xy * 0.5 + 0.5;
    float ref_depth = light_ndc.z;

    // Hardware comparison: GREATER_OR_EQUAL with Reverse-Z
    // Returns 1.0 when ref_depth >= stored (lit), 0.0 when occluded
    return texture(rt_shadow_map, vec4(shadow_uv, float(cascade), ref_depth));
}

/**
 * Compute a fade factor near the maximum shadow distance.
 *
 * Returns 1.0 well within shadow range, smoothly fading to 0.0 as
 * view_depth approaches shadow_max_distance.  Apply as:
 *     shadow = mix(1.0, shadow, shadow_distance_fade(view_depth));
 *
 * Uses the independent shadow_distance_fade_width UBO field (not blend_width).
 *
 * @param view_depth Positive linear distance from camera.
 * @return Fade factor: 1.0 = keep shadow as-is, 0.0 = no shadow (fully lit).
 */
float shadow_distance_fade(float view_depth) {
    float fade_end = global.shadow_max_distance;
    float fade_start = fade_end * (1.0 - global.shadow_distance_fade_width);
    return 1.0 - smoothstep(fade_start, fade_end, view_depth);
}

#endif // SHADOW_GLSL
