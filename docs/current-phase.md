# 当前阶段：Reflector Phase 3.0 — Gaussian Splatting 基础渲染

> 目标：正确渲染 GS 场景的最小可行实现。Phase 3.0 是 correctness baseline，不以最终性能为目标。
> 整体路线与跨阶段决策见 `docs/phase3-decisions.md`。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 背景

Phase 2 已完成 GS 数据管线（PLY → glTF 转换 + GS glTF 加载）和 CPU 端 SoA 数据结构。Phase 3.0 负责从 CPU GS 数据到屏幕的第一条可渲染路径：upload-time bake、GPU buffer / descriptor 资源、compute cull/project/sort、instanced quad 硬件光栅和最终输出。

## 范围

### 包含

- Upload-time bake：world position、world covariance、3σ cull radius、SH happy path。
- GS 持久 Set 3：static baked buffers + capacity-based work buffers。
- RenderGraph buffer barrier 扩展：GS compute/sort/draw 所需 buffer hazard 自动同步。
- Cull/Project compute：视锥剔除、2D pixel-space 投影、OBB、SH 求值、visible append、sort entry 生成。
- Bitonic sort correctness baseline；Phase 3.0 末期再接 deterministic compact / visible-count-driven 4-bit radix。
- Instanced quad draw + front-to-back premultiplied-under blend。
- GS color conversion path + TonemappingPass `LinearClamp` mode。

### 不包含

- Mixed metadata / mixed colorSpace scene。
- 非 identity transform rotation 的 SH rotation（Phase 3.0 末期补完整 Wigner-D 前）。
- Compact projected data。
- 10M splat 性能目标。
- Max-channel range compression。
- Background / mesh / skybox 合成。
- Tile-based compute renderer。

## 实现指南

本节按 `tasks/reflector-phase3.md` 的 Step 顺序组织。任务清单只描述“要做什么”；本节描述“如何做”以及每项必须满足的细节要求。任务状态只在 `tasks/reflector-phase3.md` 维护。每个 checkbox 的复杂度应适中：能独立审查，不把多个不同子系统混为一个小项，也不拆到单行代码级别。

### Step 0：RenderGraph buffer barriers

目标是先补齐 GS 依赖的通用 buffer 同步能力，避免后续每个 GS pass 长期手写 barriers。

- RenderGraph compile 阶段需要停止跳过 buffer resource usage，并为每个 buffer 记录 last stage / access。当前只需要同一 graphics/compute queue 内同步；queue family ownership 可保持现状。
- Buffer barrier 使用 `VkBufferMemoryBarrier2`，与现有 image barriers 一起提交到 `VkDependencyInfo`。实现时保持 image barrier 路径行为不变。
- Phase 3.0 至少要支持以下 usage 映射：Transfer read/write、Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read。
- 必须能表达以下依赖链：reset→cull/project、cull/project→sort、sort/projected data→graphics shader、visible_count→indirect update、indirect write→draw indirect。
- 验证方式以检查 RG 生成的 barrier 和 Vulkan validation layer 为主；如需要临时 dummy pass，仅作为本地验证，不把无用途测试 pass 留在正式代码中。

### Step 1：GS 数据契约与加载校验

本 Step 固定 CPU/GPU 数据边界，避免后续 shader 与 C++ 结构反复变动。

资源组织必须遵循已有项目模式：GS CPU 数据、GPU shader layout 和 scene-level GPU resource contract 可以集中放在 GS 数据契约文件中；真正的 GPU 资源创建、上传、销毁、descriptor 写入必须集中在 Renderer 持有的 GS scene resource owner 中。各 GS pass 只负责 pipeline、RG resource declaration 和命令录制，不得分散创建/销毁 static/work scene buffers，也不得绕过该 owner 重写持久 GS Set 3。viewport-sized composition / linear targets 仍按现有 RenderGraph managed image / Renderer resize 生命周期管理。

- Scene-level GS GPU resource 需要记录 `total_splat_count`、`sort_capacity`、static/work buffer handles 和 scene metadata；不保留无实际消费的 per-primitive range contract。
- `sort_capacity = next_power_of_two(total_splat_count)`；这是由当前 scene 派生的容量，不是固定上限。容量不足时报错，不静默截断。
- Static baked buffers 以 SoA 组织，逻辑属性至少包含 world position、world covariance、world 3σ cull radius、opacity、SH coefficients；物理布局采用大数据量友好的 packed buffers：`position_radius`（vec4：xyz = world position，w = radius）、`covariance_opacity`（2×vec4：xx/xy/xz/yy、yz/zz/opacity/unused）和 `sh_coefficients`。`sh_coefficients` stride 按 scene-level `max_sh_degree` 派生：degree 0/1/2/3 分别为 1/3/7/12 个 vec4，避免低阶 scene 固定按 degree 3 产生显存和带宽浪费。Phase 3.0 baseline 不做 per-primitive 变长 SH stride，mixed-degree primitive 仍按 scene max degree 打包。
- Projected data 按 global splat index dense 存储，逻辑字段为：

```text
center_px
axis0_extent_px
axis1_extent_px
conic              // inverse 2D covariance xx, xy, yy
opacity
rgb                // SH-evaluated RGB in primitive colorSpace
```

- 实际 GPU struct 按 std430 / vec4 packing 实现；fragment shader 不做 per-pixel matrix inverse。
- 当前 bitonic baseline 的 sort entry 物理格式为 2×32-bit：`distance_key + global_splat_index`。Phase 3.0 radix 首版采用 32-bit `distance_key` stable radix，`global_splat_index` 作为 payload 搬运；equal-key deterministic 顺序由 radix 前的 deterministic visible list 生成保证。Indirect command 固定字段由 CPU 初始化，GPU 只写 `instanceCount`。
- Direct glTF/GLB load path 必须补齐校验：`OPACITY` finite `[0,1]`、`SCALE` finite `>=0`、`ROTATION` finite unit quaternion。
- 同一 GS scene 内所有 primitive metadata 必须一致：`kernel=ellipse`、`projection=perspective`、`sortingMethod=cameraDistance`、`colorSpace` 一致。
- 非法 glTF 按 KHR 语义报错，不静默 clamp、normalize 或混合渲染。PLY 转换路径产生有效数据，不替代直接加载校验。

### Step 2：Upload-time bake 与 static buffers

本 Step 生成渲染使用的静态 GPU 数据。Phase 3.0 不在 shader 中每帧处理静态 transform。

- Node global transform 必须可分解为 regular translation、proper rotation 和 positive scale；reflection、negative determinant、shear 或不可分解线性部分一律报错，不跳过 primitive，不静默修正，GS scene 回退到空场景状态。判定容差：translation / 3×3 linear 全部 finite；列向量 scale finite 且 `> 1e-8`；归一化列向量点乘绝对值 `<= 1e-3`；归一化矩阵 determinant 满足 `abs(det - 1) <= 1e-3`。
- Position bake 使用 node global transform 把 local position 转到 world space。
- Covariance bake：KHR `SCALE` 是 Gaussian principal axes 的 σ：
  - `Σ_local = R * diag(scale²) * Rᵀ`
  - `Σ_world = M3x3 * Σ_local * M3x3ᵀ`
  - GPU 存 symmetric 3×3 的 6 个 float。
