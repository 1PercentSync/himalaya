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

## 20. Gaussian Splatting 管线重大决策

### 数据入口与职责边界

Gaussian Splatting 使用独立的数据入口和渲染路径：`GaussianSplatLoader` 负责从 glTF/GLB 读取 KHR_gaussian_splatting primitive，PLY 转换器负责把内部支持的 PLY 格式转换为 glTF 缓存后再走统一 loader。GS 与 PT scene 可以独立加载，`RenderMode` 决定每帧执行 PT 还是 GS 路径。

CPU 侧 GS 数据保持 SoA 布局，匹配后续 GPU 端按属性分 buffer 的访问模式。PLY 转换、extension JSON 提取、`extensionsRequired` 兼容等属于 GS 数据管线实现细节，记录在当前阶段文档或对应源码注释中，不作为全局技术决策展开。

### Phase 3 渲染路线

Phase 3 采用“先硬件光栅、后 tile-based compute”的演进路线：

1. Phase 3.0 先实现 compute cull/project/sort → indirect instanced quad draw → hardware blend 的正确性基线。
2. 正确性稳定后，Phase 3.5 将末端替换为 tile binning + per-tile compute blend，以获得 per-tile early-out 和更好的密集重叠性能。

该路线的核心决策是复用上游 cull/project/sort 管线，只替换渲染末端，从而降低首个可渲染版本风险，同时保留最终性能优化空间。

### GPU 数据模型

Phase 3.0 使用 upload-time bake：node transform 在上传时烘焙进 world-space position 和 world-space covariance。GPU 核心属性以 bake 后的数据为主，避免在每帧 shader 中重复处理静态 transform。

GS 使用与 PT 路径分离的资源绑定模型，绑定 static scene data 和 per-frame work data。容量由当前 GS scene 派生并随场景重建，不设置固定 splat 上限；容量不足时加载/渲染失败并报错，不静默截断。

### 排序与索引

GS 使用 front-to-back 排序，排序依据为 KHR `cameraDistance` 对应的 camera distance。排序实现必须保持相同 key 的 deterministic ordering，避免半透明累积因帧间顺序变化而闪烁。

具体 sort entry 编码、capacity 策略和 Bitonic/Radix 演进计划属于 Phase 3 文档范围。

### 色彩与输出

GS composition 在 KHR primitive colorSpace 中完成。进入 TonemappingPass 前，GS 管线必须已经输出 linear input；TonemappingPass 不负责 GS sRGB decode。

TonemappingPass 保留为最终 swapchain output pass，通过 push constant mode 区分 PT 的 `HdrAces` 和 GS 的 `LinearClamp`。PT path 执行 exposure + ACES；GS path 对 linear display-referred input 做 per-channel clamp 并输出 opaque alpha。

### RenderGraph 同步

GS 引入大量 compute/graphics buffer hazard，因此 RenderGraph 必须从 image-only barriers 扩展到 buffer barriers。长期方案是在 RG compile 阶段跟踪 per-buffer stage/access 并生成 `VkBufferMemoryBarrier2`，而不是在 GS pass 内长期手写 barrier。

### 阶段边界

- Phase 3.0 只要求 correctness baseline，重点是正确的数据 bake、排序、blend 和输出。
- Phase 3.1 聚焦运行时优化，例如拆分 SH 求值、多级剔除和 buffer 分层。
- Phase 3.2 聚焦加载时和上传后数据效率优化，例如 chunk、bake 后 GPU 数据量化和 Morton 排序；具体量化格式在 Phase 3.2 开始前重新讨论。
- Phase 3.5 及之后聚焦 tile-based compute、LOD、抗锯齿和 10M 级别交互性能。

详细的 Phase 3.x 路线和当前 Phase 3.0 实现约定分别记录在 `docs/phase3-decisions.md` 与 `docs/current-phase.md`。
