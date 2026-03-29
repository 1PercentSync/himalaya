/**
 * @file transform.glsl
 * @brief Spatial transformation utilities.
 */

#ifndef TRANSFORM_GLSL
#define TRANSFORM_GLSL

/**
 * Rotate a direction around the Y axis by a given angle.
 *
 * @param d Direction vector.
 * @param s Sine of the rotation angle.
 * @param c Cosine of the rotation angle.
 * @return Rotated direction vector.
 */
vec3 rotate_y(vec3 d, float s, float c) {
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

#endif // TRANSFORM_GLSL
