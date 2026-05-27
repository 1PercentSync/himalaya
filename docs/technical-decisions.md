# 技术选型与决策

> 渲染器当前使用的技术方案、选型理由和关键实现决策。
> 架构层面的设计见 `architecture.md`。

---

## 1. Vulkan API 基线

**Vulkan 1.4**

| 核心特性 | 用途 |
|---------|------|
| Dynamic Rendering | 替代 VkRenderPass / VkFramebuffer，与 Render Graph 天然契合 |
| Synchronization2 | 更清晰的 barrier API，Render Graph barrier 自动插入受益 |
| Extended Dynamic State | 减少 pipeline 数量，viewport/scissor/cull mode/depth 等动态设置 |
| Descriptor Indexing | Bindless 纹理支持 |

---

## 2. 基础架构决策

### 资源句柄

Generation-based（index + generation）。资源销毁时 generation 递增，使用时比对，捕获所有 use-after-free。Pipeline 不纳入资源池——所有权始终单一明确（pass 持有）。

### 对象生命周期

显式 `destroy()` 方法，不依赖析构函数。Vulkan 对象销毁顺序重要且复杂，显式调用让时机完全可控。

### 帧同步

2 Frames in Flight。每帧开始等前 N 帧 fence，执行延迟删除队列，录制新命令。

### 错误处理

- `VK_CHECK` 宏 + abort（Vulkan API 错误 = 编程错误）
- Validation Layer 开发期常开
- `VK_EXT_debug_utils`：资源 debug name + command buffer 区域标记

### 深度缓冲

- **格式**：D32Sfloat（无 stencil）
- **Reverse-Z**：near=1, far=0, clear=0.0f, compare=GREATER
- 所有 depth 相关功能统一此约定

### Application + Renderer 分离

- **Application** 持有：窗口、RHI 基础设施、ImGui、场景数据、渲染参数
- **Renderer** 持有：RenderGraph、ShaderCompiler、MaterialSystem、所有 Pass、渲染目标、per-frame GPU 数据

Renderer 持有 non-owning 引用（Context*、Swapchain* 等从 Application 接收）。

### Resize 两阶段

```
vkQueueWaitIdle → renderer.on_swapchain_invalidated() → swapchain.recreate() → renderer.on_swapchain_recreated()
```

---

## 3. Render Graph 实现决策

### 设计模式

手动编排 Pass 列表 + barrier 自动插入。不做自动拓扑排序、不做资源别名优化。Pass 声明输入输出（`RGResourceUsage`），RG 根据声明推导 barrier。

**帧间生命周期**：每帧重建（`clear()` → `import/use` → `add_pass` → `compile` → `execute`），不是过渡性设计。

**升级路径**：已有 pass 声明格式不变，后续加入拓扑排序和资源别名分析时无需改 pass 代码。

### Managed 资源

- `create_managed_image()` 注册持久 handle，`use_managed_image()` 每帧返回 `RGResourceId`
- Initial layout 统一 `UNDEFINED`（每帧覆写）
- `final_layout` 由调用方显式指定：非 temporal 传 `UNDEFINED`，temporal current 传 `SHADER_READ_ONLY_OPTIMAL`
- `get_managed_backing_image()` 在 resize handler 中即时获取新 handle 更新 descriptor
- Slot 状态通过 `backing.valid()` 判断

### Temporal 机制

- `create_managed_image(..., temporal=true)` 内部分配第二张 backing image
- `clear()` 自动 swap current/history，resize 重建两张并标记 history 无效
- `get_history_image()` 始终返回 valid RGResourceId（push descriptor 不支持 PARTIALLY_BOUND）
- `is_history_valid()` 查询有效性（首帧/resize 后无效 → blend_factor=0）

### Barrier 计算

- 从 `(RGAccessType, RGStage)` 映射到 `(VkImageLayout, VkPipelineStageFlags2, VkAccessFlags2)`
- 四种 hazard：RAR 无需 barrier，RAW/WAW/WAR 均处理
- RG 不管 loadOp/storeOp（pass 自行构造 `VkRenderingInfo`）

---

## 4. Pass 约定

### Pass 类约定

具体类（非虚基类），Renderer 持有具体类型成员。各 pass 方法集允许不统一，同功能方法保持同名。

| 方法 | 职责 | 调用时机 |
|------|------|----------|
| `setup()` | Pipeline 创建 + 存储服务指针 | 初始化 |
| `record()` | RG 资源声明 + execute lambda | 每帧 |
| `rebuild_pipelines()` | shader 重编译 + pipeline 重建 | 热重载 |
| `destroy()` | 销毁 pipeline + 私有资源 | 关闭 |

FrameContext 是纯每帧数据（RG 资源 ID + 场景数据引用 + 帧参数），不做 service locator。

### Pass 运行时配置

当前 PT 路径通过 `PTConfig` 和 Renderer 状态驱动运行时行为：DebugUI 修改配置，Renderer 在每帧写入 GlobalUBO 或 pass push constants。需要跳过的工作（如达到目标采样数、关闭降噪显示）由 Renderer 在注册 pass 前判断。

配置变更无需 GPU idle；除资源尺寸或环境贴图重载外，下一帧自然生效。

---

## 5. 描述符与绑定

### 三层架构

| Set | 内容 | 生命周期 | 份数 |
|-----|------|---------|------|
| 0 | 全局 Buffer（GlobalUBO + MaterialBuffer + RT bindings） | per-frame 双缓冲 | ×2 |
| 1 | 持久纹理资产（bindless 2D + cubemap） | 场景加载 → 卸载 | ×1 |
| 2 | 帧内 Render Target（rt_hdr_color） | init / resize / source switch | ×2（per-frame） |

### Bindless 纹理

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[];     // 上限 4096
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];   // 上限 4096
```

`PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，slot 通过 free list 回收。

### Compute / RT Pass 绑定机制

- `bind_compute_descriptor_sets()` 绑定 Set 0-2 到 COMPUTE bind point
- `push_storage_image()` / `push_sampled_image()` 绑定 Set 3 push descriptor
- `get_dispatch_set_layouts(set3_push_layout)` → `{set0, set1, set2, set3}`（compute 和 RT pipeline 共用）

Set 3 在现有 per-pass compute / RT pipeline 中通常是 push descriptor，用于绑定当前 pass 的 transient I/O。GS 渲染是例外：GS pipeline 使用自己的持久 Set 3 descriptor set 绑定 GS buffers。两者处于不同 pipeline layout 下，不混用；“Set 3”只表示 descriptor set index，不表示全局唯一 layout。

---

## 6. 数据格式与纹理管线

### HDR Color Buffer

R16G16B16A16F 全程。暗部无 banding 风险，后处理链多次读写不累积量化误差。

### 顶点格式

统一格式：position(vec3) + normal(vec3) + uv0(vec2) + tangent(vec4) + uv1(vec2)。缺失属性填默认值。

### 纹理格式

