# Reflector Phase 3.0:Gaussian Splatting 基础渲染

> 目标:正确渲染 GS 场景的最小可行实现。
> 整体路线见 `docs/phase3-decisions.md`,当前阶段约定与实现指南见 `docs/current-phase.md`。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## Step 0:RenderGraph buffer barriers

- [ ] 扩展 RenderGraph buffer resource usage 跟踪（记录 per-buffer last stage/access,不再在 compile 阶段跳过 buffer resource）
- [ ] 实现 `VkBufferMemoryBarrier2` 生成与提交（与现有 image barriers 合并到 `VkDependencyInfo`）
- [ ] 补齐 GS 所需 buffer usage 映射（Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write）
- [ ] 用最小 dummy buffer pass 验证 reset/write/read barrier 链路
- [ ] 编译验证

## Step 1:GS 数据契约与加载校验

- [ ] 定义 GS GPU 数据结构（buffer 句柄、splat 总数、`sort_capacity = next_power_of_two(total_splat_count)`、static buffers、work buffers、CPU per-primitive ranges、scene-level metadata）
- [ ] 定义 projected data / sort entry / indirect command 的 CPU 与 shader 共享布局约定（std430 / vec4 packing,2×32-bit sort entry）
- [ ] 实现 glTF 直接加载数据校验补强（opacity finite [0,1]、SCALE finite >= 0、ROTATION finite unit quaternion）
- [ ] 实现同一 GS scene 内 primitive metadata 一致性校验（`kernel=ellipse`、`projection=perspective`、`sortingMethod=cameraDistance`、`colorSpace` 一致）
- [ ] 编译验证

## Step 2:Upload-time bake 与 static buffers

- [ ] 实现 position baking（apply node global transform → world space）
- [ ] 实现 3D covariance baking（KHR scale 是 σ；`Σ_local = R * diag(scale²) * Rᵀ`；`Σ_world = M3x3 * Σ_local * M3x3ᵀ`；输出 6 floats symmetric 3×3）
- [ ] 实现 `world_radius_3sigma = 3 * sqrt(max(trace(Σ_world), 0))` 预计算
- [ ] 实现 node transform 合法性处理（reflection / negative determinant / 不可分解线性部分报错或跳过上传）
- [ ] 实现 SH upload happy path（identity/no-rotation transform 直接上传；非 identity transform rotation 暂时报错或跳过上传,避免静默错误）
- [ ] 创建并上传 static baked per-attribute storage buffers
- [ ] 编译验证

## Step 3:GS descriptors、work buffers 与 reset

