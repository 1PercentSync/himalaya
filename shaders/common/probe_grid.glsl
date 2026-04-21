/**
 * @file probe_grid.glsl
 * @brief 3D probe grid spatial query utilities.
 *
 * Requires bindings.glsl (ProbeGridBuffer at set 0 binding 10).
 */

/**
 * @brief Retrieves a cell offset from the CSR prefix-sum array.
 *
 * grid_data[] layout: [0..cell_count] = cell_offsets, followed by probe_indices.
 */
uint grid_cell_offset(uint idx) {
    return grid_data[idx];
}

/**
 * @brief Retrieves a probe index from the flat probe_indices region.
 *
 * @param base      Start of probe_indices within grid_data (= cell_count + 1).
 * @param local_idx Offset within probe_indices.
 */
uint grid_probe_index(uint base, uint local_idx) {
    return grid_data[base + local_idx];
}

/**
 * @brief Computes the flat cell index from 3D cell coordinates.
 *
 * Layout: x + y * dims.x + z * dims.x * dims.y (row-major XYZ).
 */
uint grid_flat_index(ivec3 cell, uvec3 dims) {
    return uint(cell.x) + uint(cell.y) * dims.x + uint(cell.z) * dims.x * dims.y;
}

/**
 * @brief Iterates probes in a 5x5x5 neighborhood around a world-space position.
 *
 * Writes up to max_results probe indices into out_indices and their squared
 * distances into out_dist_sq. Returns the number of probes found.
 *
 * Caller is responsible for further filtering (normal hemisphere, scoring, top-K).
 *
 * @param world_pos     Fragment world-space position.
 * @param out_indices   Output array for probe indices (caller-sized).
 * @param out_dist_sq   Output array for squared distances (parallel to out_indices).
 * @param max_results   Maximum number of results to write.
 * @return Number of probes found in the neighborhood.
 */
uint query_nearby_probes(vec3 world_pos,
                         out uint out_indices[125],
                         out float out_dist_sq[125]) {
    vec3 grid_origin = grid_origin_and_cell_size.xyz;
    float cell_size  = grid_origin_and_cell_size.w;
    uvec3 dims       = grid_dims_and_pad.xyz;

    uint cell_count = dims.x * dims.y * dims.z;
    uint probe_base = cell_count + 1;

    ivec3 center = clamp(
        ivec3(floor((world_pos - grid_origin) / cell_size)),
        ivec3(0),
        ivec3(dims) - 1
    );

    uint count = 0;

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                ivec3 cell = clamp(center + ivec3(dx, dy, dz), ivec3(0), ivec3(dims) - 1);
                uint flat = grid_flat_index(cell, dims);

                uint begin_off = grid_cell_offset(flat);
                uint end_off   = grid_cell_offset(flat + 1);

                for (uint i = begin_off; i < end_off; ++i) {
                    if (count >= 125u) { return count; }
                    uint pi = grid_probe_index(probe_base, i);
                    vec3 diff = probes[pi].position - world_pos;
                    out_indices[count]  = pi;
                    out_dist_sq[count]  = dot(diff, diff);
                    ++count;
                }
            }
        }
    }

    return count;
}