| 角色 | Format | 理由 |
|------|--------|------|
| base color, emissive | `R8G8B8A8_SRGB` | 颜色数据，GPU 自动 gamma 解码 |
| normal, metallic-roughness, occlusion | `R8G8B8A8_UNORM` | 线性数据，原样读取 |

### 纹理压缩

| 类别 | BC 格式 | VkFormat |
|------|---------|----------|
| baseColor / emissive | BC7 | `BC7_SRGB_BLOCK` / `BC7_UNORM_BLOCK` |
| metalRough | BC7 | `BC7_UNORM_BLOCK` |
| normal | BC5 | `BC5_UNORM_BLOCK` |

编码器：bc7e.ispc（SIMD）+ rgbcx（BC4/BC5）。首次压缩后缓存为 KTX2（`%TEMP%\himalaya\textures\`），后续直接加载。

### Default 纹理

| 名称 | RGBA | 用途 |
|------|------|------|
| White | (1,1,1,1) | base color / metallic-roughness / occlusion neutral |
| Flat Normal | (0.5,0.5,1.0,1.0) | normal map 无扰动 |
| Black | (0,0,0,1) | emissive 无自发光 |

GPUMaterialData 缺失纹理填 default BindlessIndex，shader 无条件采样。

---

## 7. PBR / BRDF

**参数化**：Metallic-Roughness 工作流（glTF 标准）。

### 漫反射

Lambert（Pass 1）→ Burley Diffuse（Pass 2）。视觉差异主要体现在粗糙表面掠射角，大部分场景下几乎看不出区别。

### 镜面反射（Cook-Torrance）

| 组件 | 方案 | 演进 |
|------|------|------|
| D（NDF） | GGX | Pass 2：Multiscatter GGX 能量补偿（预计算 LUT） |
| G（几何遮蔽） | Smith Height-Correlated GGX | 锁定 |
| F（菲涅尔） | Schlick 近似 | 锁定 |

GGX 是工业标准，长尾分布高光拖尾自然。Smith Height-Correlated 是 GGX NDF 数学上配套的选择。Multiscatter GGX 解决高粗糙度金属偏暗问题，实现成本低（一张 LUT + 十几行 shader），与 IBL BRDF Integration LUT 共享基础设施。

---

## 8. 材质与场景数据

### 材质系统

代码定义 + 固定数据结构。`GPUMaterialData`（80 bytes std430）定义在 `material_system.h`。

**升级路径（多着色模型）**：固定 stride 方案——所有材质 struct 填充到同大小，每个 shader variant 定义自己的 typed struct，通过 `materials[material_index]` 统一寻址。

### 场景数据接口

渲染列表：`SceneRenderData` 用 `std::span` 引用应用层数据（mesh instances + camera），渲染器只读消费。环境光由 IBL 资源提供；面光源由 emissive 材质三角形构建采样表。

### Shader 系统

运行时 shaderc 编译（GLSL → SPIR-V）+ 热重载。公共文件按依赖链组织。无条件采样（bindless + default 纹理红利），按需引入动态分支或编译变体。

---

## 9. 色彩管线

| 项目 | 方案 |
|------|------|
| 工作色彩空间 | sRGB 线性 |
| 浮点格式 | R16G16B16A16F |
| 广色域 | 不实现，架构预留 |

sRGB 色域作为工作空间够用。广色域（ACEScg/Rec.2020）边际收益在实时渲染中有限，且输入贴图大概率是 sRGB authored，需要额外色域转换。

### Tonemapping

| 演进 | 方案 |
|------|------|
| Pass 1 | ACES 拟合（资料最多，一行代码） |
| Pass 2 | Khronos PBR Neutral（专为 PBR 设计，最小化对材质外观的干扰） |

---

## 10. IBL 管线

- Framework 层 `ibl.h`，自管理全部资源
- Equirectangular .hdr → GPU cubemap → irradiance / prefiltered / BRDF LUT
- 环境 cubemap mip 剥离 + GPU BC6H 压缩，运行时 ~26 MB
- 加载失败时 fallback 1×1 中性灰 cubemap，管线照常运行
- IBL 缓存：两组独立（BRDF 固定 key + 3 cubemaps HDR hash），KTX2 格式

### 缓存基础设施

`framework/cache.h` 工具模块：`cache_root()`（`%TEMP%\himalaya\`）+ `content_hash()`（XXH3_128）+ `cache_path()`。消费者各自处理序列化。

### GPU 上传

批量 immediate command scope（`begin_immediate()` → 录制所有 copy → `end_immediate()` 单次 submit）。`upload_image_all_levels()` 单 staging buffer + 批量 copy region 上传预建 mip chain。

---

## 11. RT 基础设施

### RT API

**双轨方案**：`VK_KHR_ray_tracing_pipeline` + `VK_KHR_ray_query`，均基于 `VK_KHR_acceleration_structure`。

| API | 用途 |
|-----|------|
| Ray Tracing Pipeline | PT 渲染（大量射线调度，raygen shader） |
| Ray Query | 未来混合 RT 效果（fragment/compute 内联查询） |

两者共享加速结构，BLAS/TLAS 构建一次两边复用。

### RT 扩展可选性

RT 扩展为可选：设备选择时 RT 支持作为加分项但非硬需求。`rt_supported = false` 时禁用全部 RT 功能。Set 0 layout 根据 `rt_supported` 决定是否包含 RT binding。

### 加速结构构建策略

**Multi-geometry BLAS**：同一 glTF mesh 下的多个 primitive 合并为一个 BLAS 的多个 geometry。TLAS 每个 node instance 对应一个 instance entry。减少 TLAS instance 数量，提升构建和遍历效率。

`instanceCustomIndex` = 该 BLAS 在 Geometry Info buffer 中的 base offset。Closesthit 用 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 索引。

**Per-geometry opacity 标记**：Opaque geometry 设 `VK_GEOMETRY_OPAQUE_BIT_KHR`（硬件跳过 any-hit），non-opaque geometry 设 `VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`。

**BLAS 批量构建**：单次 `vkCmdBuildAccelerationStructuresKHR` 传入全部 BLASBuildInfo，GPU 并行构建。分配大 scratch buffer = 所有 BLAS scratch size 之和。

**生命周期**：场景加载时构建一次（`PREFER_FAST_TRACE`），场景销毁时释放。Scratch buffer 构建完成后释放。

### GeometryInfo 数据结构

`GPUGeometryInfo`（std430，24 bytes）：`vertex_buffer_address`(uint64) + `index_buffer_address`(uint64) + `material_buffer_offset`(uint32) + `_padding`(uint32)。

不存 vertex_stride（统一顶点格式，shader 端硬编码常量）、不存 count（closesthit 通过 `gl_PrimitiveID` 索引，一定有效）。

### 场景数据访问

RT shader 通过已有 descriptor 架构访问场景数据：

- **Set 0 binding 0**：GlobalUBO
- **Set 0 binding 1**：MaterialBuffer
- **Set 0 binding 2**：TLAS accelerationStructureEXT
- **Set 0 binding 3**：GeometryInfo（per-geometry buffer address + material ID）
- **Set 0 binding 4**：EnvAliasTable（环境光重要性采样）
- **Set 0 binding 5-6**：EmissiveTriangle / EmissiveAliasTable（面光源 NEE）
- **Set 1**：复用（bindless textures + cubemaps）

Closest-hit shader 通过 GeometryInfo 获取 buffer address 和 material ID，再读 MaterialBuffer 获取 PBR 参数并采样 bindless 纹理。变换矩阵通过 TLAS 内置 `gl_ObjectToWorldEXT` / `gl_WorldToObjectEXT` 获取。

### RT Shader 热重载

`RTPipeline` 封装将 VkPipeline + SBT buffer 生命周期绑定在一起。`rebuild_pipelines()` 走 destroy + create 全量重建，SBT 自然跟随更新。

### SBT 结构

1 raygen + 1 miss（环境光）+ 1 shadow miss + 1 hit group（closest-hit + any-hit）。

---

## 12. Path Tracing 核心

### 采样策略

Sobol 低差异序列 + Cranley-Patterson rotation + Blue noise 空间去相关。

- **Sobol**：预计算方向数表上传 SSBO（16 KB，128 维），避免 SPIR-V 常量膨胀
- **Blue noise**：128×128 R8Unorm 单通道纹理（Void-and-Cluster 预生成，CC0 许可），数据嵌入 C++ 头文件
- **Cranley-Patterson rotation**：`sample = fract(sobol(n, dim) + blue_noise(pixel + offset))`

### Ray Payload 设计

模式 A：closesthit 内完成全部着色计算，通过 payload 返回结果，raygen 仅做循环累积。

- **PrimaryPayload**（location 0，72B）：`color` + `next_origin` + `next_direction` + `throughput_update` + `hit_distance` + `bounce` + `env_mis_weight` + `last_brdf_pdf` + `cone_width` + `cone_spread`
- **ShadowPayload**（location 1）：`uint visible`

### Push Constants

布局 32B：`max_bounces` + `sample_count` + `frame_seed` + `blue_noise_index` + `max_clamp` + `env_sampling` + `emissive_light_count` + `lod_max_level`。

相机逆矩阵从 GlobalUBO 读取（不占 push constant 空间）。

### PT 默认参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 最大 bounce | 16 | 有 Russian Roulette 时设高无性能代价 |
| 每帧 SPP | 1 | 保持 UI 响应 |
| Russian Roulette 起始 | bounce 2-3 | 不终止 primary ray 和首次反弹 |

### 着色质量基础设施

**Ray Origin Offset**：Wächter & Binder（Ray Tracing Gems Ch.6）——对 float 位表示做整数偏移，全尺度鲁棒，无需 epsilon 调参。

**Normal Mapping**：closesthit 读取 tangent 做重心坐标插值，使用共享 TBN 法线贴图工具。Shading normal clamp 到几何法线半球，消除漏光伪影。

**Primary Ray Subpixel Jitter**：Sobol 前 2 维在像素内随机偏移，提供 PT 抗锯齿。

**Multi-lobe BRDF 采样**：每次 bounce 按 Fresnel 估计概率选择 diffuse（cosine-weighted hemisphere）或 specular（GGX VNDF）lobe。选中后 PDF 除以选择概率补偿。

**Firefly Clamping**：per-sample `min(sample_color, vec3(max_clamp))`，仅 clamp bounce > 0 的间接贡献。默认关闭（0.0）以保持无偏。

**Emissive 材质**：每个 bounce 命中时检查 emissive（`emissive_tex × emissive_factor`），非零则贡献。面光源 NEE 启用后加 MIS 权重。

**OIDN 辅助通道**：closesthit bounce 0 时 imageStore 写入 aux albedo（`base_color × (1-metallic)`，R16G16B16A16Sfloat）+ aux normal（shading normal，R16G16B16A16Sfloat）。

---

## 13. Non-opaque 几何体处理

单一 any-hit shader 处理所有 non-opaque 几何体，与 closest-hit 共用 hit group。

- **Mask**：`texel_alpha < alpha_cutoff` → `ignoreIntersectionEXT`（标准 alpha test）
- **Blend**：`rand() >= texel_alpha` → `ignoreIntersectionEXT`（stochastic alpha）

Stochastic alpha 是 alpha coverage 近似——以概率 `alpha` 接受命中。数学上等价于 alpha blending 的期望。Primary ray 和 shadow ray 使用同一个 any-hit shader。

Any-hit 随机数使用 PCG 哈希（`gl_LaunchIDEXT` + `frame_seed` + `gl_PrimitiveID` + `gl_GeometryIndexEXT`），纯 ALU 运算。

### 双面 Emissive

跟随 glTF `doubleSided` 标志。RT 管线中 closesthit 检测背面命中且 `double_sided == 0` 时，消耗一个 bounce 穿过表面继续前进。

---

## 14. Environment Map Importance Sampling

### Alias Table

选择 alias table（Vose's algorithm，O(1) 采样）而非 CDF（O(log N) binary search）。单个 SSBO，头部嵌入 `total_luminance` + `entry_count`，每 entry 8 bytes（`{float prob, uint alias}`）。源 HDR 半分辨率，典型 ~4 MB。

### 构建时机

在 IBL `load_equirect()` 中，`stbi_loadf()` 返回后从 CPU 内存直接计算（`luminance × sin(theta)` 立体角校正）。随 IBL 产物一起做二进制缓存。

### IBL 旋转处理

Alias table 在 env-local 空间构建，运行时不重建。采样方向在 shader 中用 GlobalUBO 的 `ibl_rotation_sin/cos` 旋转到世界空间。

### NEE 环境光采样

每个 bounce 的 closesthit 中：alias table 采样方向 → 旋转到世界空间 → trace shadow ray → 未遮挡时 MIS 加权贡献。

### MIS 权重（BRDF miss 方向）

Closesthit 采样 BRDF 方向后立刻计算 env MIS 权重，写入 `PrimaryPayload::env_mis_weight`。Miss shader 仅返回 raw env color，raygen 乘以预存权重。

---

## 15. Area Light NEE

### Emissive 三角形识别

场景加载时遍历所有 primitive，`any(emissive_factor.rgb > vec3(0))` 标记为 emissive。Per-triangle power = `luminance(emissive_factor) × triangle_area`（不做纹理预积分，MIS 保证无偏）。

### 采样结构

Power-weighted alias table（O(1) 采样）。EmissiveTriangle（96 bytes std430）：v0/v1/v2 + emission + area + material_index + uv0/uv1/uv2。

### MIS 公式

NEE 路径和 BRDF 路径使用 power heuristic 平衡。Bounce 0 直视 emissive 权重恒 1.0。`PrimaryPayload::last_brdf_pdf` 跨 bounce 传递 BRDF PDF。

---

## 16. Texture LOD / Ray Cones

Akenine-Möller et al. 2021。追踪每条射线的锥体扩展角，命中时根据锥体宽度和三角形纹理密度估算 LOD。

### 传播

- **初始化**：`cone_width = 0`，`cone_spread = pixel_spread = atan(2 × tan(fov/2) / screen_height)`
- **每个 bounce**：`cone_width += hit_distance × cone_spread`。穿过凹面焦点时取绝对值并翻转 spread
- **曲率修正**：`cone_spread' = cone_spread + 2 × curvature × cone_width`（从 face_normal vs interpolated_normal 偏差估算曲率）

