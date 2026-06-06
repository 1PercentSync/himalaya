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
- Bitonic sort correctness baseline；Phase 3.0 末期再接 radix capacity / visible-count-driven radix。
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
- Sort entry 物理格式固定为 2×32-bit：`distance_key + global_splat_index`。Indirect command 固定字段由 CPU 初始化，GPU 只写 `instanceCount`。
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
- SH upload 前期只支持不需要 Wigner-D 的 happy path：node proper rotation 近似 identity 时直接上传；node rotation 非 identity 但 scene/primitive `max_sh_degree == 0` 时允许直接上传；node rotation 非 identity 且存在 degree 1-3 SH 时必须报错并回退空 GS scene。该拦截属于 Renderer/GS builder 的能力限制，应在 CPU preflight / static buffer upload 前完成，避免失败场景进入 GPU buffer 创建上传。完整 Wigner-D 放到 Step 9。
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
- `center_px` 由 clip/NDC 转 pixel，使用 Vulkan framebuffer 坐标：top-left origin、x right、y down，不做 Y flip。
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
- Quad corner：`center_px + sx * axis0_extent_px + sy * axis1_extent_px`。Pixel → NDC 不做 Y flip；GS draw 使用 positive-height normal viewport。
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
- GS near plane 初始为 scene AABB diagonal × 0.005，仅 GS 模式使用；填齐 `GSPushConstants` 的 count、capacity、colorSpace、maxSH、near、extent 与 discard thresholds。
- 新增 `render_gaussian_splatting()` 作为 GS path orchestration，顺序为 reset → cull/project → sort → draw → TonemappingPass；基础接入先支持 `lin_rec709_display` 直接作为 TonemappingPass input。
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

### Step 9：Phase 3.0 末期补全

Step 9 在硬件光栅 correctness baseline 建立后执行，用于补齐 Phase 3.0 末期目标。

- Radix sort 保持 32-bit `distance_key` + `global_splat_index` payload，不退回 64-bit packed key；equal-key deterministic 策略需要在实现前确认。
- Radix capacity path 包含 histogram、prefix sum、scatter 和 ping-pong payload 搬运；完成后替换 bitonic dispatch。
- Radix capacity 必须与 Bitonic capacity baseline 对比渲染结果。
- Visible-count-driven radix 由 GPU 侧 `visible_count` 限制排序工作量，不做 CPU readback。
- Wigner-D SH rotation 从 transform 提取 proper rotation，旋转 degree 1-3 SH 系数，并集成到 upload bake。
- Wigner-D 验证至少包括 identity 不变、degree 1 已知旋转、与 PLY 坐标翻转规则一致性检查。


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

- GS 使用与 PT/reference view 相同的 camera pose、FOV、aspect 和 viewport；可使用 GS-specific near plane 保持投影稳定。
- Projected center、2D covariance、OBB axes/extents 均使用 Vulkan framebuffer pixel space：top-left origin、x right、y down。
- GS draw pass 使用 positive-height normal viewport；pixel → NDC 不做 Y flip；fragment 直接使用 `gl_FragCoord.xy - center_px`。
- 2D covariance 使用 view-space covariance 和 pixel focal length 推导，保证 covariance、OBB extents、conic 和 `gl_FragCoord.xy` 在同一 pixel coordinate system。
- World-space frustum cull 使用 sphere(center=`position_world`, radius=`world_radius_3sigma`) vs frustum planes。
- Behind-camera / near-plane 不稳定 / projected OBB 过大的 splat 可 discard；projection z clamp 只用于防止 NaN/Inf，不用于强行保留贴脸 splat。
- Projected OBB 任一半轴超过 screen short side × 0.25 时 discard，不做 extent clamp。

### Sort 与 draw range

- Sort entry 为 2×32-bit：`distance_key + global_splat_index`。
- `distance_key = floatBitsToUint(camera_distance_squared)`，仅对 finite non-negative float 使用；invalid sentinel 为 `{UINT_MAX, UINT_MAX}`。
- 每帧 reset：`visible_count = 0`、sort entries 填 sentinel、`indirect.instanceCount = 0`。
- Cull/project append valid entries 到 `sort_entries[0..visible_count)`；ascending sort 后 valid entries 在前，sentinel 在尾。
- Bitonic compare 使用 lexicographic `(distance_key, global_splat_index)`；未来 sort 实现也必须对相同 `distance_key` 保持 deterministic ordering，避免透明累积闪烁。
- Phase 3.0 Bitonic baseline 使用 primary `sort_entries` 的 in-place compare-and-swap；scratch sort buffer 保留给后续 out-of-place radix / ping-pong scatter，不为 bitonic 强行增加 buffer 搬运。
- Bitonic orchestration 作为单个 RenderGraph pass 内的多 dispatch 序列执行，并在 dispatch step 之间手写 compute-to-compute buffer barrier，避免按 step 拆分导致 RG pass 数随 `log2(N) * (log2(N)+1) / 2` 膨胀。
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
- 必须验证：投影中心、3σ OBB、front-to-back 排序、premultiplied-under blend、color conversion、TonemappingPass GS bypass、resize、`visible_count = 0`、非法 asset rejection。
