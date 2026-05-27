# Phase 3 决策：Gaussian Splatting 渲染

> 渲染管线总体路线、子阶段规划与关键技术选型。
> 每个子阶段仅记录目的、采纳技术和结束目标，实现细节在各阶段开始时细化。

---

## 总体路线

**两阶段演进**：

1. **硬件光栅方案**：Compute Cull+Sort → Indirect Draw instanced quads → 硬件 alpha blend
2. **Tile-Based Compute 方案**：Compute Cull+Sort → Tile Binning → Per-tile compute blend（early-out）

先实现硬件光栅方案验证正确性，再演进到 Tile-Based Compute 方案获取 per-tile early-out 能力。从 1 到 2 只替换渲染末端，上游 cull/sort 管线 100% 复用。

**两个阶段统一使用前到后（front-to-back）排序**。排序键采用 camera distance squared，符合 KHR `sortingMethod = cameraDistance`，并避免每 splat 开平方。排序项物理存储为 2×32-bit（uint distance_key + uint global_splat_index），而非 shader `uint64_t` packed key；这样 Radix sort 仅处理 32-bit distance_key，global_splat_index 作为 payload 搬运，可减少 radix pass 数。Phase 3.0 使用 premultiplied under 硬件混合，Phase 3.5 在 shared memory 中做前到后累积 + early-out。排序方向一致，演进时无需修改 sort 逻辑。

**Phase 编号说明**：3.0/3.1/3.2 为硬件光栅阶段的基础和优化，3.5 为演进到 tile-based compute 的转折点，3.6/3.7 为后续优化。3.3/3.4 为预留编号。

**渲染模式互斥**：GS 渲染（Phase 3）与 PT 渲染是独立的 `RenderMode`，每帧只走一条路径，不存在 GS + mesh 同屏合成的需求。GS 专用的 near plane、投影参数等仅在 GS 模式下生效。

## 贯穿实现约定

以下不是独立阶段，而是从 Phase 3.0 第一个 shader 起就必须遵循的技术约定：

### Subgroup 操作优先

所有 compute shader 中涉及 append、归约、scan 的操作，默认使用 subgroup 模式而非朴素 per-thread atomic。典型应用：

- Visible list append：`subgroupBallot` + `subgroupElect` + `subgroupBroadcastFirst`，每 subgroup 只发一次 `atomicAdd`（原子操作减少约 32x）
- Tile binning append：同上模式
- Prefix-sum / compaction：subgroup inclusive/exclusive scan

### Subgroup Control Flow 写法

Phase 3.0 不强制要求 `VK_KHR_shader_subgroup_uniform_control_flow`。Shader 必须把 subgroup 操作写在 uniform control flow 中：所有 active invocation 无条件执行 `subgroupBallot(visible)`、`subgroupElect`、`subgroupBroadcastFirst` 等操作，`visible` 只作为 ballot 参数和最终写入条件，不把 subgroup intrinsic 放进 `if (visible)` 等 divergent 分支。

这样仍保留每 subgroup 最多一次 `atomicAdd` 的性能收益，同时避免把 GS 基础渲染绑定到额外 extension。若设备支持 `VK_KHR_shader_subgroup_uniform_control_flow`，后续可选择启用；若 Phase 3.5/3.6 的复杂 subgroup scan / divergent loop 确实需要该语义，再重新评估是否将其设为 GS 必需能力。

### 投影 Extent 收紧（Oriented Bounding Box）

投影阶段计算 2D 协方差椭圆的 OBB（贴合主轴的旋转矩形），而非 AABB。长条 splat 的 AABB 面积可达 OBB 的 2-4 倍，多余区域内 fragment 经高斯衰减后贡献近零。Phase 3.0 中用 oriented quad 替代 axis-aligned quad；Phase 3.5 中 tile binning 使用 OBB-tile 相交测试。