- `world_radius_3sigma = 3 * sqrt(max(lambda_max(Σ_world), 0))`，作为 world-space frustum sphere cull 半径；`lambda_max` 为 symmetric covariance 的最大特征值。该值在 upload-time 预计算，避免每帧 shader 重复求解。
- SH upload 前期只支持不需要 Wigner-D 的 happy path：node proper rotation 近似 identity 时直接上传；node rotation 非 identity 但 scene/primitive `max_sh_degree == 0` 时允许；node rotation 非 identity 且存在 degree 1-3 SH 时必须报错并回退空 GS scene。该拦截属于 Renderer/GS builder 的能力限制，应在 CPU preflight / static buffer upload 前完成，避免失败场景进入 GPU buffer 创建上传。完整 Wigner-D 放到 Step 13。
- 多个 primitive 按顺序拼接成 global splat buffers；Phase 3.0 不保留 per-primitive range contract，报错定位使用当前遍历的 primitive/local splat index，shader 仅依赖 global splat index。
- GPU static buffers 不保留 raw rotation / scale 作为渲染必需数据；CPU 侧可保留用于 reload/debug/future rebake。GS GPU scene resource owner 仿照现有 PT 路径的 `SceneASBuilder` / `EmissiveLightBuilder` 模式：Loader 只产出 CPU scene，Renderer 持有 builder/owner，Application 只负责 scene switch orchestration 和 immediate scope。

### Step 3：GS descriptors、work buffers 与 reset

本 Step 建立 GS 渲染运行时资源，但还不要求产生可见列表。

- GS 使用独立持久 Set 3 descriptor set，绑定 static baked buffers 和 work buffers。它不是 PT / compute pass 的 push descriptor Set 3。
- Set 3 descriptor layout / pipeline layout 生命周期按 PT 模式归属于 renderer-lifetime pass / pipeline owner；GS scene resource owner 只管理 scene buffers、descriptor set allocation/write/rewrite，不引入 `shutdown()` 式双重销毁语义。
- Descriptor 随 scene load/reload 或 buffer recreate 写入；每帧只更新 buffer 内容，不每帧 push descriptor。
- `GSPushConstants` 只放 GS 专用小参数：`total_splat_count`、`sort_capacity`、`color_space`、`max_sh_degree`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`。`max_sh_degree` 来自 scene-level metadata，shader 由该字段推导 `sh_coefficients` packed vec4 stride（degree 0/1/2/3 = 1/3/7/12）。
- View/projection/view_projection/camera_position/screen_size 复用 GlobalUBO，不在 GS push constants 中重复矩阵。
- Work buffers 至少包含 visible count atomic、dense-by-global-index projected data、sort entry ping-pong、indirect draw command；容量全部由 `total_splat_count` / `sort_capacity` 派生。
- 每帧 reset 必须发生在 cull/project 前：`visible_count = 0`、sort entries 填 `{UINT_MAX, UINT_MAX}`、`indirect.instanceCount = 0`。
- `VkDrawIndirectCommand` 固定字段由 CPU 初始化：`vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0`。GPU 只在 cull/project 后把 `visible_count` 写入 `instanceCount`。

### Step 4：Cull/Project compute pass

本 Step 产生 projected data、visible range、sort entries 和 draw indirect instance count。

- Shader skeleton 先接通 Set 3、GlobalUBO 和 push constants，workgroup size 初始使用 256。
- World-space cull 使用 sphere(center=`position_world`, radius=`world_radius_3sigma`) vs frustum planes。
- 投影防御包括 behind-camera discard、near-plane unstable discard 和 projection NaN/Inf 防御。Projection z clamp 只用于防止 NaN/Inf，不用于强行保留贴脸 splat。
- `center_px` 由 camera/projection NDC 转 pixel，使用 framebuffer 坐标：top-left origin、x right、y down；由于相机/PT NDC 为 Y-up，而 Vulkan positive-height viewport 的屏幕方向为 Y-down，NDC→pixel 在 GS cull/project 中执行一次 Y flip。
- 2D covariance 使用 view-space covariance 和 pixel focal length：
  - `fx = 0.5 * width * abs(proj[0][0])`
  - `fy = 0.5 * height * abs(proj[1][1])`
  - `cov_2d = J * cov_view * Jᵀ`
- 对 `cov_2d` 做正定化防御，再求 inverse covariance / conic 和 3σ OBB extent。Projected OBB 任一半轴超过 screen short side × 0.25 时 discard。
- SH 求值方向为 camera → splat：`normalize(splat_world_position - camera_world_position)`；长度接近 0 时使用 camera forward fallback。
- 对每个可见 splat 求一次 all-degree SH RGB，负分量 clamp 到 0，写入 projected data；fragment shader 不求 SH。
- 可见 splat append 到 `sort_entries[0..visible_count)`；`distance_key = floatBitsToUint(camera_distance_squared)`，只接受 finite non-negative distance，异常值写 sentinel。
- Subgroup intrinsic 必须位于 uniform control flow 中，不放入 divergent branch。Cull/project 结束时将 `visible_count` 写入 indirect command 的 `instanceCount`。

### Step 5：Bitonic sort correctness baseline

Bitonic sort 用于建立 deterministic correctness baseline，后续 radix 必须与其对比。

- Bitonic 按 `sort_capacity` 全量排序；invalid sentinel 应自然排到末尾。
- Compare 使用 lexicographic `(distance_key, global_splat_index)` ascending；ascending 等价于 front-to-back。
- Bitonic baseline 采用 in-place compare-and-swap，最终结果始终落在 primary `sort_entries` buffer；`sort_entries_scratch` 在本 Step 中有意不使用，保留给后续 radix / ping-pong scatter 路径。shader 与 C++ sort pass 代码实现时必须注释说明该例外，避免误判为遗漏。
- Dispatch orchestration 使用 `N = sort_capacity`，按 bitonic `log2(N)` stages × `log(N)` steps 执行。为避免 1M splat 时产生约 210 个 RenderGraph pass，sort 作为单个 RG pass 录制完整多 dispatch 序列，并在每个 dispatch step 之间手动插入 compute SSBO buffer barrier（compute write → compute read/write）。这是 Phase 3.0 bitonic correctness baseline 的局部同步例外；跨 pass 依赖仍交给 RenderGraph。
- Bitonic sort 使用独立的 sort push constants（至少包含 `sort_capacity`、`stage_k`、`step_j`），不把 sort step 临时参数加入通用 `GSPushConstants` / `gs_common.glsl` 渲染参数契约。
- 排序后 `[0, visible_count)` 必须全为 valid entries；draw 使用 `visible_count`，不 draw capacity。
- Equal-key ordering 必须 deterministic，避免透明累积因帧间顺序变化闪烁。

### Step 6：Quad rendering path

本 Step 完成 GS composition target 的硬件光栅输出。

- 使用 non-indexed instanced draw，每个 splat 6 vertices，`gl_VertexIndex % 6` 展开两个三角形。
- Vertex shader 通过 sorted entry 读取 `global_splat_index`，再读取 `projected_data[global_splat_index]`。
- Quad corner：`center_px + sx * axis0_extent_px + sy * axis1_extent_px`。Pixel → Vulkan NDC 使用 positive-height viewport 的正常映射，不做第二次 Y flip。
- Fragment shader 直接使用 `gl_FragCoord.xy - center_px`，与 pixel-space conic 保持同一坐标系。

```glsl
float mahalanobis =
    conic.x * d.x * d.x +
    2.0 * conic.y * d.x * d.y +
    conic.z * d.y * d.y;
