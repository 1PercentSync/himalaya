# Reflector Phase 3.0:Gaussian Splatting 基础渲染

> 目标:正确渲染 GS 场景的最小可行实现。
> 整体路线见 `docs/phase3-decisions.md`,当前阶段约定见 `docs/current-phase.md`。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## Step 0:GPU 数据上传

- [ ] 扩展 RenderGraph buffer barrier 支持（现有 RG 只处理 image barriers；新增 per-buffer hazard tracking、`VkBufferMemoryBarrier2` emission、GS 所需 stage/access 映射：Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write）
- [ ] 定义 GS GPU 数据结构(buffer 句柄、splat 总数、`sort_capacity = next_power_of_two(total_splat_count)`、多个 primitive 拼接后的 global splat buffers、CPU per-primitive range 映射;GPU primitive metadata buffer 可选/预留)
- [ ] 实现 glTF 直接加载数据校验补强(opacity 必须 finite 且在 [0,1];SCALE 必须 finite 且所有分量 >= 0;ROTATION 必须 finite 且为 unit quaternion;同一 GS scene 内 primitive metadata 必须一致:`kernel=ellipse`、`projection=perspective`、`sortingMethod=cameraDistance`、`colorSpace` 一致;非法 glTF 按 KHR 规范报错,不静默 clamp)
- [ ] 实现 position baking(apply transform → world space)
- [ ] 实现 3D 协方差预计算（KHR scale 是 σ，使用 scale²；`Σ_local = R * diag(scale²) * Rᵀ`；`Σ_world = M3x3 * Σ_local * M3x3ᵀ`；输出 6 floats symmetric 3×3；同时预计算 `world_radius_3sigma = 3 * sqrt(max(trace(Σ_world), 0))`）
- [ ] 实现 SH upload happy path(identity/no-rotation transform 直接上传;非 identity transform rotation 暂时报错或跳过上传,避免静默错误)
- [ ] 实现 GPU buffer 创建与数据上传(per-attribute storage buffers)
- [ ] 定义 GS Set 3 持久 descriptor layout(静态 baked scene buffers + 每帧 GPU work buffers)和 `GSPushConstants` 结构体(`total_splat_count`、`sort_capacity`、`color_space`、`flags`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`;复用 GlobalUBO camera/screen 字段,不重复矩阵)
- [ ] 创建 GS Set 3 descriptor set 并写入 buffer 绑定(descriptor 随 buffer 创建/重建更新,非每帧 push)
- [ ] 编译验证

## Step 1:Cull/Project Compute Pass

- [ ] 创建 intermediate buffers(visible count atomic、dense-by-global-index projected data buffers、sort entry ping-pong、indirect draw command;projected data 逻辑字段为 center_px/axis0_extent_px/axis1_extent_px/conic(opacity packed)/rgb,容量由当前 scene `total_splat_count` 派生;resize 不重建,下一帧 cull/project 覆盖内容)
- [ ] 实现每帧 work buffer reset(`visible_count = 0`、sort entries 填 invalid sentinel、`indirect.instanceCount = 0`;indirect 固定字段由 CPU 初始化)
- [ ] 创建 cull/project compute shader(buffer 声明、workgroup 256、main 框架)
- [ ] 实现视锥剔除（world-space sphere center=`position_world`, radius=`world_radius_3sigma` vs frustum planes）+ 防御性措施（behind-camera / near-plane 不稳定 splat discard、projection z 防 NaN/Inf clamp、协方差正定化 ε、长宽比 clamp、projected OBB 半轴超过 screen short side × 0.25 时 discard）
- [ ] 实现 3D→2D 协方差投影 + OBB 计算(与 PT/reference view 共用 camera pose/FOV/aspect/viewport,允许 GS-specific near plane;`center_px` 由 clip/NDC 转 pixel 且不做 Y flip;使用 pixel focal length `fx = 0.5 * width * abs(proj[0][0])`、`fy = 0.5 * height * abs(proj[1][1])`;`cov_2d = J * cov_view * Jᵀ`;eigendecomposition → pixel-space oriented extent,3σ 截断)
- [ ] 实现 per-visible-splat SH 求值(`view_dir = normalize(splat_world_position - camera_world_position)`,即 camera → splat;evaluate all degrees → RGB;负分量 clamp 到 0;写入 projected data;fragment shader 不求 SH)
- [ ] 实现 uniform-control-flow 写法的 subgroup ballot 可见列表 append + 排序项生成(valid entries 写入 `sort_entries[0..visible_count)`;`distance_key = floatBitsToUint(camera_distance_squared)`,异常值写 sentinel;subgroup intrinsic 不放入 divergent 分支)
- [ ] 创建 compute pipeline + dispatch 逻辑(C++ 端)
- [ ] 编译验证

## Step 2:Bitonic Sort

- [ ] 创建 bitonic sort compute shader(2×32-bit sort entry compare-and-swap;lexicographic `(distance_key, global_splat_index)` 升序;sentinel 排末尾;排序后 `[0, visible_count)` 必须全为 valid entries)
- [ ] 实现 capacity sort 多 pass dispatch 逻辑(`N = sort_capacity`,log2(N) stages × log(N) steps)
- [ ] 编译验证

## Step 3:Quad Rendering

- [ ] 创建 vertex shader(从 sorted entries 读取 global splat index,再读取 `projected_data[global_index]`;展开 6-vertex instanced quad,并将 pixel-space corners 转为 NDC;GS 使用 positive-height normal viewport,pixel→NDC 不做 Y flip)
- [ ] 创建 fragment shader(直接使用 `gl_FragCoord.xy - center_px` 与 pixel-space 2D covariance / conic 计算高斯 alpha 衰减,不做 Y flip;`power < -20` discard;`alpha = clamp(opacity * exp(power), 0, 1)`;`alpha < 1e-4` discard;SH-evaluated RGB 负分量 clamp 到 0 后输出 premultiplied color)
- [ ] 实现 indirect draw command buffer 设置(GPU 将 visible count 写入 `VkDrawIndirectCommand::instanceCount`;固定字段 `vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0` 由 CPU 初始化)
- [ ] 创建 R16G16B16A16Sfloat GS 渲染目标(loadOp=CLEAR `vec4(0,0,0,0)`,storeOp=STORE;swapchain resize 时重建 viewport-sized composition/linear targets 并更新 descriptors)
- [ ] 配置 graphics pipeline(depthTest/depthWrite disabled,cull none,colorWriteMask=RGBA,front-to-back premultiplied-under blend:shader 输出 `vec4(rgb * alpha, alpha)`,srcColor/srcAlpha=`ONE_MINUS_DST_ALPHA`,dstColor/dstAlpha=`ONE`,op=`ADD`)
- [ ] 编译验证

## Step 4:管线集成

- [ ] 创建 render_gaussian_splatting() 方法(orchestrate cull → sort → draw)
- [ ] 实现 RenderMode 分发(render() 根据 RenderMode 调用对应渲染路径)
- [ ] 实现 GS color conversion path(`srgb_rec709_display`:composition target → sRGB→linear pass → linear target;`lin_rec709_display`:bypass conversion,composition target 直接作为 linear target;conversion 只转换 accumulated premultiplied RGB,alpha 原样保留;sRGB decode 前对 composed sRGB RGB 做 [0,1] hard clamp)
- [ ] 扩展 TonemappingPass push constant mode(`HdrAces` / `LinearClamp`;PT 使用 `HdrAces`,GS 使用 `LinearClamp`;不放入 GlobalUBO,不新增 pipeline)
- [ ] 集成 TonemappingPass(GS 输出给 TonemappingPass 的输入始终为 linear;GS `LinearClamp` 模式跳过 exposure / tonemap curve,不在 TonemappingPass 内做 sRGB→linear;GS final RGB per-channel hard clamp 到 [0,1],忽略 GS alpha 并输出 opaque alpha = 1)
- [ ] 实现 GS 专用 near plane 计算(初始为 scene AABB diagonal × 0.005,仅 GS 模式使用)
- [ ] 端到端渲染验证(bitonic sort correctness baseline:投影中心/3σ OBB/排序/blend/color conversion/Tonemapping bypass/resize/visible_count=0/非法 asset rejection)

## Phase 3.0 完成标准

- [ ] Happy path 支持:KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform
- [ ] 支持单个或多个 GS primitive,primitive 拼接为 global splat buffers 并保留 CPU per-primitive ranges
- [ ] 支持 `srgb_rec709_display` 和 `lin_rec709_display` 二选一 scene,不支持 mixed colorSpace scene
- [ ] 以 1M splat 级别 correctness baseline 为目标,不以 10M 性能为 Phase 3.0 完成条件
- [ ] 明确前期不支持:mixed metadata/colorSpace、非 identity SH rotation、compact projected data、tile-based renderer、max-channel range compression、background/mesh/skybox 合成

## Step 5:Radix Sort

- [ ] 创建 radix sort compute shader(per-digit histogram + prefix sum + scatter;仅处理 32-bit distance_key,global_splat_index 作为 payload 搬运;必须对相同 `distance_key` 保持 deterministic ordering,具体方案在实现前确认)
- [ ] 实现 capacity radix 多 pass dispatch 逻辑(`N = sort_capacity`,每 pass 处理若干 bit,从 LSB 到 MSB)
- [ ] 替换 bitonic sort dispatch 为 radix capacity dispatch
- [ ] 正确性验证(radix capacity 与 bitonic capacity 基线对比渲染结果)
- [ ] 实现 visible-count-driven radix sort(GPU 侧根据 visible_count 限制排序工作量,不进行 CPU readback)
- [ ] 正确性验证(visible-count-driven radix 与 radix capacity 基线对比渲染结果)
- [ ] 实现 Wigner-D 矩阵 SH rotation(从 transform 提取旋转分量,旋转 degree 1-3 SH 系数)
- [ ] Wigner-D 正确性验证(identity 不变、degree 1 已知旋转、与 PLY 坐标翻转规则一致性检查)
- [ ] 编译验证
