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
- **Set 1**：复用（bindless textures + cubemaps）

Closest-hit shader 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 buffer address 和 material ID，再读 MaterialBuffer 获取 PBR 参数并采样 bindless 纹理。变换矩阵通过 TLAS 内置的 `gl_ObjectToWorldEXT` / `gl_WorldToObjectEXT` 获取。

**RT shader 不访问 InstanceBuffer**——transform 来自 TLAS 内置矩阵，material 来自 GeometryInfo。InstanceBuffer 仅服务于光栅化路径。

光栅化 shader 不引用 binding 4/5，存在于 set layout 中不产生开销。

---

## OIDN 集成

**数据流**：Vulkan readback → CPU 内存 → OIDN GPU 降噪 → CPU 内存 → Vulkan upload。

"CPU 中转"指 Vulkan 与 OIDN 之间的数据通道走 CPU 内存，降噪计算本身在 GPU 上执行（CUDA/HIP/SYCL）。

选择 CPU 中转而非 VK_KHR_external_memory 零拷贝的理由：M1 的烘焙器是离线的，参考视图降噪不需要每帧执行（每隔 N 次采样触发一次），CPU 中转延迟完全可接受。省去外部内存互操作的复杂度。

**降噪频率**：参考视图不必每帧降噪，可配置为每 N 次采样后触发一次 OIDN，或由用户手动触发。

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

- 每累积 **N 个采样**自动触发一次（N 可配置，默认 64）
- ImGui 提供自动触发开关，关闭自动时允许手动触发
- 降噪结果写入单独的 denoised buffer，不覆盖 accumulation buffer（累积继续进行）

---

## 参考视图输出显示

默认显示 OIDN 降噪后结果。ImGui 开关可切换为显示原始累积结果（用于观察真实噪点状况）。降噪功能关闭时自然显示原始累积。

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
- **PrimaryPayload**（location 0）：`vec3 color` + `vec3 next_origin` + `vec3 next_direction` + `vec3 throughput_update` + `float hit_distance`（miss 时 -1 终止）
- **ShadowPayload**（location 1）：`uint visible`（初始 0 = 遮挡，shadow_miss 设 1）

---

## PT 采样策略

Sobol 低差异序列 + Cranley-Patterson rotation + Blue noise 空间去相关。

- **Sobol**：预计算方向数表嵌入 shader 常量数组，提供跨帧/采样点均匀分布的低差异采样值
- **Blue noise**：128×128 R8Unorm 单通道纹理，像素数据从公开数据集嵌入代码，初始化时上传 GPU 注册到 bindless 数组
- **Cranley-Patterson rotation**：`sample = fract(sobol(n, dim) + blue_noise(pixel + offset))`，不同采样维度通过空间偏移从同一张纹理派生

---

## PT Push Constants

raygen shader 的 push constant 仅包含 PT 特有参数（12 字节）：`max_bounces`（uint）、`sample_count`（uint）、`frame_seed`（uint）。

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

**M1 局限性**：Stochastic alpha 只处理薄表面 alpha 混合（`AlphaMode::Blend`），不处理体积介质的折射/透射/衰减。玻璃等需要 IOR、transmission 参数的物体在 M1 中表现为按 alpha 概率穿透的薄膜，无折射效果。

**M2 演进**：解析 `KHR_materials_transmission` + `KHR_materials_volume` 扩展，closest-hit 中对有 transmission 参数的材质实现 Snell 折射 + Fresnel + Beer 衰减。Stochastic alpha 与折射正交共存——stochastic 在 any-hit 层决定"射线是否穿过"，折射在 closest-hit 层决定"穿过后的方向和衰减"。无 transmission 参数的 Blend 物体仍走 stochastic alpha。

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
