/**
 * @file probe_grid.glsl
 * @brief 3D probe grid CSR data access helpers.
 *
 * Requires bindings.glsl (ProbeGridBuffer at set 0 binding 10).
 * The 5x5x5 traversal loop lives in forward.frag (inline top-2 maintenance,
 * no fixed-size collection array — safe regardless of per-cell probe count
 * after relocation).
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
