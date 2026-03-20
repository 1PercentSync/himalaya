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
 * Compares view_depth against cascade_splits boundaries (PSSM-distributed).
 * Falls through to the last cascade if beyond all splits.
 *
 * When the fragment lies in the blend region near a cascade's far boundary,
 * blend_factor is non-zero (0..1) indicating how much to blend with the
 * next cascade.  For the last cascade, blend_factor is always 0 (distance
 * fade handles the far edge instead).
 *
 * @param view_depth   Positive linear distance from camera.
 * @param blend_factor Output: 0.0 = no blend, 1.0 = fully next cascade.
 * @return Cascade index [0, shadow_cascade_count).
 */
int select_cascade(float view_depth, out float blend_factor) {
    blend_factor = 0.0;

    int count = int(global.shadow_cascade_count);

    // cascade_splits holds far boundaries; pick the first cascade
    // whose far boundary exceeds the fragment's view-space depth.
    for (int i = 0; i < count - 1; ++i) {
        if (view_depth < global.cascade_splits[i]) {
            // Blend region: last (blend_width) fraction of this cascade's
            // split distance.  Smoothly ramps blend_factor from 0 to 1.
            float blend_start = global.cascade_splits[i]
                                * (1.0 - global.shadow_blend_width);
            if (view_depth > blend_start) {
                blend_factor = (view_depth - blend_start)
                               / (global.cascade_splits[i] - blend_start);
            }
            return i;
        }
    }
    return count - 1;
}

/**
 * Sample the shadow map with a single hardware comparison (hard shadow).
 *
 * Applies normal offset bias before projecting to light space: the sampling
 * position is pushed along the surface normal by an amount proportional to
 * the cascade's texel world size (precomputed on CPU, stored in GlobalUBO).
 * This complements the hardware depth bias (slope) set during shadow map
 * rendering.
 *
 * @param world_pos    Fragment world-space position.
 * @param world_normal Fragment world-space shading normal (normalized).
 * @param cascade      Cascade index.
 * @return Shadow factor: 1.0 = fully lit, 0.0 = fully in shadow.
 */
float sample_shadow(vec3 world_pos, vec3 world_normal, int cascade) {
    // Normal offset: push along normal to reduce acne on surfaces
    // nearly parallel to the light direction
    float texel_ws = global.cascade_texel_world_size[cascade];
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
 * Sample the shadow map with PCF (Percentage-Closer Filtering).
 *
 * Performs a (2R+1) x (2R+1) grid of hardware 2x2 comparison samples,
 * where R = shadow_pcf_radius from GlobalUBO.  Each texture() call on
 * sampler2DArrayShadow returns a bilinear-interpolated comparison over
 * a 2x2 texel footprint, so the effective filter is wider than the grid.
 *
 * When R = 0, falls back to a single hardware comparison (hard shadow),
 * identical to sample_shadow().
 *
 * @param world_pos    Fragment world-space position.
 * @param world_normal Fragment world-space shading normal (normalized).
 * @param cascade      Cascade index.
 * @return Shadow factor: 1.0 = fully lit, 0.0 = fully in shadow.
 */
float sample_shadow_pcf(vec3 world_pos, vec3 world_normal, int cascade) {
    // Normal offset (same logic as sample_shadow)
    float texel_ws = global.cascade_texel_world_size[cascade];
    vec3 offset_pos = world_pos + world_normal * global.shadow_normal_offset * texel_ws;

    // Project to light clip space (orthographic: w = 1)
    vec4 light_clip = global.cascade_view_proj[cascade] * vec4(offset_pos, 1.0);
    vec3 light_ndc = light_clip.xyz / light_clip.w;

    // NDC [-1,1] -> UV [0,1]
    vec2 shadow_uv = light_ndc.xy * 0.5 + 0.5;
    float ref_depth = light_ndc.z;

    int radius = int(global.shadow_pcf_radius);
    if (radius == 0) {
        return texture(rt_shadow_map, vec4(shadow_uv, float(cascade), ref_depth));
    }

    // (2R+1) x (2R+1) grid — each sample leverages hardware 2x2 comparison
    float sum = 0.0;
    float texel = global.shadow_texel_size;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            vec2 uv = shadow_uv + vec2(float(x), float(y)) * texel;
            sum += texture(rt_shadow_map, vec4(uv, float(cascade), ref_depth));
        }
    }

    return sum / float((2 * radius + 1) * (2 * radius + 1));
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

/**
 * Evaluate shadow with cascade blending and distance fade.
 *
 * Single entry point for the forward pass: selects the cascade, samples
 * with PCF, blends adjacent cascades in the overlap region, and applies
 * distance fade at the far edge of the last cascade.
 *
 * Blend strategy: linear interpolation (lerp) between the current and
 * next cascade.  Only ~10% of shadow pixels hit the blend region, so the
 * cost of the extra PCF sample is bounded.  The blend method is isolated
 * here to facilitate a future switch to dithering if needed.
 *
 * @param world_pos    Fragment world-space position.
 * @param world_normal Fragment world-space shading normal (normalized).
 * @param view_depth   Positive linear distance from camera.
 * @return Shadow factor: 1.0 = fully lit, 0.0 = fully in shadow.
 */
float blend_cascade_shadow(vec3 world_pos, vec3 world_normal, float view_depth) {
    float blend_factor;
    int cascade = select_cascade(view_depth, blend_factor);

    float shadow = sample_shadow_pcf(world_pos, world_normal, cascade);

    // Blend with next cascade in the overlap region
    if (blend_factor > 0.0 && cascade < int(global.shadow_cascade_count) - 1) {
        float next_shadow = sample_shadow_pcf(world_pos, world_normal, cascade + 1);
        shadow = mix(shadow, next_shadow, blend_factor);
    }

    // Distance fade: last cascade far edge blends to fully lit
    shadow = mix(1.0, shadow, shadow_distance_fade(view_depth));

    return shadow;
}

#endif // SHADOW_GLSL