float power = -0.5 * mahalanobis;
```

- `power < -20` discard；`alpha = clamp(opacity * exp(power), 0, 1)`；`alpha < 1e-4` discard。
- Fragment 输出 `vec4(rgb * alpha, alpha)`；SH-evaluated RGB 负分量应已在 cull/project 中 clamp 到 0。
- GS composition target 使用 R16G16B16A16Sfloat，`loadOp = CLEAR`，clear value 为 `vec4(0,0,0,0)`，`storeOp = STORE`。
- Pipeline state：depth test/write disabled，cull none，colorWriteMask=RGBA，front-to-back premultiplied-under blend（srcColor/srcAlpha=`ONE_MINUS_DST_ALPHA`，dstColor/dstAlpha=`ONE`，op=`ADD`）。
- Swapchain resize 只重建 viewport-sized composition/linear targets，并更新对应 descriptors；work buffers 不因 resize 重建。

### Step 7：RenderMode 与 output 集成

本 Step 把 GS path 接入 renderer，并保证进入 TonemappingPass 的 GS input 一定是 linear。实现顺序按任务清单拆分，先扩展最终输出 pass，再接入 RenderMode 和 GS path，最后补齐 sRGB→linear conversion。

- TonemappingPass 保持最终 swapchain output pass，并通过 pass-local push constant 显式选择 mode；调用方不得依赖默认 mode。PT path 显式使用 `HdrAces`；GS path 使用 `LinearClamp`，跳过 exposure / tone curve，对 linear display-referred input 做 per-channel hard clamp `[0,1]` 并输出 alpha=1。mode 不放入 GlobalUBO，不新增 pipeline。
- 复用 `framework::RenderMode { PathTracing, GaussianSplatting }` 状态模型，替换 `pt_mode_` 过渡状态并清理 PT-only UI placeholder。
- `Renderer::render()` 按 `RenderMode` 分发 PT / GS；两个 scene 可以独立加载，RenderMode 只控制当前帧走哪条路径；无可渲染场景时走明确 fallback。
- GS projection stability near 值独立于 PT/camera projection near，默认值为 0.25 world units（约 1/4 米）；Rendering 面板的 GS Near 滑块可直接控制 GS near，避免离群 splat 拉大 scene AABB 时产生过远 near 并错误裁切主体内容。
- 新增 `render_gaussian_splatting()` 作为 GS path orchestration，在其中填齐 `GSPushConstants` 的 count、capacity、colorSpace、maxSH、near、extent 与 discard thresholds，并按 reset → cull/project → sort → draw → TonemappingPass 顺序录制；基础接入先支持 `lin_rec709_display` 直接作为 TonemappingPass input。
- `srgb_rec709_display`：composition target 存 sRGB display-referred premultiplied RGB + alpha；sRGB→linear conversion pass 只转换 RGB，alpha 原样保留，输出 linear target 后再进入 TonemappingPass。
- `lin_rec709_display`：composition target 已是 linear，直接作为 TonemappingPass input。
- TonemappingPass 不做 GS sRGB decode；GS 进入 TonemappingPass 前必须已经是 linear。

### Step 8：Phase 3.0 correctness validation

本 Step 是 Phase 3.0 baseline 的验收点，不以最终性能为目标。

- Happy path：KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform。
- Multi-primitive 验证需要覆盖 global buffer 拼接和 shader 仅按 global splat index 访问；不验证也不保留无实际消费的 per-primitive range contract。
- ColorSpace 验证需要分别覆盖 `srgb_rec709_display` conversion 和 `lin_rec709_display` bypass；mixed colorSpace scene 应被拒绝。
- 核心渲染正确性至少覆盖投影中心、3σ OBB、front-to-back sorting、premultiplied-under blend、Tonemapping bypass。
- 生命周期与异常路径至少覆盖 resize、`visible_count = 0`、非法 asset rejection、reload/resize 后 descriptor 不指向已销毁资源。
- 目标是 1M splat 级别 correctness baseline；10M 性能不是 Phase 3.0 完成条件。

### Step 9：GS radix 方案冻结

Step 9 在硬件光栅 correctness baseline 建立后执行，用于冻结 GS radix 首版实现契约，避免后续实现时临时决策。代码实现拆分到 Step 10-12；Wigner-D 拆分到 Step 13。

- RenderGraph indirect command usage 统一命名为 `IndirectCommand`，覆盖 draw indirect 与 dispatch indirect；Vulkan 映射仍为 `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT` + `VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT`。
- Radix sort 首版采用 32-bit `distance_key` stable radix，`global_splat_index` 作为 payload 搬运，不使用 64-bit packed key。首版 digit width 为 4-bit：`bucket_count = 16`、`digit_count = 8`；后续 Phase 3.1 再评估 8-bit radix，不提前分配 256-bucket buffers。
- Equal-key deterministic 顺序通过 radix 前的 deterministic visible list 保证：cull/project 对每个 global splat 全量写 `distance_keys_by_global[i] = visible ? distance_key : UINT_MAX`，其中 `UINT_MAX` 表示 invisible / invalid；projected data 仍按 global splat index dense 写入，invisible splat 的 projected data 未定义。
- Visibility prefix 使用 256 block size 的专用 hierarchical exclusive scan，不引入通用 GPU scan 工具。`visibility_prefix_local` 存 block-local exclusive prefix；多级 sums 打包在 `visibility_scan_buffer` 中，level offsets/counts 由 CPU-side scan level layout helper 计算并通过 push constants 传递。Visibility scan finalization 写 `visible_count` 和 `indirect.instanceCount`。
- Visible compact 按 `total_splat_count` dispatch，现场根据 packed scan levels 计算 original block 的 global offset，并写 `sort_entries[local_prefix + block_offset] = {distance_key, global_splat_index}`。不额外物化完整 block offset buffer。Deterministic compact 完成后先接回 Bitonic 做简略视觉验证，再接入 Radix。
- Radix args 使用独立 `GS Radix Args` pass 生成，读取 GPU 侧 `visible_count`，写 `active_count`、`active_capacity = next_power_of_two(visible_count)`、`block_count`、histogram/scatter indirect dispatch commands，以及每层 radix prefix scan 的 indirect dispatch commands / level counts。`visible_count = 0` 时 `active_capacity = 0`，相关 indirect dispatch 写 zero dispatch。
- Radix histogram / prefix / scatter 在单个 `GS Radix Sort` RG pass 内按 digit 循环录制，digit 内部手写必要 compute-to-compute barriers。Histogram 每个 digit 完整覆盖 active blocks × 16 buckets，不额外 clear；inactive entries 未定义且不得读取。
- Radix histogram 和 block offsets 使用 bucket-major layout：`index = bucket * max_block_count + block`。`radix_histogram` 与 `radix_block_offsets` 分开存储；radix prefix 使用专用 hierarchical exclusive scan，临时 sums 打包在独立 `radix_scan_buffer` 中。`bucket_bases[16]` 放入 `radix_args`，每个 digit prefix 后覆盖同一份 bucket bases。
- Radix scatter 的 local rank 使用 subgroup ballot rank + shared subgroup histogram prefix；shader 使用 `gl_SubgroupSize` 动态处理，不假设固定 subgroup size。Scatter 公式为 `dst = bucket_base[bucket] + block_offset[bucket, block] + local_rank`。
- Radix 以 primary `sort_entries` 为输入，`sort_entries_scratch` 作为 ping-pong scratch。4-bit 共 8 轮，最终结果必须回到 primary `sort_entries`；draw pass 永远读取 primary。
- Debug UI 提供 GS sort mode 运行时切换，默认 Radix，Bitonic 保留为 debug baseline。Bitonic 与 Radix 共用 deterministic compact 输出；Bitonic 仍按 `sort_capacity` 全量排序。Radix / Bitonic 一致性通过 Debug UI 视觉 A/B 对比验证，不实现 GPU exact compare 或 CPU readback。
- Phase 3.1 再评估 workgroup-local compact + group prefix、range-aware reset / tail sentinel 初始化、8-bit radix 等运行时优化；Phase 3.0 首版不提前决定这些演进方式。

#### Step 9 radix implementation details

以下细节仅约束 Step 9 radix 首版实现，不回写 Step 0-8 已完成小项。

Set 3 新增 binding 追加在现有 0-7 之后，编号固定为：

| Binding | Buffer | Element / layout | Size | Usage | Reset |
|---------|--------|------------------|------|-------|-------|
| 8 | `distance_keys_by_global` | `uint32_t`，`UINT_MAX` = invisible / invalid | `total_splat_count * 4` | StorageBuffer | 不 reset；cull/project 全量覆盖 |
| 9 | `visibility_prefix_local` | `uint32_t` block-local exclusive prefix | `total_splat_count * 4` | StorageBuffer | 不 reset；visibility scan 覆盖 |
| 10 | `visibility_scan_buffer` | packed `uint32_t` scan levels | `sum(visibility_level_counts) * 4` | StorageBuffer | 不 reset；scan level dispatch 覆盖 active ranges |
| 11 | `radix_args` | radix params + 16-byte indirect command slots + `bucket_totals[16]` + `bucket_bases[16]` | 固定 struct size，C++/GLSL `static_assert` 对齐 | StorageBuffer + IndirectBuffer | args pass 覆盖 |
| 12 | `radix_histogram` | `uint32_t` bucket-major counters | `16 * max_radix_block_count * 4` | StorageBuffer | 不 clear；histogram 每 digit 覆盖 active range |
| 13 | `radix_block_offsets` | `uint32_t` bucket-major offsets | `16 * max_radix_block_count * 4` | StorageBuffer | 不 reset；radix prefix 覆盖 active range |
| 14 | `radix_scan_buffer` | packed `uint32_t` radix scan levels | `16 * sum(radix_level_counts) * 4` | StorageBuffer | 不 reset；radix prefix 覆盖 active ranges |

公共常量：

```text
kGSScanBlockSize = 256
kGSRadixBlockSize = 256
kGSRadixDigitBits = 4
kGSRadixBucketCount = 16
kGSRadixDigitCount = 8
kGSScanMaxLevels = 8
max_radix_block_count = ceil(sort_capacity / kGSRadixBlockSize)
```

`radix_args` struct 使用 16-byte dispatch slot，不直接用 12-byte packed `VkDispatchIndirectCommand` 数组。Vulkan indirect dispatch offset 指向 slot 开头，只读取前三个 `uint32_t`，第 4 个 padding 被忽略。

```text
struct GSDispatchIndirectSlot {
    uint x;
    uint y;
    uint z;
    uint pad;
}