Phase 3.0 的 projected data 统一使用 screen pixel space：projected center、2D covariance、OBB axes/extents 都以 pixel 为单位。Screen pixel space 使用 Vulkan framebuffer 坐标：top-left origin、x right、y down。GS draw pass 使用 positive-height normal viewport，不使用 negative viewport Y-flip。Vertex shader 从 pixel-space projected data 展开 quad corners 后转换到 NDC，pixel→NDC 不做 Y flip；fragment shader 直接使用 `gl_FragCoord.xy - center_px` 与 pixel-space inverse covariance / conic coefficients 计算高斯 alpha 衰减。

GS 使用与 PT/reference view 相同的 camera pose、FOV、aspect 和 viewport，不引入独立相机模型；可使用 GS-specific near plane 保持投影稳定。`center_px` 由 clip/NDC 转 pixel 得到，不做 Y flip。2D covariance 使用 view-space covariance 和 pixel focal length：`fx = 0.5 * width * abs(proj[0][0])`、`fy = 0.5 * height * abs(proj[1][1])`，`cov_2d = J * cov_view * Jᵀ`，从而保证 covariance、OBB extents、conic 和 `gl_FragCoord.xy` 处于同一 pixel coordinate system。

Projected data 的逻辑字段为：`center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`（inverse covariance coefficients: xx, xy, yy）、`opacity`、`rgb`。Cull/Project compute pass 预计算 conic，fragment shader 不做 per-pixel 2×2 matrix inverse。实际 GPU struct 按 std430 / vec4 packing 实现。

Fragment alpha 防御规则：`power = -0.5 * mahalanobis_distance`，`power < -20` 时 discard；`alpha = clamp(opacity * exp(power), 0, 1)`；`alpha < 1e-4` 时 discard。3σ cutoff 边界的 power 约为 -4.5，因此 -20 只裁掉极低贡献或异常 fragment。glTF 直接加载阶段按 KHR 校验 opacity，shader 侧 clamp 仅作为防御。

GPU work buffers 每帧先 reset：`visible_count = 0`，sort entries 全部填 invalid sentinel `{UINT_MAX, UINT_MAX}`，`indirect.instanceCount = 0`。Cull/project append valid entries 到 `sort_entries[0..visible_count)`；ascending sort 后 valid entries 位于前段，sentinel 位于尾部；draw instanceCount=`visible_count`，不 draw capacity。`VkDrawIndirectCommand` 的固定字段由 CPU 初始化（`vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0`），GPU 只在 cull/project 后将 `visible_count` 写入 `instanceCount`。同步依赖必须覆盖 reset→cull/project、cull/project→sort、sort/projected data→graphics shader、visible_count→indirect update、indirect write→draw indirect。

Sort 必须对相同 `distance_key` 保持 deterministic ordering，否则半透明累积可能因相等 key 的帧间顺序变化而闪烁。Bitonic baseline compare 使用 lexicographic `(distance_key, global_splat_index)` 升序。后续 Radix sort 仍只处理 32-bit distance key 以保持性能目标，但实现前必须确认 equal-key deterministic ordering 方案。

### Tile Shared Memory 协作预加载（Phase 3.5 起）

Per-tile compute shader 中，workgroup 内所有线程协作将该 tile 的 splat 数据批量加载进 shared memory，而非每线程独立从全局内存随机读取。分批处理：每批加载 workgroup 大小数量的 splat 进 shared memory，处理完再加载下一批。

### 颜色累积精度（Phase 3.5 起）

Tile compute shader 中前到后 alpha blend 的颜色累积使用 FP32（shared memory 中 `vec4` 全精度），仅在最终 `imageStore` 输出时转为目标格式。避免 FP16 累积在 100+ 次 blend 后产生颜色漂移和灰雾。

## 性能目标

| 场景规模 | 目标 |
|---------|------|
| 1M gaussians | Phase 3.0：正确性基线，性能未必实时；Phase 3.1 后目标基本实时（30+ FPS @ 1080p） |
| 10M gaussians | Phase 3.0-3.2：可正确渲染，允许低帧率；Phase 3.5-3.7 优化后目标可交互（20+ FPS） |

