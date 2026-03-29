/**
 * @file depth.glsl
 * @brief Depth utility functions.
 *
 * Depends on bindings.glsl being included first (GlobalUBO).
 */

#ifndef DEPTH_GLSL
#define DEPTH_GLSL

/**
 * Convert raw Reverse-Z depth to positive linear view-space distance.
 *
 * Derivation from the perspective projection matrix P:
 *   depth = (P[2][2] * vz + P[3][2]) / (-vz)
 *   => linear_distance = -vz = P[3][2] / (depth + P[2][2])
 *
 * @param d Raw depth value (Reverse-Z: near=1, far=0).
 * @return Positive linear view-space distance from camera.
 */
float linearize_depth(float d) {
    return global.projection[3][2] / (d + global.projection[2][2]);
}

#endif // DEPTH_GLSL
