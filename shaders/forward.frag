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

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_normal;
layout(location = 2) in vec2 frag_uv0;
layout(location = 3) in vec4 frag_tangent;
layout(location = 4) flat in uint frag_material_index;

layout(location = 0) out vec4 out_color;

/** Rotate a direction around the Y axis by angle (sin, cos). */
vec3 rotate_y(vec3 d, float s, float c) {
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
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
    if (global.debug_render_mode >= 4u) {
        vec3 vis;
        switch (global.debug_render_mode) {
            case 4u: vis = N * 0.5 + 0.5; break;
            case 5u: vis = vec3(metallic); break;
            case 6u: vis = vec3(roughness); break;
            case 7u: vis = vec3(texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r); break;
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

    // Occlusion modulates IBL only (direct lights have real-time shadows in M2)
    float ao = texture(textures[nonuniformEXT(mat.occlusion_tex)], frag_uv0).r;
    ao = 1.0 + mat.occlusion_strength * (ao - 1.0);

    // Emissive
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_tex)], frag_uv0).rgb
                    * mat.emissive_factor.rgb;

    // ---- Combine based on debug render mode ----
    vec3 color;
    switch (global.debug_render_mode) {
        case 1u: // Diffuse Only
            color = direct_diffuse + global.ibl_intensity * ibl_diffuse * ao;
            break;
        case 2u: // Specular Only
            color = direct_specular + global.ibl_intensity * ibl_specular * ao;
            break;
        case 3u: // IBL Only
            color = global.ibl_intensity * (ibl_diffuse + ibl_specular) * ao;
            break;
        default: // Full PBR
            color = (direct_diffuse + direct_specular)
                  + global.ibl_intensity * (ibl_diffuse + ibl_specular) * ao
                  + emissive;
            break;
    }

    // Output raw HDR linear values; exposure is applied in the tonemapping pass.
    out_color = vec4(color, base_color.a);
}
