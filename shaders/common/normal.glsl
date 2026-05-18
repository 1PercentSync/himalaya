/**
 * @file normal.glsl
 * @brief Normal map utilities — TBN construction and tangent-space normal decoding.
 *
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
    // Decode tangent-space XY from [0,1] to [-1,1], reconstruct Z from BC5 RG.
    vec2 xy = normal_rg * 2.0 - 1.0;
    xy *= normal_scale;
    float z = sqrt(max(0.0, 1.0 - dot(xy, xy)));
    vec3 ts_normal = vec3(xy, z);

    // Degenerate tangent guard: skip TBN if tangent is zero-length.
    float tangent_len = length(tangent.xyz);
    if (tangent_len < 0.001) {
        return N;
    }

    // Construct TBN matrix.
    vec3 T = tangent.xyz / tangent_len;
    vec3 B = cross(N, T) * tangent.w;
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * ts_normal);
}

#endif // NORMAL_GLSL
