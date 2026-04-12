# M1 路径追踪架构决策

> 阶段六~八（RT 基础设施 + PT 参考视图 + PT 烘焙器 + 间接光照集成）的架构决策。
> Milestone 级 RT 技术选型见 `../project/technical-decisions.md` 第 13 章和 `../project/decision-process.md` 第 13 章。

---

## 四层架构放置

RT 能力在每层添加新内容，遵循已有的单向依赖约束：

| 层 | 新增内容 | 说明 |
|----|---------|------|
| Layer 0 (RHI) | AS 资源类型、RT Pipeline 创建、SBT 管理、Command 扩展 | 不知道什么是路径追踪，只知道怎么创建 AS、构建 SBT、录制 trace_rays |
| Layer 1 (Framework) | Scene AS Builder、PT 配置结构 | 输入 scene data → 构建 BLAS/TLAS；烘焙器和参考视图共用的参数/配置 |
| Layer 2 (Passes) | Reference View Pass、Lightmap Baker Pass、Probe Baker Pass | 独立模块，声明输入输出，注册到 Render Graph |
| Layer 3 (App) | 渲染模式切换、烘焙 UI 控制 | ImGui 面板触发烘焙、配置参数、切换渲染模式 |

---

## 三条独立渲染路径

Renderer 内部维护三条独立路径，每条有自己的帧流程：

| 模式 | 帧流程 | 激活条件 |
|------|--------|---------|
| 光栅化 | 现有完整管线（shadow → prepass → AO → forward → skybox → postprocess） | 默认 |
| PT 参考视图 | PT Pass → OIDN → Tonemapping → Swapchain | 用户切换 |
| 烘焙模式 | Baker dispatch → 累积 → 预览进度 → Swapchain | 用户触发烘焙 |

选择独立路径而非条件跳过的理由：PT 帧流程和光栅化差异极大（无 depth prepass、无 shadow、无 AO、无 forward pass），强行塞进同一套 pass 注册逻辑会让条件分支遍布 Renderer::render()。

---

## 场景数据访问

RT shader 通过已有 descriptor 架构访问场景数据：

- **Set 0 binding 0-3**：复用（GlobalUBO + LightBuffer + MaterialBuffer + InstanceBuffer）
- **Set 0 binding 4**：TLAS（`accelerationStructureEXT`，新增）
- **Set 0 binding 5**：Geometry Info SSBO（per-geometry vertex/index buffer device address + material ID，新增）
- **Set 0 binding 6**：Environment Alias Table SSBO（env importance sampling 用，新增，详见「Environment Map Importance Sampling」章节）
- **Set 1**：复用（bindless textures + cubemaps）

Closest-hit shader 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 buffer address 和 material ID，再读 MaterialBuffer 获取 PBR 参数并采样 bindless 纹理。变换矩阵通过 TLAS 内置的 `gl_ObjectToWorldEXT` / `gl_WorldToObjectEXT` 获取。

**RT shader 不访问 InstanceBuffer**——transform 来自 TLAS 内置矩阵，material 来自 GeometryInfo。InstanceBuffer 仅服务于光栅化路径。

光栅化 shader 不引用 binding 4/5，存在于 set layout 中不产生开销。

---

## OIDN 集成

**版本**：OIDN 2.4.1（2026-01 发布），预编译库手动集成到 `third_party/oidn/`（`include/` + `lib/` + `bin/`），vcpkg 无端口。OIDN 2.x 原生支持 CUDA/HIP/SYCL GPU 降噪，使用 C++11 wrapper API（`oidn.hpp`）。

**设备类型日志**：`init()` 中 OIDN device commit 后查询实际设备类型（`device.get<oidn::DeviceType>("type")`）。GPU 后端 → `spdlog::info` 输出设备类型（CUDA/SYCL/HIP/Metal）；CPU fallback → `spdlog::warn` 警告性能降级（~25x slower）。

**数据流**：Vulkan readback → CPU 内存 → OIDN GPU 降噪 → CPU 内存 → Vulkan upload。

"CPU 中转"指 Vulkan 与 OIDN 之间的数据通道走 CPU 内存，降噪计算本身在 GPU 上执行（CUDA/HIP/SYCL）。

选择 CPU 中转而非 VK_KHR_external_memory 零拷贝的理由：M1 的烘焙器是离线的，参考视图降噪不需要每帧执行（每隔 N 次采样触发一次），CPU 中转延迟完全可接受。省去外部内存互操作的复杂度。

**异步执行**：降噪完全不阻塞主线程。三段操作分离执行：

1. **Readback**：RG 内 Transfer Pass（与渲染命令同一 submit，GPU 流水线化）
2. **OIDN 执行**：`std::jthread` 后台线程（`vkWaitSemaphores` 等待 readback 完成 → memcpy → `oidnExecuteFilter()` → memcpy）
3. **Upload**：下一帧 RG 内 Transfer Pass（后台线程完成后，主线程检测并注册）

选择 RG 内 Transfer Pass 而非 immediate scope（`begin_immediate` / `end_immediate`）的理由：immediate scope 用 `vkQueueWaitIdle` 阻塞主线程。RG 内 copy 与渲染命令一起提交，CPU 零等待。选择 RG upload 而非后台线程自行 submit 的理由：避免引入 queue mutex（`vkQueueSubmit` 同一 queue 需外部同步），后台线程自行 submit 会侵入 Context 基础设施，收益仅是少一帧延迟（秒级降噪间隔中完全无感）。

**Timeline Semaphore 同步**：Denoiser 持有一个 Vulkan timeline semaphore（1.2 核心，项目目标 1.4）。降噪帧的 `vkQueueSubmit` 额外 signal 该 semaphore，后台线程 `vkWaitSemaphores` 纯 CPU 等待。选择 timeline semaphore 而非复用 per-frame fence 的理由：per-frame fence 在下一帧 `begin_frame()` 被 wait + reset，后台线程与主线程竞态。timeline semaphore 的 wait 不消耗/不 reset 信号值，多方可安全并发等待。

**线程模型**：按需 `std::jthread`，每次降噪创建新线程，完成后自然结束。降噪频率极低（秒级），线程创建开销可忽略。不用持久线程 + condition_variable（省去 CV 通知和空闲管理的复杂度）。`std::jthread` 析构自动 `request_stop` + `join`，赋值 `thread_ = {}` 即安全终止。

**Accumulation generation**：`uint32_t accumulation_generation_`（Renderer 侧，单调递增），每次 accumulation 重置时 +1。降噪触发时记录当前 generation，upload 时比对。不匹配 → 结果过期，丢弃（跳过 upload pass，状态 → Idle）。统一覆盖所有丢弃场景（相机移动、IBL 旋转、resize、场景加载、`max_bounces`/`firefly_clamp` 变更导致的重置）。

**状态机**：

```
Idle → ReadbackPending → Processing → UploadPending → Idle
```