### LOD 计算

```
lod = min(log2(cone_width × sqrt(uv_area / world_area)) + 0.5 × log2(tex_w × tex_h), lod_max_level)
```

Per-texture 分辨率项确保不同分辨率纹理得到正确 mip level。`lod_max_level`（push constant，默认 4）防止过度模糊。

### Anyhit 近似

Vulkan spec 限制 anyhit 不能读写 payload。使用近似：`cone_width ≈ gl_HitTEXT × pixel_spread`。整体偏保守（多数路径无凹面聚焦）。

---

## 17. OIDN 降噪系统

### 集成概要

OIDN 2.x GPU 降噪（CUDA/HIP/SYCL）。数据流：Vulkan readback → CPU 内存 → OIDN GPU 降噪 → CPU 内存 → Vulkan upload。选择 CPU 中转而非零拷贝以省去外部内存互操作复杂度。

### 异步执行

三段分离：
1. **Readback**：RG 内 Transfer Pass（与渲染命令同一 submit）
2. **OIDN 执行**：`std::jthread` 后台线程（`vkWaitSemaphores` 等待 readback → OIDN filter → memcpy）
3. **Upload**：下一帧 RG 内 Transfer Pass

### Timeline Semaphore 同步

Denoiser 持有 timeline semaphore（Vulkan 1.2 核心）。降噪帧的 submit 额外 signal 该 semaphore，后台线程 `vkWaitSemaphores` 纯 CPU 等待。选择 timeline semaphore 而非复用 per-frame fence：per-frame fence 在下一帧被 wait + reset，与后台线程竞态。

