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

SoA 布局。主流 GS 渲染实现（compute sort + tile-based rendering）按属性分 buffer 送入 GPU，SoA 是天然匹配的布局。GPU sort 操作排 `(depth_key, index)` 对，通过 index 间接访问各属性 buffer。

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
| Scale / Rotation | 保留原始值 | 不预计算 covariance，保留运行时变换能力 |
| Node transform | 保留矩阵，GPU 实时应用 | 保留运行时移动/变换能力 |
| Color space | 原样保留 SH 系数 + 记录元数据 | SH 系数不能直接做 color space 转换，需先求值再转换 |

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

## 22. Gaussian Splatting 渲染

### 渲染管线架构

**Compute Tile-Based Rendering**（纯 compute 软光栅）。选型理由：

- **Early-out**：per-tile 前到后遍历，transmittance 趋近零时跳过剩余 splat，避免密集场景的大量无效计算
- **全 compute 控制力强**：排序、culling、blending 全部可定制
- 3DGS 领域事实标准方案，有充分的开源实现和文献资料

### 四阶段 Compute 管线

```
Projection + Culling → Radix Sort → Tile Binning → Tile Rendering
```

| 阶段 | 输入 | 输出 |
|------|------|------|
| Projection + Culling | 原始 splat 数据（GPU buffer） | 可见 splat 的 2D 属性 + RGB + depth key |
| Radix Sort | depth key + splat index | 按深度排序的 key-value 对 |
| Tile Binning | 排序后的 splat + 2D 覆盖范围 | per-tile (offset, count) 表 |
| Tile Rendering | per-tile splat 列表 + 2D 属性 + RGB | color buffer 像素输出 |

四阶段录制在同一个 RG pass 的 execute lambda 内，阶段之间手动插入 `vkCmdPipelineBarrier2`（COMPUTE → COMPUTE buffer memory barrier）。RenderGraph 只处理跨 pass 的 image barrier，pass 内的 buffer 同步由 pass 自行管理。

### 可见 Splat 数量

投影 pass 剔除不可见 splat 后，可见数量是动态的。投影 shader 使用 atomic counter 写入可见 splat 数。indirect dispatch buffer（`VkDispatchIndirectCommand`）由 GsProjectionPass 在 Step 3 创建，但不在 Step 3 填充；填充由 sort pass（Step 4）开头的一个小 compute dispatch 完成——读取 counter 值并转换为 dispatch struct。counter buffer 和 indirect dispatch buffer 均由 GsProjectionPass 持有。

### Shader 文件组织

GS shader 文件位于 `shaders/gs/` 目录：

| 文件 | 用途 |
|------|------|
| `gs_projection.comp` | 投影 + 剔除 + SH 求值 + Mip Splatting |
| `gs_sort_histogram.comp` | Radix sort 按 digit 统计直方图 |
| `gs_sort_scan.comp` | Prefix sum（scan） |
| `gs_sort_scatter.comp` | Radix sort scatter |
| `gs_tile_count.comp` | Per-tile splat 计数 |
| `gs_tile_scatter.comp` | Scatter splat index 到 per-tile 区域 |
| `gs_tile_render.comp` | Tile rendering（alpha blend + early termination） |

### GPU 排序

自行实现 GPU Radix Sort。32-bit key（depth）+ 32-bit value（index）分离方案。

| 特性 | 选择 | 理由 |
|------|------|------|
| 算法 | Radix Sort | 百万级 splat 下性能可预测，O(n·k) |
| Key 位宽 | 32-bit depth + 32-bit value 分离 | 4 个 pass（每 pass 8 bit），depth 精度无损，index 范围无限制 |
| 替代方案排除 | 64-bit key 需 8 pass，带宽翻倍；32-bit 混合 key 的 index 范围受限 | — |

Tile ID 不编码进排序 key（高分辨率下与 depth 共享 32 bit 精度不足），通过独立的 Tile Binning 阶段建立 per-tile 列表。