- `Idle`：可触发降噪
- `ReadbackPending`：已请求，同帧 RG 注册 readback copy pass（帧内瞬态，不跨帧）
- `Processing`：后台线程执行中（wait semaphore + OIDN）
- `UploadPending`：后台线程完成，等下一帧 RG 注册 upload pass

**Timeline semaphore 注入 submit**：Denoiser 在 framework 层，submit 在 Application 层，Renderer 不接触 submit。传递路径：Renderer 新增 `pending_denoise_signal()` getter 返回 `{VkSemaphore, uint64_t value}`（无降噪帧返回 `{VK_NULL_HANDLE, 0}`），Application 在 `vkQueueSubmit2` 前检查并追加到 `signalSemaphoreInfos`。

**Readback / Upload Pass 注册方式**：RG 只追踪 image barrier，buffer 无 layout 概念。Readback pass 声明 accumulation + aux 为 `Read + Transfer`（RG 自动插入 barrier → `TRANSFER_SRC_OPTIMAL`），execute lambda 手动录制 `vkCmdCopyImageToBuffer`（staging buffer 不经 RG 追踪）。Upload pass 同理：声明 denoised image 为 `Write + Transfer`，lambda 手动录制 `vkCmdCopyBufferToImage`。

**Pass 顺序**：Readback 和 Upload 在不同帧。降噪帧：Reference View → Readback Copy → Tonemapping（读 accumulation）。Upload 帧：Upload → Reference View → Tonemapping（读 denoised buffer）。Upload 在 Reference View 之前，确保 denoised buffer 在 Tonemapping 读取前已写入。

**launch_processing() 调用时机**：在 `render()` 内调用（submit 之前）。后台线程 `vkWaitSemaphores` 等待 GPU 完成 readback，早启动只是多等一会。比 post-submit hook 简单。

**Resize / Destroy / 场景加载安全**：`on_resize()`、`destroy()` join 后台线程后强制 `state_ → Idle`，丢弃任何 UploadPending 结果（避免旧尺寸 staging → 新尺寸 denoised buffer 的尺寸不匹配）。场景加载前调用 `Denoiser::abort()`（join + Idle），调用方同步做 accumulation 重置 + generation++。`oidnExecuteFilter()` 不可中断，最坏等待 20-50ms，发生在本身就阻塞的操作中（`vkQueueWaitIdle` / 资源重建），用户无感。

**内存序**：`state_` store 使用 `memory_order_release`，load 使用 `memory_order_acquire`，确保后台线程 staging buffer 写入对主线程可见。

**OIDN 错误处理**：`oidnExecuteFilter()` 失败时后台线程 log error + `state_ → Idle`（跳过 upload），不 crash。`last_error()` 返回错误信息供 UI 显示，下次成功请求时清除。

**auto_denoise_interval 最小值**：UI slider 最小值 ≥ 16（默认 64）。`interval ≥ 3 × OIDN_time / frame_time` 保证 ≤25% GPU 占用。有 RT core 的显卡上 OIDN/PT 算力比值大致稳定（~6:1），interval=16 约 21% GPU 占用。CPU fallback 场景 OIDN 执行极慢（实测 1080p ~307ms，9800X3D AVX-512），被自身执行时间节流。

**OIDNBuffer**：OIDN 2.x GPU 模式要求使用 `OIDNBuffer` 对象传递图像数据（GPU 无法访问系统 malloc 内存）。Denoiser 创建持久 `OIDNBuffer`（beauty + albedo + normal + output），Vulkan readback 后 `memcpy` 到 OIDNBuffer，执行后从 OIDNBuffer `memcpy` 到 upload staging buffer。

**像素格式与零转换**：
- Accumulation（RGBA32F）→ `OIDN_FORMAT_FLOAT3`，pixelByteStride=16（跳过 alpha）
- Aux Albedo（R16G16B16A16Sfloat）→ `OIDN_FORMAT_HALF3`，pixelByteStride=8（跳过 alpha）
- Aux Normal（R16G16B16A16Sfloat）→ `OIDN_FORMAT_HALF3`，pixelByteStride=8（跳过 alpha）
- Output（RGBA32F）→ `OIDN_FORMAT_FLOAT3`，pixelByteStride=16

三种输入无需任何 CPU 端格式转换。Aux albedo 原始设计为 R8G8B8A8Unorm（Step 7），Step 9 实现前升级为 R16G16B16A16Sfloat（half 精度高于 R8 的 256 级，零质量损失；VRAM 增加从 4→8 bytes/pixel，1080p 下约 +8MB 可忽略）。两张 aux 统一格式简化 readback 和 OIDN 配置。

---

## 烘焙器执行模型

烘焙模式激活时接管渲染：光栅化和 PT 参考视图均跳过，GPU 全力用于烘焙。

每帧 dispatch N 个采样 → 累积到 accumulation buffer → 展示当前进度到 swapchain。达到目标采样数后执行 OIDN 降噪 → BC6H 压缩 → KTX2 持久化。

---

## 加速结构构建策略

### Multi-geometry BLAS

同一 glTF mesh 下的多个 primitive 合并为**一个 BLAS 的多个 geometry**，而非每个 primitive 单独 BLAS。TLAS 每个 node instance 对应一个 instance entry（而非每个 primitive 一个）。

好处：减少 TLAS instance 数量（N primitives/mesh → 1 TLAS instance/node），提升 TLAS 构建和遍历效率。

`Mesh` 结构体新增 `group_id`（标识 primitive 来源的 glTF mesh）和 `material_id`（primitive 固有的材质，与 MeshInstance.material_id 冗余但解耦 RT 和光栅化路径）。SceneASBuilder 按 `group_id` 分组构建 BLAS，按 `(group_id, transform)` 去重构建 TLAS instances。

`instanceCustomIndex` = 该 BLAS 在 Geometry Info buffer 中的 base offset。Closesthit shader 用 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 索引。

### Per-geometry opacity 标记

Multi-geometry BLAS 内每个 geometry 独立设置 opacity flag：

- **Opaque geometry**（`AlphaMode::Opaque`）：`VK_GEOMETRY_OPAQUE_BIT_KHR`——硬件跳过 any-hit shader，三角形命中即确认
- **Non-opaque geometry**（`AlphaMode::Mask` / `Blend`）：`VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`——硬件调用 any-hit shader 执行 alpha test / stochastic alpha，且保证每个 primitive 最多调用一次

SceneASBuilder 构建 `BLASGeometry` 时根据对应 primitive 的材质 `alpha_mode` 设置 `BLASGeometry::opaque` 字段，`build_blas()` 据此选择 geometry flag。TLAS instance geometry 的 `flags` 设为 0（不设 `OPAQUE_BIT`），不覆盖 BLAS 层 per-geometry 设置。TLAS instance 的 `VkAccelerationStructureInstanceKHR::flags` 也不设 `FORCE_OPAQUE` / `FORCE_NO_OPAQUE`，opacity 语义完全由 BLAS geometry 层决定。

