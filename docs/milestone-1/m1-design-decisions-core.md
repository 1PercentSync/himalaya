# M1 设计决策：核心参考

> 从各阶段设计决策中提炼的、对日后开发仍有参考价值的决策结果。
> 只记「选了什么 + 关键参数 + 升级路径」，不记候选方案和排除理由。
> 完整决策过程见各阶段归档文档（`archive/m1-phase*-decisions.md`）和当前阶段文档（`m1-phase5-decisions.md`）。
> 接口定义见 `m1-interfaces.md`，帧流程见 `m1-frame-flow.md`。

---

## 设计原则

既不过度设计（避免在项目初期花太多时间在框架上），也不欠缺考虑（避免以后需要推翻重来）。每个组件选择的实现级别都预留了向长远目标演进的通道。

---

## 基础架构

### Vulkan 1.4 核心特性

| 特性 | 用途 |
|------|------|
| Dynamic Rendering | 替代 VkRenderPass / VkFramebuffer，与 RG 天然契合 |
| Synchronization2 | 更清晰的 barrier API |
| Extended Dynamic State | viewport/scissor/cull mode/depth 等动态设置，减少 pipeline 数量 |
| Descriptor Indexing | Bindless 纹理支持 |

### 资源句柄

Generation-based（index + generation）。资源销毁时 generation 递增，使用时比对，捕获所有 use-after-free。

**Pipeline 例外**：Pipeline 不纳入资源池——所有权始终单一明确（pass 持有），由 Validation Layer 兜底。

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
- 所有 depth 相关 pass 统一此约定

---

## Render Graph

### 设计模式

手动编排 Pass 列表 + barrier 自动插入。不做自动拓扑排序、不做资源别名优化。Pass 声明输入输出（`RGResourceUsage`），RG 根据声明推导 barrier。

**帧间生命周期**：每帧重建（`clear()` → `import/use` → `add_pass` → `compile` → `execute`），不是过渡性设计。

**升级路径**：已有 pass 声明格式不变，后续加入拓扑排序和资源别名分析时无需改 pass 代码。

### 能力建设

| 能力 | 引入阶段 | 理由 |
|------|---------|------|
| Barrier 自动插入 | 二 | 核心价值 |
| 资源导入（import） | 二 | 外部资源导入 RG 追踪状态 |
| Managed 资源管理 | 三 | RG 创建、缓存、resize 自动重建 |
| Temporal 资源管理 | 五 | 首个 temporal pass（AO temporal filter） |

### Managed 资源

- `create_managed_image()` 注册持久 handle，`use_managed_image()` 每帧返回 `RGResourceId`
- Initial layout 统一 `UNDEFINED`（每帧覆写）
- `final_layout` 由调用方显式指定：非 temporal 传 `UNDEFINED`，temporal current 传 `SHADER_READ_ONLY_OPTIMAL`
- `get_managed_backing_image()` 在 resize handler 中即时获取新 handle 更新 descriptor
- Slot 状态通过 `backing.valid()` 判断

### Temporal 机制

- `create_managed_image(..., temporal=true)` 内部分配第二张 backing image
- `clear()` 自动 swap current/history，resize 重建两张并标记 history 无效
- `get_history_image()` **始终返回 valid** RGResourceId（push descriptor 不支持 PARTIALLY_BOUND，所有 binding 必须有效）
- `is_history_valid()` 查询有效性（首帧/resize 后无效 → blend_factor=0）
- M2 SSR/SSGI 复用同一机制

### MSAA Resolve

纯 Dynamic Rendering resolve（`VkRenderingAttachmentInfo::resolveImageView`），zero 额外 pass。

| Resolve 产物 | 产生者 | 模式 |
|-------------|--------|------|
| Resolved depth | DepthPrePass | MAX_BIT（前景深度） |
| Resolved normal | DepthPrePass | AVERAGE |
| Resolved hdr_color | ForwardPass | AVERAGE |

**Depth Resolve MAX_BIT**：7 个消费者中 6 个偏好 MAX（SSAO、Contact Shadows、DOF、SSR、SSGI、Camera Motion Blur），唯一偏好 MIN 的 God Rays 用 MAX 瑕疵不可感知。

### Framework 层 Vulkan 类型例外

以下组件允许直接使用 Vulkan 类型（VkImageLayout 等）：

| 组件 | 理由 |
|------|------|
| Render Graph | 本质是 barrier 管理器 |
| ImGui Backend | 第三方库天然需要 |
| Vertex 管线描述 | VkVertexInput 类型，再包一层纯增复杂度 |
| `upload_image` dst_stage | 同步原语，调用点仅两处 |