### Accumulation Generation

`uint32_t accumulation_generation_` 单调递增，每次 accumulation 重置时 +1。降噪触发时记录当前 generation，upload 时比对。不匹配 → 结果过期，丢弃。

### 状态机

```
Idle → ReadbackPending → Processing → UploadPending → Idle
```

### 线程模型

按需 `std::jthread`，每次降噪创建新线程，完成后自然结束。降噪频率极低（秒级），线程创建开销可忽略。

### 像素格式

- Accumulation（RGBA32F）→ `OIDN_FORMAT_FLOAT3`，pixelByteStride=16
- Aux Albedo/Normal（R16G16B16A16Sfloat）→ `OIDN_FORMAT_HALF3`，pixelByteStride=8
- 三种输入无需 CPU 端格式转换

### OIDNBuffer

OIDN 2.x GPU 模式要求使用 `OIDNBuffer` 对象传递图像数据。Denoiser 创建持久 `OIDNBuffer`（beauty + albedo + normal + output），Vulkan readback 后 `memcpy` 到 OIDNBuffer，执行后从 OIDNBuffer `memcpy` 到 upload staging buffer。

### Resize / Destroy 安全

`on_resize()`、`destroy()` join 后台线程后强制 `state_ → Idle`，丢弃任何 UploadPending 结果（避免旧尺寸 staging → 新尺寸 denoised buffer 尺寸不匹配）。场景加载前调用 `Denoiser::abort()`（join + Idle）。`oidnExecuteFilter()` 不可中断，最坏等待 20-50ms，发生在本身就阻塞的操作中，用户无感。

### 内存序

`state_` store 使用 `memory_order_release`，load 使用 `memory_order_acquire`，确保后台线程 staging buffer 写入对主线程可见。

### auto_denoise_interval 最小值

UI slider 最小值 ≥ 16（默认 64）。有 RT core 的显卡上 interval=16 约 21% GPU 占用。CPU fallback 场景 OIDN 执行极慢，被自身执行时间节流。

---

## 18. Reference View

### 累积策略

RGBA32F accumulation buffer，running average（每帧 1 SPP），持久跨帧。VP 矩阵变化立即重置。

### 降噪触发

全部满足才触发：`state == Idle` + `denoise_enabled` + `show_denoised` + `sample_count > 0`。

- **自动**：sample count 增量 >= interval（默认 64）
- **手动**：UI 按钮

### 输出显示

- **Show Denoised**（默认）：显示最近降噪结果，同时暂停所有降噪触发切回后恢复
- **Show Raw**：显示原始累积（带噪点）
- **denoise_enabled 关闭**：显示原始累积，进行中的降噪静默完成但不显示

### 后处理

仅 Tonemapping → Swapchain。不走其他后处理，参考视图目的是验证 PT 光照正确性。

---

## 19. PT 演进方向（参考）

> 以下为 main 分支的长期 PT 技术路线，记录为参考方向而非 reflector 确定规划。

### 实时 PT

ReSTIR DI（直接光，reservoir 时空重采样）+ SHaRC（间接光，空间哈希辐射度缓存）+ NRD（帧间 temporal 降噪）。

### 间接光升级

ReSTIR GI 替换 SHaRC——间接光从低频近似提升到像素级路径重采样。

### 神经网络渲染

NRC（Neural Radiance Cache）替代 SHaRC、DLSS Ray Reconstruction 替代 NRD、Cooperative Vectors 标准化 shader 内推理。

### 算法统一

ReSTIR PT（GRIS 框架）对完整光传输路径做重采样，统一替代 ReSTIR DI + GI。

---

## 20. Gaussian Splatting 数据管线

### 模块架构

| 模块 | 层级 | 职责 |
|------|------|------|
| `GaussianSplatLoader` | App | 从 glTF 加载 GS 数据到 CPU 端 SoA 结构 |
| `gltf_utils` | App | 共享 glTF 解析函数（`parse_gltf`、`transform_aabb`），供 SceneLoader 和 GaussianSplatLoader 共用 |
| `GaussianSplatScene` / `GaussianSplatPrimitive` | Framework | GS 数据结构定义（SoA 布局） |
| PLY 转换器 | Framework | PLY → .gltf 转换，仅供渲染器内部调用 |

### CPU 数据结构

SoA 布局。主流 GS 渲染实现（compute sort + tile-based rendering）按属性分 buffer 送入 GPU，SoA 是天然匹配的布局。GPU sort 操作排 `(distance_key, global_splat_index)` 对，通过 index 间接访问各属性 buffer。排序项物理存储为两个 32-bit uint；Radix sort 只对 32-bit distance_key 做分 digit 排序，global_splat_index 作为 payload 搬运。选择该格式的主要原因是性能：相比对 64-bit packed key 做 radix，32-bit key 的 radix pass 数约减半。`distance_key = floatBitsToUint(camera_distance_squared)`，仅对 finite non-negative float 使用；invalid entry sentinel 为 `{UINT_MAX, UINT_MAX}`，ascending sort 为 front-to-back。