## 防御性措施

内置于基础实现（Phase 3.0），不单独成阶段：

| 异常场景 | 防御方式 |
|---------|---------|
| 巨型屏幕投影 | projected OBB 任一半轴超过屏幕短边 25% 时 discard，不做 extent clamp |
| 针状 splat（训练 artifact） | 加载时长宽比过滤/clamp |
| 近平面投影爆炸 | behind-camera / near-plane 不稳定 splat 可 discard；z clamp 仅用于防止 projection math NaN/Inf |
| 视锥边缘大 splat 误剔除 | 球体 vs frustum 平面测试（包围半径膨胀） |
| 2D 协方差矩阵退化 | 对角线加 ε 保正定 |
| 高斯贴脸 | GS 专用 near plane（初始为场景 AABB 对角线的 0.5%）；贴脸 splat 视觉无意义且性能危险，允许 discard |

---

## Phase 3.0：基础渲染

**目的**：正确渲染 GS 场景的最小可行实现。

**技术**：

- Upload-time bake（transform 烘焙、3D 协方差预计算；SH rotation 前期先支持 identity/no-rotation happy path，Phase 3.0 末期补完整 Wigner-D）
- GPU 数据上传（per-attribute SoA storage buffers；通过 GS 持久 Set 3 descriptor set 绑定）
- Compute frustum cull + 2D 投影 + per-splat SH 求值
- GPU Bitonic Sort（按 camera distance squared 排序可见列表；排序项为 uint distance_key + uint global_splat_index，后续演进为自实现 Radix Sort）
- Indirect Draw instanced quads + front-to-back premultiplied-under 硬件 blend（fragment 输出 `vec4(rgb * alpha, alpha)`；srcColor/srcAlpha=`ONE_MINUS_DST_ALPHA`、dstColor/dstAlpha=`ONE`、op=`ADD`；colorWriteMask=RGBA；R16G16B16A16Sfloat target loadOp=CLEAR `vec4(0,0,0,0)`、storeOp=STORE；depth test/write off；cull none）
- R16G16B16A16Sfloat GS composition 输出 + GS 管线内部 colorSpace→linear 转换 + TonemappingPass 复用（GS 使用 LinearClamp mode bypass exposure / tonemap curve）
- 上述防御性措施

**GS descriptor set 约定**：Phase 3.0 使用 GS 子系统独立的持久 Set 3 descriptor set，包含静态 baked scene buffers 与每帧 GPU work buffers。该 Set 3 与现有 PT / compute pass 的 Set 3 push descriptor layout 是不同 pipeline layout 下的不同语义，不混用、不冲突。Descriptor 在 buffer 创建或重建时写入；每帧只重置或改写 work buffer 内容。

**排序长度演进**：Bitonic baseline 先按 `sort_capacity = next_power_of_two(total_splat_count)` 全量排序，invalid entries 使用 sentinel 排到末尾。Radix sort 先实现同样的 capacity 路径并与 bitonic baseline 对比，确认算法正确后，再实现 visible-count-driven radix sort，由 GPU 侧 `visible_count` 限制排序工作量，不进行 CPU readback。

**3D covariance bake**：KHR `SCALE` 表示 Gaussian principal axes 的标准差 σ，因此 local covariance 使用 `scale²`：`Σ_local = R * diag(scale²) * Rᵀ`。Upload-time bake 用 glTF node global transform 的线性部分 `M3x3` 得到 `Σ_world = M3x3 * Σ_local * M3x3ᵀ`，position 使用完整 global transform 变换到 world space。GS static baked GPU buffers 存储 world position、world covariance、opacity、SH 和 optional/reserved primitive metadata，不存储原始 rotation/scale；原始 rotation/scale 可继续保留在 CPU 侧用于 reload、debug 或 future rebake。