### Barrier 计算

- 从 `(RGAccessType, RGStage)` 映射到 `(VkImageLayout, VkPipelineStageFlags2, VkAccessFlags2)`
- 四种 hazard：RAR 无需 barrier，RAW/WAW/WAR 均处理
- RG 不管 loadOp/storeOp（pass 自行构造 `VkRenderingInfo`）

---

## 描述符与绑定

### 三层架构

| Set | 内容 | 生命周期 | 份数 |
|-----|------|---------|------|
| 0 | 全局 Buffer（GlobalUBO + LightBuffer + MaterialBuffer + InstanceBuffer） | per-frame 双缓冲 | ×2 |
| 1 | 持久纹理资产（bindless 2D + cubemap） | 场景加载 → 卸载 | ×1 |
| 2 | 帧内 Render Target | init → destroy | ×2（per-frame，阶段五扩展） |

- Graphics pipeline 统一 layout `{Set 0, Set 1, Set 2}`
- Compute pipeline `{Set 0, Set 1, Set 2, Set 3(push)}`

### Bindless 纹理

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[];     // 上限 4096
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];   // 上限 256
```

`PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，slot 通过 free list 回收。

### Set 0 — 全局 Buffer

| Binding | 类型 | 内容 |
|---------|------|------|
| 0 | UBO | GlobalUBO |
| 1 | SSBO | LightBuffer |
| 2 | SSBO | MaterialBuffer |
| 3 | SSBO | InstanceBuffer（阶段四引入） |

### Set 1 — 持久纹理资产

| 纹理 | Binding |
|------|---------|
| 材质纹理、BRDF LUT | binding 0（sampler2D[]） |
| IBL cubemap | binding 1（samplerCube[]） |

### Set 2 — Render Target

| Binding | 类型 | 名称 | 引入阶段 |
|---------|------|------|---------|
| 0 | `sampler2D` | hdr_color | 三 |
| 1 | `sampler2D` | depth_resolved | 五 |
| 2 | `sampler2D` | normal_resolved | 五 |
| 3 | `sampler2D` | ao_texture | 五 |
| 4 | `sampler2D` | contact_shadow_mask | 五 |
| 5 | `sampler2DArrayShadow` | shadow_map | 四 |
| 6 | `sampler2DArray` | shadow_map_depth | 四 |

Per-frame 双缓冲（2 份对应 2 frames in flight）。Temporal binding 每帧更新当前帧 copy；非 temporal binding 在 init/resize/MSAA 切换时写入两份。

### Compute Pass 绑定机制

- `bind_compute_descriptor_sets(layout, first_set, sets, count)` — 绑定 Set 0-2 到 compute pipeline（COMPUTE bind point）
- `push_storage_image(ResourceManager&, layout, set, binding, ImageHandle)` — compute 输出
- `push_sampled_image(ResourceManager&, layout, set, binding, ImageHandle, SamplerHandle)` — compute 输入
- `get_dispatch_set_layouts(set3_push_layout)` → `{set0, set1, set2, set3}`（compute 和 RT pipeline 共用）

显式传 `ResourceManager&` 保持 CommandBuffer 作为纯 VkCommandBuffer wrapper。`bind_compute_descriptor_sets` 与 `bind_descriptor_sets` 对称（后者为 GRAPHICS bind point），compute pass 用于绑定全局预分配 Set 0-2。

### GlobalUBO

当前 720 bytes（阶段四），阶段五新增 `inv_projection`(64) + `prev_view_projection`(64) → ~848 bytes，远小于 16KB。完整布局见 `m1-interfaces.md`。

---

## 数据格式

### HDR Color Buffer

R16G16B16A16F 全程。暗部无 banding 风险，M2 后处理链多次读写不累积量化误差。

### Normal Buffer

R10G10B10A2_UNORM, world-space。`n*0.5+0.5` 线性映射，MSAA AVERAGE resolve 正确，消费方 `normalize()` 即可。10-bit/通道角度分辨率 ~0.1°。2-bit A 通道预留 material flag。

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

## 渲染管线

### Pass 类约定

具体类（非虚基类），Renderer 持有具体类型成员。各 pass 方法集允许不统一，同功能方法保持同名。