- [ ] 定义 GS Set 3 持久 descriptor layout（static baked scene buffers + 每帧 GPU work buffers；不复用 PT/compute push descriptor Set 3 layout）
- [ ] 定义 `GSPushConstants`（`total_splat_count`、`sort_capacity`、`color_space`、`flags`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`；复用 GlobalUBO camera/screen 字段,不重复矩阵）
- [ ] 创建 GS work buffers（visible count atomic、dense-by-global-index projected data、sort entry ping-pong、indirect draw command；容量由当前 scene `total_splat_count` 派生）
- [ ] 创建 GS Set 3 descriptor set 并写入 buffer 绑定（descriptor 随 buffer 创建/重建更新,非每帧 push）
- [ ] 实现每帧 work buffer reset（`visible_count = 0`、sort entries 填 invalid sentinel、`indirect.instanceCount = 0`；indirect 固定字段由 CPU 初始化）
- [ ] 编译验证

## Step 4:Cull/Project compute pass

- [ ] 创建 cull/project compute shader（buffer 声明、workgroup 256、main 框架）
- [ ] 实现视锥剔除（world-space sphere center=`position_world`, radius=`world_radius_3sigma` vs frustum planes）
- [ ] 实现投影防御策略（behind-camera / near-plane unstable discard、projection z 防 NaN/Inf clamp、projected OBB 半轴超过 screen short side × 0.25 discard）
- [ ] 实现 3D→2D covariance projection（共用 PT/reference camera pose/FOV/aspect/viewport；`center_px` clip/NDC→pixel 且不做 Y flip；`fx/fy` pixel focal length；`cov_2d = J * cov_view * Jᵀ`）
- [ ] 实现 2D covariance 正定化、conic 预计算与 3σ pixel-space OBB extent
- [ ] 实现 per-visible-splat SH 求值（camera → splat view dir；all degrees → RGB；负分量 clamp 到 0；写入 projected data）
- [ ] 实现 uniform-control-flow subgroup append 与 sort entry 生成（valid entries 写入 `sort_entries[0..visible_count)`；`distance_key = floatBitsToUint(camera_distance_squared)`；异常值写 sentinel）
- [ ] 创建 compute pipeline + dispatch 逻辑（C++ 端）
- [ ] 编译验证

## Step 5:Bitonic sort correctness baseline

- [ ] 创建 bitonic sort compute shader（2×32-bit sort entry compare-and-swap；lexicographic `(distance_key, global_splat_index)` 升序；sentinel 排末尾）
- [ ] 实现 capacity sort 多 pass dispatch 逻辑（`N = sort_capacity`,log2(N) stages × log(N) steps）
- [ ] 验证排序后 `[0, visible_count)` 全为 valid entries,draw 不使用 capacity
- [ ] 编译验证

## Step 6:Quad rendering path

- [ ] 创建 vertex shader（从 sorted entries 读取 global splat index,再读取 `projected_data[global_index]`;展开 6-vertex instanced quad,pixel-space corners 转 NDC;positive-height normal viewport,pixel→NDC 不做 Y flip）
- [ ] 创建 fragment shader（`gl_FragCoord.xy - center_px` + pixel-space conic 计算 alpha；`power < -20` discard；`alpha = clamp(opacity * exp(power), 0, 1)`；`alpha < 1e-4` discard；输出 premultiplied color）
- [ ] 实现 indirect draw command buffer 设置（GPU 写 `VkDrawIndirectCommand::instanceCount`;固定字段 `vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0` 由 CPU 初始化）
- [ ] 创建 R16G16B16A16Sfloat GS composition target（loadOp=CLEAR `vec4(0,0,0,0)`,storeOp=STORE）
- [ ] 配置 graphics pipeline（depthTest/depthWrite disabled,cull none,colorWriteMask=RGBA,front-to-back premultiplied-under blend）
- [ ] 编译验证

## Step 7:RenderMode 与 output 集成

- [ ] 创建 `render_gaussian_splatting()` 方法（orchestrate reset → cull/project → sort → draw）
- [ ] 实现 `RenderMode` 分发（`render()` 根据 RenderMode 调用 PT 或 GS 路径）
- [ ] 实现 GS 专用 near plane 计算（初始为 scene AABB diagonal × 0.005,仅 GS 模式使用）
- [ ] 实现 GS color conversion path（`srgb_rec709_display`:composition target → sRGB→linear pass → linear target；`lin_rec709_display`:bypass conversion,composition target 直接作为 linear target；conversion 只转换 accumulated premultiplied RGB,alpha 原样保留）
- [ ] 扩展 TonemappingPass push constant mode（`HdrAces` / `LinearClamp`;PT 使用 `HdrAces`,GS 使用 `LinearClamp`;不放入 GlobalUBO,不新增 pipeline）
- [ ] 集成 TonemappingPass（GS 输入始终 linear；GS `LinearClamp` 跳过 exposure / tonemap curve；final RGB hard clamp [0,1]；忽略 GS alpha 并输出 opaque alpha = 1）
- [ ] 编译验证

## Step 8:Phase 3.0 correctness validation

- [ ] 验证 happy path（KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform）
- [ ] 验证单个与多个 GS primitive（primitive 拼接为 global splat buffers,保留 CPU per-primitive ranges）
- [ ] 验证 `srgb_rec709_display` 与 `lin_rec709_display` 二选一 scene,不支持 mixed colorSpace scene
- [ ] 验证投影中心、3σ OBB、front-to-back 排序、premultiplied-under blend、color conversion、Tonemapping bypass
- [ ] 验证 resize、`visible_count = 0`、非法 asset rejection、reload/resize 后 descriptor 不指向已销毁资源
- [ ] 确认 Phase 3.0 baseline 目标（1M correctness baseline；不以 10M 性能为完成条件）

## Step 9:Phase 3.0 末期补全

- [ ] 创建 radix sort compute shader（per-digit histogram + prefix sum + scatter；仅处理 32-bit distance_key,global_splat_index 作为 payload 搬运；必须对相同 `distance_key` 保持 deterministic ordering,具体方案在实现前确认）
- [ ] 实现 capacity radix 多 pass dispatch 逻辑（`N = sort_capacity`,每 pass 处理若干 bit,从 LSB 到 MSB）
- [ ] 替换 bitonic sort dispatch 为 radix capacity dispatch
- [ ] 正确性验证（radix capacity 与 bitonic capacity baseline 对比渲染结果）
- [ ] 实现 visible-count-driven radix sort（GPU 侧根据 visible_count 限制排序工作量,不进行 CPU readback）
- [ ] 正确性验证（visible-count-driven radix 与 radix capacity baseline 对比渲染结果）
- [ ] 实现 Wigner-D 矩阵 SH rotation（从 transform 提取旋转分量,旋转 degree 1-3 SH 系数）
- [ ] Wigner-D 正确性验证（identity 不变、degree 1 已知旋转、与 PLY 坐标翻转规则一致性检查）
- [ ] 编译验证