Phase 3.0 渲染侧使用 GS 子系统独立的持久 Set 3 descriptor set 绑定 GPU buffers。Set 3 中包含两组 SSBO：一组是 scene load / bake 后上传的静态数据（world position、world covariance、opacity、SH、optional/reserved primitive metadata；完整 Wigner-D 完成后 SH 为 rotated SH），另一组是每帧由 GPU 改写的 work buffers（visible count、projected data、sort ping-pong、indirect draw command）。Descriptor 随 buffer 创建或重建写入；每帧只更新 buffer 内容。

GS work buffer 容量由当前场景派生：`sort_capacity = next_power_of_two(total_splat_count)`。这不是固定上限，场景切换时会随 splat 数重建。Bitonic 先按 sort capacity 全量排序并用 sentinel 填充 invalid entries；Radix 先实现同样的 capacity 路径用于验证，再实现 visible-count-driven radix sort，避免长期排序不可见 sentinel。

GPU work buffers 每帧先 reset：`visible_count = 0`，sort entries 全部填 invalid sentinel `{UINT_MAX, UINT_MAX}`，`indirect.instanceCount = 0`。Cull/project append valid entries 到 `sort_entries[0..visible_count)`；ascending sort 后 valid entries 位于前段，sentinel 位于尾部；draw instanceCount=`visible_count`，不 draw capacity。`VkDrawIndirectCommand` 的固定字段由 CPU 初始化（`vertexCount = 6`、`firstVertex = 0`、`firstInstance = 0`），GPU 只在 cull/project 后将 `visible_count` 写入 `instanceCount`。同步依赖必须覆盖 reset→cull/project、cull/project→sort、sort/projected data→graphics shader、visible_count→indirect update、indirect write→draw indirect。

Sort 必须对相同 `distance_key` 保持 deterministic ordering，否则半透明累积可能因相等 key 的帧间顺序变化而闪烁。Bitonic baseline compare 使用 lexicographic `(distance_key, global_splat_index)` 升序。后续 Radix sort 仍只处理 32-bit distance key 以保持性能目标，但实现前必须确认 equal-key deterministic ordering 方案。

Phase 3.0 projected data 按 global splat index dense 存储，sort entry payload 存 global splat index。Cull/project 对可见 splat 写 `projected_data[global_index]` 并写入 sort entry `{distance_key, global_index}`；不可见 splat 的 projected data 未定义。Draw 使用 `sorted_entries[gl_InstanceIndex].splat_index` 读取 `projected_data[global_index]`。Draw instance count 固定为 `visible_count`，不 draw `sort_capacity`。Dense projected data 约占 `64B × total_splat_count` capacity，1M baseline 可接受；10M 目标前需重新评估 compact projected data（visible-index dense）以降低 VRAM/cache 压力。

Projected data 只存 draw 阶段需要的投影后数据：`center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`（inverse 2D covariance xx/xy/yy）、`opacity`、SH-evaluated `rgb`。它不存原始 position / rotation / scale / world covariance / SH coefficients / sort key；这些分别属于 static baked buffers 或 sort entries。Projected data 内容依赖 viewport，但容量只依赖 `total_splat_count`，因此 swapchain resize 时不重建 projected data buffer，下一帧 cull/project 会用新 viewport 覆盖内容。

```
GaussianSplatPrimitive              // 单个 GS primitive
├── positions[]                     // vec3, local space
├── rotations[]                     // quat (xyzw), 椭球朝向
├── scales[]                        // vec3, 三轴缩放（线性正值）
├── opacities[]                     // float, 0~1
├── sh_coefs_0[]                    // vec3, degree 0（必选）
├── sh_coefs_1[3][]                 // vec3 ×3, degree 1（可选）
├── sh_coefs_2[5][]                 // vec3 ×5, degree 2（可选）
├── sh_coefs_3[7][]                 // vec3 ×7, degree 3（可选）
├── transform                       // mat4, node 世界变换
├── bounds                           // AABB, 从 positions 计算
└── metadata
    ├── kernel                       // "ellipse"
    ├── color_space                  // "srgb_rec709_display" / "lin_rec709_display"
    ├── projection                   // "perspective"
    ├── sorting_method               // "cameraDistance"
    ├── max_sh_degree                // 0-3
    └── splat_count

GaussianSplatScene                  // 场景级容器
├── vector<GaussianSplatPrimitive>  // 一个或多个 primitive（各自有独立 transform/metadata）
└── scene_bounds                    // 所有 primitive 的 AABB 并集
```

### 数据加载策略

| 策略 | 选择 | 理由 |
|------|------|------|
| Component type | 统一转 float | fastgltf `iterateAccessor` 内置类型转换，shader 端统一 float |
| SH 存储 | 按实际 degree 分配 | spec 保证同一 primitive 内 SH degree 统一，按需分配避免浪费 |
| Scale / Rotation | CPU scene data 保留原始值；Phase 3.0 GPU upload 使用 baked world covariance | 原始值用于 reload/debug/future rebake；GPU static baked buffers 不保留 raw rotation/scale |
| Node transform | CPU scene data 保留矩阵；Phase 3.0 upload-time bake 到 world position / world covariance | Phase 3.0 优先静态正确性路径，低频变更时重上传 |
| Color space | 原样保留 SH 系数 + 记录元数据；Phase 3.0 要求同一 GS scene 内所有 primitive colorSpace 一致 | SH 系数不能直接做 color space 转换，需先求值再转换；混合 colorSpace composition 留待后续 |

### GS 渲染色彩约定

KHR_gaussian_splatting 的 `colorSpace`、`kernel`、`projection`、`sortingMethod` 定义在 primitive extension 对象中，因此同一 asset 可以包含 metadata 不同的 GS primitive。Phase 3.0 不实现 per-primitive 分支管线，加载/上传前要求同一 GS scene 内所有 primitive metadata 一致：`kernel = ellipse`、`projection = perspective`、`sortingMethod = cameraDistance`、`colorSpace` 一致；不一致时报错。多个 primitive 在 upload 时拼接为连续 global splat buffers，并在 CPU 侧保留 per-primitive ranges / source primitive index / metadata 供 debug、error reporting 和未来 per-primitive behavior 使用。Phase 3.0 shader 按 global splat index 访问 baked SoA buffers，不按 primitive metadata 分支；GPU primitive metadata buffer 可选/预留。

GS composition 在 primitive colorSpace 中完成：`srgb_rec709_display` 的 SH 求值结果先按 sRGB display-referred 数值进行 alpha blend，blend 完成后的 composition target 仍存储 sRGB blended values，随后由 GS sRGB→linear conversion pass 输出 linear target；`lin_rec709_display` 直接在线性 display-referred 数值中 blend，composition target 已是 linear，可 bypass conversion pass 并直接作为 TonemappingPass 输入。SH 求值方向使用 KHR 定义的 camera → splat：`normalize(splat_world_position - camera_world_position)`。Phase 3.0 在 cull/project compute pass 中对每个可见 splat 求一次 SH RGB，并写入 projected data；fragment shader 不求 SH。Phase 3.1 可将 SH evaluation 拆成独立 post-cull compute pass，并与距离自适应 SH 截断配合。Phase 3.x 当前不规划 SH cache 或独立低阶 fallback。传给 TonemappingPass 的输入始终是 linear。TonemappingPass 只增加 GS bypass 开关以跳过 exposure / tonemap curve，不承担 GS sRGB decode。