| 方法 | 职责 | 调用时机 |
|------|------|----------|
| `setup()` | Pipeline 创建 + 存储服务指针 | 初始化 |
| `on_sample_count_changed()` | MSAA pipeline 重建 | MSAA 切换（仅受影响 pass） |
| `record()` | RG 资源声明 + execute lambda | 每帧 |
| `rebuild_pipelines()` | shader 重编译 + pipeline 重建 | 热重载 |
| `destroy()` | 销毁 pipeline + 私有资源 | 关闭 |

FrameContext 是纯每帧数据（RG 资源 ID + 场景数据引用 + 帧参数），不做 service locator。

### Pass 运行时开关

1. `RenderFeatures` 结构体（bool 字段，DebugUI 操作）
2. `GlobalUBO.feature_flags` bitmask（shader 动态分支）
3. Renderer 根据 features 跳过 pass `record()`
4. 消费端检查 FrameContext 资源 ID 有效性，条件声明 RG 依赖

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
```

Toggle 无需 GPU idle、无 descriptor 更新，下一帧生效。

### DepthPrePass 约定

- Depth + Normal 一步到位（阶段五 AO、M2 SSR/SSGI 均依赖）
- 阶段五新增 R8 roughness 输出（原为 B1 SO 设计，SO 改为 GTSO 后 M1 无消费方，保留供 M2 SSR 使用）
- Alpha Mask 双 pipeline（Opaque 无 discard 保 Early-Z，Mask 有 discard），先 opaque 后 mask
- Forward Pass 配合：`EQUAL + depth write OFF + invariant gl_Position`（确定性 zero overdraw）

### MSAA 策略

运行时 1x/2x/4x/8x 可配置，默认 4x。1x 不创建 MSAA 资源，直接渲染到 resolved target。切换时 `vkQueueWaitIdle` → 重建 managed 资源 + pipeline。

### Debug 渲染模式

HDR / passthrough 二分。HDR 模式（< `PASSTHROUGH_START`）经 exposure + ACES，passthrough 模式（≥ `PASSTHROUGH_START`）直接写入。新增 HDR 模式插入并递增阈值，新增 passthrough 模式追加到末尾。

### Instancing

CPU 侧 `(mesh_id, alpha_mode, double_sided)` 分组 + instanced draw + per-instance SSBO（Set 0 Binding 3, 128 bytes/instance，含预计算 normal matrix）。Shader 通过 `gl_InstanceIndex` 索引。Push constant 仅 4 bytes（`cascade_index`，shadow pass 用）。透明物体不做 instancing（需 back-to-front 排序）。

---

## 材质与场景

### 材质系统

代码定义 + 固定数据结构。`GPUMaterialData`（80 bytes std430）定义在 `material_system.h`。

**升级路径（多着色模型）**：固定 stride 方案——所有材质 struct 填充到同大小，每个 shader variant 定义自己的 typed struct，通过 `materials[material_index]` 统一寻址。

### 场景数据接口

渲染列表：`SceneRenderData` 用 `std::span` 引用应用层数据（mesh instances + lights + camera），渲染器只读消费。剔除结果是独立索引列表，不修改渲染列表。

### Shader 系统

运行时 shaderc 编译（GLSL → SPIR-V）+ 热重载。公共文件按依赖链组织（`constants.glsl` → `brdf.glsl`，`bindings.glsl` 独立，`normal.glsl` 独立）。

**当前策略**：无条件采样（阶段三，bindless + default 纹理红利），后续阶段按需引入动态分支或编译变体。

---

## App 层

### Application + Renderer 分离

- **Application** 持有：窗口、RHI 基础设施（Context/Swapchain/ResourceManager/DescriptorManager）、ImGui、App 模块（camera/scene_loader/debug_ui/**renderer**）、场景数据、IBL/渲染参数
- **Renderer** 持有：RenderGraph、ShaderCompiler、MaterialSystem、所有 Pass、渲染目标、共享渲染资源、per-frame GPU 数据、Swapchain 追踪

Renderer 持有 non-owning 引用（Context*、Swapchain* 等从 Application 接收）。

### Resize 两阶段

```
vkQueueWaitIdle → renderer.on_swapchain_invalidated() → swapchain.recreate() → renderer.on_swapchain_recreated()
```

---

## 阴影

### CSM 概要

- 直接 CSM（不经单张中间步骤），Texture 2D Array（每 cascade 一个 layer）
- 4 cascade 统一 2048²，cascade 数量运行时可调（1-4），分辨率运行时可调（触发资源重建）
- PSSM 分割（lambda 默认 0.75），`max_distance` 场景加载时自动初始化（`diagonal × 1.5`）
- Lerp cascade blend（blend_width=0.1 比例制），distance fade 独立参数
- Shadow map 资源 ShadowPass 自管理，每帧 import RG

### Bias 三件套

| Bias | 作用端 | 覆盖角度 |
|------|--------|---------|
| Constant | 渲染端 `vkCmdSetDepthBias`（D32Sfloat 下无效，已移除） | — |
| Slope-scaled | 渲染端 `vkCmdSetDepthBias` | 中等倾斜 |
| Normal offset | 采样端 `shadow.glsl`（与 cascade texel 世界尺寸成正比） | 极端掠射角 |

### PCSS

经典三步 PCSS（Fernando 2005），适配方向光正交投影：

- **世界空间统一半影**：per-cascade `cascade_pcss_scale = depth_range × 2tan(θ/2) / width_x`，同一遮挡距离在所有 cascade 中产生完全一致的半影
- **UV 各向异性校正**：`cascade_uv_scale_y = width_x / width_y`，V 方向乘此比值
- **Poisson Disk** 硬编码 + per-pixel interleaved gradient noise 旋转
- **采样数量**：3 档（Low 16+16 / Medium 16+25 / High 32+49），UBO 传入实际采样数
- **半影 clamp**：`[1 texel, 64 texels]`
- **Blocker early-out**：全命中返回 0.0，`pcss_flags` bit 0 运行时开关
- **Receiver plane depth bias**：`dFdx/dFdy` 梯度修正大 kernel acne，cascade 边界检测 + 梯度归零

### Shadow Shader 接口

分步函数（`shadow.glsl`），forward.frag 自行组装，debug cascade 可视化无障碍：

```glsl
int select_cascade(float view_depth, out float blend_factor);
float sample_shadow_pcf(vec3 world_pos, vec3 world_normal, int cascade);
float sample_shadow_pcss(ShadowProjData proj, int cascade, ...);
float blend_cascade_shadow(float shadow_current, float shadow_next, float blend_factor);
float shadow_distance_fade(float view_depth);
```

---

## 屏幕空间效果

### AO 命名

Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config 结构体），Implementation 层用 `gtao`（shader 文件名、pass 类名）。

### AO 算法

GTAO horizon search + cosine-weighted 解析积分。Step 10a 修正：投影法线长度权重、正确的切平面极限初始化/falloff、thickness heuristic（薄物体光晕）、帧间噪声变化。Step 10b 增强：二次步进分布、R1 步进抖动、5×5 edge-aware bilateral spatial blur。

### AO 集成

- **Diffuse indirect**：`ssao × material_ao` + Jimenez 2016 multi-bounce 色彩补偿（浅色表面压暗减轻）
- **Specular indirect**：仅由 SO 控制（material AO 不参与——标量 AO 乘方向相关的 specular 物理不正确）
- SO 方案：Bent Normal + GTSO（Jimenez 2016）。GTAO 计算 bent normal（XeGTAO Algorithm 2），输出 RGBA8（RGB=bent normal, A=AO）。forward.frag 用解析公式计算 visibility cone 与 specular cone 交集
- 仅调制间接光（IBL diffuse + IBL specular），直接光已有 shadow map + contact shadows 覆盖
- 管线：GTAO → Spatial Blur → Temporal Filter → Forward 采样

### Contact Shadows

Screen-space ray march + 世界空间搜索距离 + 深度自适应 thickness（`base_thickness × linear_depth`）+ 距离衰减（首次命中 + 远端 smoothstep fade）。无 temporal（确定性输出）。Push constant 传光方向（shader 不假设光源类型）。

---

## IBL 与资源管线

### IBL 管线

- Framework 层 `ibl.h`，自管理全部资源
- Equirectangular .hdr → GPU cubemap → irradiance / prefiltered / BRDF LUT
- Skybox mip 剥离 + GPU BC6H 压缩，运行时 ~26 MB
- 加载失败时 fallback 1×1 中性灰 cubemap，管线照常运行
- IBL 缓存：两组独立（BRDF 固定 key + 3 cubemaps HDR hash），KTX2 格式

### 缓存基础设施

`framework/cache.h` 工具模块：`cache_root()`（`%TEMP%\himalaya\`）+ `content_hash()`（XXH3_128）+ `cache_path()`。消费者各自处理序列化。

### GPU 上传

批量 immediate command scope（`begin_immediate()` → 录制所有 copy → `end_immediate()` 单次 submit）。`upload_image_all_levels()` 单 staging buffer + 批量 copy region 上传预建 mip chain。