#### Depth Key 编码

排序 key = camera distance（splat 中心到相机位置的欧氏距离）的 float bits 重解释为 uint32。IEEE 754 正浮点数的 bit 表示保持单调序，camera distance 恒正，因此 `floatBitsToUint(distance)` 直接作为排序 key，升序排列 = 前到后。

#### 排序 Buffer

Key 和 value 各需两个 buffer（ping-pong 交替读写）。每 pass 从源 buffer 读取，按 digit scatter 到目标 buffer，下一 pass 交换源和目标。4 pass 后结果在目标 buffer 中。

```
key_buffers[2]:   uint32[] × 2    // ping-pong
value_buffers[2]: uint32[] × 2    // ping-pong
histogram_buffer: uint32[]        // per-digit per-block 计数
```

### Tile Binning

全局深度排序后，binning 阶段将每个 splat 映射到其覆盖的 tile：

1. 第一遍：统计每个 tile 的 splat 数量（atomicAdd）
2. Prefix sum：计算 per-tile 偏移
3. 第二遍：按偏移将 splat index scatter 到 per-tile 区域

由于遍历全局有序数组，每个 tile 内的 splat 自然保持深度有序。

### Tile 大小

16×16（256 threads/workgroup）。原始 3DGS 论文和主流实现的标准选择，GPU occupancy 良好。

### Early Termination

Tile rendering 中，当像素的累积 transmittance 低于阈值时跳过该像素剩余 splat。阈值 = `1.0 / 255.0`（≈ 0.004），低于此值的贡献在 8-bit 输出中不可见。

### SH 求值

投影阶段 per-splat 一次。用 `normalize(camera_pos - splat_center)` 作为方向求值 SH，结果 RGB 写入中间 buffer。后续 tile rendering 只读 RGB。

SH 低频特性决定 per-splat 一次求值在视觉上几乎无差别，性能优势显著（尤其 SH degree 3 时 192 bytes 系数只读一次）。

### 多 Primitive 处理

核心属性合并 + SH 按 degree 分组 dispatch。

- 各 primitive 的 `GaussianSplatCore`（position/rotation/scale/opacity）合并为一个大 GPU buffer，position 上传时应用各自的 node transform 转到世界空间
- SH 系数按 degree 分组：同 degree 的 primitive 拼接成一个 SH buffer，投影 pass 按组 dispatch（push constant 传入 `splat_offset, splat_count, sh_degree`）。`splat_offset` 是 merged core/output buffer 的全局偏移；SH buffer 组内从 0 开始按 local index 读取。
- 投影结果（2D 属性 + RGB）写入统一输出 buffer，同时写入 depth key/value buffer，后续 sort → binning → rendering 不关心 SH degree

spec 保证同一 primitive 内 SH degree 统一（SH 由 primitive 的 attribute 定义），不同 primitive 之间可以有不同 degree。

### GPU 数据布局

混合方案：核心属性打包 + SH 独立。

**CPU 端数据结构同步修改**：`GaussianSplatPrimitive` 中原有的 4 个独立 vector（positions/rotations/scales/opacities）替换为 `vector<GaussianSplatCore>`。SH 系数保持独立 vector 不变。上传时核心属性直接 memcpy，SH 按 degree 分组 memcpy。

```
GaussianSplatCore {              // 打包 struct，GPU 直接映射（std430）
    vec3  position;              // offset 0,  12 bytes
    float _pad0;                 // offset 12, 4 bytes（vec4 对齐 padding）
    vec4  rotation;              // offset 16, 16 bytes
    vec3  scale;                 // offset 32, 12 bytes
    float opacity;               // offset 44, 4 bytes
};                               // = 48 bytes
```

std430 规则下 `vec4 rotation` 需 16 字节对齐，`position`（12 bytes）之后插入 4 字节 padding。C++ 端使用 `alignas(16)` 或显式 padding 字段，并添加 `static_assert(sizeof(GaussianSplatCore) == 48)` + `offsetof` 校验。