struct GSRadixArgs {
    uint active_count;
    uint active_capacity;
    uint block_count;
    uint radix_scan_level_count;

    uint radix_scan_input_counts[8];

    GSDispatchIndirectSlot histogram_dispatch;
    GSDispatchIndirectSlot scatter_dispatch;
    GSDispatchIndirectSlot prefix_dispatches[8];

    uint bucket_totals[16];
    uint bucket_bases[16];
}
```

`GSRadixArgs` 预期 offset：

```text
active_count              0
active_capacity           4
block_count               8
radix_scan_level_count    12
radix_scan_input_counts   16
histogram_dispatch        48
scatter_dispatch          64
prefix_dispatches         80
bucket_totals             208
bucket_bases              272
sizeof                    336
```

`visible_count` 特例写法：

```text
visible_count == 0:
    active_count = 0
    active_capacity = 0
    block_count = 0
    radix_scan_level_count = 0
    all radix_scan_input_counts = 0
    all dispatch slots = {0, 1, 1, 0}
    bucket_totals = 0
    bucket_bases = 0

visible_count == 1:
    active_count = 1
    active_capacity = 1
    block_count = 0
    radix_scan_level_count = 0
    all radix_scan_input_counts = 0
    all dispatch slots = {0, 1, 1, 0}
    bucket_totals = 0
    bucket_bases = 0

visible_count >= 2:
    active_count = visible_count
    active_capacity = next_power_of_two(visible_count)
    block_count = ceil(active_capacity / 256)
    radix_scan_input_counts[0] = block_count
    radix_scan_input_counts[i + 1] = ceil(radix_scan_input_counts[i] / 256)，直到上一层 count 为 1
    radix_scan_level_count = 有效 radix_scan_input_counts 数量
    histogram_dispatch = {block_count, 1, 1, 0}
    scatter_dispatch = {block_count, 1, 1, 0}
    active prefix_dispatches[level] = {ceil(radix_scan_input_counts[level] / 256), 16, 1, 0}
    inactive prefix_dispatches[level] = {0, 1, 1, 0}
```

CPU-side scan level layout helper（用于 packed scan buffer 的 parent levels）：

```text
build_scan_level_layout(input_count):
    count = ceil(input_count / 256)
    while count > 0:
        append level_count = count
        if count == 1: break
        count = ceil(count / 256)

level_offset[0] = 0
level_offset[i + 1] = level_offset[i] + level_count[i]
total_level_values = sum(level_count)
```

Visibility 使用 `build_scan_level_layout(total_splat_count)` 得到 `visibility_level_count/offset`。Radix buffer 容量使用 `build_scan_level_layout(max_radix_block_count)` 得到 `radix_level_count/offset`；运行时 `radix_scan_input_counts[level]` 可以小于或等于对应 capacity level，prefix shader 只读写 active count 范围。

Visibility scan push constants:

```text
struct GSVisibilityLeafScanPush {
    uint total_splat_count;
    uint level0_offset;
}

