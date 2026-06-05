# Reflector Phase 3.0:Gaussian Splatting 基础渲染

> 目标:正确渲染 GS 场景的最小可行实现。
> 整体路线见 `docs/phase3-decisions.md`,当前阶段约定与实现指南见 `docs/current-phase.md`。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应请求用户在 CLion 中编译验证。

---

## Step 0:RenderGraph buffer barriers

- [x] 扩展 RenderGraph buffer resource usage tracking（记录 per-buffer last stage/access,不再在 compile 阶段跳过 buffer resource）
- [x] 实现 buffer barrier emission（生成 `VkBufferMemoryBarrier2`,并与现有 image barriers 合并到 `VkDependencyInfo`）
- [x] 补齐 Phase 3.0 所需 buffer usage 映射（Transfer read/write、Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read）
- [x] 检查 RG 生成的 buffer barrier 链路覆盖 GS 依赖（reset→cull/project、cull/project→sort、sort/projected data→graphics、indirect write→draw indirect）
- [x] 请求用户在 CLion 中编译验证

## Step 1:GS 数据契约与加载校验

- [x] 定义 scene-level GS GPU resource（splat 总数、`sort_capacity`、static/work buffer handles、scene metadata、CPU per-primitive ranges；Step 2 将清理无实际消费的 range 字段）
- [x] 定义 CPU/shader 共享 GPU layouts（projected data、sort entry、indirect command 固定字段、std430/vec4 packing 约定）
- [x] 实现 glTF 直接加载数值校验（opacity finite [0,1]、SCALE finite >= 0、ROTATION finite unit quaternion）
- [x] 实现 primitive metadata 一致性校验与 global primitive range 记录（`kernel=ellipse`、`projection=perspective`、`sortingMethod=cameraDistance`、`colorSpace` 一致；Step 2 将清理无实际消费的 range 字段）
- [x] 请求用户在 CLion 中编译验证

## Step 2:Upload-time bake 与 static buffers

- [x] 修正 GS static baked buffer 契约为 packed SoA layout（`position_radius`、`covariance_opacity`、`sh_coefficients`，替换 Step 1 的分离 radius/opacity handles）
- [x] 实现 node transform 合法性检查与 position baking（regular/proper/positive transform；local position → world space）
- [x] 实现 covariance baking 与 cull radius 预计算（`Σ_local`、`Σ_world`、6-float symmetric covariance、`world_radius_3sigma`）
- [x] 清理无实际消费的 primitive range contract（移除 `GaussianSplatPrimitiveRange`、scene/gpu `primitive_ranges`、loader range recording，并同步验证项）
- [x] 实现 SH upload happy path（identity/no-rotation transform 直接上传；degree 0 可允许 node rotation；degree 1-3 需要 SH rotation 时暂时报错并回退空 GS scene）
- [x] 创建并上传 static baked packed SoA storage buffers（`position_radius`、`covariance_opacity`、`sh_coefficients`）
- [x] 请求用户在 CLion 中编译验证（packed static buffer upload）
- [x] 按 scene-level `max_sh_degree` 派生 `sh_coefficients` packed vec4 stride（degree 0/1/2/3 = 1/3/7/12 vec4，避免低阶 scene 固定 12 vec4 浪费）
- [x] 前移 degree 1-3 非 identity node rotation 拦截到 CPU preflight / upload 前（失败场景不进入 static buffer 创建上传）
- [x] 请求用户在 CLion 中编译验证（SH stride 与 rotation preflight 调整）

## Step 3:GS descriptors、work buffers 与 reset

- [x] 定义 GS Set 3 持久 descriptor layout 与 `GSPushConstants`（static baked buffers + work buffers；复用 GlobalUBO camera/screen 字段）
- [x] 按 PT 模式拆分 GS Set 3 生命周期职责（descriptor layout 属于 renderer-lifetime pass/pipeline owner；GS scene resource owner 只管理 scene buffers、descriptor set allocation/write/rewrite，不使用 `shutdown()` 式双重销毁语义）
- [x] 创建 capacity-based GS work buffers（visible count、projected data、sort entry ping-pong、indirect draw command）
- [x] 创建 GS Set 3 descriptor set 并实现 descriptor 写入/重写逻辑（scene reload 或 buffer recreate 时更新,非每帧 push）
- [x] 实现每帧 work buffer reset 与 indirect command 固定字段初始化（visible_count、sort sentinel、instanceCount）
- [x] 请求用户在 CLion 中编译验证

## Step 4:Cull/Project compute pass

