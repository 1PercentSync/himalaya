/**
 * @file normal.glsl
 * @brief Normal map utilities — TBN construction, normal map decoding,
 *        R10G10B10A2 UNORM world-space encoding/decoding.
 *
 * Shared by depth_prepass and forward shaders.
 * Does NOT depend on bindings.glsl — callers provide pre-sampled values.
 */

#ifndef NORMAL_GLSL
#define NORMAL_GLSL

/**
 * Get world-space shading normal from a normal map sample.
 *
 * Constructs TBN matrix from geometric normal and vertex tangent,
 * decodes the tangent-space normal map sample, and transforms to world-space.
 * Falls back to geometric normal when tangent is degenerate (zero-length).
 *
 * @param N             Normalized geometric normal (world-space)
 * @param tangent       Vertex tangent (xyz = direction, w = handedness sign)
 * @param normal_rg     Normal map RG texel ([0,1] encoded, from BC5 texture)
 * @param normal_scale  Normal map intensity scale (glTF normalTexture.scale)
 * @return Normalized world-space shading normal
 */
vec3 get_shading_normal(vec3 N, vec4 tangent, vec2 normal_rg, float normal_scale) {
    // Decode tangent-space XY from [0,1] to [-1,1], reconstruct Z from BC5 RG
    vec2 xy = normal_rg * 2.0 - 1.0;
    xy *= normal_scale;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    vec3 ts_normal = vec3(xy, z);

    // Degenerate tangent guard: skip TBN if tangent is zero-length
    float tangent_len = length(tangent.xyz);
    if (tangent_len < 0.001) {
        return N;
    }

    // Construct TBN matrix
    vec3 T = tangent.xyz / tangent_len;
    vec3 B = cross(N, T) * tangent.w;
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * ts_normal);
}

/**
 * Encode world-space normal to R10G10B10A2 UNORM format.
 *
 * Maps [-1,1] to [0,1] via n * 0.5 + 0.5. This linear mapping is
 * compatible with MSAA AVERAGE resolve — averaging encoded values
 * produces the correct average normal (after decode + normalize).
 *
 * A channel is set to 1.0 (reserved for future material flags).
 *
 * @param n Normalized world-space normal
 * @return  Encoded value suitable for R10G10B10A2_UNORM render target
 */
vec4 encode_normal_r10g10b10a2(vec3 n) {
    return vec4(n * 0.5 + 0.5, 1.0);
}

/**
 * Decode R10G10B10A2 UNORM back to world-space normal.
 *
 * Maps [0,1] to [-1,1] via n * 2.0 - 1.0, then normalizes to
 * restore unit length (necessary after MSAA AVERAGE resolve).
 *
 * @param encoded R10G10B10A2 texel (only rgb used)
 * @return Normalized world-space normal
 */
vec3 decode_normal_r10g10b10a2(vec4 encoded) {
    return normalize(encoded.rgb * 2.0 - 1.0);
}

#endif // NORMAL_GLSL
