#version 460

/**
 * @file closesthit.rchit
 * @brief Path tracing closest-hit shader — full surface shading in a single stage.
 *
 * Performs vertex interpolation, normal mapping, material sampling, NEE for
 * directional lights (shadow rays), multi-lobe BRDF sampling (diffuse +
 * specular), and writes results back through PrimaryPayload. Also outputs
 * OIDN auxiliary data (albedo + normal) on bounce 0.
 *
 * Mode A architecture: all shading is computed here; raygen only accumulates.
 */

#define HIMALAYA_RT
#include "common/bindings.glsl"
#include "rt/pt_common.glsl"
#include "common/normal.glsl"

// ---- Push constants (shared pipeline layout with raygen) ----

layout(push_constant) uniform PushConstants {
    uint  max_bounces;
    uint  sample_count;
    uint  frame_seed;
    uint  blue_noise_index;
    float max_clamp;
} pc;

// ---- OIDN auxiliary images (push descriptor, Set 3) ----

layout(set = 3, binding = 1, rgba16f) uniform image2D aux_albedo_image;
layout(set = 3, binding = 2, rgba16f) uniform image2D aux_normal_image;

// ---- Ray payloads ----

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;
layout(location = 1) rayPayloadEXT ShadowPayload shadow_payload;

// ---- Hit attributes ----

hitAttributeEXT vec2 bary;

// ---- Sobol dimension layout (must match raygen) ----

const uint DIMS_PER_BOUNCE = 6; // lobe_select, brdf_xi0, brdf_xi1, rr, env_nee_r1, env_nee_r2