Loader 适配：fastgltf `iterateAccessor` 按属性逐一读取，先读入临时 vector，再循环交织填入 `GaussianSplatCore` 数组。

#### SH GPU Buffer 布局

SH 系数按 degree 分组，每组一个 SSBO。同一 degree 的所有 primitive 的 SH 数据拼接成连续数组。

```
SH Buffer (degree N):
  splat[0].sh_coef_0  (vec3)    // degree 0: 1 组, degree 1: 3 组, ...
  splat[0].sh_coef_1  (vec3)    // (仅 degree ≥ 1)
  ...
  splat[1].sh_coef_0  (vec3)
  ...
```

每个 splat 的 SH 系数在 buffer 内连续排列，stride = `coefs_per_degree × sizeof(vec3)`。投影 shader 通过 `splat_index × stride` 索引。各 degree 的系数数量：

| Degree | 系数组数（累计） | Stride（bytes） |
|--------|-----------------|----------------|
| 0 | 1 | 12 |
| 1 | 4 (1+3) | 48 |
| 2 | 9 (1+3+5) | 108 |
| 3 | 16 (1+3+5+7) | 192 |

#### 中间 Buffer 数据布局

投影 pass 输出每个可见 splat 的 2D 渲染数据：

```
GSSplatData2D {                  // 投影输出，per visible splat（std430，64 bytes）
    vec2  center;                // offset 0,  8 bytes — 屏幕空间中心（像素坐标）
    vec2  axis_u;                // offset 8,  8 bytes — 2D 椭圆主轴 u（含长度）
    vec2  axis_v;                // offset 16, 8 bytes — 2D 椭圆主轴 v（含长度）
    // 8 bytes padding          // vec3 alignment=16 → color 必须在 offset 32
    vec3  color;                 // offset 32, 12 bytes — SH 求值后 RGB
    float alpha;                 // offset 44, 4 bytes — opacity × 椭球中心 Gaussian 值
    uvec2 tile_min;              // offset 48, 8 bytes — 覆盖 tile 最小索引 (x, y)
    uvec2 tile_max;              // offset 56, 8 bytes — 覆盖 tile 最大索引 (x, y)
};                               // = 64 bytes, alignment = 16
```

2D covariance 矩阵分解为两个主轴向量（含长度），避免渲染阶段重新做特征值分解。Tile 覆盖范围用于 binning 阶段的 tile 分配。

GLSL 中 `tile_min` / `tile_max` 使用 `uvec2`（非 4 个独立 `uint`），节省 offset 计算且与 tile binning 阶段索引方式一致。64 bytes/splat，百万 splat = 64 MB，在显存预算内。

C++ 端结构体与 `static_assert` 定义在 `framework/include/himalaya/framework/gaussian_splat_data.h`，与 `GaussianSplatCore` 同级。投影输出 buffer 由 GsProjectionPass 持有（含 `GSSplatData2D` SSBO、depth key/value buffer、counter buffer + indirect dispatch buffer），与 ReferenceViewPass 持有 accumulation 资源模式一致。

Projection pass dispatch 每 workgroup 256 个 splat（`ceil(total_splat_count / 256)`），与后续 sort 的 workgroup size 独立。

Atomic counter 写入由 projection shader 完成；indirect dispatch buffer（`VkDispatchIndirectCommand`）由 GsProjectionPass 创建并持有，但填充留到 Step 4（sort 编排开头加一个小 compute dispatch 把 counter 值转换为 dispatch struct）。两个 buffer 均由 GsProjectionPass 持有。

Tile binning 输出：

```
tile_offsets[]:  uint32[tile_count]    // 每个 tile 在 tile_splat_ids 中的起始偏移
tile_counts[]:   uint32[tile_count]    // 每个 tile 的 splat 数量
tile_splat_ids[]: uint32[]             // 所有 tile 的 splat index 列表（拼接）
```