未来如果需要支持混合 colorSpace，可采用近似方案：选择第一个 primitive 的 colorSpace 作为 scene composition space，将其他 primitive 的求值结果先转换到该空间后再 blend。该方案会偏离各 primitive 原生 colorSpace 下先 blend 后转换的严格语义，需单独评估后实现。

### Extension JSON 提取

fastgltf 的 `Primitive` 不暴露 extension JSON。使用 nlohmann/json 对 glTF JSON 做二次解析，提取 `kernel`、`colorSpace`、`projection`、`sortingMethod`。

对 .gltf 文件直接解析 JSON。对 .glb 文件需先提取 JSON chunk：

```
.glb 二进制布局：
偏移 0-3:   magic (0x46546C67 = "glTF")
偏移 4-7:   version (2)
偏移 8-11:  total length
偏移 12-15: chunk 0 length (JSON 数据字节数)
偏移 16-19: chunk 0 type (0x4E4F534A = "JSON")
偏移 20..20+chunk0Length-1: JSON 数据
（后续可选 BIN chunk，结构相同：8 字节 header + data）
```

提取步骤：跳过 12 字节 glb header → 读 4 字节 chunk length → 验证 4 字节 chunk type 为 JSON → 读 length 字节 JSON 数据 → nlohmann/json 解析。

### extensionsRequired 兼容

fastgltf 的 `extensionStrings` 表中没有 `KHR_gaussian_splatting`。当 glTF 文件声明 `"extensionsRequired": ["KHR_gaussian_splatting"]` 时，fastgltf 返回 `UnknownRequiredExtension` 拒绝解析。

GS loader 自身实现了 KHR_gaussian_splatting 支持（extension metadata 由 nlohmann/json 解析，attribute 数据由 fastgltf 按通用 accessor 读取），因此有权从 `extensionsRequired` 中移除该扩展名再交给 fastgltf。只移除 `KHR_gaussian_splatting`，不移除其他未知扩展。

处理流程：

1. nlohmann/json 解析原始 JSON（已有逻辑）
2. 检查 `extensionsRequired` 是否包含 `KHR_gaussian_splatting`
3. 若不包含：走 `gltf_utils::parse_gltf(path, ...)` 正常路径（零开销）
4. 若包含：从 JSON 副本的 `extensionsRequired` 中移除 `KHR_gaussian_splatting`，将消毒后的数据传给 fastgltf

.gltf 消毒：将修改后的 JSON dump 为字符串 → `GltfDataBuffer::FromBytes` → `parser.loadGltf`（外部 .bin 通过 `path.parent_path()` 解析）。

.glb 消毒：读取原始文件 → 用修改后的 JSON 重组内存 GLB → `GltfDataBuffer::FromBytes`。JSON chunk 之后的所有 chunk 原样保留（不假定只有一个 BIN chunk）。

重组规则：
- JSON chunk 需 4 字节对齐，padding 字符为空格（0x20）
- 重组后更新三个字段：GLB header 中的 total length、JSON chunk length、JSON chunk type
- 后续 chunk（从原始文件偏移 `12 + 8 + orig_json_chunk_length` 处开始）整体复制，不解析不修改

### 加载入口

渲染器有两个独立加载入口，各入口自行处理数据，两个场景可同时加载：

| 入口 | 行为 |
|------|------|
| PT 入口（SceneLoader） | 加载 mesh primitive；无 mesh 时保持现有行为（警告） |
| GS 入口（GaussianSplatLoader） | 加载 GS primitive；无 GS primitive 时警告 |

两个入口可以加载同一文件或不同文件。`.ply` 文件经 PLY 转换器转为缓存 .gltf 后由 GS 入口加载。

### UI 集成

DebugUI 的 Scene 面板下有两个独立文件选择器：

| 选择器 | Filter | 持久化字段 |
|--------|--------|-----------|
| PT Scene（现有） | `*.gltf;*.glb` | `config.scene_path` |
| GS Scene（新增） | `*.gltf;*.glb;*.ply` | `config.gs_scene_path` |

RenderMode 切换（PT / GS）与文件选择无关——两个场景可独立加载和卸载，RenderMode 仅控制每帧走哪条渲染路径。

### 相机初始化

加载新场景时根据当前活跃的 bounds 定位相机：

- 仅 PT 场景：使用 `SceneLoader::scene_bounds()`
- 仅 GS 场景：使用 `GaussianSplatScene::scene_bounds`
- 两者都有：使用最后加载/切换的那个场景的 bounds

### 错误处理

| 场景 | 处理 |
|------|------|
| PLY 缺少必要属性 | 报错，加载失败 |
| PLY 转换失败 | 不缓存，报错，加载失败 |
| glTF primitive `kernel` 非 `"ellipse"` | Warning，跳过该 primitive |
| glTF primitive 缺少必要 GS attribute | 报错，加载失败 |
| glTF 无任何 GS primitive | 报错，加载失败 |
| 缓存文件损坏（GaussianSplatLoader 加载缓存 .gltf 失败） | 删除缓存文件，重新转换 |

---

## 21. PLY 转换器

### 解析

tinyply（`third_party/tinyply/` 源码编译集成）。仅支持 INRIA 3DGS 格式（pre-sigmoid opacity、log-scale、wxyz 四元数）。其他训练框架的格式变体后续按需适配。

### 数据转换

| 步骤 | 操作 |
|------|------|
| 激活函数 | `opacity = sigmoid(raw_opacity)`，`scale = exp(raw_scale)` |
| 坐标系 | COLMAP → glTF：position `(x, -y, -z)`，Y/Z 取反 |
| 四元数 | wxyz → xyzw（glTF 顺序），同时 Y/Z 分量取反 |
| SH 系数 | 见下方 SH 坐标系翻转表 |

### SH 坐标系翻转规则

COLMAP→glTF 坐标变换 `(x, y, z) → (x, -y, -z)` 下，将替换代入每个 SH 基函数的方向因子（见 spec 附录 A），方向因子符号变化的系数需要取反。

**原理**：SH 系数存储的是训练空间（COLMAP）下的球谐展开。坐标系变换后，view direction 在新空间中的 y/z 分量符号相反。为使 SH 求值结果不变，需要翻转那些在 y→-y, z→-z 代入后改变符号的基函数对应的系数。