struct GSVisibilityLevelScanPush {
    uint input_offset;
    uint output_offset;
    uint input_count;
    uint has_output_level;
}

struct GSVisibleCompactPush {
    uint total_splat_count;
    uint visibility_level_count;
    uint visibility_level_offsets[8];
    uint visibility_level_counts[8];
}
```

Visibility scan 契约：

- Leaf scan dispatch `ceil(total_splat_count / 256)` 个 workgroup。
- 对 splat `i`，`flag = distance_keys_by_global[i] != UINT_MAX ? 1 : 0`。
- Leaf scan 写 `visibility_prefix_local[i] = exclusive_prefix(flag)`，范围仅限当前 256-splat leaf block。
- Leaf scan 写 raw leaf block sums 到 `visibility_scan_buffer[level_offset[0] + leaf_block_id]`。
- 对 `level = 0..visibility_level_count-1` 逐层扫描 `visibility_scan_buffer`：dispatch 前当前 level range 存 raw sums；dispatch 后当前 level range 存 exclusive prefixes；若存在 parent level，则同时把 parent raw sums 写到下一层 range。
- 最后一层 scan dispatch 根据 total sum 写 `visible_count` 和 `indirect.instanceCount`。
- Compact 通过累加各 level 已扫描 prefix，计算 original leaf block `b` 的 global offset：

```text
block_offset = sum over level l:
    visibility_scan_buffer[level_offset[l] + floor(b / 256^l)]
```

Compact 写入：

```text
if distance_keys_by_global[i] != UINT_MAX:
    dst = visibility_prefix_local[i] + block_offset
    sort_entries[dst] = { distance_keys_by_global[i], i }
```

`GS Visibility Scan` pass 内 dispatch / barrier 顺序固定为：

```text
leaf scan dispatch
barrier visibility_prefix_local/visibility_scan_buffer: Compute write -> Compute read/write

for level in 0..visibility_level_count-1:
    level scan dispatch
    if level + 1 < visibility_level_count:
        barrier visibility_scan_buffer: Compute write -> Compute read/write

finalization happens in the last level scan dispatch
```

若 `visibility_level_count == 1`，`level = 0` 的 scan dispatch 扫描 leaf sums 并承担 finalization：写 `visible_count` 和 `indirect.instanceCount`。`visible_count` / `indirect_draw` 写入后的跨 pass 依赖由 RenderGraph 根据 pass resource declaration 处理，不在本 pass 末尾手写 barrier。

Radix sort push constants:

```text
struct GSRadixHistogramPush {
    uint digit_shift;
    uint max_radix_block_count;
}

struct GSRadixPrefixPush {
    uint level;
    uint input_offset;
    uint output_offset;
    uint capacity_level_count;
    uint capacity_level_offsets[8];
    uint capacity_level_counts[8];
    uint max_radix_block_count;
}

struct GSRadixBucketBasesPush {
    uint bucket_count; // fixed to 16 for Phase 3.0
}

struct GSRadixScatterPush {
    uint digit_shift;
    uint capacity_level_count;
    uint capacity_level_offsets[8];
    uint capacity_level_counts[8];
    uint max_radix_block_count;
}
```

RHI command contract:

- Step 10 must add `CommandBuffer::dispatch_indirect(VkBuffer buffer, VkDeviceSize offset)`.
- The method records `vkCmdDispatchIndirect` with the raw Vulkan buffer and byte offset, matching the existing `draw_indirect()` style where `CommandBuffer` does not resolve engine buffer handles.
- `radix_args` indirect slot offsets are fixed by the `GSRadixArgs` offsets above:

```text
histogram_dispatch_offset = 48
scatter_dispatch_offset   = 64
prefix_dispatch_offset(i) = 80 + i * 16
```

`GSRadixPrefixPush` 只包含 CPU 录制命令时已知的 capacity layout 和当前 level 参数；runtime active count 从 `radix_args.radix_scan_input_counts[level]` 读取。CPU 必须按 capacity layout 录制所有 prefix levels，不能按 `radix_args.radix_scan_level_count` 循环：

```text
for level in 0..capacity_level_count-1:
    bind GSRadixPrefixPush(level, input_offset, output_offset, capacity layout)
    dispatchIndirect(radix_args.prefix_dispatches[level])
```

Inactive runtime levels 依赖 `radix_args.prefix_dispatches[level] = {0, 1, 1, 0}` 形成 zero dispatch；shader 内再读取 `radix_args.radix_scan_level_count` 判断 active / final。

`GSRadixPrefixPush` offset 映射固定为：

```text
level == 0:
    input = radix_histogram
    input_offset = unused
    output_offset = capacity_level_offsets[0]

level > 0:
    input = radix_scan_buffer[capacity_level_offsets[level - 1]]
    input_offset = capacity_level_offsets[level - 1]

level + 1 < capacity_level_count:
    output_offset = capacity_level_offsets[level]

level + 1 == capacity_level_count:
    output_offset = unused
```

`GSRadixPrefixPush` level 语义：

- `level == 0`：input 是 `radix_histogram`，local exclusive prefix 写 `radix_block_offsets`。
- `level > 0`：input 是 `radix_scan_buffer[input_offset]`，当前 level exclusive prefix 原地写回同一 range。
- Shader 内判断 `is_active = level < radix_args.radix_scan_level_count`；zero dispatch 的 inactive level 通常不会执行，但 shader 仍应防御性 early return。
- Shader 内判断 `is_final = level + 1 == radix_args.radix_scan_level_count`。
- Shader 内判断 `has_output_level = level + 1 < radix_args.radix_scan_level_count`。
- 若 `has_output_level`，parent raw sums 写 `radix_scan_buffer[output_offset]`。
- 若 `is_final`，prefix shader 不写 parent level，而是写 `radix_args.bucket_totals[16]`。
- Final level 写 `bucket_totals[bucket]` 时必须使用 scan 前的 raw sum / workgroup total；不能使用当前 level 原地扫描后的 exclusive prefix 值。

Radix dispatch 形状：

- Histogram：`dispatchIndirect(radix_args.histogram_dispatch)`，`x = block_count`，每个 256-entry radix block 一个 workgroup。
- Scatter：`dispatchIndirect(radix_args.scatter_dispatch)`，`x = block_count`，每个 256-entry radix block 一个 workgroup。
- Prefix：CPU 按 `capacity_level_count` 录制 `dispatchIndirect(radix_args.prefix_dispatches[level])`；active level 的 `x = ceil(radix_args.radix_scan_input_counts[level] / 256)`、`y = 16` buckets；inactive level 使用 zero dispatch。
- Bucket bases：一个普通 compute dispatch，单个 workgroup 读取 `bucket_totals[16]`，写 `bucket_bases[16]`。
- Histogram / scatter shader 从 `radix_args.active_capacity` 读取 runtime active capacity。Last active block 读取 input entry 前必须 guard `entry_index < radix_args.active_capacity`。
- Radix 必须处理完整 `[0, active_capacity)` 范围，不得只处理 `[0, active_count)`；`[active_count, active_capacity)` 由全量 reset 保证为 sentinel，并参与每个 digit 的 histogram/scatter，避免后续 digit 读 undefined。
- Histogram shader 每个 digit 必须为每个 active block 覆盖写满 16 个 bucket counters；prefix 不得读取 inactive blocks。

Radix histogram / offset buffer 使用 bucket-major indexing：

```text
index = bucket * max_radix_block_count + block
```

`radix_scan_buffer` layout 固定为 level-major，level 内 bucket-major：

```text
radix_scan_index(level, bucket, index_in_level) =
    16 * radix_level_offset[level]
  + bucket * radix_level_count[level]
  + index_in_level