void main() {
    // ---- Geometry info lookup ----
    GeometryInfo geo = geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT];

    // ---- Vertex interpolation (object space) ----
    HitAttributes hit = interpolate_hit(geo, bary, gl_PrimitiveID);

    // ---- Transform to world space ----
    vec3 world_pos = vec3(gl_ObjectToWorldEXT * vec4(hit.position, 1.0));

    // Face normal (true geometric normal from triangle edges) for ray origin offset.
    // Transform via normal matrix: transpose(inverse(model)) = transpose(WorldToObject).
    // GLSL: v * M = transpose(M) * v
    vec3 N_face = normalize(hit.face_normal * mat3(gl_WorldToObjectEXT));

    // Interpolated vertex normal for shading (TBN, lighting)
    vec3 N_interp = normalize(hit.normal * mat3(gl_WorldToObjectEXT));

    // Flip both normals to face the incoming ray (back-face handling)
    if (dot(N_face, gl_WorldRayDirectionEXT) > 0.0) {
        N_face = -N_face;
        N_interp = -N_interp;
    }

    // Tangent: transforms like a direction (model matrix, not normal matrix)
    vec3 T_world = normalize(mat3(gl_ObjectToWorldEXT) * hit.tangent.xyz);

    // ---- Material lookup ----
    GPUMaterialData mat = materials[geo.material_buffer_offset];

    vec4 base_color = texture(textures[nonuniformEXT(mat.base_color_tex)], hit.uv0)
                      * mat.base_color_factor;

    vec4 mr_texel = texture(textures[nonuniformEXT(mat.metallic_roughness_tex)], hit.uv0);
    float metallic  = mr_texel.b * mat.metallic_factor;
    float roughness = max(mr_texel.g * mat.roughness_factor, 0.04);

    // ---- Normal mapping + consistency correction ----
    vec2 normal_rg = texture(textures[nonuniformEXT(mat.normal_tex)], hit.uv0).rg;
    vec3 N_shading = get_shading_normal(N_interp, vec4(T_world, hit.tangent.w),
                                        normal_rg, mat.normal_scale);
    N_shading = ensure_normal_consistency(N_shading, N_face);

    // ---- PBR parameters ----
    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);
    vec3 diffuse_color = base_color.rgb * (1.0 - metallic);

    vec3 V = -gl_WorldRayDirectionEXT;
    float NdotV = max(dot(N_shading, V), 1e-4);

    // ---- OIDN auxiliary output (bounce 0 only) ----
    if (payload.bounce == 0u) {
        ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);
        imageStore(aux_albedo_image, pixel, vec4(diffuse_color, 1.0));
        imageStore(aux_normal_image, pixel, vec4(N_shading, 1.0));
    }

    // ---- Emissive contribution (all bounces, Step 12 adds MIS weight) ----
    vec3 emissive = texture(textures[nonuniformEXT(mat.emissive_tex)], hit.uv0).rgb
                    * mat.emissive_factor.rgb;

    // ---- Ray origin offset (shared by shadow rays and next bounce) ----
    vec3 offset_pos = offset_ray_origin(world_pos, N_face);

    // ---- NEE: Directional lights (delta distribution, MIS weight = 1) ----
    vec3 nee_radiance = vec3(0.0);
    for (uint i = 0; i < global.directional_light_count; ++i) {
        vec3 L = normalize(-directional_lights[i].direction_and_intensity.xyz);
        float NdotL = dot(N_shading, L);
        if (NdotL <= 0.0) {
            continue;
        }

        // Shadow ray (terminate on first hit, skip closest-hit shader)
        shadow_payload.visible = 0;
        traceRayEXT(
            tlas,
            gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
            0xFF,
            0, 0,       // SBT offset, stride
            1,          // miss index 1 (shadow miss)
            offset_pos,
            0.0,
            L,
            10000.0,
            1           // payload location 1
        );

        if (shadow_payload.visible == 1u) {
            float intensity = directional_lights[i].direction_and_intensity.w;
            vec3 light_color = directional_lights[i].color_and_shadow.xyz;

            vec3 H = normalize(V + L);
            float NdotH = max(dot(N_shading, H), 0.0);
            float VdotH = max(dot(V, H), 0.0);

            float D   = D_GGX(NdotH, roughness);
            float Vis = V_SmithGGX(NdotV, NdotL, roughness);
            vec3  F   = F_Schlick(VdotH, F0);

            vec3 specular = D * Vis * F;
            vec3 diffuse  = (1.0 - F) * diffuse_color * INV_PI;

            nee_radiance += (diffuse + specular) * light_color * intensity * NdotL;
        }
    }

    // ---- NEE: Environment light (alias table importance sampling + MIS) ----
    {
        ivec2 px = ivec2(gl_LaunchIDEXT.xy);
        uint env_dim = 2u + payload.bounce * DIMS_PER_BOUNCE + 4u;
        float env_r1 = rand_pt(env_dim, pc.sample_count, px,
                               pc.frame_seed, pc.blue_noise_index);
        float env_r2 = rand_pt(env_dim + 1u, pc.sample_count, px,
                               pc.frame_seed, pc.blue_noise_index);

        vec3 L = sample_env_alias_table(env_r1, env_r2);
        float NdotL = dot(N_shading, L);

        if (NdotL > 0.0) {
            // Shadow ray toward environment (infinite distance)
            shadow_payload.visible = 0;
            traceRayEXT(
                tlas,
                gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT,
                0xFF,
                0, 0,
                1,          // miss index 1 (shadow miss)
                offset_pos,
                0.0,
                L,
                10000.0,
                1           // payload location 1
            );

            if (shadow_payload.visible == 1u) {
                // Look up environment radiance at sampled direction
                vec3 env_dir = rotate_y(L, global.ibl_rotation_sin,
                                        global.ibl_rotation_cos);
                vec3 env_color = texture(cubemaps[nonuniformEXT(global.skybox_cubemap_index)],
                                         env_dir).rgb * global.ibl_intensity;

                // Evaluate full BRDF at light direction
                vec3 H = normalize(V + L);
                float NdotH_e = max(dot(N_shading, H), 0.0);
                float VdotH_e = max(dot(V, H), 0.0);

                float D_e   = D_GGX(NdotH_e, roughness);
                float Vis_e = V_SmithGGX(NdotV, NdotL, roughness);
                vec3  F_e   = F_Schlick(VdotH_e, F0);

                vec3 spec_e = D_e * Vis_e * F_e;
                vec3 diff_e = (1.0 - F_e) * diffuse_color * INV_PI;
                vec3 brdf_val = diff_e + spec_e;

                // Combined multi-lobe BRDF PDF at the light direction
                float local_p_spec = specular_probability(NdotV, F0);
                float pdf_spec = pdf_ggx_vndf(NdotH_e, NdotV, VdotH_e, roughness);
                float pdf_diff = NdotL * INV_PI;
                float brdf_pdf = local_p_spec * pdf_spec + (1.0 - local_p_spec) * pdf_diff;

                // MIS weight (light sampling strategy)
                float pdf_light = env_pdf(L);
                float mis_w = mis_power_heuristic(pdf_light, brdf_pdf);

                nee_radiance += env_color * brdf_val * NdotL * mis_w / max(pdf_light, 1e-7);
            }
        }
    }

    // ---- Multi-lobe BRDF sampling ----
    // Orthonormal basis from shading normal (isotropic BRDF, any basis works)
    vec3 T_basis, B_basis;
    if (abs(N_shading.z) < 0.999) {
        T_basis = normalize(cross(vec3(0.0, 0.0, 1.0), N_shading));
    } else {
        T_basis = normalize(cross(vec3(1.0, 0.0, 0.0), N_shading));
    }
    B_basis = cross(N_shading, T_basis);

    // Random numbers for lobe selection and sampling
    uint dim_base = 2u + payload.bounce * DIMS_PER_BOUNCE;
    ivec2 pixel = ivec2(gl_LaunchIDEXT.xy);

    float rand_lobe = rand_pt(dim_base, pc.sample_count, pixel,
                              pc.frame_seed, pc.blue_noise_index);
    float rand_xi0  = rand_pt(dim_base + 1u, pc.sample_count, pixel,
                              pc.frame_seed, pc.blue_noise_index);
    float rand_xi1  = rand_pt(dim_base + 2u, pc.sample_count, pixel,
                              pc.frame_seed, pc.blue_noise_index);

    float p_spec = specular_probability(NdotV, F0);

    vec3 next_dir;
    vec3 throughput_update;
    float brdf_pdf_combined; // Full multi-lobe PDF for env MIS

    if (rand_lobe < p_spec) {
        // ---- Specular lobe: GGX VNDF importance sampling ----
        vec3 Ve = vec3(dot(V, T_basis), dot(V, B_basis), dot(V, N_shading));
        vec3 H_ts = sample_ggx_vndf(Ve, roughness, vec2(rand_xi0, rand_xi1));
        vec3 L_ts = reflect(-Ve, H_ts);

        if (L_ts.z <= 0.0) {
            // Sampled direction below surface — terminate path
            payload.color = emissive + nee_radiance;
            payload.throughput_update = vec3(0.0);
            payload.hit_distance = -1.0;
            payload.env_mis_weight = 1.0;
            return;
        }

        next_dir = T_basis * L_ts.x + B_basis * L_ts.y + N_shading * L_ts.z;

        float NdotL = L_ts.z;
        float NdotH = max(H_ts.z, 0.0);
        float VdotH = max(dot(Ve, H_ts), 0.0);

        float D   = D_GGX(NdotH, roughness);
        float Vis = V_SmithGGX(NdotV, NdotL, roughness);
        vec3  F   = F_Schlick(VdotH, F0);
        float pdf = pdf_ggx_vndf(NdotH, NdotV, VdotH, roughness);

        // Weight = BRDF * cos(theta) / PDF / lobe_probability
        throughput_update = (D * Vis * F * NdotL) / max(pdf * p_spec, 1e-7);

        // Combined multi-lobe PDF: specular + diffuse contribution at this direction
        brdf_pdf_combined = p_spec * pdf + (1.0 - p_spec) * (NdotL * INV_PI);
    } else {
        // ---- Diffuse lobe: cosine-weighted hemisphere sampling ----
        vec3 L_ts = sample_cosine_hemisphere(vec2(rand_xi0, rand_xi1));
        next_dir = T_basis * L_ts.x + B_basis * L_ts.y + N_shading * L_ts.z;

        // Weight simplification:
        // BRDF = diffuse_color * INV_PI
        // PDF  = cos(theta) * INV_PI
        // weight = BRDF * cos / PDF / (1 - p_spec) = diffuse_color / (1 - p_spec)
        throughput_update = diffuse_color / (1.0 - p_spec);

        // Combined multi-lobe PDF: evaluate specular PDF at diffuse-sampled direction
        float NdotL_d = max(dot(N_shading, next_dir), 1e-4);
        vec3 H_d = normalize(V + next_dir);
        float NdotH_d = max(dot(N_shading, H_d), 0.0);
        float VdotH_d = max(dot(V, H_d), 0.0);
        float pdf_spec_d = pdf_ggx_vndf(NdotH_d, NdotV, VdotH_d, roughness);
        brdf_pdf_combined = p_spec * pdf_spec_d + (1.0 - p_spec) * (NdotL_d * INV_PI);
    }

    // ---- Precompute env MIS weight for potential miss (BRDF strategy) ----
    payload.env_mis_weight = mis_power_heuristic(brdf_pdf_combined, env_pdf(next_dir));

    // ---- Write payload ----
    payload.color = emissive + nee_radiance;
    payload.next_origin = offset_pos;
    payload.next_direction = next_dir;
    payload.throughput_update = throughput_update;
    payload.hit_distance = gl_HitTEXT;
}
