/**
 * @file gs_common.glsl
 * @brief Shared Gaussian Splatting descriptor and push-constant declarations.
 *
 * Callers should include common/bindings.glsl before this file when they need
 * GlobalUBO camera matrices, camera position, or screen size.
 */

#ifndef GS_COMMON_GLSL
#define GS_COMMON_GLSL

// ---- Scene metadata values ----

const uint GS_COLOR_SPACE_SRGB_REC709_DISPLAY = 0u;
const uint GS_COLOR_SPACE_LIN_REC709_DISPLAY = 1u;

const uint GS_MAX_SH_DEGREE = 3u;

// ---- Set 3 persistent descriptor bindings ----

const uint GS_BINDING_POSITION_RADIUS = 0u;
const uint GS_BINDING_COVARIANCE_OPACITY = 1u;
const uint GS_BINDING_SH_COEFFICIENTS = 2u;
const uint GS_BINDING_VISIBLE_COUNT = 3u;
const uint GS_BINDING_PROJECTED_DATA = 4u;
const uint GS_BINDING_SORT_ENTRIES = 5u;
const uint GS_BINDING_SORT_ENTRIES_SCRATCH = 6u;
const uint GS_BINDING_INDIRECT_DRAW = 7u;

// ---- GPU layouts ----

/** Static baked world covariance plus opacity, matching C++ std430 layout. */
struct GaussianSplatCovarianceOpacity {
    vec4 covariance0;          // x/y/z/w = covariance xx/xy/xz/yy
    vec4 covariance1_opacity;  // x/y/z/w = covariance yz/zz/opacity/unused
};

/** Projected splat data consumed by GS draw shaders, matching C++ std430 layout. */
struct GaussianSplatProjectedData {
    vec4 center_opacity;       // xy = center_px, z = opacity, w unused
    vec4 axis0_axis1;          // xy = axis0_extent_px, zw = axis1_extent_px
    vec4 conic;                // xyz = inverse covariance xx, xy, yy; w unused
    vec4 rgb;                  // xyz = SH-evaluated RGB, w unused
};

/** Sort entry payload, matching C++ std430 layout. */
struct GaussianSplatSortEntry {
    uint distance_key;         // floatBitsToUint(camera_distance_squared)
    uint global_splat_index;   // payload index into global GS buffers
};

/** VkDrawIndirectCommand-compatible command data. */
struct GaussianSplatDrawIndirectCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

// ---- Push constants ----

layout (push_constant) uniform GSPushConstantBlock {
    uint total_splat_count;
    uint sort_capacity;
    uint color_space;
    uint max_sh_degree;
    float near_gs;
    float max_projected_extent_px;
    float alpha_discard_threshold;
    float power_discard_threshold;
} gs_pc;

/** Returns the packed vec4 stride for one splat's SH coefficients. */
uint gs_sh_packed_vec4_stride() {
    if (gs_pc.max_sh_degree == 0u) {
        return 1u;
    }
    if (gs_pc.max_sh_degree == 1u) {
        return 3u;
    }
    if (gs_pc.max_sh_degree == 2u) {
        return 7u;
    }
    return 12u;
}

// ---- Set 3: static baked buffers + work buffers ----

#ifndef GS_WORK_BUFFER_QUALIFIER
#define GS_WORK_BUFFER_QUALIFIER
#endif

layout (set = 3, binding = 0) readonly buffer GSPositionRadiusBuffer {
    vec4 gs_position_radius[]; // xyz = world position, w = world 3-sigma radius
};

layout (set = 3, binding = 1) readonly buffer GSCovarianceOpacityBuffer {
    GaussianSplatCovarianceOpacity gs_covariance_opacity[];
};

layout (set = 3, binding = 2) readonly buffer GSSHCoefficientBuffer {
    vec4 gs_sh_coefficients[]; // indexed by global_splat_index * sh_packed_vec4_stride + local_vec4
};

layout (set = 3, binding = 3) GS_WORK_BUFFER_QUALIFIER buffer GSVisibleCountBuffer {
    uint gs_visible_count;
};

layout (set = 3, binding = 4) GS_WORK_BUFFER_QUALIFIER buffer GSProjectedDataBuffer {
    GaussianSplatProjectedData gs_projected_data[];
};

layout (set = 3, binding = 5) GS_WORK_BUFFER_QUALIFIER buffer GSSortEntriesBuffer {
    GaussianSplatSortEntry gs_sort_entries[];
};

layout (set = 3, binding = 6) GS_WORK_BUFFER_QUALIFIER buffer GSSortEntriesScratchBuffer {
    GaussianSplatSortEntry gs_sort_entries_scratch[];
};

layout (set = 3, binding = 7) GS_WORK_BUFFER_QUALIFIER buffer GSIndirectDrawBuffer {
    GaussianSplatDrawIndirectCommand gs_indirect_draw;
};

#endif // GS_COMMON_GLSL