```

`radix_block_offsets` 只存 level0 local exclusive prefix，不存 full per-bucket block offset。Scatter 现场从 `radix_scan_buffer` 累加 parent level prefix，合成 full offset；这与 visibility compact 的现场累加策略一致，并避免单独的 add-propagation pass。

每个 digit 的 radix prefix / bucket-bases sequence 最终得到：

```text
bucket_total[bucket] = sum(histogram[bucket][0..block_count))
bucket_bases[0] = 0
bucket_bases[bucket] = sum(bucket_total[0..bucket))
```

`bucket_totals[16]` 由 radix prefix final level 写入 `radix_args`。随后 `GS Radix Bucket Bases` dispatch 读取 `bucket_totals[16]` 并写 `bucket_bases[16]`。每个 digit 覆盖同一份当前 digit 的 totals/bases。

Radix scatter local-rank 契约：

```text
bucket = (entry.distance_key >> digit_shift) & 0xFu
subgroup_rank = count previous active lanes in this subgroup with the same bucket
subgroup_bucket_count[subgroup_id][bucket] = subgroup count for bucket
workgroup_bucket_prefix = exclusive prefix of subgroup_bucket_count over subgroup_id
local_rank = workgroup_bucket_prefix[subgroup_id][bucket] + subgroup_rank
```

- Subgroup size 动态使用 `gl_SubgroupSize`；不得假设固定 32-wide subgroup。
- Shared subgroup histogram array 必须按 256-thread workgroup 下的最大 subgroup 数量和 16 buckets 预留；按 Vulkan core 最小 subgroup size 4 计算，最坏为 `16 * 64` 个 `uint`。
- 每个 scatter workgroup 在 per-entry scatter 前先为 16 个 bucket 计算当前 block 的 full offset，并写入 shared memory。Capacity `radix_level_offset/count` 来自 push constants；active parent level 数量从 `radix_args.radix_scan_level_count` 派生为 `max(radix_args.radix_scan_level_count - 1, 0)`：

```text
full_block_offset[bucket] = radix_block_offsets[bucket, block]
for l in 0..active_parent_level_count-1:
    parent_index = floor(block / 256^(l + 1))
    full_block_offset[bucket] += radix_scan_buffer[
        16 * radix_level_offset[l] + bucket * radix_level_count[l] + parent_index
    ]
```

- Scatter destination 使用合成后的 full offset 和当前 digit 的 bucket base：

```text
dst = bucket_base[bucket] + full_block_offset[bucket] + local_rank
```

4-bit radix 的 ping-pong parity 固定为：

```text
digit 0: sort_entries -> sort_entries_scratch
digit 1: sort_entries_scratch -> sort_entries
...
digit 7: sort_entries_scratch -> sort_entries
```

8 个 digit 后排序结果必须位于 primary `sort_entries`；4-bit 首版不允许增加 copy-back pass。CPU 不基于 `active_count` 跳过 `GS Radix Sort` pass，因为 command recording 时不知道 GPU visible count；`active_count <= 1` 时由 `radix_args` 写 zero dispatch，使 histogram / prefix / scatter no-op，primary `sort_entries` 保持 compact 输出。

Pass 顺序固定为：

```text
GS Work Buffer Reset
GS Cull Project Keys
GS Visibility Scan
GS Visible Compact
if sort_mode == Bitonic:
    GS Bitonic Sort
else:
    GS Radix Args
    GS Radix Sort
GS Draw
```

- Reset writes `visible_count`、`sort_entries`、`sort_entries_scratch`、`indirect_draw.instanceCount`.
- Cull/project reads static GS buffers + GlobalUBO, writes `projected_data` and `distance_keys_by_global`.
- Visibility scan reads `distance_keys_by_global`, writes `visibility_prefix_local`、`visibility_scan_buffer`、`visible_count`、`indirect_draw.instanceCount`.
- Compact reads `distance_keys_by_global`、`visibility_prefix_local`、`visibility_scan_buffer`, writes primary `sort_entries`.
- Radix args reads `visible_count`, writes `radix_args`.
- Radix sort reads/writes `sort_entries`、`sort_entries_scratch`、`radix_histogram`、`radix_block_offsets`、`radix_scan_buffer`、`radix_args`.
- Draw reads primary `sort_entries`、`projected_data`、`indirect_draw`.

`radix_args` 的 RenderGraph usage 契约：

- `radix_args` remains a single buffer with `StorageBuffer | IndirectBuffer` usage.
- RenderGraph must support declaring multiple usages for the same buffer in the same pass and aggregate them into one stage/access set for dependency calculation.
- If the same resource appears multiple times in one pass declaration, RenderGraph ORs all resolved stages and accesses for that pass-local resource usage.
- RenderGraph must not emit same-pass barriers based on the declaration order of duplicate usages; the aggregated usage is used only for cross-pass dependency calculation.
- `GS Radix Sort` declares `radix_args` as both Compute SSBO read/write and IndirectCommand read.
- The cross-pass `GS Radix Args` → `GS Radix Sort` dependency is handled by RenderGraph through the aggregated usage.
- Manual barriers are still required inside `GS Radix Sort` between internal dispatches that read/write the same buffers within one RG pass.

`GS Radix Sort` pass 内部 barrier 顺序：

```text
for each digit:
    histogram dispatch
    barrier radix_histogram: Compute write -> Compute read

    for level in 0..capacity_level_count-1:
        bind prefix push constants using capacity offset mapping
        dispatchIndirect(radix_args.prefix_dispatches[level])
        shader reads runtime input_count / active / final-level state from radix_args
        barrier prefix outputs: Compute write -> Compute read/write
            level 0 output includes radix_block_offsets when active
            non-final active level output includes radix_scan_buffer parent range
            final active level output includes radix_args.bucket_totals
            inactive levels are zero dispatch and produce no writes

    bucket bases dispatch
    barrier radix_args.bucket_bases/radix_block_offsets/radix_scan_buffer: Compute write -> Compute read

    scatter dispatch
    barrier scatter output sort buffer: Compute write -> Compute read/write