| Degree | Coef 索引 | 方向因子 | 代入 (x,-y,-z) 后 | 翻转 |
|--------|-----------|----------|-------------------|------|
| 0 | 0 | 常数 | 不变 | 否 |
| 1 | 0 (m=-1) | y | -y → 变号 | **是** |
| 1 | 1 (m=0) | z | -z → 变号 | **是** |
| 1 | 2 (m=1) | x | x → 不变 | 否 |
| 2 | 0 (m=-2) | xy | x·(-y) → 变号 | **是** |
| 2 | 1 (m=-1) | yz | (-y)·(-z) → 不变 | 否 |
| 2 | 2 (m=0) | 2z²-x²-y² | z²=z², y²=y² → 不变 | 否 |
| 2 | 3 (m=1) | xz | x·(-z) → 变号 | **是** |
| 2 | 4 (m=2) | x²-y² | y²=y² → 不变 | 否 |
| 3 | 0 (m=-3) | y(3x²-y²) | (-y)(3x²-y²) → 变号 | **是** |
| 3 | 1 (m=-2) | xyz | x·(-y)·(-z) → 不变 | 否 |
| 3 | 2 (m=-1) | y(4z²-x²-y²) | (-y)(4z²-x²-y²) → 变号 | **是** |
| 3 | 3 (m=0) | z(2z²-3x²-3y²) | (-z)(2z²-3x²-3y²) → 变号 | **是** |
| 3 | 4 (m=1) | x(4z²-x²-y²) | x(4z²-x²-y²) → 不变 | 否 |
| 3 | 5 (m=2) | z(x²-y²) | (-z)(x²-y²) → 变号 | **是** |
| 3 | 6 (m=3) | x(x²-3y²) | x(x²-3y²) → 不变 | 否 |

### 输出

nlohmann/json 手动构造 glTF JSON + binary buffer，输出 .gltf + .bin 文件。不使用 fastgltf exporter（无法注入自定义 extension JSON）。

### 缓存