Tile 索引 = `tile_y * tile_count_x + tile_x`。

### 颜色空间处理

GS 渲染侧原样输出 SH 求值结果，不做颜色空间转换。gamma 转换由 PresentPass 通过 swapchain image view 格式切换控制（见第 23 节）。

- `srgb_rec709_display`：SH 结果已是 sRGB 非线性值，PresentPass 用 UNORM view 直通输出
- `lin_rec709_display`：SH 结果是线性值，PresentPass 用 SRGB view 输出（硬件自动 linear → sRGB）

### 抗锯齿

直接实现 Mip Splatting（Yu et al. 2024）。两部分改动均在投影 compute shader 中：

1. **3D 频率限制**：根据 splat 到相机距离和像素大小计算采样率，将 scale 各轴 clamp 到最小阈值，防止 sub-pixel Gaussian 产生锯齿

   ```
   pixel_size_at_splat = (2 * tan(fov/2) * distance) / screen_height
   min_threshold = pixel_size_at_splat * sqrt(1 / (2 * ln(2)))
   scale_filtered = max(scale, min_threshold)   // 每轴独立 clamp
   ```

2. **2D Mip Filter**：投影得到 2D covariance 后，加各向同性滤波核

   ```
   cov2d_filtered = cov2d + 0.3 * I   // I = 2×2 单位矩阵
   ```

   `filter_size = 0.3` 是原始 3DGS 实现使用的值。此加法同时提供数值正则化——防止极端视角下 covariance 投影产生近奇异矩阵。

不需要新 pass、新 buffer、新数据结构，改动约 10-20 行 shader 代码。

### 输出目标

GS 渲染写入独立的 RG managed image（R16G16B16A16F，`VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`，屏幕尺寸）。PT 路径有自己的 accumulation buffer（RGBA32F），两者不共享。PresentPass 根据 RenderMode 读取对应 buffer。

Renderer 始终创建 GS color buffer（与 PT accumulation 同为 managed image），不依赖 GS 场景是否加载。背景色为纯黑（`vec4(0)`），tile rendering pass 在混合前初始化。

### Descriptor 绑定

Push Descriptor（Set 3），与现有 compute/RT pass 模式一致。GS compute pass 同时绑定 Set 0（GlobalUBO 读取相机矩阵和位置），通过现有 `bind_compute_descriptor_sets()` 完成。Push Descriptor 用于 GS 专用 buffer。

### Push Constant 布局

| Pass | 字段 | 说明 |
|------|------|------|
| Projection | `splat_offset`, `splat_count`, `sh_degree`, `screen_width`, `screen_height` | 按 SH degree 分组 dispatch |
| Sort | `element_count`, `pass_index` | 哪个 8-bit 段 |
| Tile Binning | `visible_count`, `tile_count_x`, `tile_count_y` | tile 网格尺寸 |
| Tile Rendering | `tile_count_x`, `tile_count_y` | tile 网格尺寸 |

相机矩阵、位置、near/far plane 从 GlobalUBO（Set 0 binding 0）读取，不占 push constant 空间。

### Resize 处理

GS GPU 资源分两类：

| 类型 | 随什么变化 | Resize 行为 |
|------|-----------|-------------|
| 场景数据 buffer（core attributes, SH） | splat 数量 | 场景加载时创建/销毁，resize 不影响 |
| 中间 buffer（projection output, sort, tile） | splat 数量 + 屏幕尺寸 | splat 数量决定大小的在场景加载时分配；tile 相关 buffer（tile_offsets, tile_counts, tile_splat_ids）在 resize 时按新 tile 数量重建 |
| GS color buffer | 屏幕尺寸 | RG managed image，resize 自动重建 |

GS pass 类需要实现 `on_resize()` 处理 tile 数量变化。