```

- Histogram → prefix barrier covers `radix_histogram` reads.
- Prefix level barriers cover `radix_block_offsets` and in-place scanned parent levels in `radix_scan_buffer`.
- Prefix final level writes `radix_args.bucket_totals`.
- Bucket bases dispatch reads `bucket_totals` and writes `bucket_bases`.
- Bucket bases → scatter barrier covers `radix_args.bucket_bases`、`radix_block_offsets` 和 `radix_scan_buffer` reads.
- Scatter output barrier makes the current digit output visible to the next digit input. The final digit output is primary `sort_entries`; the following draw pass dependency is handled by RenderGraph.

### Step 10：RG indirect command 与 GS radix 资源契约

本 Step 将 Step 9 冻结的 resource / synchronization contract 落到 framework 和 GS scene resource owner 中，不实现排序算法本体。

- RenderGraph indirect command usage 由 `DrawIndirect` 重命名为 `IndirectCommand`，覆盖 draw indirect 与 dispatch indirect；Vulkan 映射仍为 `VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT` + `VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT`。
- RenderGraph 支持同一 pass 内同一 resource 多 usage 聚合：stage/access OR 合并，只用于 cross-pass dependency，不根据同 pass duplicate usage 声明顺序生成 same-pass barrier。
- RHI `CommandBuffer` 新增 `dispatch_indirect(VkBuffer buffer, VkDeviceSize offset)`，与现有 `draw_indirect()` 风格一致，不在 `CommandBuffer` 内解析 engine `BufferHandle`。
- GS Set 3 追加 Step 9 固定的新 binding 8-14，并由 GS scene resource owner 创建、销毁、写 descriptor。
- `radix_args` 使用 `StorageBuffer | IndirectBuffer`，其他新增 work buffers 按 Step 9 表格中的 usage / reset contract 创建。

### Step 11：Deterministic visible list

本 Step 替换 cull/project 的 atomic append 输出，生成 deterministic visible list，并先接回 Bitonic baseline 做视觉验证。

- Cull/project 全量写 `distance_keys_by_global`，`UINT_MAX` 表示 invisible / invalid；不再 append `sort_entries`，不再写 `visible_count` 或 `indirect.instanceCount`。
- Visibility scan 使用 Step 9 定义的 hierarchical exclusive scan，写 `visibility_prefix_local`、`visibility_scan_buffer`、`visible_count` 和 `indirect.instanceCount`。
- Visible compact 按 global splat index 顺序写 primary `sort_entries[0..visible_count)`，block offset 在 compact 中现场从 scan levels 累加。
- Bitonic debug baseline 共用 deterministic compact 输出，继续按 `sort_capacity` 全量排序，用于在 radix 接入前做简略视觉验证。

### Step 12：Visible-count-driven 4-bit radix sort

本 Step 实现并接入 Step 9 冻结的 visible-count-driven 4-bit radix sort，默认启用 Radix，并保留 Bitonic Debug UI 对比路径。

- `GS Radix Args` pass 读取 GPU 侧 `visible_count`，写 `GSRadixArgs` scalar fields、indirect dispatch slots、runtime scan input counts、`bucket_totals` / `bucket_bases` 初始值。
- `GS Radix Sort` pass 内部按 digit 循环 histogram、radix prefix、bucket bases、scatter，并按 Step 9 barrier contract 手写 pass 内 compute-to-compute barriers。
- Radix sort 必须处理完整 `[0, active_capacity)`，tail sentinel 参与每个 digit；最终结果必须位于 primary `sort_entries`。
- Debug UI 提供 GS sort mode 切换，默认 Radix；Bitonic 保留为 runtime baseline。Radix / Bitonic 一致性通过视觉 A/B 验证，不实现 GPU exact compare 或 CPU readback。

### Step 13：Wigner-D SH rotation

本 Step 补齐非 identity transform rotation 下的 SH upload bake。

- Wigner-D SH rotation 从 transform 提取 proper rotation，旋转 degree 1-3 SH 系数，并集成到 upload bake。
- Wigner-D 验证至少包括 identity 不变、degree 1 已知旋转、与 PLY 坐标翻转规则一致性检查。

### Step 14：Phase 3.0 final validation

本 Step 由用户在 CLion 中执行最终编译验证。Agent 不运行 CMake / build / run。

- 请求用户在 CLion 中编译验证。


## Phase 3.0 决策

### 数据 bake 与校验

- Transform 在 upload 时 bake 到 world-space position / covariance；低频变更通过重上传处理。
- KHR `SCALE` 表示 σ，local covariance 使用 `scale²`，world covariance 使用 node global transform 的线性部分 bake。
- GPU static baked buffers 不保留 raw rotation / scale；CPU 侧可保留用于 reload、debug、future rebake。
- Upload bake 预计算 per-splat `world_radius_3sigma`，Phase 3.0 使用 `Σ_world` 最大特征值得到 3σ 最长半轴作为紧致 sphere cull 半径。
- 直接 glTF/GLB 加载必须校验 opacity finite 且在 `[0,1]`、scale finite 且 `>= 0`、rotation finite 且为 unit quaternion。
- Node global transform 必须可分解为 regular translation、proper rotation 和 positive scale；reflection / negative determinant / shear / 不可分解线性部分在 Phase 3.0 报错并回退空 GS scene，不跳过 primitive。
- Phase 3.0 前期只支持不需要 Wigner-D 的 SH 直接上传：identity/no-rotation transform 可直接上传；非 identity rotation 仅在 `max_sh_degree == 0` 时允许；degree 1-3 遇到非 identity rotation 必须在 CPU preflight / upload 前报错，不静默渲染错误，也不创建 static buffers。完整 Wigner-D degree 1-3 放到 Phase 3.0 末期。

### Primitive metadata 与场景一致性

- 同一 GS scene 内所有 primitive metadata 必须一致：`kernel = ellipse`、`projection = perspective`、`sortingMethod = cameraDistance`、`colorSpace` 一致。
- 多个 primitive 在 upload 时拼接为 global splat buffers；CPU/GPU scene contract 不保留 per-primitive ranges，除非未来出现明确消费者。
- Phase 3.0 shader 按 global splat index 访问 baked SoA buffers，不按 primitive metadata 分支。
- GPU primitive metadata buffer 可选 / 预留，不作为 Phase 3.0 shader 依赖。

### GPU buffers、索引与生命周期

- GS scene GPU resources 必须集中由 Renderer 持有的 GS scene resource owner 管理；数据契约 struct 只记录 handles / counts / metadata，不负责创建或销毁资源。
- GS passes 只拥有 pipeline / shader 等 pass-local 对象，并通过 RenderGraph 声明资源读写；不得各自长期持有或重建 scene static/work buffers。
- GS 使用独立持久 Set 3 descriptor set，不与现有 PT / compute push descriptor Set 3 混用。
- Work buffer capacity 由当前 scene `total_splat_count` 派生；`sort_capacity = next_power_of_two(total_splat_count)`。容量不是固定渲染器上限，不允许静默截断。
- Projected data 按 global splat index dense 存储；sort entry payload 也存 global splat index。
- Invisible splat 的 projected data 未定义；draw `instanceCount = visible_count`，不 draw capacity。
- Projected data 只存 draw 阶段需要的投影后数据：`center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`、`opacity`、SH-evaluated `rgb`。
- Scene load/reload 重建 static baked buffers 和 capacity-based work buffers，并重写 GS Set 3。
- Swapchain resize 只重建 viewport-sized GS composition / linear targets；projected data / work buffers 不因 resize 重建，下一帧 cull/project 覆盖内容。
- Reload/resize 后 descriptor 不得指向已销毁资源。

### RenderGraph 同步

- 现有 RenderGraph 只自动处理 image barriers；buffer resource usage 在 compile 阶段会跳过。
- Phase 3.0 接入 GS compute/sort/draw 前必须扩展 RG buffer barrier 支持：per-buffer last stage/access tracking、`VkBufferMemoryBarrier2` emission。
- GS 所需 stage/access 映射至少包括：Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write。
- 优先扩展 RG，不在 GS pass 内长期手写 barriers。

### Per-frame 参数

- GS 复用 GlobalUBO 中的 view、projection、view_projection、camera_position、screen_size。
- GS 专用小参数放入 `GSPushConstants`：`total_splat_count`、`sort_capacity`、`color_space`、`max_sh_degree`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`。`max_sh_degree` 来自 scene-level metadata，shader 由该字段推导 `sh_coefficients` packed vec4 stride（degree 0/1/2/3 = 1/3/7/12）。
- `max_projected_extent_px` 初始为 screen short side × 0.25；discard thresholds 初始为 alpha=1e-4、power=-20。
- Tonemapping mode 是 TonemappingPass 独立 push constant，不属于 GS push constants。

