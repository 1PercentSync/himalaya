# 当前阶段：Reflector Phase 3.0 — Gaussian Splatting 基础渲染

> 目标：正确渲染 GS 场景的最小可行实现。Phase 3.0 是 correctness baseline，不以最终性能为目标；happy path 为 KHR ellipse + perspective + cameraDistance + scene-level consistent metadata + identity/no-rotation transform。
> 整体路线与子阶段规划见 `docs/phase3-decisions.md`。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 背景

Phase 2 完成了 GS 数据管线（PLY → glTF 转换 + GS glTF 加载），CPU 端 SoA 数据结构已就绪。Phase 3.0 构建从 CPU 数据到屏幕的完整渲染路径——数据 bake + 上传、计算管线（剔除 + 投影 + 排序）、硬件光栅输出。

## 范围

### 新增

- **GPU 数据上传**：CPU bake（transform 烘焙、3D 协方差预计算；SH 前期走 identity/no-rotation happy path，Phase 3.0 末期补 Wigner-D rotation）+ per-attribute GPU buffer 创建
- **Cull/Project Compute Pass**：视锥剔除、3D→2D 投影、OBB extent、SH 求值、可见列表构建、排序键生成
- **Sort Compute Pass**：Bitonic sort（正确性验证）→ 自实现 Radix sort（最终方案），排序项为 uint distance_key + uint global_splat_index；Radix sort 仅处理 32-bit distance_key
- **Quad Rendering**：Instanced quad indirect draw + premultiplied-under 硬件 blend
- **管线集成**：GS 持久 Set 3 descriptor set、push constant、RenderMode 分发、TonemappingPass 复用

### 不在范围内

- Mixed metadata / mixed colorSpace scene
- 非 identity transform rotation 的 SH rotation（Phase 3.0 末期补完整 Wigner-D 前）
- Compact projected data
- 10M splat 性能目标
- Max-channel range compression
- Background / mesh / skybox 合成
- 运行时优化（SH 分离 pass、多级剔除、buffer 分层等，Phase 3.1）
- 加载时优化（空间分块、FP16 量化、Morton 排序等，Phase 3.2）
- Tile-based compute 渲染（Phase 3.5）

## 管线流程

Per-frame parameter ownership:

    GlobalUBO → view / projection / view_projection / camera_position / screen_size
    GSPushConstants → total_splat_count / sort_capacity / color_space / flags / near_gs /
                      max_projected_extent_px / alpha_discard_threshold / power_discard_threshold
    TonemappingPass push constant → output mode (HdrAces / LinearClamp)

Upload（CPU bake → GPU buffers → GS Set 3）:

    position    → apply transform → world space
    covariance  → quat + scale(σ) → R·diag(scale²)·Rᵀ → M3x3·Σ_local·M3x3ᵀ → Σ_world (6 floats)
    cull radius → world_radius_3sigma = 3 * sqrt(max(trace(Σ_world), 0))
    SH          → early happy path: identity/no-rotation transform 直接上传；Phase 3.0 末期补 Wigner-D rotation
    opacity     → 直传
    primitive metadata → scene-level consistency check:
      - kernel = ellipse
      - projection = perspective
      - sortingMethod = cameraDistance
      - colorSpace consistent across all primitives
    validation  → opacity finite [0,1], scale finite >=0, rotation finite unit quaternion, node transform decomposable/proper
    indexing    → concatenate primitives into global splat buffers; keep CPU per-primitive ranges

GS Set 3 是 GS 子系统的持久 descriptor set，包含两类 SSBO：

    static baked scene buffers：position / covariance / opacity / SH / optional/reserved primitive metadata
    GPU work buffers：visible count / dense-by-global-index projected data / sort ping-pong / indirect draw command

Projected data 逻辑字段：

    center_px          : splat center in screen pixel space
    axis0_extent_px    : OBB axis 0 with 3σ extent in pixels
    axis1_extent_px    : OBB axis 1 with 3σ extent in pixels
    conic              : inverse 2D covariance coefficients (xx, xy, yy)
    opacity            : splat opacity
    rgb                : SH-evaluated color in primitive colorSpace

实际 GPU struct 按 std430 / vec4 packing 实现。

Descriptor 随 buffer 创建或重建写入；每帧变化的是 GPU work buffer 内容，不是 descriptor 本身。`capacity` 由当前 GS scene 的 `total_splat_count` 派生，`sort_capacity = next_power_of_two(total_splat_count)`，随场景重建，不是固定上限；若当前场景所需 buffer 无法分配，则加载/渲染失败并报错，不静默截断。

Resource lifetime:

    scene load/reload → rebuild static baked buffers + capacity-based work buffers, rewrite GS Set 3
    swapchain resize  → rebuild viewport-sized GS composition/linear targets, update render target descriptors
    projected_data/work buffers are not rebuilt on resize unless total_splat_count changes
    projected_data contents depend on viewport but are overwritten by next cull/project pass
    descriptors must never point to destroyed buffers/images after reload/resize

RenderGraph requirement:

    current RG only auto-barriers images; buffer usages are skipped in compile()
    Phase 3.0 must extend RG buffer barriers before GS compute/sort/draw integration
    required mappings: Compute SSBO read/write, Vertex/Fragment SSBO read, DrawIndirect read, Transfer read/write
    prefer reusable RG buffer barriers over long-term manual barriers inside GS passes