Upload bake 同时预计算 per-splat `world_radius_3sigma` 作为 frustum cull sphere 半径。Phase 3.0 使用保守 trace bound：`radius = 3 * sqrt(max(trace(Σ_world), 0))`。该 bound 满足 `max_eigenvalue(Σ_world) <= trace(Σ_world)`，因此不会因半径过小误剔除；代价是细长 splat 的 sphere 偏保守。Cull 使用 world-space sphere(center=`position_world`, radius=`world_radius_3sigma`) vs frustum planes。Projected OBB giant discard 是独立的 screen-space safety check，不替代 world-space frustum cull。未来可用 max eigenvalue 得到更紧半径。

**glTF GS 数据校验**：直接加载 glTF/GLB 时，`OPACITY` 必须 finite 且在 [0,1]，`SCALE` 必须 finite 且所有分量 >= 0，`ROTATION` 必须 finite 且为 unit quaternion。非法数据按 KHR 规范报错，不静默 clamp 或 normalize。PLY converter 产生的 opacity/scale 经过 sigmoid/exp 处理，但这不替代 glTF 直接加载校验。

**Node transform 限制**：KHR 要求 GS node transform 可分解为 regular translation、proper rotation 和 positive scale。Phase 3.0 对包含 reflection / negative determinant 或不可分解线性部分的 global transform 报错或跳过上传，避免 covariance 与 SH rotation 在非 proper rotation 情况下静默错误。

**SH rotation 实现顺序**：为优先跑通 happy path，Phase 3.0 前期只支持 identity/no-rotation transform 的 SH 直接上传；遇到非 identity transform rotation 必须报错或跳过上传，不能静默渲染错误。完整 Wigner-D degree 1-3 rotation 放到 Phase 3.0 末期实现，并补充 identity 不变、degree 1 已知旋转、PLY 坐标翻转一致性等验证。

**Capacity 语义**：`capacity` 不是固定渲染器上限，而是由当前 GS scene 的 `total_splat_count` 派生并随场景重建。高斯数量不会因为超过预设 capacity 而被静默截断；若显存或 Vulkan 限制不足以分配当前场景所需 buffers，则加载/渲染失败并报错。

**Distance key 编码**：排序项为 `{uint distance_key, uint global_splat_index}`。`distance_key = floatBitsToUint(camera_distance_squared)`，仅对 finite non-negative float 使用；异常距离写 invalid sentinel。Invalid entry sentinel 固定为 `{UINT_MAX, UINT_MAX}`，ascending sort 即 front-to-back，sentinel 自然排到末尾。Bitonic baseline 使用 `global_splat_index` 作为稳定 tie-break；Radix sort 只处理 32-bit `distance_key`，`global_splat_index` 作为 payload 搬运。

**SH view direction**：SH 求值使用 KHR 定义的 viewing direction：`normalize(splat_world_position - camera_world_position)`，即 camera → splat。不要使用 PBR 中常见的 surface/splat → camera 方向，否则奇数阶 SH 项符号会反转。若长度接近 0，使用 camera forward 作为 fallback。

**SH evaluation 位置**：Phase 3.0 在 cull/project compute pass 中对每个可见 splat 求一次 SH RGB，并写入 projected data；fragment shader 不求 SH。这样避免 per-fragment 重复求值，并保持 quad fragment 阶段只负责高斯 alpha 衰减和 premultiplied 输出。Phase 3.1 可将 SH evaluation 拆成独立 post-cull compute pass，并与已规划的距离自适应 SH 截断配合。Phase 3.x 当前不规划 SH cache 或独立低阶 fallback。