### RenderMode 流转

`Application` 中的 `pt_mode_` bool 替换为 `RenderMode render_mode_` 枚举。RenderInput 新增 `render_mode` 字段。Renderer::render() 根据 `input.render_mode` 分发到 `render_path_tracing()` 或 `render_gaussian_splatting()`。

- GS 模式但无 GS 场景：走 `render_imgui_only()`（与 PT 无场景时行为一致）
- PT 模式但无 RT 支持：走 `render_imgui_only()`（保持现有行为）
- DebugUI 的 RenderMode 下拉菜单控制切换

---

## 23. PresentPass

### 重构背景

原 TonemappingPass 承担 HDR buffer → swapchain 的最终输出。GS 引入后，渲染结果不一定是 HDR 线性值——GS 输出 display-referred 颜色，不需要 tonemapping。重命名为 PresentPass，职责泛化为"根据渲染模式将 color buffer 准备为 swapchain 可输出的格式"。

### 模式分支

通过 push constant 传入 `uint mode`，shader 根据 mode 选择处理路径：

| 模式 | 来源 | 处理 |
|------|------|------|
| PT | 线性 HDR | exposure 调节 + ACES tone curve |
| GS | display-referred | passthrough（直接输出） |

### 硬件 Gamma 控制

Swapchain image 创建两个 VkImageView：

| View | 格式 | 效果 | 使用场景 |
|------|------|------|---------|
| SRGB view | `VK_FORMAT_B8G8R8A8_SRGB` | 硬件自动 linear → sRGB | PT 输出 / GS `lin_rec709_display` |
| UNORM view | `VK_FORMAT_B8G8R8A8_UNORM` | 直通，不做 gamma | GS `srgb_rec709_display` |

PresentPass 在 `record()` 时根据 RenderMode 和 color_space 元数据选择 view 作为 color attachment。shader 代码无需关心 gamma 转换。

#### Swapchain Mutable Format 前置条件

创建 UNORM view 要求 swapchain 支持格式重解释。Swapchain 创建时需要：

1. `VkSwapchainCreateInfoKHR::flags` 设置 `VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR`
2. 通过 `VkImageFormatListCreateInfo` pNext 声明兼容格式列表（SRGB + UNORM）

这是 RHI 层（Swapchain）的改动。

#### GsColorSpace 枚举

定义在 `scene_data.h`，与 `RenderMode` 同级：

```cpp
enum class GsColorSpace : uint8_t {
    Unknown,              // PT / no GS loaded
    SrgbRec709Display,    // srgb_rec709_display → UNORM view
    LinRec709Display,     // lin_rec709_display → SRGB view
};
```

### UNORM ImageView 管理

UNORM views 存储在 `Swapchain` 类中（`unorm_image_views`），与 `image_views`（SRGB）平行管理。同一批 VkImage、同一生命周期，随 swapchain 创建/销毁。PresentPass 通过 swapchain 指针引用。

### View 选择逻辑（C++ 侧）

`FrameContext` 新增 `render_mode`、`image_index`、`gs_color_space` 字段。PresentPass::record() 根据两者组合选择：

| RenderMode | GsColorSpace | 使用的 View |
|------------|-------------|------------|
| PathTracing | 任意 | SRGB |
| GaussianSplatting | SrgbRec709Display | UNORM |
| GaussianSplatting | LinRec709Display | SRGB |

Step 1 暂不改 Application/RenderInput，FrameContext 中填写默认值（PathTracing / Unknown），完整流转留待 Step 6。

### DebugUI 控制

GS 相关的 DebugUI 控制项：

| 控制 | 说明 |
|------|------|
| RenderMode 下拉菜单 | PathTracing / GaussianSplatting 切换 |
| Splat Count 显示 | 当前 GS 场景的总 splat 数（只读） |
| Visible Splat Count 显示 | 当前帧可见 splat 数（GPU readback，只读） |