### Projection、cull 与 draw

- GS 使用与 PT/reference view 相同的 camera pose、FOV、aspect 和 viewport；GS projection-stability near 独立于 camera projection near，默认 0.25 world units，且由 Rendering 面板 GS Near 滑块控制。
- Projected center、2D covariance、OBB axes/extents 均使用 framebuffer pixel space：top-left origin、x right、y down。
- 相机/PT projection NDC 约定为 Y-up；GS cull/project 在 NDC→pixel 与 2D covariance Jacobian 中执行一次 Y flip，使 projected data 与 `gl_FragCoord` 的 y-down 坐标一致。
- GS draw pass 使用 positive-height normal viewport；pixel → Vulkan NDC 使用正常 viewport 映射，不做第二次 Y flip；fragment 直接使用 `gl_FragCoord.xy - center_px`。
- 2D covariance 使用 view-space covariance 和 pixel focal length 推导，保证 covariance、OBB extents、conic 和 `gl_FragCoord.xy` 在同一 pixel coordinate system。
- World-space frustum cull 使用 sphere(center=`position_world`, radius=`world_radius_3sigma`) vs frustum planes。
- Behind-camera / near-plane 不稳定 / projected OBB 过大的 splat 可 discard；projection z clamp 只用于防止 NaN/Inf，不用于强行保留贴脸 splat。
- Projected OBB 任一半轴超过 screen short side × 0.25 时 discard，不做 extent clamp。

### Sort 与 draw range

- Sort entry 为 2×32-bit：`distance_key + global_splat_index`。`distance_key = floatBitsToUint(camera_distance_squared)`，仅对 finite non-negative float 使用；invalid sentinel 为 `{UINT_MAX, UINT_MAX}`。
- Step 11 起 cull/project 不再 atomic append visible list，而是全量写 `distance_keys_by_global`。`distance_keys_by_global[i] == UINT_MAX` 表示 invisible；visibility scan / compact 根据该 sentinel 按 global splat index 顺序生成 deterministic visible list。
- 每帧 reset：`visible_count = 0`、`sort_entries` / `sort_entries_scratch` 全量填 sentinel、`indirect.instanceCount = 0`。`distance_keys_by_global` 不 reset，由 cull/project 全量覆盖。
- Visibility scan finalization 写 `visible_count` 和 `indirect.instanceCount`；draw 使用 `visible_count`，不 draw capacity。
- Bitonic compare 使用 lexicographic `(distance_key, global_splat_index)`；Bitonic debug baseline 与 Radix 共用 deterministic compact 输出，但仍按 `sort_capacity` 全量排序。
- Radix 使用 stable 4-bit LSD radix，仅排序 `[0, active_capacity)`，其中 `active_capacity` 由 GPU 侧 `visible_count` 派生。Compact 后 `[visible_count, active_capacity)` 的 sentinel 由全量 reset 保证。
- Radix 完成后 sorted result 必须位于 primary `sort_entries`；draw pass 不感知 sort 内部 ping-pong 状态。
- `VkDrawIndirectCommand` 固定字段由 CPU 初始化：`vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0`。GPU 只写 `instanceCount = visible_count`。

### Color、alpha 与 output

- GS composition 在 primitive / scene colorSpace 中完成。
- `srgb_rec709_display`：composition target 存 sRGB display-referred blended values，再由 GS sRGB→linear conversion pass 输出 linear target。
- `lin_rec709_display`：composition target 已是 linear，可直接作为 TonemappingPass 输入。
- Composition / linear targets 存 accumulated premultiplied RGB + accumulated alpha。Conversion pass 只转换 RGB，alpha 原样保留。
- Phase 3.0 不做 unpremultiply，不做背景合成；TonemappingPass GS mode 忽略 alpha，swapchain 输出 alpha=1。
- SH-evaluated RGB 负分量在 premultiply 前 clamp 到 0；Phase 3.0 不做 per-splat upper clamp。
- 最终 GS 输出采用 per-channel hard clamp 到 `[0,1]`；未来如需更好保 hue，可增加 max-channel range compression。
- TonemappingPass 保留为最终 swapchain output pass，通过 push constant `mode` 区分 `HdrAces` 与 `LinearClamp`。PT 使用 `HdrAces`，GS 使用 `LinearClamp`。
- TonemappingPass 不做 GS sRGB decode；GS 进入 TonemappingPass 前必须已经是 linear。

### Blend state

- Fragment 输出 `vec4(rgb * alpha, alpha)`。
- Blend 使用 front-to-back premultiplied-under：srcColor/srcAlpha=`ONE_MINUS_DST_ALPHA`，dstColor/dstAlpha=`ONE`，op=`ADD`。
- colorWriteMask=RGBA。
- GS composition target 为 R16G16B16A16Sfloat，loadOp=CLEAR `vec4(0,0,0,0)`，storeOp=STORE。
- Graphics pipeline 禁用 depth test/write，cull none。

## 完成标准

- Happy path：KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform。
- 支持单个或多个 GS primitive，primitive 拼接为 global splat buffers；不保留无实际消费的 CPU per-primitive ranges。
- 支持 `srgb_rec709_display` 和 `lin_rec709_display` 二选一 scene；不支持 mixed colorSpace scene。
- 以 1M splat 级别 correctness baseline 为目标，不以 10M 性能为 Phase 3.0 完成条件。
- 必须验证：投影中心、3σ OBB、front-to-back 排序、premultiplied-under blend、color conversion、TonemappingPass GS bypass、resize、`visible_count = 0`、非法 asset rejection。Step 11-13 额外验证 deterministic compact + Bitonic 简略视觉结果、Radix / Bitonic Debug UI 视觉 A/B，以及 Wigner-D identity / degree 1 / PLY 坐标翻转一致性。