### 生命周期

- **BLAS**：场景加载时 per unique mesh group 构建一次，`PREFER_FAST_TRACE`（不带 `ALLOW_UPDATE`），场景销毁时释放
- **TLAS**：场景加载时构建一次，包含所有 node instance 变换，场景销毁时释放
- **Scratch buffer**：M1 构建完成后释放（AccelerationStructureManager 内部管理，调用方不感知）
- M1 场景静态，不需要运行时重建

### M3 动态演进备注

- 动态物体引入后 TLAS 需每帧重建：AccelerationStructureManager 新增 `rebuild_tlas()`，scratch buffer 改为持久保留（内部变更，调用方无感）
- Skinned mesh 的 BLAS 需要 refit：构建时带 `ALLOW_UPDATE` flag（仅 skinned mesh，静态 mesh 不带）
- 这些变更局限在 AccelerationStructureManager 内部 + SceneASBuilder 调用逻辑，不影响 RHI 层其他模块和上层 pass

---

## Lightmap UV2

**生成**：xatlas 运行时自动生成。per-mesh 按需标记是否需要 lightmap UV，M1 全部静态 mesh 标记。

**缓存**：xxHash 内容哈希 + 自定义二进制格式。

缓存文件内容：
- Header：magic + version + 源 mesh xxHash + atlas 尺寸 + 顶点数 + 索引数
- Data：UV2 坐标数组 + 新 index buffer + 顶点重映射表（新顶点 → 原始顶点）

缓存命中时直接读取，未命中时跑 xatlas 后保存。mesh 几何变化 → hash 变化 → 自动重新生成（旧 lightmap 也需重新烘焙）。

---

## Reflection Probe 放置

**网格 + 几何过滤**：场景 AABB 内按固定间距放置 3D 网格探针，利用 RT 射线检测剔除落在几何体内部的探针。密度通过 ImGui 参数可调。

---

## 烘焙产物格式

| 资源 | 累积格式 | 使用格式 | 持久化格式 |
|------|---------|---------|-----------|
| Lightmap | RGBA32F | BC6H | KTX2 (BC6H) |
| Reflection Probe | RGBA32F | BC6H + mip chain | KTX2 (BC6H + mip chain) |

复用已有 BC6H 压缩 shader 和 KTX2 读写基础设施。Probe 的 prefilter mip chain 复用 IBL prefilter 管线。

---

## RT 扩展可选性

RT 扩展（`acceleration_structure` + `ray_tracing_pipeline` + `ray_query`）为可选：

- 设备选择时 RT 支持作为**加分项**（优先选有 RT 的设备），但不是硬需求
- 只有所有候选设备都不支持 RT 时才标记 `rt_supported = false`
- `rt_supported = false` 时禁用全部 RT 功能，仅光栅化模式可用
- 阶段八间接光照集成不受影响——Lightmap/Probe 数据从 KTX2 加载，无论是自家烘焙器还是外部工具生成的
- Set 0 layout 根据 `rt_supported` 决定是否包含 binding 4/5（DescriptorManager init 时确定）

---

## 参考视图后处理

PT 参考视图仅经过 Tonemapping → Swapchain，不走其他后处理（Bloom、Vignette、Color Grading 等）。复用现有 TonemappingPass：PT 路径中将降噪/累积结果导入为 hdr_color，tonemapping 照常执行。

理由：参考视图的目的是 debug 和验证 PT 光照正确性，额外后处理会干扰对光照结果的判断。

---

## 参考视图累积与缓存

**累积策略**：RGBA32F accumulation buffer，running average（每帧 1 SPP），持久跨帧。

**重置触发**：比较当前帧与上一帧的 view-projection 矩阵，任何变化立即重置（清零 buffer + sample count 归零）。

**模式切换缓存**：accumulation buffer 在切换到光栅化模式时不销毁、不清零。切回 PT 时比较当前 VP 与缓存的 VP，相同则继续累积（完整保留和部分累积都自然缓存），不同则重置。

---

## OIDN 降噪触发

降噪触发守卫（全部满足才触发）：
- `denoiser_.state() == Idle`（无正在执行的降噪）
- `denoise_enabled_`（功能开关开启）
- `show_denoised_`（非 Show Raw 模式——Show Raw 暂停所有降噪触发）
- `sample_count > 0`（有有效采样数据）

触发方式：
- **自动**：`auto_denoise_ && (sample_count - last_denoised_sample_count_ >= interval)`
- **手动**：`DebugUIActions::pt_denoise_requested`

`last_denoised_sample_count_` 记录**触发时**的 sample count（非完成时）。降噪执行期间若 sample count 已超过下一个间隔点 → 跳过（状态 ≠ Idle）。完成后以触发时的值为基准重新计算，可能立即再次触发（补偿等待期间错过的更新）。

降噪结果写入单独的 denoised buffer，不覆盖 accumulation buffer（累积继续进行）。

---

## 参考视图输出显示

Tonemapping 输入选择：`show_denoised_ && denoise_enabled_ && denoised_generation_ == accumulation_generation_` → 导入 denoised buffer，否则导入 accumulation buffer。

- **Show Denoised**（默认）：显示最近一次降噪结果（如有），降噪完成时画面瞬间更新
- **Show Raw**：显示原始累积画面（带噪点），**同时暂停所有降噪触发**（自动和手动均不执行）。切回 Show Denoised 后恢复计时
- **denoise_enabled 关闭**：显示原始累积，进行中的降噪静默完成但结果不显示。重新开启时旧结果通过 generation 比对决定是否可用
- **模式切换（PT → 光栅化 → PT 同位置）**：denoised buffer 保留，切回后仍可显示（generation 匹配）。仅 accumulation 重置才导致 denoised 结果失效

---

## PT 默认参数

| 参数 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| 最大 bounce | 8 | 1-32 | 有 Russian Roulette 时设高无性能代价 |
| 每帧 SPP | 1 | 固定 | 保持 UI 响应，总吞吐量不变 |
| Russian Roulette 起始 | bounce 2-3 | 硬编码 | 不终止 primary ray 和首次反弹 |

---

## 参考视图 ImGui 面板

**状态信息：**
- 累积采样数 / 目标采样数
- 累积总耗时

**PT 参数：**
- 最大 bounce 数（slider，默认 8）
- 目标采样数（0 = 无限累积）
- 重置按钮

**OIDN 控件：**
- 降噪开关
- 显示原始/降噪切换
- 自动降噪开关 + 触发间隔（每 N 个采样）
- 手动降噪按钮（自动关闭时可用）
- 上次降噪时采样数

---

## Renderer 渲染路径组织

`Renderer::render()` 按渲染模式拆分为独立的私有方法（`render_rasterization()`、`render_path_tracing()`），不引入新类型或新抽象。共享的 GPU buffer 填充逻辑（GlobalUBO 核心字段、LightBuffer）提取为 `fill_common_gpu_data()`。新增渲染路径 = 新增一个私有方法 + switch case 一行。