**Primitive metadata / ColorSpace 约定**：KHR 的 `colorSpace`、`kernel`、`projection`、`sortingMethod` 是 primitive extension 属性，因此同一 asset 可以包含 metadata 不同的 GS primitive。Phase 3.0 不实现 per-primitive 分支管线，要求同一 GS scene 内所有 primitive metadata 一致：`kernel = ellipse`、`projection = perspective`、`sortingMethod = cameraDistance`、`colorSpace` 一致；不一致时报错。GS composition 遵循 scene/primitive colorSpace：`srgb_rec709_display` 在 sRGB display-referred 数值中 blend，blend 完成后的 composition target 仍存储 sRGB blended values，随后由 GS sRGB→linear conversion pass 输出 linear target；`lin_rec709_display` 直接在线性 display-referred 数值中 blend，composition target 已是 linear，可 bypass conversion pass 并直接作为 TonemappingPass 输入。TonemappingPass 的输入始终是 linear，GS 使用 `LinearClamp` mode bypass exposure / tonemap curve，不负责 GS sRGB decode。未来如需支持混合 colorSpace，可选第一个 primitive 的 colorSpace 作为 composition space，将其他 primitive 先转换到该空间后再参与 blend，作为近似方案另行实现。

多个 primitive 在 upload 时拼接为连续 global splat buffers，并在 CPU 侧保留 per-primitive ranges / source primitive index / metadata 供 debug、error reporting 和未来 per-primitive behavior 使用。Phase 3.0 shader 按 global splat index 访问 baked SoA buffers，不按 primitive metadata 分支；GPU primitive metadata buffer 可选/预留。

**Projected data 索引策略**：Phase 3.0 projected data 按 global splat index dense 存储，sort entry payload 存 global splat index。Cull/project 对可见 splat 写 `projected_data[global_index]` 并写入 sort entry `{distance_key, global_index}`；不可见 splat 的 projected data 未定义。Draw 使用 `sorted_entries[gl_InstanceIndex].splat_index` 读取 `projected_data[global_index]`。Draw instance count 固定为 `visible_count`，不 draw `sort_capacity`。Dense projected data 约占 `64B × total_splat_count` capacity，1M baseline 可接受；10M 目标前需重新评估 compact projected data（visible-index dense）以降低 VRAM/cache 压力。

Projected data 只存 draw 阶段需要的投影后数据：`center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`（inverse 2D covariance xx/xy/yy）、`opacity`、SH-evaluated `rgb`。它不存原始 position / rotation / scale / world covariance / SH coefficients / sort key；这些分别属于 static baked buffers 或 sort entries。Projected data 内容依赖 viewport，但容量只依赖 `total_splat_count`，因此 swapchain resize 时不重建 projected data buffer，下一帧 cull/project 会用新 viewport 覆盖内容。

**近裁剪 / 巨型投影策略**：GS near plane 初始为 scene AABB diagonal × 0.005，仅 GS 模式使用。贴脸 splat 在渲染上无意义且可能导致巨大 overdraw，因此 behind-camera、near-plane 不稳定或 projected OBB 过大的 splat 可以直接 discard。Projection z clamp 只用于防止 Jacobian / covariance projection 中出现 NaN/Inf，不用于强行保留贴脸 splat。Projected OBB 任一半轴超过 screen short side × 0.25 时 discard，不做 extent clamp。

**Alpha 语义**：GS composition target 和 GS linear target 均存储 accumulated premultiplied RGB 与 accumulated alpha。sRGB→linear conversion pass 只转换 RGB 通道，alpha 原样保留。Phase 3.0 不做 unpremultiply，不做背景合成；alpha 仅用于 GS 内部 front-to-back under 累积。TonemappingPass 在 GS 模式下忽略 alpha，最终 swapchain 输出 opaque alpha = 1。

**TonemappingPass mode**：TonemappingPass 保留为最终 swapchain output pass，通过 push constant `mode` 区分 `HdrAces` 与 `LinearClamp`。PT 使用 `HdrAces`：linear HDR → exposure → ACES；GS 使用 `LinearClamp`：linear display-referred input → per-channel hard clamp [0,1] → alpha=1。Mode 是 TonemappingPass 局部输出策略，不放入 GlobalUBO，避免改变全局 std140 layout 和影响其他 shader；也不新增 pipeline，单个 uniform branch 成本可忽略。TonemappingPass 不做 GS sRGB decode，GS 进入 TonemappingPass 前必须已经是 linear。