复用 `cache.h` 基础设施。PLY 文件 XXH3_128 content hash 作为 key，缓存目录 `%TEMP%\himalaya\gaussians\`。

### CLI 转换模式

通过 CLI11（vcpkg）实现命令行参数解析。无参数启动 = 当前 GUI 渲染器；附加转换参数时作为 CLI PLY→glTF 转换器运行，不启动窗口和 Vulkan。

---

## 22. Gaussian Splatting 渲染方案（Phase 3 方向）

Phase 3 采用分阶段演进路线：先用硬件光栅完成正确性基线，再演进到 tile-based compute renderer。

### Phase 3.0：硬件光栅基础路径

基础路径为 compute cull/project/sort → indirect instanced quad draw → 硬件 alpha blend。Cull、projection、visible list、sort key 和排序结果构成后续阶段复用的上游管线；末端使用硬件光栅是为了降低首个可渲染版本的实现风险，优先验证数据 bake、投影、排序、颜色和 blend 的正确性。

Phase 3.0 是 correctness baseline，不以最终性能为目标。Happy path 为 KHR ellipse kernel + perspective projection + cameraDistance sorting + scene-level consistent metadata + identity/no-rotation transform；支持单个或多个 GS primitive，primitive 拼接为 global splat buffers 并保留 CPU per-primitive ranges；支持 `srgb_rec709_display` 和 `lin_rec709_display` 二选一 scene，不支持 mixed colorSpace scene。必须验证投影中心、3σ OBB、front-to-back 排序、premultiplied-under blend、sRGB→linear conversion、linear GS conversion bypass、TonemappingPass GS bypass、resize 后 targets/work buffers 重建、`visible_count = 0` 空可见集和非法 asset rejection。Phase 3.0 前期明确不支持 mixed metadata/colorSpace、非 identity SH rotation、compact projected data、tile-based renderer、10M 性能目标、max-channel range compression、background/mesh/skybox 合成。

Phase 3.0 使用 upload-time bake：node transform 在上传时烘焙进 world-space position 和 world-space covariance；SH rotation 前期先支持 identity/no-rotation happy path，非 identity transform rotation 不静默渲染错误，完整 Wigner-D degree 1-3 rotation 放到 Phase 3.0 末期补齐。GPU 侧核心属性因此不再以 rotation / scale 为主，而是以 bake 后的 position、covariance、opacity、SH 和 per-primitive range / metadata 为主。

KHR `SCALE` 表示 Gaussian principal axes 的标准差 σ，因此 local covariance 使用 `scale²`：`Σ_local = R * diag(scale²) * Rᵀ`。Upload-time bake 用 glTF node global transform 的线性部分 `M3x3` 得到 `Σ_world = M3x3 * Σ_local * M3x3ᵀ`，position 使用完整 global transform 变换到 world space。GS static baked GPU buffers 存储 world position、world covariance、opacity、SH 和 optional/reserved primitive metadata，不存储原始 rotation/scale；原始 rotation/scale 可继续保留在 CPU 侧用于 reload、debug 或 future rebake。

Upload bake 同时预计算 per-splat `world_radius_3sigma` 作为 frustum cull sphere 半径。Phase 3.0 使用保守 trace bound：`radius = 3 * sqrt(max(trace(Σ_world), 0))`。该 bound 满足 `max_eigenvalue(Σ_world) <= trace(Σ_world)`，因此不会因半径过小误剔除；代价是细长 splat 的 sphere 偏保守。Cull 使用 world-space sphere(center=`position_world`, radius=`world_radius_3sigma`) vs frustum planes。Projected OBB giant discard 是独立的 screen-space safety check，不替代 world-space frustum cull。未来可用 max eigenvalue 得到更紧半径。

直接加载 glTF/GLB 时，`OPACITY` 必须 finite 且在 [0,1]，`SCALE` 必须 finite 且所有分量 >= 0，`ROTATION` 必须 finite 且为 unit quaternion。非法数据按 KHR 规范报错，不静默 clamp 或 normalize。KHR 还要求 GS node transform 可分解为 regular translation、proper rotation 和 positive scale；Phase 3.0 对包含 reflection / negative determinant 或不可分解线性部分的 global transform 报错或跳过上传，避免 covariance 与 SH rotation 在非 proper rotation 情况下静默错误。

Cull/Project 输出使用 screen pixel space：projected center、2D covariance、OBB axes/extents 都以 pixel 为单位。Screen pixel space 使用 Vulkan framebuffer 坐标：top-left origin、x right、y down。GS draw pass 使用 positive-height normal viewport，不使用 negative viewport Y-flip。Vertex shader 从 pixel-space projected data 展开 oriented quad corners，再转换为 NDC，pixel→NDC 不做 Y flip；fragment shader 直接使用 `gl_FragCoord.xy - center_px` 与 pixel-space inverse covariance / conic coefficients 计算高斯 alpha 衰减。

GS 使用与 PT/reference view 相同的 camera pose、FOV、aspect 和 viewport，不引入独立相机模型；可使用 GS-specific near plane 保持投影稳定。`center_px` 由 clip/NDC 转 pixel 得到，不做 Y flip。2D covariance 使用 view-space covariance 和 pixel focal length：`fx = 0.5 * width * abs(proj[0][0])`、`fy = 0.5 * height * abs(proj[1][1])`，`cov_2d = J * cov_view * Jᵀ`，从而保证 covariance、OBB extents、conic 和 `gl_FragCoord.xy` 处于同一 pixel coordinate system。

Projected data 逻辑字段为 `center_px`、`axis0_extent_px`、`axis1_extent_px`、`conic`（inverse covariance xx/xy/yy）、`opacity`、`rgb`；实际 GPU struct 按 std430 / vec4 packing 实现。

Fragment alpha 防御规则：`power = -0.5 * mahalanobis_distance`，`power < -20` 时 discard；`alpha = clamp(opacity * exp(power), 0, 1)`；`alpha < 1e-4` 时 discard。3σ cutoff 边界的 power 约为 -4.5，因此 -20 只裁掉极低贡献或异常 fragment。glTF 直接加载阶段按 KHR 校验 opacity，shader 侧 clamp 仅作为防御。

GS near plane 初始为 scene AABB diagonal × 0.005，仅 GS 模式使用。贴脸 splat 在渲染上无意义且可能导致巨大 overdraw，因此 behind-camera、near-plane 不稳定或 projected OBB 过大的 splat 可以直接 discard。Projection z clamp 只用于防止 Jacobian / covariance projection 中出现 NaN/Inf，不用于强行保留贴脸 splat。Projected OBB 任一半轴超过 screen short side × 0.25 时 discard，不做 extent clamp。

GS composition target 和 GS linear target 均存储 accumulated premultiplied RGB 与 accumulated alpha。sRGB→linear conversion pass 只转换 RGB 通道，alpha 原样保留。Phase 3.0 不做 unpremultiply，不做背景合成；alpha 仅用于 GS 内部 front-to-back under 累积。TonemappingPass 在 GS 模式下忽略 alpha，最终 swapchain 输出 opaque alpha = 1。

SH 求值后的 RGB 负分量必须在 premultiply 前 clamp 到 0，符合 KHR 对 negative color 的要求。Phase 3.0 不做 per-splat upper clamp，避免改变 splat 累积结果。最终 GS 输出采用 per-channel hard clamp 到 [0,1]，作为 KHR 允许的 clamped output；GS 模式下 TonemappingPass bypass exposure / tonemap curve，仅执行 linear passthrough + hard clamp + alpha=1。`srgb_rec709_display` 的 conversion pass 在 sRGB decode 前对 composed sRGB RGB 做 [0,1] hard clamp。未来如需更好保 hue，可增加 max-channel range compression 作为 GS display-referred tonemapping 选项。

TonemappingPass 保留为最终 swapchain output pass，通过 push constant `mode` 区分 `HdrAces` 与 `LinearClamp`。PT 使用 `HdrAces`：linear HDR → exposure → ACES；GS 使用 `LinearClamp`：linear display-referred input → per-channel hard clamp [0,1] → alpha=1。Mode 是 TonemappingPass 局部输出策略，不放入 GlobalUBO，避免改变全局 std140 layout 和影响其他 shader；也不新增 pipeline，单个 uniform branch 成本可忽略。TonemappingPass 不做 GS sRGB decode，GS 进入 TonemappingPass 前必须已经是 linear。

Scene load/reload 重建 static baked buffers（world position/covariance/radius/opacity/SH/optional metadata）和 capacity-based work buffers（visible count、projected data、sort ping-pong、indirect command），并重写 GS Set 3。Swapchain resize 只重建 viewport-sized GS composition/linear targets，并更新对应 render target descriptors。Projected data/work buffers 容量只依赖 `total_splat_count`，resize 时不重建；其内容虽依赖 viewport，但下一帧 cull/project 会覆盖。Reload/resize 后不得有 descriptor 指向已销毁的 buffers/images。

现有 RenderGraph 只自动处理 image barriers，buffer resource usage 在 compile 阶段会跳过。Phase 3.0 接入 GS compute/sort/draw 前需要扩展 RG buffer barrier 支持：track per-buffer last stage/access，emit `VkBufferMemoryBarrier2`，并补齐 GS 所需 stage/access 映射（Compute SSBO read/write、Vertex/Fragment SSBO read、DrawIndirect read、Transfer read/write）。优先扩展 RG，不在 GS pass 内长期手写 barriers。

GS 复用 GlobalUBO 中的 view、projection、view_projection、camera_position、screen_size，不在 GS push constants 中重复矩阵。GS 专用 per-frame 小参数放入 `GSPushConstants`：`total_splat_count`、`sort_capacity`、`color_space`、`flags`、`near_gs`、`max_projected_extent_px`、`alpha_discard_threshold`、`power_discard_threshold`。`max_projected_extent_px` 通常为 screen short side × 0.25，discard thresholds 初始为 alpha=1e-4、power=-20。Tonemapping mode 是 TonemappingPass 独立 push constant，不属于 GS push constants。

硬件光栅末端采用 front-to-back premultiplied-under blend。Fragment shader 输出 premultiplied color：`vec4(rgb * alpha, alpha)`。Color 与 alpha 的 blend state 相同：`srcColorBlendFactor/srcAlphaBlendFactor = ONE_MINUS_DST_ALPHA`，`dstColorBlendFactor/dstAlphaBlendFactor = ONE`，`blendOp = ADD`，`colorWriteMask = RGBA`。GS composition target 格式为 R16G16B16A16Sfloat，`loadOp = CLEAR` 且 clear value 为 `vec4(0,0,0,0)`，`storeOp = STORE`。Graphics pipeline 禁用 depth test/write，cull mode 为 none。

### Phase 3.1 / 3.2：硬件光栅路径优化

Phase 3.1 优化每帧运行时开销，例如 SH 求值分离、多级剔除、buffer 热/冷/暖分离和距离自适应 SH 截断。

Phase 3.2 优化加载时和上传后的数据效率。由于 Phase 3.0 已经 bake transform，量化对象应是实际 GPU buffer，例如 world-space covariance、opacity、SH，以及可能结合空间分块后的 position 局部表示。原始 rotation / scale 可继续作为 CPU 侧源数据保留，但不再是 Phase 3.2 GPU 上传量化的主要对象。具体量化格式、误差预算、是否保留某些属性为 FP32，均在 Phase 3.2 开始前重新讨论决定。

### Phase 3.5：Tile-Based Compute Renderer

在正确性基线稳定后，将末端从 instanced quad draw 替换为 tile binning + per-tile compute blend。上游 cull/project/sort 管线保持复用，tile shader 在 shared memory 中按前到后顺序累积颜色，并在 transmittance 足够低时 early-out。

### 选型理由

- **降低首个版本风险**：硬件光栅路径能更快得到可观察画面，便于验证数据、投影、排序和色彩问题。
- **保留演进空间**：上游 compute cull/project/sort 与 tile-based compute renderer 兼容，后续替换末端即可演进。
- **获取最终性能能力**：tile-based compute 提供 per-tile early-out 和 shared memory 协作加载，适合密集重叠 GS 场景。