---

## 降噪系统分离

M1 的 OIDN 和 M2 的 NRD 是**独立系统**，不统一接口：

| | OIDN | NRD |
|---|---|---|
| 服务对象 | 参考视图 + 烘焙器（离线质量） | 实时 PT（帧率优先） |
| 数据流 | CPU 内存中转 | 纯 GPU-side |
| 执行频率 | 每隔 N 帧 | 每帧 |

M2 引入 NRD 时新建独立模块，OIDN 保留继续服务参考视图和烘焙器。两者的消费者不同，不需要运行时多态切换，不设计统一 `IDenoiser` 接口。

---

## RT Shader 热重载

`RTPipeline` 封装将 VkPipeline + SBT buffer 生命周期绑定在一起。`create_rt_pipeline()` 每次调用都重新查询 `vkGetRayTracingShaderGroupHandlesKHR` 并构建 SBT buffer。`ReferenceViewPass::rebuild_pipelines()` 走 destroy + create 全量重建，SBT 自然跟随更新。无需额外的 SBT 联动机制。

---

## BLAS 批量构建策略

单次 `vkCmdBuildAccelerationStructuresKHR` 调用传入全部 BLASBuildInfo，GPU 并行构建。

分配一个大 scratch buffer = 所有 BLAS scratch size 之和（每段对齐到 `minAccelerationStructureScratchOffsetAlignment`），每个 BLAS 使用 scratch 内的独立区域。构建完成后释放 scratch。

选择并行构建而非逐个构建（共用 scratch + 每次之间插 barrier）的理由：实现复杂度差异仅十几行代码，但 GPU 可并行，加载时构建更快。M3 的动态 BLAS refit 是不同操作，不受此选择影响。

---

## Ray Payload 设计

采用模式 A：closesthit 内完成全部着色计算（顶点插值 + 材质采样 + BRDF 计算 + NEE shadow ray + 下一方向采样），通过 payload 返回计算结果，raygen 仅做循环累积。

理由：closesthit 是唯一能访问 `gl_InstanceCustomIndexEXT`、`gl_GeometryIndexEXT`、`gl_PrimitiveID`、重心坐标等 RT 内置变量的 shader。就地完成全部计算后返回结果，payload 更小（52B 计算结果 vs 60B+ 原始几何数据），raygen 逻辑更简单。

两种 payload：
- **PrimaryPayload**（location 0），逐步演进：
  - Step 6（56B）：`vec3 color` + `vec3 next_origin` + `vec3 next_direction` + `vec3 throughput_update` + `float hit_distance`（miss 时 -1 终止）+ `uint bounce`（raygen 设置，closesthit 读取，OIDN 辅助通道 bounce 0 判断用）
  - Step 11（60B）：+ `float env_mis_weight`（BRDF 采样方向的 env MIS 权重，closesthit 预计算）
  - Step 12（64B）：+ `float last_brdf_pdf`（上一个 bounce 的 BRDF PDF，供下一个 closesthit 做 emissive MIS）
  - Step 13（72B）：+ `float cone_width`（Ray Cones 累积宽度）+ `float cone_spread`（扩展角，含曲率修正）
- **ShadowPayload**（location 1）：`uint visible`（初始 0 = 遮挡，shadow_miss 设 1）

`env_mis_weight` 由 closesthit 在 BRDF 采样方向确定后立刻计算（查 env cubemap 算 `pdf_env`，与已知 `pdf_brdf` 做 power heuristic），写入 payload。若 BRDF 射线命中几何体（非 miss），此字段被忽略。miss shader 只返回 raw env color，raygen 乘以预存权重。这样 miss shader 不需要知道上一个表面的 BRDF 信息。

---

## PT 采样策略

Sobol 低差异序列 + Cranley-Patterson rotation + Blue noise 空间去相关。