- [x] 清理 `GSPushConstants::flags` 预留字段，替换为明确的 `max_sh_degree` 字段并同步 C++/GLSL/文档契约
- [x] 创建 cull/project compute shader 与 C++ pipeline/dispatch skeleton（buffer 声明、workgroup 256、Set 3 + GlobalUBO + push constants）
- [x] 实现 world-space cull 与投影防御（frustum sphere、behind-camera/near-plane discard、projection NaN/Inf 防御）
- [x] 实现 screen-space projection 数据生成（center_px、2D covariance、正定化、conic、3σ OBB extent、giant projection discard）
- [x] 实现 per-visible-splat SH evaluation 与 projected data 写入（camera→splat view dir、all-degree RGB、负分量 clamp）
- [x] 实现 visible append、sort entry 生成与 indirect instanceCount 更新（subgroup uniform control flow、distance_key、sentinel、global_splat_index）
- [ ] 请求用户在 CLion 中编译验证

## Step 5:Bitonic sort correctness baseline

- [ ] 创建 bitonic sort compute shader（2×32-bit sort entry compare-and-swap；lexicographic `(distance_key, global_splat_index)` 升序；sentinel 排末尾）
- [ ] 实现 bitonic capacity dispatch orchestration（`N = sort_capacity`,完整 stages/steps,sort ping-pong 或 in-place 策略明确）
- [ ] 接入 cull/project 输出并确认 draw range 语义（排序后 `[0, visible_count)` 为 valid entries,draw 不使用 capacity）
- [ ] 请求用户在 CLion 中编译验证

## Step 6:Quad rendering path

- [ ] 创建 GS vertex/fragment shaders（sorted entry → projected data → instanced quad；pixel-space conic alpha；premultiplied output）
- [ ] 创建 GS composition target 与 graphics pipeline（R16G16B16A16Sfloat、clear/store、depth off、cull none、premultiplied-under blend）
- [ ] 实现 indirect draw integration（CPU 初始化 fixed fields,GPU 写 instanceCount,draw indirect 读取 command buffer）
- [ ] 接入 resize 生命周期（只重建 viewport-sized composition/linear targets,不重建 capacity work buffers）
- [ ] 请求用户在 CLion 中编译验证

## Step 7:RenderMode 与 output 集成

- [ ] 创建 `render_gaussian_splatting()` orchestration（reset → cull/project → sort → draw → optional conversion → TonemappingPass）
- [ ] 实现 `RenderMode` 分发与 GS near plane 计算（scene AABB diagonal × 0.005,仅 GS 模式使用）
- [ ] 实现 GS color conversion path（`srgb_rec709_display` sRGB→linear pass；`lin_rec709_display` bypass；RGB only conversion,alpha preserved）
- [ ] 扩展并集成 TonemappingPass `HdrAces` / `LinearClamp` mode（GS input always linear,hard clamp [0,1],output alpha=1）
- [ ] 请求用户在 CLion 中编译验证

## Step 8:Phase 3.0 correctness validation

- [ ] 验证 happy path scene 能正确渲染（ellipse + perspective + cameraDistance + consistent metadata + identity/no-rotation transform）
- [ ] 验证 multi-primitive global buffer 拼接正确（不保留无实际消费的 per-primitive range contract）
- [ ] 验证 colorSpace 输出路径（`srgb_rec709_display` conversion 与 `lin_rec709_display` bypass）
- [ ] 验证核心渲染正确性（投影中心、3σ OBB、front-to-back sorting、premultiplied-under blend、Tonemapping bypass）
- [ ] 验证生命周期与异常路径（resize、`visible_count = 0`、非法 asset rejection、reload/resize descriptor safety）
- [ ] 确认 Phase 3.0 baseline 目标（1M correctness baseline；不以 10M 性能为完成条件）

## Step 9:Phase 3.0 末期补全

- [ ] 确认 radix equal-key deterministic 策略与 pass layout（保持 32-bit distance_key + global_splat_index payload,不退回 64-bit packed key）
- [ ] 实现 radix capacity sort shader passes（histogram、prefix sum、scatter,支持 ping-pong payload 搬运）
- [ ] 实现 radix capacity dispatch orchestration 并替换 bitonic dispatch
- [ ] 验证 radix capacity 与 bitonic capacity baseline 渲染结果一致
- [ ] 实现 visible-count-driven radix dispatch（GPU 侧根据 visible_count 限制排序工作量,不进行 CPU readback）
- [ ] 实现 Wigner-D SH rotation upload bake（提取 proper rotation,旋转 degree 1-3 SH 系数）
- [ ] 验证 Wigner-D 正确性（identity 不变、degree 1 已知旋转、与 PLY 坐标翻转规则一致）
- [ ] 请求用户在 CLion 中编译验证