Per-frame work buffer reset:

    visible_count = 0
    sort entries = invalid sentinel {UINT_MAX, UINT_MAX}
    indirect.instanceCount = 0

Indirect command fixed fields are CPU-initialized:

    vertexCount = 6
    firstVertex = 0
    firstInstance = 0

GPU only updates instanceCount from visible_count before draw indirect.

Cull/Project Compute（workgroup 256）:

    → frustum cull: world-space sphere(position_world, world_radius_3sigma) vs planes
    → use same camera pose/FOV/aspect/viewport as PT/reference view; GS may use GS-specific near plane
    → center_px from clip/NDC to pixel without Y flip
    → 3D Σ → view-space covariance → pixel-space 2D cov projection
      - fx = 0.5 * width * abs(proj[0][0])
      - fy = 0.5 * height * abs(proj[1][1])
      - cov_2d = J * cov_view * Jᵀ
    → pixel-space OBB extent (3σ cutoff)
    → SH(view_dir camera→splat) → RGB
    → defensive measures
      - discard behind-camera / near-plane unstable splats
      - z clamp only prevents projection NaN/Inf
      - discard if projected OBB half-axis > screen short side * 0.25
    → subgroup ballot visible list append (subgroup ops in uniform control flow)
    → write projected_data[global_splat_index]
    → sort entry (uint distance_key + uint global_splat_index)
      - distance_key = floatBitsToUint(camera_distance_squared)
      - only finite non-negative distances are valid
      - invalid sentinel = {UINT_MAX, UINT_MAX}
      - append valid entries into sort_entries[0..visible_count)
      - ascending sort = front-to-back
      - bitonic compare = lexicographic(distance_key, global_splat_index)
      - deterministic tie-break is required to avoid transparency flicker

Sort strategy:

    Bitonic baseline：sort_capacity 全量排序，invalid entries 用 sentinel 排到末尾
    Radix step 1：先实现同样的 radix capacity sort，与 bitonic baseline 对比
    Radix step 2：确认正确后实现 visible-count-driven radix，减少无效排序工作

Bitonic/Radix Sort Compute → sorted visible list

Indirect command update:

    instanceCount = visible_count

Indirect Draw（instanced quads）:

    → draw indirect reads VkDrawIndirectCommand
    → 6 verts/quad (gl_VertexIndex % 6)
    → GS draw uses positive-height normal viewport (no negative viewport Y-flip)
    → screen pixel space = Vulkan framebuffer coordinates (top-left origin, x right, y down)
    → read sorted_entries[gl_InstanceIndex].splat_index as global_splat_index
    → read projected_data[global_splat_index], expand OBB corners, convert to NDC without Y flip
    → fragment uses gl_FragCoord.xy directly with pixel-space conic/covariance
    → power = -0.5 * mahalanobis_distance; discard if power < -20
    → alpha = clamp(opacity * exp(power), 0, 1); discard if alpha < 1e-4
    → fragment output vec4(rgb * alpha, alpha)
    → front-to-back premultiplied-under blend
      - srcColor/srcAlpha: ONE_MINUS_DST_ALPHA
      - dstColor/dstAlpha: ONE
      - op: ADD
      - colorWriteMask: RGBA
      - target format: R16G16B16A16Sfloat
      - loadOp: CLEAR vec4(0,0,0,0)
      - storeOp: STORE
      - depth test/write off
      - cull none
    → R16G16B16A16Sfloat GS composed output（仍处于 primitive colorSpace）

GS color conversion → linear output:

    srgb_rec709_display → GS composition target stores sRGB blended premultiplied RGB + alpha
                         → sRGB→linear conversion pass converts RGB only and preserves alpha
                         → writes linear target
    lin_rec709_display  → GS composition target already stores linear premultiplied RGB + alpha
                         → bypass conversion pass, use composition target as linear target

TonemappingPass（final swapchain output pass）:

    mode = HdrAces      → PT path: linear HDR * exposure → ACES → alpha = 1
    mode = LinearClamp  → GS path: linear display-referred input → hard clamp [0,1] → alpha = 1

Mode is a TonemappingPass push constant, not GlobalUBO. TonemappingPass does not perform GS sRGB decode.

## 实现步骤

共 6 个 Step，详见 `tasks/reflector-phase3.md`。

| Step | 内容 | 说明 |
|------|------|------|
| 0 | GPU 数据上传 | CPU bake + GPU buffer 创建 + GS 持久 Set 3 descriptor set |
| 1 | Cull/Project Compute | 视锥剔除 + 2D 投影 + SH 求值 + 可见列表 + 排序键 |
| 2 | Bitonic Sort | 2×32-bit sort entry，按 sort_capacity 排序（正确性验证用） |
| 3 | Quad Rendering | Instanced quad + 硬件 blend + render target |
| 4 | 管线集成 | RenderMode 分发 + GS sRGB→linear conversion path（linear GS bypass）+ Tonemapping bypass + 端到端验证（投影/OBB/排序/blend/color conversion/resize/empty visible set/invalid asset rejection） |
| 5 | Radix Sort + SH rotation 完整化 | 先实现 radix capacity sort 替换 bitonic，再实现 visible-count-driven radix，最后补完整 Wigner-D SH rotation |