- **Sobol**：预计算方向数表上传 SSBO（Set 3 binding 3，16 KB，128 维 × 32 bit = 4096 uint32），避免 SPIR-V 常量膨胀和编译减速。方向数嵌入 C++ 头文件（`constexpr uint32_t[]`），Renderer init 时一次性上传。提供跨帧/采样点均匀分布的低差异采样值
- **Blue noise**：128×128 R8Unorm 单通道纹理，数据源为 Christoph Peters 的 Void-and-Cluster 预生成纹理（[Calinou/free-blue-noise-textures](https://github.com/Calinou/free-blue-noise-textures)，CC0 公共领域许可）。像素数据嵌入 C++ 头文件为 `constexpr uint8_t[]`，初始化时上传 GPU 注册到 bindless 数组
- **Cranley-Patterson rotation**：`sample = fract(sobol(n, dim) + blue_noise(pixel + offset))`，不同采样维度通过空间偏移从同一张纹理派生

---

## PT Push Constants

raygen shader 的 push constant 逐步演进：

- Step 6（20B）：`max_bounces`（uint）、`sample_count`（uint）、`frame_seed`（uint）、`blue_noise_index`（uint，blue noise 纹理在 bindless textures[] 中的索引）、`max_clamp`（float，firefly clamping 阈值，0 = 关闭）
- Step 11（28B）：+ `env_sampling`（uint，1 = 环境光重要性采样）+ `directional_lights`（uint，1 = PT 中启用方向光）
- Step 12（32B）：+ `emissive_light_count`（uint，0 = 跳过 NEE emissive）
- Step 13（36B）：+ `lod_max_level`（uint，Ray Cones LOD clamp 上限，默认 4）

相机逆矩阵（`inv_view`、`inv_projection`）从 GlobalUBO 读取。理由：Vulkan 保证 `maxPushConstantsSize` ≥ 128 字节，两个 mat4 = 128 字节会逼近下限；且与现有 compute pass 的轻量 push constant 模式一致。GlobalUBO 阶段六新增 `inv_view` 字段（`inv_projection` 阶段五已有）。

---

## GeometryInfo 数据结构

`GPUGeometryInfo`（std430，24 bytes）：`vertex_buffer_address`(uint64) + `index_buffer_address`(uint64) + `material_buffer_offset`(uint32) + `_padding`(uint32)。

不存 `vertex_stride`（统一顶点格式，stride = `sizeof(Vertex)` = 56，shader 端硬编码常量）。不存 `vertex_count` / `index_count`（closesthit 通过 `gl_PrimitiveID` 索引，一定有效，不需要边界信息）。`material_buffer_offset` 直接存 `MaterialInstance::buffer_offset`，shader 直接 `materials[buffer_offset]` 读取 PBR 参数，无需间接查找。

---

## Non-opaque 几何体处理

RT 管线通过 any-hit shader 处理 `AlphaMode::Mask` 和 `AlphaMode::Blend` 几何体。

### Any-hit shader

单一 any-hit shader（`shaders/rt/anyhit.rahit`）处理所有 non-opaque 几何体，与 closest-hit 共用同一个 hit group。数据访问路径与 closest-hit 相同：

1. `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` → 获取 vertex/index buffer address + `material_buffer_offset`
2. 通过 `gl_PrimitiveID` + 重心坐标从 buffer reference 插值 UV
3. `MaterialBuffer[material_buffer_offset]` → 读取 `alpha_mode`、`alpha_cutoff`、`base_color_factor.a`、`base_color_tex`
4. 采样 `base_color_tex` 获取纹素 alpha

行为：
- **Mask**：`texel_alpha < alpha_cutoff` → `ignoreIntersectionEXT`（标准 alpha test）
- **Blend**：`rand() >= texel_alpha` → `ignoreIntersectionEXT`（stochastic alpha）

Opaque geometry 被 `VK_GEOMETRY_OPAQUE_BIT_KHR` 硬件级跳过，不进入 any-hit。GeometryInfo 不扩展——any-hit 通过 `material_buffer_offset` 间接读取 MaterialBuffer 获取 alpha 参数，一次 SSBO 寻址相对于纹理采样开销可忽略。

### Stochastic alpha

Blend 物体使用概率性命中判定：以概率 `alpha` 接受命中（closest-hit 正常着色），以概率 `1-alpha` 拒绝命中（射线穿透继续前进）。数学上等价于 alpha blending 的期望：`E[贡献] = alpha × 表面着色 + (1-alpha) × 背后场景`。

Primary ray 和 shadow ray 使用同一个 any-hit shader，无需区分射线类型。半透明窗户自然按 alpha 比例透光。Closest-hit 中不需要对 Blend 物体做特殊处理——stochastic 决策已在 any-hit 中完成，命中被接受时表面当作完整表面正常着色，不乘 alpha（概率已隐含权重）。

**M1 局限性**：Stochastic alpha 是 **alpha coverage** 近似——把 `AlphaMode::Blend` 的 alpha 值当作薄表面覆盖率，概率性地决定射线是否"穿过空隙"。这与真实的光学透射（折射、体积衰减）是完全不同的物理概念。M1 中玻璃等物体表现为按 alpha 概率穿透的薄膜，无折射效果。

**M2 演进**：引入 **physical transmission** 路径——解析 `KHR_materials_transmission` + `KHR_materials_volume` 扩展，closest-hit 中对有 transmission 参数的材质实现 Snell 折射 + Fresnel + Beer 衰减。两者正交共存但语义完全不同：

| | Alpha coverage（M1） | Physical transmission（M2） |
|---|---|---|
| 控制层 | any-hit shader | closest-hit shader |
| 语义 | 表面覆盖率（有多少射线"穿过空隙"） | 光学透射（射线穿过介质时的方向改变和能量衰减） |
| 材质参数 | `alpha_mode` + `base_color.a` | `transmission_factor` + `IOR` + `attenuation_color/distance` |
| 适用场景 | 纱帘、铁丝网、树叶 | 玻璃、水面、宝石 |

有 transmission 参数的材质走 physical transmission 路径；无 transmission 参数的 `AlphaMode::Blend` 物体仍走 stochastic alpha coverage。

### Any-hit 随机数生成

Any-hit shader 无法访问 raygen 的采样序列状态（Vulkan spec 限制 any-hit 不能读写调用方 payload），使用 PCG 哈希生成独立随机数：

```
seed = gl_LaunchIDEXT.x ^ (gl_LaunchIDEXT.y * 1973) ^ (frame_seed * 277) ^ (gl_PrimitiveID * 5039) ^ (gl_GeometryIndexEXT * 3571)
r = pcg_hash(seed) / float(0xFFFFFFFF)
```

- `gl_LaunchIDEXT`：不同像素不同种子
- `frame_seed`（push constant）：帧间变化
- `gl_PrimitiveID` + `gl_GeometryIndexEXT`：同一射线穿过多个半透明面时每次调用不同种子

纯 ALU 运算，无额外资源访问。对累积收敛的参考视图和烘焙器，哈希质量足够。

---

## Environment Map Importance Sampling

### 动机

M1 的 BRDF 采样方向 miss 后命中 IBL cubemap 获取环境光。当 HDR 环境贴图包含高亮度区域（如太阳，占像素不到 0.1% 但贡献 90% 能量），BRDF 采样很少碰巧命中该方向，导致高方差和萤火虫噪点。Environment importance sampling 主动按环境贴图亮度分布采样方向，大幅降低方差、加速收敛。最终收敛值数学上相同（无偏估计），差异仅在方差大小。

### 采样方案：Alias Table

选择 alias table（Vose's algorithm 构建，O(1) 采样）而非 CDF（O(log N) binary search 采样）。

| | CDF | Alias Table |
|---|---|---|
| GPU 采样复杂度 | O(log W + log H) ≈ 21 次循环迭代 | O(1)，1 次查表 + 1 次比较 |
| 分支发散 | binary search 有分支 | 无 |
| CPU 构建 | prefix sum | Vose's algorithm（O(N)） |

参考视图累积型 PT 中两者性能差异可忽略，选择 alias table 是为 M2 实时 PT 提前铺路。

### 存储

单个 SSBO（Set 0 binding 6），头部嵌入元数据：

```
struct EnvAliasEntry {  // std430, 8 bytes
    float prob;         // Vose 概率阈值
    uint  alias;        // 备选像素索引
};

layout(set = 0, binding = 6) readonly buffer EnvAliasTable {
    float total_luminance;  // 环境贴图总亮度（PDF 归一化用）
    uint  entry_count;      // W × H
    EnvAliasEntry entries[];
};
```

- **分辨率**：源 HDR 半分辨率（宽高各除 2，典型 2048×1024 → 1024×512 = 512K entries）
- **格式**：`{float32 prob, uint32 alias}`，8 bytes/entry
- **总大小**：8 + entries × 8（典型 512K entries ≈ 4 MB）
- **无独立 PDF 纹理**：MIS 权重计算时直接从已有 IBL cubemap（Set 1 bindless）采样计算 luminance，除以 `total_luminance` 得到 PDF

`alias` 索引为 uint32（全分辨率 2M 像素超出 uint16 范围，半分辨率 512K 虽可用 uint16 但 std430 对齐后省不了空间，统一 uint32 保持简单）。

### 构建时机

在 IBL `load_equirect()` 流程中，`stbi_loadf()` 返回 float32 RGB 数据后、`stbi_image_free()` 前，直接从 CPU 内存中的原始 HDR 像素计算。不需要 GPU readback，精度最高，零额外 GPU 开销。

流程：
1. 对每个像素计算 `luminance × sin(theta)`（立体角校正）
2. Vose's algorithm 构建 alias table（CPU，O(N)）
3. 上传 alias table SSBO 到 GPU
4. 后续照常：RGB→RGBA float16 → 上传 equirect → cubemap 转换 → ...

alias table 随 IBL 产物一起做二进制缓存（key = `hdr_hash + "_alias_table"`）。

### IBL 旋转处理

Alias table 在 env-local 空间构建（与源 HDR 方向一致），运行时不重建。采样得到的方向在 shader 中用 `ibl_rotation_sin/cos`（GlobalUBO）旋转到世界空间。旋转变化不触发 alias table 重建。

### NEE 环境光采样流程

每个 bounce 的 closesthit 中新增 NEE 环境光步骤（与 NEE 方向光并列）：

```
1. 从 alias table 采样一个方向 env_dir（概率与亮度成正比）
2. 旋转到世界空间（应用 IBL rotation）
3. trace shadow ray → env_dir（终止条件同 NEE 方向光）
4. if (miss = 未遮挡) {
       env_radiance = cubemap(env_dir)
       pdf_env = luminance(env_radiance) / total_luminance × (W×H) / (2π²×sin_theta)
       pdf_brdf = BRDF PDF for env_dir
       weight = mis_power_heuristic(pdf_env, pdf_brdf)
       direct += env_radiance × BRDF(N, env_dir, V) × NdotL × weight / pdf_env
   }
```

### MIS 与方向光

M1 方向光为 delta 分布（Dirac delta PDF），BRDF 采样永远无法命中方向光。NEE 方向光的 MIS 权重恒为 1，退化为纯 NEE 无权重直接贡献。pt_common.glsl 中定义 `mis_power_heuristic()` 函数服务于 Step 11 env importance sampling 和 Step 12 area light NEE。

### MIS 权重计算（BRDF miss 方向）

closesthit 采样 BRDF 方向后，**立刻**计算该方向的 env MIS 权重（查 IBL cubemap 算 `pdf_env`，与已知 `pdf_brdf` 做 power heuristic），写入 `PrimaryPayload::env_mis_weight`。

- 若 BRDF 射线命中几何体 → 下一个 bounce，`env_mis_weight` 被忽略
- 若 BRDF 射线 miss → miss shader 返回 raw env color，raygen 乘以预存 `env_mis_weight`

miss shader 保持极简，不需要知道上一个表面的任何 BRDF 信息。

### Descriptor 守卫

Set 0 binding 6 与 binding 4/5 同样受 `rt_supported` 条件控制：`rt_supported = false` 时不创建 binding 6，不构建 alias table。binding 6 使用 `PARTIALLY_BOUND` flag（IBL 加载完成前 descriptor 未初始化）。

### M2 演进备注

- M2 实时 PT 每帧有限 SPP 时 alias table 的 O(1) 无分支优势充分发挥
- 显式环境采样 + env importance sampling 可叠加（独立优化）
- alias table 构建可从 CPU 迁移到 GPU compute（大分辨率 HDR 加速），接口不变

---

## PT 着色质量基础设施

阶段六 PT shader 实现中纳入的质量/正确性基础设施。这些不是"增强"而是 PT 正确出图的基本要求。

### Ray Origin Offset

closesthit 发射新射线（shadow ray 或 next bounce）时，需要沿几何法线偏移起点以避免自交（self-intersection）。

**方案**：Wächter & Binder（Ray Tracing Gems Ch.6）——对 float 位表示做整数偏移，按法线符号方向推离表面。全尺度鲁棒，无需场景相关的 epsilon 调参。pt_common.glsl 工具函数，closesthit 的 shadow ray 和 next_origin 共用。

### Normal Mapping

closesthit 通过 buffer_reference 额外读取 tangent（vec4，Vertex 结构体中已有），与 position/normal/UV 一同做重心坐标插值。采样 normal_tex 后调用与光栅化相同的 TBN 法线贴图逻辑（`#include "common/normal.glsl"` 复用 `get_shading_normal()`）。

### Shading Normal 一致性

Normal map 产生的 shading normal 可能指向几何表面以下（与入射方向同侧），导致光线"穿过"表面。closesthit 法线处理后 clamp shading normal 到几何法线半球，消除漏光伪影。

### Primary Ray Subpixel Jitter

reference_view.rgen 中 primary ray 使用 Sobol 前 2 维在像素内随机偏移（`pixel + vec2(rand(0), rand(1))`），提供 PT 抗锯齿。没有亚像素抖动，几何边缘永远锯齿，无论累积多少帧。per-bounce 采样维度从 dim 2 开始。

### Emissive 材质

closesthit 在每个 bounce 命中时检查材质 emissive（`emissive_tex × emissive_factor`），非零则加到 `payload.color`。所有 bounce 均加 emissive（物理正确），不仅限于 bounce 0 直视。

Step 12 引入面光源 NEE 后，BRDF 采样命中 emissive 表面的贡献加 MIS 权重（避免与 NEE 双重计算）。Step 12 之前无 MIS，直接贡献。

Emissive 表面仍正常做 NEE 和 BRDF 采样——emissive 是自发光分量，表面仍有 BRDF 反射其他光源。

### Multi-lobe BRDF 采样

每次 bounce 按 Fresnel 估计概率选择 diffuse（cosine-weighted hemisphere）或 specular（GGX VNDF）lobe。概率基于 `luminance(F_Schlick(NdotV, F0))`：高金属度偏向 specular，低金属度偏向 diffuse。选中后 PDF 除以选择概率补偿。

不做 multi-lobe 时，纯金属表面浪费 cosine 样本（diffuse 为零），纯电介质浪费 GGX 样本（specular 很弱），混合材质收敛慢。

### Firefly Clamping

raygen 累积前对 per-sample 贡献做 clamp：`sample_color = min(sample_color, vec3(max_clamp))`。**仅 clamp bounce > 0 的间接贡献**——bounce 0 不产生萤火虫（primary ray 确定性，无 Russian Roulette 补偿），直视 emissive 表面的正确高亮度不应被压暗。

`max_clamp` 通过 push constant 传递，Step 10 ImGui 面板提供 slider（默认 10.0，0 = 关闭）。引入微小偏差（能量损失），参考视图可接受。

### OIDN 辅助降噪通道

closesthit bounce 0 时通过 imageStore 写入两张辅助 image（aux albedo + aux normal）：

- **Albedo**：表面漫反射颜色（`base_color × (1 - metallic)`），不含 emissive 和光照结果。R16G16B16A16Sfloat（Step 9 从 R8G8B8A8Unorm 升级，配合 OIDN `HALF3` 零转换）
- **Normal**：shading normal（法线贴图后，非几何法线），世界空间。R16G16B16A16Sfloat

PrimaryPayload 新增 `uint bounce` 字段，raygen 在 traceRayEXT 前设置，closesthit 读取判断是否为 bounce 0。辅助 image 通过 Set 3 push descriptor binding 1/2 绑定（与 accumulation buffer 的 binding 0 同一 set）。

Step 7 ReferenceViewPass 创建并管理辅助 image。Step 9 OIDN filter 配置辅助通道（albedo + normal input），显著提升低采样数降噪质量。

---

## Area Light NEE（Step 12）

### 动机

Step 6 中 emissive 表面仅通过 BRDF 采样偶然命中来贡献光照。小面积高亮度 emissive 表面（灯罩、LED 屏幕）命中概率极低，导致高方差和萤火虫。Area light NEE 主动按发光功率采样 emissive 三角形，配合 MIS 消除与 BRDF 采样的双重计算。

### Emissive 三角形识别

场景加载时遍历所有 mesh primitive，`any(emissive_factor.rgb > vec3(0))` 的材质标记为 emissive。不做 emissive 纹理预积分——per-triangle power 仅用 `luminance(emissive_factor) × triangle_area`，忽略纹理空间变化。MIS 保证结果无偏，仅影响采样效率。

### 采样结构

Power-weighted alias table（Vose's algorithm，复用 Step 11 env importance sampling 的构建逻辑）。O(1) 无分支采样。

### 数据结构

EmissiveTriangle（std430，96 bytes）：

```
struct EmissiveTriangle {
    vec3  v0;              // offset  0  (+4B pad)
    vec3  v1;              // offset 16  (+4B pad)
    vec3  v2;              // offset 32  (+4B pad)
    vec3  emission;        // offset 48  — raw emissive_factor
    float area;            // offset 60  — 预计算世界空间三角形面积
    uint  material_index;  // offset 64  — MaterialBuffer 索引（纹理采样用）
    vec2  uv0;             // offset 72  — 顶点纹理坐标（NEE 采样点插值用）
    vec2  uv1;             // offset 80
    vec2  uv2;             // offset 88
};  // 96 bytes
```

存 UV 以精确采样 emissive 纹理。存 material_index 以读取完整材质参数。

### Descriptor

- Set 0 binding 7：EmissiveTriangleBuffer（SSBO，`PARTIALLY_BOUND`，`rt_supported` 守卫）
- Set 0 binding 8：EmissiveAliasTable（SSBO，header `[uint entry_count, float total_power]` 8B + `{float prob, uint alias}` 8B/entry，`PARTIALLY_BOUND`）

两个独立 SSBO（三角形数据 vs alias table），职责清晰。

### MIS 公式

NEE emissive 和 BRDF 采样命中 emissive 使用 power heuristic 平衡。

**NEE 路径**（closesthit 主动采样发光三角形）：
- `light_pdf = selection_prob / area × dist² / cos_light`（立体角度量）
- `brdf_pdf`：当前表面 BRDF 朝采样方向的概率（现场可算）
- `weight = power_heuristic(light_pdf, brdf_pdf)`

**BRDF 路径**（BRDF 采样偶然命中 emissive）：
- `brdf_pdf`：上一个 bounce 的 BRDF 采样 PDF → 通过 PrimaryPayload `last_brdf_pdf` 传递
- `light_pdf`：现场算（三角形面积、距离、角度）
- `weight = power_heuristic(brdf_pdf, light_pdf)`

**Bounce 0 直视 emissive**：权重恒 1.0（primary ray 非 BRDF 采样，无 brdf_pdf）。

### 双面 Emissive

跟随 glTF `doubleSided` 标志：`doubleSided = true` 双面发光，`false` 仅正面发光。

GPUMaterialData 的 `_padding`（offset 76）改为 `uint double_sided`。SceneLoader 填充 doubleSided 字段。

光栅化管线中 single-sided 背面已由硬件 face culling 处理（`VkCullModeFlagBits` 由 `MeshDrawGroup::double_sided` 控制），forward.frag 无需额外 discard。

RT 管线中 closesthit 检测到命中背面（`dot(N_geo_unflipped, ray_dir) > 0`）且 `double_sided == 0` 时，消耗一个 bounce 穿过表面：`color = vec3(0)`、`throughput_update = vec3(1.0)`、`hit_distance = gl_HitTEXT`、`next_origin/next_direction` 沿原 ray 方向继续。raygen 无需修改——RR 在 throughput=1.0 时存活概率 100%，路径自然继续。max_bounces 默认值从 8 调至 16 以配合偶尔的背面穿透消耗。

### 代码归属

新建 `framework/emissive_light_builder.h/.cpp`（EmissiveLightBuilder 类），与 SceneASBuilder 独立。输入相同（meshes, mesh_instances, materials），互不依赖。Renderer 场景加载后先调 SceneASBuilder::build() 再调 EmissiveLightBuilder::build()。

### 无 Emissive 场景

Push constant 新增 `uint emissive_light_count`（0 = 跳过 NEE emissive）。Binding 7/8 未写入（`PARTIALLY_BOUND` 保护），closesthit 读 emissive_light_count 判断是否执行 NEE emissive。

### Shadow Ray

同方向光 NEE flags（`gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT`）。tMin 使用 Wächter & Binder 偏移。tMax = `distance * (1.0 - 1e-4)`（相对偏移，避免命中目标三角形本身）。

### M2 演进备注

- M2 实时 PT 中 area light NEE 的 O(1) alias table 采样效率优势充分发挥
- 大量 emissive 三角形（>10K）时可升级为 Light BVH
- 纹理 emissive 预积分可提升采样效率（per-triangle 实际功率 vs factor-only 估计）

---

## Texture LOD / Ray Cones（Step 13）

### 动机

RT shader 中 `texture()` 默认采样 mip 0（最高分辨率）。所有纹理都采样最高分辨率浪费带宽，远处表面可能闪烁。参考视图累积后闪烁被平均掉，但带宽浪费持续存在。

### 方案：Ray Cones

Akenine-Möller et al. 2021。追踪每条射线的锥体扩展角（spread angle），命中时根据锥体宽度和三角形纹理密度估算 texture LOD。

- **初始化**（raygen，bounce 0）：`pixel_spread = atan(2 × tan(fov/2) / screen_height)`，`payload.cone_width = 0`（锥在起点无宽度），`payload.cone_spread = pixel_spread`（初始扩展角 = 像素张角）
- **传播**（每个 bounce）：`cone_width = payload.cone_width + hit_distance × payload.cone_spread`（cone 随距离和当前扩展角扩散）
- **曲率修正**（每个 bounce 反射后）：`cone_spread' = max(cone_spread + 2 × curvature × cone_width, 0)`（凹面收窄、凸面发散、平面不变）
- **LOD 计算**（closesthit）：从 cone width + 三角形 UV/world 面积比 + 纹理分辨率算出 mip level

**设计决策**：
- **表面曲率**：从 face_normal 与 interpolated_normal 的偏差估算（详见「Surface Curvature Estimation」章节）
- **纹理密度**：运行时从已读取的顶点位置 + UV 算三角形的 world/UV 面积比，不预存到 GeometryInfo
- **退化三角形**：world_area 或 uv_area 接近零时回退 LOD 0（全分辨率），避免面积比爆炸
- **LOD 上限 clamp**：`min(lod, lod_max_level)` 防止过度模糊（详见「LOD Clamp」章节）

### Surface Curvature Estimation

closesthit 中已有 `N_face`（三角形几何法线）和 `N_interp`（插值顶点法线），两者的偏差反映表面弯曲程度。Ray Cones 论文的完整传播模型在反射后修正扩展角：

```
cone_spread' = max(cone_spread + 2 × curvature × cone_width, 0)
```

曲率估算方法：沿射线方向的法线变化率。`N_interp` 相对于 `N_face` 偏向入射方向时表面凹（聚焦，curvature < 0），偏离时表面凸（发散，curvature > 0），两者一致时平面（curvature = 0）。

```
curvature ≈ dot(N_interp - N_face, -ray_direction) / max(cone_width, epsilon)
```

精度有限（基于三角形级别的法线差异，非解析曲率），但正确捕捉了凹/凸/平的符号和量级。`max(..., 0)` clamp 防止极端凹面导致 spread 变负（锥"反转"无物理意义）。

PrimaryPayload 需同时存储 `cone_width`（累积宽度）和 `cone_spread`（当前扩展角）。扩展角不再是常量——每次反射后可能因曲率而变化。

### FOV 获取

`tan(fov_y/2) = abs(global.inv_projection[1][1])`，从已有 GlobalUBO 推导，不新增字段。Vulkan Y-flip 不影响此元素的绝对值。raygen 和 closesthit 均可访问 GlobalUBO。

### Texel Density 计算

`compute_texel_density()` 作为 pt_common.glsl 中的独立工具函数，接收 `GeometryInfo`、`gl_PrimitiveID`、`gl_ObjectToWorldEXT`，内部通过 buffer_reference 重新读取三角形三个顶点的位置和 UV。

选择重新读取而非扩展 HitAttributes 的理由：`interpolate_hit()` 刚访问过完全相同的内存地址，GPU L1 cache 命中率极高（实质等同寄存器读取）。避免为单个 Step 的需求污染通用数据结构。直接在世界空间计算边叉积得到面积，无需处理 object → world 的 determinant 转换。

### Per-texture LOD

LOD 公式拆为两部分：

```
lod = min(log2(cone_width × sqrt(uv_area / world_area)) + 0.5 × log2(tex_w × tex_h), lod_max_level)
      ────────── per-triangle base_lod ──────────   ──── per-texture 分辨率项 ────   ─── clamp ───
```

`base_lod` 只跟三角形几何属性和 cone width 有关，每次命中算一次。每个纹理通过 `textureSize()` 取各自分辨率（描述符元数据读取，开销极小），确保不同分辨率的纹理得到正确的 mip level。最终 LOD clamp 到 `lod_max_level`（push constant，可调）防止过度模糊。

### Anyhit LOD（近似方案）

Vulkan spec 限制 anyhit 不能读写 payload，无法获取真实 cone width。使用近似：`cone_width ≈ gl_HitTEXT × pixel_spread`（`pixel_spread` 从 `inv_projection` + `screen_size` 重新计算）。

- Primary ray：精确（cone_spread = 0 时等价于真实公式）
- Bounce ray：下界（忽略前几段累积的宽度），LOD 偏向高分辨率 mip

下界意味着 alpha test 保守安全（不会错误采用过低分辨率导致 alpha 判断失误），唯一 tradeoff 是 non-opaque 几何体的带宽节省未达理想值。anyhit 中已读取 v0/v1/v2 做 UV 插值，顺便计算 texel density 零额外内存访问。

### NEE Emissive 纹理采样

closesthit 中 NEE 采样 emissive 光源纹理保持 `texture()`（隐式 mip 0）。Ray cone footprint 描述的是当前着色点的屏幕覆盖范围，不适用于 NEE 主动采样的目标三角形表面。

### LOD Clamp

Push constant 新增 `uint lod_max_level`，应用时 `final_lod = min(computed_lod, lod_max_level)`。防止多次 bounce 累积、凸面曲率高估、极窄三角形等情况导致 LOD 过高（纹理过度模糊）。

默认值 4（保留至少 16×16 细节，对 4K 纹理 = 最多降到 mip 8，256×256）。值为 0 = 强制全分辨率（调试用）。Step 13 ImGui 面板加 slider（范围 0 ~ `textureQueryLevels - 1`）。

不使用 lod_bias（全局偏移）：bias 无法针对性解决模糊问题，会误伤所有正常 LOD。clamp 只截断极端值，不影响正常区域。

### PrimaryPayload

新增两个字段（64B → 72B）：

- `float cone_width`：累积 cone 宽度（世界空间长度）。raygen 初始化为 0，closesthit 每次 bounce 更新为 `旧值 + hit_distance × cone_spread`
- `float cone_spread`：当前扩展角（rad）。raygen 初始化为 `pixel_spread`（像素张角），closesthit 每次反射后根据曲率估算修正：`max(旧值 + 2 × curvature × cone_width, 0)`

两者都需要跨 bounce 传递——`cone_spread` 不再是常量（曲率修正后每个 bounce 可能不同）。

### Shader 改动

- **pt_common.glsl**：新增 `init_ray_cone()`、`propagate_ray_cone()`、`estimate_curvature()`、`compute_texel_density()`、`compute_ray_cone_lod()`
- **closesthit**：propagate cone + 估算曲率修正 spread + 材质纹理采样 `texture()` → `textureLod()`（~4 处：base_color、metallic_roughness、normal、emissive）+ LOD clamp。NEE emissive 光源纹理采样保持 `texture()`
- **anyhit**：alpha 纹理采样 `texture()` → `textureLod()`（~1 处，近似 cone width）
- **raygen**：初始化 `payload.cone_width = 0`、`payload.cone_spread = pixel_spread`

### M2 演进备注

- M2 实时 PT（单帧有限 SPP）对 LOD 精度要求更高，可升级曲率估算精度（当前基于三角形级法线差异，M2 可考虑二阶法线插值）
- anyhit 近似可升级为 per-pixel SSBO 传递精确 cone width

---

## 实现细节备注

以下为实现时直接采用的标准做法，不需要额外决策：

| 项 | 方案 |
|----|------|
| SBT 结构 | 1 raygen + 1 miss（环境光）+ 1 shadow miss + 1 hit group（closest-hit + any-hit） |
| Buffer Device Address | Vulkan 1.2 核心，Geometry Info 存 VkDeviceAddress |
| Miss shader 环境采样 | 读 GlobalUBO skybox_cubemap_index，采样 Set 1 cubemaps[] |
| Shadow ray（NEE） | 单独 miss group，miss = 未遮挡 |
| 顶点格式硬编码 | `vertexFormat` = `R32G32B32_SFLOAT`（position offset 0），`indexType` = `UINT32` |
| GLSL 扩展 | `GL_EXT_ray_tracing` + `GL_EXT_buffer_reference` / `buffer_reference2` + `GL_EXT_shader_explicit_arithmetic_types_int64` + `GL_EXT_nonuniform_qualifier` |
