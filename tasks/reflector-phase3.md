# Reflector Phase 3.0:Gaussian Splatting 基础渲染

> 目标:正确渲染 GS 场景的最小可行实现。
> 整体路线见 `docs/phase3-decisions.md`,实现计划见 `docs/current-phase.md`。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | Transform 处理 | Baked:upload 时烘焙到 position/covariance/SH,低频变更时重上传 |
| 2 | GPU buffer 布局 | Per-attribute 独立 storage buffer(SoA) |
| 3 | SH 上传范围 | 全部 degree(完整画质) |
| 4 | CPU 内存布局 | 保持现有 SoA 结构,upload 函数中转置高阶 SH |
| 5 | 排序键值 | Camera distance squared(符合 KHR `sortingMethod = cameraDistance`,不做 sqrt) |
| 6 | 排序算法 | Bitonic sort 先行(正确性验证),后自实现 Radix sort |
| 7 | 排序项格式 | 2×32-bit(uint distance_key + uint global_splat_index);Radix sort 仅排序 32-bit key,index 作为 payload 搬运 |
| 8 | Cull 与 Projection | 合并在一个 compute shader |
| 9 | Workgroup size | 256 |
| 10 | Draw 方式 | Non-indexed instanced draw(gl_VertexIndex % 6,两三角形) |
| 11 | 输出颜色格式 | R16G16B16A16Sfloat |
| 12 | Color / Tonemapping | GS 管线内部按 primitive `colorSpace` 完成 composition;`srgb_rec709_display` 在 sRGB display-referred 数值中 blend 后由 GS sRGB→linear conversion pass 输出 linear target,`lin_rec709_display` 直接将 composition target 作为 linear input。TonemappingPass 输入始终为 linear,GS 使用 `LinearClamp` 模式 bypass exposure / tonemap curve。 |
| 13 | Descriptor set | GS 使用独立的持久 Set 3 descriptor set;包含静态 baked scene buffers 与每帧 GPU work buffers。它不是现有 PT/compute pass 的 Set 3 push descriptor layout。 |
| 14 | Per-frame 参数 | Push constant(GS 专用 uniform) |
| 15 | 模式分发 | RenderInput 中 RenderMode,Renderer::render() 分支 |
| 16 | 资源创建策略 | PT + GS 两套资源均在初始化时创建 |
| 17 | 3D Covariance 计算 | KHR `SCALE` 是 Gaussian principal axes 的 σ,local covariance 为 `R * diag(scale2) * RT`。Upload 时用 node global transform 线性部分 `M3x3` 烘焙 `Σ_world = M3x3 * Σ_local * M3x3T`,存储 6 floats symmetric 3×3。GPU static baked buffers 不保留原始 rotation/scale。 |
| 18 | SH 求值位置 | Phase 3.0 在 cull/project compute shader 中对每个可见 splat 求一次 SH RGB,并写入 projected data;fragment shader 不求 SH。Phase 3.1 可拆成独立 post-cull compute pass,并与已规划的距离自适应 SH 截断配合。Phase 3.x 当前不规划 SH cache 或独立低阶 fallback。 |
| 19 | SH rotation | Phase 3.0 前期先走 identity/no-rotation happy path;非 identity transform rotation 暂时报错或跳过上传。完整 Wigner-D degree 1-3 放到 Phase 3.0 末期实现。 |
| 20 | OBB 截断半径 | 3σ(KHR 规范要求) |
| 21 | 排序长度策略 | Bitonic 先按 `sort_capacity = next_power_of_two(total_splat_count)` 排序,invalid entries 使用 sentinel;Radix 先实现同样的 capacity 路径并与 Bitonic 对比,确认正确后再实现 visible-count-driven radix。 |
| 22 | Capacity 语义 | `capacity` 由当前 GS scene 的 `total_splat_count` 派生并随场景重建,不是固定上限;不会因 splat 数超过预设容量而静默截断。若显存或 Vulkan 限制不足以分配当前场景所需 buffers,则加载/渲染失败并报错。 |
| 23 | GS colorSpace | KHR 允许每个 primitive 拥有自己的 `colorSpace`。Phase 3.0 要求同一 GS scene 内所有 primitive 的 `colorSpace` 一致;不一致时报错。未来如有需要,可选第一个 primitive 的 colorSpace 作为 composition space,将其他 primitive 先转换到该空间后再 blend,作为近似方案另行实现。 |
| 24 | Blend state | Front-to-back premultiplied-under。Fragment 输出 `vec4(rgb * alpha, alpha)`;blend factor 为 srcColor/srcAlpha=`ONE_MINUS_DST_ALPHA`、dstColor/dstAlpha=`ONE`、op=`ADD`;colorWriteMask=RGBA。GS target 为 R16G16B16A16Sfloat,loadOp=CLEAR `vec4(0,0,0,0)`,storeOp=STORE;depth test/write off;cull none。 |
| 25 | distance key 编码 | `distance_key = floatBitsToUint(camera_distance_squared)`,仅对 finite non-negative float 使用;异常值写 sentinel。Invalid entry sentinel 为 `{distance_key = UINT_MAX, global_splat_index = UINT_MAX}`;ascending sort 即 front-to-back。 |
| 26 | SH view direction | SH 求值方向为 KHR 定义的 camera → splat:`normalize(splat_world_position - camera_world_position)`,不要使用 PBR 常见的 surface/splat → camera 方向。长度接近 0 时使用 camera forward fallback。 |
| 27 | 2D covariance 空间 | Cull/Project 输出使用 screen pixel space:center、2D covariance、OBB axes/extents 均以 pixel 为单位;fragment shader 使用 `gl_FragCoord.xy` 直接计算高斯衰减;vertex shader 将 pixel-space quad corners 转回 NDC。 |
| 28 | Projected data 字段 | 逻辑字段为 `center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`(inverse covariance: xx, xy, yy)、`opacity`、`rgb`。Compute pass 预计算 conic,fragment shader 不做 per-pixel matrix inverse。实际 GPU struct 按 std430 / vec4 packing 实现。 |
| 29 | Fragment alpha 防御 | Fragment 中计算 `power = -0.5 * mahalanobis_distance`;`power < -20` discard;`alpha = clamp(opacity * exp(power), 0, 1)`;`alpha < 1e-4` discard。glTF 直接加载阶段按 KHR 校验 opacity,shader 侧 clamp 仅作防御。 |
| 30 | 近裁剪 / 巨型投影 | GS near plane 初始为 scene AABB diagonal × 0.005。贴脸、behind-camera、near-plane 不稳定或投影过大的 splat 可直接 discard;z clamp 仅用于防止 projection math NaN/Inf,不用于强行保留贴脸 splat。Projected OBB 任一半轴超过 screen short side × 0.25 时 discard,不做 extent clamp。 |
| 31 | GS linear 输出 | GS composition target 存储 primitive colorSpace 中的 blended result。`srgb_rec709_display` 场景在 GS draw 后增加 sRGB→linear conversion pass,输出 linear target 给 TonemappingPass;`lin_rec709_display` 场景直接将 composition target 作为 linear input,bypass conversion pass。TonemappingPass 输入始终 linear,不做 GS sRGB decode。 |
| 32 | GS alpha 语义 | GS targets 存储 accumulated premultiplied RGB 与 accumulated alpha。sRGB→linear conversion pass 只转换 RGB,alpha 原样保留。Phase 3.0 不做 unpremultiply,不做背景合成;TonemappingPass 在 GS 模式下忽略 alpha 并输出 opaque alpha = 1。 |
| 33 | GS color clamp | SH-evaluated RGB 负分量在 premultiply 前 clamp 到 0;Phase 3.0 不做 per-splat upper clamp。最终 GS 输出采用 per-channel hard clamp 到 [0,1],符合 KHR 的 clamped output;未来如需更好保 hue,可增加 max-channel range compression 作为 GS display-referred tonemapping 选项。 |
| 34 | GPU work buffer reset / barriers | 每帧先 reset `visible_count = 0`、sort entries 全部填 invalid sentinel、`indirect.instanceCount = 0`。Indirect command 固定字段 CPU 初始化(`vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0`),GPU 只写 `instanceCount = visible_count`。RenderGraph/command barriers 必须覆盖 reset→cull/project、cull/project→sort、sort/projected data→graphics shader、visible_count→indirect update、indirect write→draw indirect。 |
| 35 | Pixel space / viewport | GS draw pass 使用 positive-height normal viewport,不做 negative viewport Y-flip。Screen pixel space 使用 Vulkan framebuffer 坐标:top-left origin、x right、y down。Vertex shader 将 pixel corners 转 NDC 时不做 Y flip;fragment shader 直接用 `gl_FragCoord.xy - center_px`。 |
| 36 | 2D covariance 投影 | GS 使用与 PT/reference view 相同的 camera pose、FOV、aspect 和 viewport,不引入独立相机模型;可使用 GS-specific near plane 保持投影稳定。`center_px` 由 clip/NDC 转 pixel 得到,不做 Y flip。2D covariance 使用 view-space covariance 和 pixel focal length:`fx = 0.5 * width * abs(proj[0][0])`、`fy = 0.5 * height * abs(proj[1][1])`,`cov_2d = J * cov_view * Jᵀ`。 |
| 37 | Multi-primitive metadata | Phase 3.0 要求同一 GS scene 内 primitive metadata 一致:`kernel=ellipse`、`projection=perspective`、`sortingMethod=cameraDistance`、`colorSpace` 一致;否则加载/上传报错。多个 primitive 拼接为 global splat buffers,通过 per-primitive ranges 记录来源;shader 按 global splat index 访问 baked SoA buffers,不按 primitive metadata 分支。GPU primitive metadata buffer 可选/预留。 |
| 38 | Projected data 索引 | Phase 3.0 projected data 按 global splat index dense 存储，sort entry payload 存 global splat index。Draw 使用 `sorted_entries[gl_InstanceIndex].splat_index` 读取 `projected_data[global_index]`。Invisible splat 的 projected data 未定义；draw instanceCount=`visible_count`，不 draw capacity。Dense projected data 约 64B × total_splat_count capacity，1M baseline 可接受，10M 目标前需重新评估 compact projected data。 |
| 39 | Frustum cull radius | Upload bake 时预计算 per-splat `world_radius_3sigma`，Phase 3.0 使用保守 trace bound：`radius = 3 * sqrt(max(trace(Σ_world), 0))`。Cull 使用 world-space sphere(center=position_world, radius=world_radius_3sigma) vs frustum planes。Projected OBB giant discard 是独立的 screen-space safety check。未来可用 max eigenvalue 得到更紧半径。 |
| 40 | Sort valid range / tie-break | Reset 将 sort capacity 全部填 sentinel；cull/project append valid entries 到 `sort_entries[0..visible_count)`；ascending sort 后 valid entries 位于前段，sentinel 位于尾部；draw instanceCount=`visible_count`，不 draw capacity。Bitonic compare 使用 lexicographic `(distance_key, global_splat_index)`，所有未来 sort 实现都必须对相同 `distance_key` 保持 deterministic ordering，避免透明累积闪烁。 |
| 41 | TonemappingPass GS mode | TonemappingPass 保留为最终 swapchain output pass，通过 push constant `mode` 区分 `HdrAces` 与 `LinearClamp`。PT 使用 `HdrAces`：linear HDR → exposure → ACES；GS 使用 `LinearClamp`：linear display-referred input → per-channel hard clamp [0,1] → alpha=1。Mode 不放入 GlobalUBO，不新增 pipeline；TonemappingPass 不做 GS sRGB decode。 |
| 42 | Resource lifetime | Scene load/reload 重建 static baked buffers 和 capacity-based work buffers，并重写 GS Set 3。Swapchain resize 只重建 viewport-sized GS composition/linear targets，并更新对应 render target descriptors。Projected data/work buffers 容量只依赖 `total_splat_count`，resize 时不重建，下一帧 cull/project 覆盖内容。禁止 reload/resize 后 descriptor 指向已销毁资源。 |
| 43 | RenderGraph buffer barriers | 现有 RenderGraph 只自动处理 image barriers，buffer resource usage 在 compile 阶段会跳过。Phase 3.0 接入 GS compute/sort/draw 前需要扩展 RG buffer barrier 支持：track per-buffer last stage/access，emit `VkBufferMemoryBarrier2`，并补齐 GS 所需 stage/access 映射（Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write）。优先扩展 RG，不在 GS pass 内长期手写 barriers。 |
| 44 | GS push constants | GS 复用 GlobalUBO 中的 view/projection/view_projection/camera_position/screen_size，不在 GS push constants 里重复矩阵。GS 专用 per-frame 小参数放入 `GSPushConstants`：`total_splat_count`、`sort_capacity`、`color_space`、`flags`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`。Tonemapping mode 是 TonemappingPass 独立 push constant，不属于 GS push constants。 |

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
