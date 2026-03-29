/**
 * @file noise.glsl
 * @brief Per-pixel noise utilities.
 */

#ifndef NOISE_GLSL
#define NOISE_GLSL

/**
 * Interleaved gradient noise (Jorge Jimenez, Call of Duty: Advanced Warfare).
 *
 * Produces a deterministic pseudo-random value in [0, 1) based on pixel
 * position.  Suitable for per-pixel jitter (Poisson Disk rotation, search
 * direction randomization, temporal dithering).
 *
 * @param screen_pos Pixel coordinate (integer or fractional).
 * @return Noise value in [0, 1).
 */
float interleaved_gradient_noise(vec2 screen_pos) {
    return fract(52.9829189 * fract(dot(screen_pos, vec2(0.06711056, 0.00583715))));
}

/**
 * Temporally varying interleaved gradient noise.
 *
 * Offsets the noise pattern each frame so TAA can accumulate diverse
 * samples across frames, effectively multiplying the sample count.
 *
 * @param screen_pos Pixel coordinate (integer or fractional).
 * @param frame      Frame counter for temporal variation.
 * @return Noise value in [0, 1).
 */
float interleaved_gradient_noise(vec2 screen_pos, uint frame) {
    screen_pos += float(frame) * 5.588238;
    return fract(52.9829189 * fract(dot(screen_pos, vec2(0.06711056, 0.00583715))));
}

#endif // NOISE_GLSL
