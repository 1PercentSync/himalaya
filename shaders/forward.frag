#version 460
#extension GL_EXT_nonuniform_qualifier : require

/**
 * @file forward.frag
 * @brief Forward pass fragment shader — Cook-Torrance direct + IBL environment lighting.
 *
 * Full PBR pipeline: samples all 5 material textures (base_color, normal,
 * metallic_roughness, occlusion, emissive), applies Cook-Torrance BRDF for
 * directional lights, Split-Sum IBL for environment lighting, and adds
 * emissive contribution.  Alpha Mask mode discards fragments below alpha_cutoff.
 *
 * Supports debug render modes (via GlobalUBO debug_render_mode) for
 * visualizing individual lighting components and material properties.
 */

#include "common/bindings.glsl"
#include "common/normal.glsl"
#include "common/brdf.glsl"
#include "common/shadow.glsl"
#include "common/transform.glsl"

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;
layout(location = 4) flat in uint frag_material_index;

layout(location = 0) out vec4 out_color;


/**
 * Multi-bounce AO color compensation (Jimenez 2016).
 *
 * Reduces over-darkening on light-colored surfaces: high-albedo materials
 * scatter more light in occluded regions, so raw AO over-attenuates them.
 * Returns a per-channel occlusion factor >= ao.
 *
 * @param ao     Combined ambient occlusion scalar [0,1].
 * @param albedo Surface diffuse color (base_color * (1 - metallic)).
 * @return Per-channel occlusion factor (always >= ao).
 */