**Resource lifetime**：Scene load/reload 重建 static baked buffers（world position/covariance/radius/opacity/SH/optional metadata）和 capacity-based work buffers（visible count、projected data、sort ping-pong、indirect command），并重写 GS Set 3。Swapchain resize 只重建 viewport-sized GS composition/linear targets，并更新对应 render target descriptors。Projected data/work buffers 容量只依赖 `total_splat_count`，resize 时不重建；其内容虽依赖 viewport，但下一帧 cull/project 会覆盖。Reload/resize 后不得有 descriptor 指向已销毁的 buffers/images。

**RenderGraph buffer barriers**：现有 RenderGraph 只自动处理 image barriers，buffer resource usage 在 compile 阶段会跳过。Phase 3.0 接入 GS compute/sort/draw 前需要扩展 RG buffer barrier 支持：track per-buffer last stage/access，emit `VkBufferMemoryBarrier2`，并补齐 GS 所需 stage/access 映射（Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write）。优先扩展 RG，不在 GS pass 内长期手写 barriers。

**GS push constants**：GS 复用 GlobalUBO 中的 view、projection、view_projection、camera_position、screen_size，不在 GS push constants 中重复矩阵。GS 专用 per-frame 小参数放入 `GSPushConstants`：`total_splat_count`、`sort_capacity`、`color_space`、`flags`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`。`max_projected_extent_px` 通常为 screen short side × 0.25，discard thresholds 初始为 alpha=1e-4、power=-20。Tonemapping mode 是 TonemappingPass 独立 push constant，不属于 GS push constants。

**GS color clamp**：SH 求值后的 RGB 负分量必须在 premultiply 前 clamp 到 0，符合 KHR 对 negative color 的要求。Phase 3.0 不做 per-splat upper clamp，避免改变 splat 累积结果。最终 GS 输出采用 per-channel hard clamp 到 [0,1]，作为 KHR 允许的 clamped output；GS 模式下 TonemappingPass bypass exposure / tonemap curve，仅执行 linear passthrough + hard clamp + alpha=1。`srgb_rec709_display` 的 conversion pass 在 sRGB decode 前对 composed sRGB RGB 做 [0,1] hard clamp。未来如需更好保 hue，可增加 max-channel range compression 作为 GS display-referred tonemapping 选项。

**实现决策**：详见 `tasks/reflector-phase3.md` 决策记录表。

**结束目标**：能正确渲染 1M gaussian 场景，画面正确无明显 artifact，性能未必达到实时。Phase 3.0 是 correctness baseline，不以最终性能为目标。Happy path 为 KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform；支持单个或多个 GS primitive，primitive 拼接为 global splat buffers 并保留 CPU per-primitive ranges；支持 `srgb_rec709_display` 和 `lin_rec709_display` 二选一 scene，不支持 mixed colorSpace scene。

Phase 3.0 必须验证：投影中心、3σ OBB、front-to-back 排序、premultiplied-under blend、sRGB→linear conversion、linear GS conversion bypass、TonemappingPass GS bypass、resize 后 targets/work buffers 重建、`visible_count = 0` 空可见集、非法 asset rejection。

Phase 3.0 前期明确不支持：mixed metadata/colorSpace、非 identity SH rotation、compact projected data、tile-based renderer、10M 性能目标、max-channel range compression、background/mesh/skybox 合成。

---

## Phase 3.1：运行时优化

**目的**：减少每帧计算量和带宽消耗。

**技术**：

- SH 颜色预求值（从 cull/project shader 分离为独立 post-cull compute pass，仅对可见 splat 求值；与距离自适应 SH 截断配合——远处 splat 只求低阶 SH）
- 多级剔除（sub-pixel 半径 / 低 opacity / 异常大投影）
- GPU buffer 热/冷/暖分离
- 距离自适应 SH 截断（远处只算低阶 SH）

**优先级**：SH 预求值 > 多级剔除 > 热冷分离 > SH 截断

**结束目标**：1M 场景达到基本实时（30+ FPS）。

---

## Phase 3.2：加载时优化

**目的**：通过预处理改善运行时全管线的数据效率。

**技术**：

- 空间分块 + 逐块 AABB（chunk 级视锥剔除，cull 只遍历可见 chunk）
- FP16 / 压缩量化（以 Phase 3.0 bake 后的 GPU 数据为对象，例如 world-space covariance、opacity、SH、position 的分块局部表示；具体量化格式、误差预算和是否保留 FP32 在 Phase 3.2 开始前重新讨论决定）
- Morton/Z-order 空间排序（全管线缓存命中率 + 分支一致性）
- 死 splat 剪枝（opacity / scale / position 异常值过滤）
- 世界空间包围球半径预计算

**优先级**：空间分块 > bake 后 GPU 数据量化方案设计 > Morton 排序 > 死 splat 剪枝 > 包围球预计算

**结束目标**：相比 FP32 全属性上传，VRAM 占用减少约 45%；cull pass 从全量遍历降为可见 chunk 遍历；10M 场景可加载和渲染。

---

## Phase 3.5：Tile-Based Compute 渲染

**目的**：替换硬件光栅末端为全 compute 管线，获得 per-tile early-out。

**技术**：

- Tile binning（splat → tile 多对多映射，per-tile splat index list）
- Per-tile compute shader（shared memory 前到后 alpha blend，transmittance 归零时 early-out）
- `imageStore` 输出到 storage image

**保留**：cull、projection、sort、visible list compaction、SH pre-eval 等上游管线不变。
**替换**：instanced quad indirect draw → tile binning + per-tile compute render。

**结束目标**：密集重叠场景性能大幅提升，10M 场景中等重叠下可交互。

---

## Phase 3.6：高级优化

**目的**：压榨排序、剔除、调度环节的剩余性能空间。

**技术**（按性价比排序）：

- Budget Rendering（按 `opacity × projected_area` 重要性截断可见列表，锁定渲染上限保帧率）
- Non-Empty Tile Indirect Dispatch（只 dispatch 有覆盖的 tile）
- Temporal Sort Reuse（帧间排序复用：验证 pass 检查相邻元素有序性，无序率低于阈值时做局部修正，超过阈值时 fallback 全量 radix sort。缓慢移动时省 50-80% sort 成本）
- Previous-Frame Depth Occlusion（前帧深度 reproject 遮挡剔除）
- Pipeline Double-Buffer Overlap（Frame N+1 cull+sort 与 Frame N tile render 重叠）

**决策点**：Phase 3.6 结束时评估 10M 场景 VRAM 占用，决定是否追加 VRAM Streaming 模块。

**结束目标**：全部已识别的性能瓶颈得到缓解，1M 场景高帧率，10M 场景流畅可交互。

---

## Phase 3.7：LOD + 抗锯齿

**目的**：10M 场景空间自适应精度控制 + 远景画质改善。

**技术**：

- Chunk 级重要性子采样 LOD：加载时 per-chunk 按 `opacity × max_scale²` 排序，运行时按 chunk 到相机距离选择渲染比例（近 100% → 远 10-25%）。利用 Phase 3.2 已有的空间分块基础设施，运行时额外开销仅为 per-chunk 距离计算和 count 选择（在 chunk 级 cull pass 中顺带完成）
- Mip-Splatting 抗锯齿：在 cull+projection compute shader 中对 2D 协方差施加像素 footprint 下界约束，防止 splat 小于一个像素导致闪烁。放在 Phase 3.7 而非更早，是因为优先级低于性能优化——它改善画质但不影响正确性或性能

**结束目标**：10M 场景实时可交互（LOD 大幅压缩远景可见集），远景无 aliasing 闪烁。