vec3 multi_bounce_ao(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

/**
 * Lagarde specular occlusion approximation (Lagarde & de Rousiers 2014).
 *
 * Estimates specular occlusion from diffuse AO, view angle, and roughness.
 * Rougher surfaces have wider specular lobes, so they are less affected.
 * Kept as a comparison baseline alongside GTSO; selected at runtime via
 * global.ao_so_mode.
 *
 * @param NdotV    Clamped dot(N, V).
 * @param ao       Screen-space ambient occlusion [0,1].
 * @param roughness Surface roughness [0,1].
 * @return Specular occlusion factor [0,1].
 */
float lagarde_so(float NdotV, float ao, float roughness) {
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

/**
 * GTSO specular occlusion (Jimenez 2016, smoothstep approximation).
 *
 * Evaluates the overlap between a visibility cone (derived from bent
 * normal + AO) and a specular cone (reflection direction + roughness).
 * Industry-standard approximation used by XeGTAO, UE, and Frostbite.
 *
 * Includes ao^2 grazing-angle compensation: the cone intersection model
 * treats the hemisphere boundary (alpha_v = pi/2) as an occlusion edge,
 * causing false darkening at grazing angles when AO is near 1.0.  The
 * mix toward 1.0 eliminates this artifact while preserving directional
 * accuracy at lower AO values where GTSO matters most.
 *
 * @param bent_normal World-space bent normal (normalized).
 * @param R           World-space reflection direction (normalized).
 * @param ao          Diffuse ambient occlusion [0,1].
 * @param roughness   Surface roughness [0,1].
 * @return Specular occlusion factor [0,1].
 */
float gtso_specular_occlusion(vec3 bent_normal, vec3 R, float ao, float roughness) {
    // Visibility cone half-angle: area of unoccluded cap → half-angle
    float alpha_v = acos(sqrt(1.0 - ao));

    // Specular cone half-angle: Jimenez 2016 optimal fit (u = 0.01)
    float alpha_s = max(acos(pow(0.01, 0.5 * roughness * roughness)), 0.01);

    // Angle between cone axes
    float beta = acos(clamp(dot(bent_normal, R), -1.0, 1.0));

    // Smoothstep cone intersection + ao^2 grazing-angle compensation
    float raw_so = smoothstep(0.0, 1.0, (alpha_v - beta) / alpha_s);
    return mix(raw_so, 1.0, ao * ao);
}

void main() {
    GPUMaterialData mat = materials[frag_material_index];

    // Base color
    vec4 base_color = texture(textures[nonuniformEXT(mat.base_color_tex)], frag_uv0)
                      * mat.base_color_factor;

    // Alpha Mask: discard if below cutoff
    if (mat.alpha_mode == 1u && base_color.a < mat.alpha_cutoff) {
        discard;
    }

    // Shading normal (geometric normal + normal map via TBN)
    vec3 N = normalize(frag_normal);
    vec2 normal_rg = texture(textures[nonuniformEXT(mat.normal_tex)], frag_uv0).rg;
    N = get_shading_normal(N, frag_tangent, normal_rg, mat.normal_scale);

    // Metallic/roughness from texture (glTF: G = roughness, B = metallic)
    vec4 mr_texel = texture(textures[nonuniformEXT(mat.metallic_roughness_tex)], frag_uv0);
    float metallic  = mr_texel.b * mat.metallic_factor;
    float roughness = mr_texel.g * mat.roughness_factor;

    // ---- Debug: material property visualizations (early out, no lighting) ----
    if (global.debug_render_mode >= DEBUG_MODE_PASSTHROUGH_START) {
        vec3 vis;
        switch (global.debug_render_mode) {
            case DEBUG_MODE_NORMAL:    vis = N * 0.5 + 0.5; break;
            case DEBUG_MODE_METALLIC:  vis = vec3(metallic); break;
            case DEBUG_MODE_ROUGHNESS: vis = vec3(roughness); break;
            case DEBUG_MODE_AO: {
                // Combined AO: ssao × material_ao (material-only when AO disabled)
                float dbg_mat_ao = texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r;
                dbg_mat_ao = 1.0 + mat.occlusion_strength * (dbg_mat_ao - 1.0);
                float dbg_ssao = 1.0;
                if ((global.feature_flags & FEATURE_AO) != 0u) {
                    dbg_ssao = texture(rt_ao_texture, gl_FragCoord.xy / global.screen_size).a;
                }
                vis = vec3(dbg_ssao * dbg_mat_ao);
                break;
            }
            case DEBUG_MODE_AO_SSAO: {
                // Raw GTAO output (A channel, temporal-filtered)
                if ((global.feature_flags & FEATURE_AO) != 0u) {
                    vis = vec3(texture(rt_ao_texture, gl_FragCoord.xy / global.screen_size).a);
                } else {
                    vis = vec3(1.0);
                }
                break;
            }
            case DEBUG_MODE_CONTACT_SHADOWS: {
                // Raw contact shadow mask (R channel, 1=lit 0=shadowed)
                if ((global.feature_flags & FEATURE_CONTACT_SHADOWS) != 0u) {
                    vis = vec3(texture(rt_contact_shadow_mask, gl_FragCoord.xy / global.screen_size).r);
                } else {
                    vis = vec3(1.0);
                }
                break;
            }
            case DEBUG_MODE_SHADOW_CASCADES: {
                if ((global.feature_flags & FEATURE_SHADOWS) != 0u) {
                    float vd = -(global.view * vec4(frag_world_pos, 1.0)).z;
                    float bf;
                    int ci = select_cascade(vd, bf);
                    const vec3 colors[4] = vec3[4](
                        vec3(1.0, 0.2, 0.2),   // cascade 0: red
                        vec3(0.2, 1.0, 0.2),   // cascade 1: green
                        vec3(0.2, 0.2, 1.0),   // cascade 2: blue
                        vec3(1.0, 1.0, 0.2));  // cascade 3: yellow
                    vis = colors[ci];
                } else {
                    vis = vec3(0.5);
                }
                break;
            }
            default: vis = vec3(1.0, 0.0, 1.0); break;
        }
        out_color = vec4(vis, 1.0);
        return;
    }

    // Metallic workflow separation
    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);
    vec3 diffuse_color = base_color.rgb * (1.0 - metallic);

    // View direction and reflection
    vec3 V = normalize(global.camera_position_and_exposure.xyz - frag_world_pos);
    vec3 R = reflect(-V, N);
    float NdotV = max(dot(N, V), 0.0);

    // View-space depth for shadow cascade selection and distance fade
    float view_depth = -(global.view * vec4(frag_world_pos, 1.0)).z;

    // Screen UV (shared by contact shadows and AO sampling)
    vec2 screen_uv = gl_FragCoord.xy / global.screen_size;

    // Contact shadow mask (1.0 = lit, 0.0 = fully shadowed)
    float contact_shadow = 1.0;
    if ((global.feature_flags & FEATURE_CONTACT_SHADOWS) != 0u) {
        contact_shadow = texture(rt_contact_shadow_mask, screen_uv).r;
    }

    // ---- Direct lighting: Cook-Torrance + Lambert (split for debug modes) ----
    vec3 direct_diffuse = vec3(0.0);
    vec3 direct_specular = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        vec3 L = normalize(-directional_lights[i].direction_and_intensity.xyz);
        float intensity = directional_lights[i].direction_and_intensity.w;
        vec3 light_color = directional_lights[i].color_and_shadow.xyz;

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        vec3 H = normalize(V + L);
        float NdotH = max(dot(N, H), 0.0);
        float VdotH = max(dot(V, H), 0.0);

        float D   = D_GGX(NdotH, roughness);
        float Vis = V_SmithGGX(NdotV, NdotL, roughness);
        vec3  F   = F_Schlick(VdotH, F0);

        vec3 radiance = light_color * intensity * NdotL;

        // Shadow attenuation (guarded by feature flag and per-light cast_shadows)
        if ((global.feature_flags & FEATURE_SHADOWS) != 0u
            && directional_lights[i].color_and_shadow.w > 0.5) {
            radiance *= blend_cascade_shadow(frag_world_pos, N, view_depth);
        }

        // Contact shadow attenuation (primary directional light only —
        // the compute shader traces rays for directional_lights[0])
        if (i == 0u) {
            radiance *= contact_shadow;
        }

        direct_diffuse  += (1.0 - F) * diffuse_color * INV_PI * radiance;
        direct_specular += D * Vis * F * radiance;
    }

    // ---- IBL environment lighting (split for debug modes) ----
    vec3 rotated_N = rotate_y(N, global.ibl_rotation_sin, global.ibl_rotation_cos);
    vec3 rotated_R = rotate_y(R, global.ibl_rotation_sin, global.ibl_rotation_cos);

    vec3 irradiance  = texture(cubemaps[nonuniformEXT(global.irradiance_cubemap_index)], rotated_N).rgb;
    float mip        = roughness * float(global.prefiltered_mip_count - 1u);
    vec3 prefiltered = textureLod(cubemaps[nonuniformEXT(global.prefiltered_cubemap_index)], rotated_R, mip).rgb;
    vec2 brdf_lut    = texture(textures[nonuniformEXT(global.brdf_lut_index)], vec2(NdotV, roughness)).rg;

    vec3 ibl_diffuse  = irradiance * diffuse_color;
    vec3 ibl_specular = prefiltered * (F0 * brdf_lut.x + brdf_lut.y);

    // ---- Ambient Occlusion ----

    // Material AO (baked occlusion texture)
    float material_ao = texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r;
    material_ao = 1.0 + mat.occlusion_strength * (material_ao - 1.0);

    // Screen-space AO + bent normal (GTAO temporal-filtered, guarded by FEATURE_AO)
    float ssao = 1.0;
    float specular_ao = 1.0;
    if ((global.feature_flags & FEATURE_AO) != 0u) {
        vec4 ao_data = texture(rt_ao_texture, screen_uv);
        ssao = ao_data.a;

        if (global.ao_so_mode == AO_SO_GTSO) {
            // Decode bent normal: view-space (encoded x0.5+0.5) → world-space
            vec3 bent_ws = transpose(mat3(global.view)) * (ao_data.rgb * 2.0 - 1.0);
            bent_ws = (dot(bent_ws, bent_ws) > EPSILON) ? normalize(bent_ws) : N;

            // GTSO specular occlusion (visibility cone × specular cone intersection)
            specular_ao = gtso_specular_occlusion(bent_ws, R, ssao, roughness);
        } else {
            // Lagarde approximation (no bent normal needed)
            specular_ao = lagarde_so(NdotV, ssao, roughness);
        }
    }

    // Diffuse AO: combine SSAO with material AO + multi-bounce color compensation
    // Jimenez 2016: prevents over-darkening on light-colored surfaces (high albedo)
    float combined_ao = ssao * material_ao;
    vec3 diffuse_ao = multi_bounce_ao(combined_ao, diffuse_color);

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_tex)], frag_uv0).rgb
                    * mat.emissive_factor.rgb;

    // ---- Combine based on debug render mode ----
    vec3 color;
    switch (global.debug_render_mode) {
        case DEBUG_MODE_DIFFUSE_ONLY:
            color = direct_diffuse + global.ibl_intensity * ibl_diffuse * diffuse_ao;
            break;
        case DEBUG_MODE_SPECULAR_ONLY:
            color = direct_specular + global.ibl_intensity * ibl_specular * specular_ao;
            break;
        case DEBUG_MODE_IBL_ONLY:
            color = global.ibl_intensity * (ibl_diffuse * diffuse_ao + ibl_specular * specular_ao);
            break;
        default: // DEBUG_MODE_FULL_PBR
            color = (direct_diffuse + direct_specular)
                  + global.ibl_intensity * (ibl_diffuse * diffuse_ao + ibl_specular * specular_ao)
                  + emissive;
            break;
    }

    // Output raw HDR linear values; exposure is applied in the tonemapping pass.
    out_color = vec4(color, base_color.a);
}
