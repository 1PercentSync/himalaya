# M1 阶段四设计决策：阴影

> 阶段四：Instancing、运行时加载、BC 纹理压缩+KTX2 缓存、IBL 缓存、RenderFeatures 运行时开关、CSM+PCF+PCSS、Alpha mask 阴影、Culling 重构、Cascade 数量/分辨率运行时可调。
> 核心决策摘要见 `../milestone-1/m1-design-decisions-core.md`。

---

## 决策总览

| 组件 | 实现级别 | 核心理由 |
|------|----------|----------|
| Pass 运行时开关 | FeatureFlags 结构体 + GlobalUBO feature_flags bitmask + shader 动态分支 | 调试价值高，零性能开销，无需 GPU idle |
| CSM 阴影 | 直接 CSM（不经单张中间步骤），Texture 2D Array，4 cascade 2048²（运行时可调），PSSM 分割，ShadowPass 自管理资源每帧 import RG | 一步到位避免重写，array 无 PCF 边界问题 |
| Shadow 质量 | PCF + lerp cascade blend + dual bias（slope 硬件 + normal offset shader）+ alpha mask 阴影 | 性价比最优组合 |
| Shadow shader | `common/shadow.glsl` 分步函数，数据全部在 GlobalUBO | forward.frag 组装调用，debug cascade 可视化无障碍 |
| PushConstantData | 4 bytes（仅 cascade_index） | model + material_index 移至 InstanceBuffer SSBO |
| Per-cascade 剔除 | Step 1c 重构 Culling 为纯几何，Step 6 接入 | 职责分离：几何剔除与材质路由解耦 |
| Shadow max_distance | 场景加载时 `diagonal × 1.5` | auto 初始化避免不同场景手动调参 |
| Cascade blend_width | 0.1（10% 比例制） | 近窄远宽匹配人眼敏感度 |
| Distance fade | 独立 `distance_fade_width` | 语义不同于 cascade blend |
| Instancing | CPU 侧 mesh_id 分组 + per-instance SSBO + instanced draw | draw call 显著减少 |
| 运行时加载 | ImGui + Windows 原生对话框 + nlohmann/json 配置持久化 | 运行时切换场景/环境，无需重启 |
| 缓存基础设施 | `framework/cache.h`（`%TEMP%\himalaya\` + XXH3_128） | 统一缓存管理 |
| 纹理压缩 | BC7 + BC5，bc7e.ispc SIMD，KTX2 缓存 | 4:1 压缩比，GPU 原生解码 |
| KTX2 读写 | 自写最小模块（7 种格式），不依赖 libktx | libktx vcpkg 构建失败 |
| 多级上传 | `upload_image_all_levels()` 单 staging buffer + 批量 copy | 预建 mip chain 一次性上传 |
| IBL 缓存 | 两组独立缓存（BRDF 固定 key + cubemaps HDR hash），BC6H readback | 启动从分钟级降至秒级 |

---

## Pass 运行时开关

**选择：FeatureFlags 结构体 + GlobalUBO feature_flags bitmask + shader 动态分支（阶段四引入）**

M1 的多个 pass 是可选效果（阴影、SSAO、Contact Shadows、Skybox、后处理等），运行时开关对调试有重要价值——对比开启/关闭某个效果的画面差异，排查视觉问题的来源。

### 可开关 pass 分析

| Pass | 可独立关闭 | 关闭后影响 |
|------|-----------|-----------|
| DepthPrePass | 不能 | Forward 依赖 EQUAL depth test |
| ForwardPass | 不能 | 主光照 |
| TonemappingPass | 不能 | HDR→LDR 必需 |
| SkyboxPass | 可以 | 天空变黑，不影响其他 pass |
| ShadowPass | 可以 | 无阴影，画面变平 |
| SSAO | 可以 | 无环境光遮蔽 |
| Contact Shadows | 可以 | 无接触阴影 |
| 后处理各 pass | 可以 | 各自效果消失 |

### 核心问题：shader 如何安全采样可能不存在的 render target

禁用 pass 后其输出 render target 不存在。Set 2 使用 `PARTIALLY_BOUND`，访问未绑定 binding 是未定义行为。

| 路线 | 说明 |
|------|------|
| 路线 1：Feature flags + shader 动态分支 | GlobalUBO 的 `feature_flags` bitmask 控制 shader 是否采样 |
| 路线 2：Dummy textures + shader 无条件采样 | 禁用时绑定白色 dummy 到 Set 2 binding |

**选择路线 1。** 理由：

- Shadow map 类型是 `sampler2DArrayShadow`（硬件比较 + array），创建匹配 dummy 复杂
- Toggle 不需要 GPU idle——`feature_flags` 是 GlobalUBO 的一个 uint，下一帧自然生效
- 现代 GPU 对 uniform 条件分支（warp 内全走同一分支）零性能开销
- 不需要在 toggle 时更新 Set 2 descriptor

排除路线 2：每种特殊类型 render target（shadow array、未来可能的 3D texture 等）都需要专门 dummy。Toggle 需要 `vkQueueWaitIdle` + `vkUpdateDescriptorSets`，比路线 1 重。

### 设计机制

**1. RenderFeatures 结构体**（定义在 `framework/scene_data.h`）：

```cpp
struct RenderFeatures {
    bool skybox          = true;
    bool shadows         = true;   // 阶段四引入
    bool ao              = true;   // 阶段五引入
    bool contact_shadows = true;   // 阶段五引入
    // 后处理 flags 随阶段八扩展
};
```

**2. GlobalUBO 新增 `feature_flags` 字段**（`uint32_t`，bitmask）：

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
```

**3. Renderer 编排**：根据 `RenderFeatures` 决定是否调用 pass 的 `record()`。禁用的 pass 不注册到 RG，其输出 `RGResourceId` 在 FrameContext 中保持 invalid。

**4. 消费端条件资源声明**：ForwardPass 等消费 pass 的 `record()` 检查 FrameContext 资源 ID 有效性，仅对有效资源声明 RG 依赖：

```cpp
auto resources = base_resources(ctx);
if (ctx.shadow_map.valid())
    resources.push_back({ctx.shadow_map, Read, Fragment});
if (ctx.ao_texture.valid())
    resources.push_back({ctx.ao_texture, Read, Fragment});
```

**5. Shader 端动态分支**：

```glsl
float shadow = 1.0;
if ((global.feature_flags & FEATURE_SHADOWS) != 0u) {
    shadow = sample_shadow_map(...);
}
float ao = 1.0;
if ((global.feature_flags & FEATURE_AO) != 0u) {
    ao = texture(rt_ao_texture, uv).r;
}
```

### 引入时机

**阶段四引入骨架**。阶段三没有有调试价值的可开关 pass（Skybox 是唯一可选但调试价值不大）。阶段四引入阴影时是第一个有意义的开关场景，此时同步引入 `RenderFeatures` + `feature_flags`。后续阶段按需扩展。

### DebugUI 集成

阶段四起 DebugUI 新增 feature toggle 面板，直接操作 `RenderFeatures` 的 bool 字段。Toggle 生效路径：DebugUI → `RenderFeatures` → Renderer 跳过 pass → FrameContext 资源 invalid → ForwardPass 条件声明 → GlobalUBO `feature_flags` → shader 动态分支。无 GPU idle，无 descriptor 更新，下一帧即生效。

---

## CSM 阴影系统

阶段四引入。方向光 CSM（Cascaded Shadow Mapping）+ PCF 软阴影 + cascade blend。

**方向光数量限制**：M1 限制 `kMaxDirectionalLights = 1`。CSM 基础设施（shadow map 2D Array、GlobalUBO `cascade_view_proj[4]`、ShadowPass 渲染循环）均围绕单光源设计。多方向光 CSM 需要 N 张独立 shadow map（或 N×4 层 array）、per-light cascade 数据、shader 增加 light index 参数，架构改动显著，放 M2 实现。

### 实现策略：直接 CSM

**选择：直接实现 CSM，cascade=1 起步，不经过"单张 Shadow Map"中间步骤。**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 先单张 Shadow Map，再升级 CSM | 两步验证 |
| B. 直接 CSM，cascade=1 起步 | 一步到位 |

**选择 B。** cascade=1 的 CSM 运行时行为等价于单张 shadow map，但代码结构直接是最终形态。从单张升级到 CSM 需要重写 7 个改动项中的 5 个（资源类型、descriptor 类型、UBO layout、shadow pass 结构、光空间矩阵计算、shader cascade 查找），是"替换"而非"增量"。方案 B 的每一步都是增量的、不丢弃的。

调试隔离性差异很小——cascade=1 多引入的变量（array texture layer=0、matrix array 对齐）要么 trivial 要么在 cascade=1 时也能验证。

### Shadow Map 数据结构

**选择：Texture 2D Array（每 cascade 一个 layer）**

候选方案：

| 方案 | 说明 |
|------|------|
| A. Texture 2D Array | 每 cascade 一个 layer，`sampler2DArrayShadow` 硬件 PCF |
| B. 单张大 Atlas | 2D 纹理划分区域，UV 重映射 |

**选择 A。** PCF 采样无边界溢出问题（每 layer 独立纹理空间），采样代码简洁（layer index = cascade index），与 `m1-interfaces.md` 预定义的 `sampler2DArrayShadow` 一致。M3 Shadow Atlas 是面向多种光源的统一管理系统，与 CSM texture array 在管理复杂度上差距巨大，无论现在选哪种方案 M3 都需要另建 atlas 管理系统。

Atlas 相比 Array 的唯一优势是支持 per-cascade 不同分辨率。但 per-cascade 不同分辨率是不必要的——**PSSM 对数分割本身就通过给近处 cascade 分配更小的世界空间覆盖范围来实现非均匀 texel density**：同样的 2048² 像素覆盖 3m（cascade 0，~680 px/m）vs 覆盖 140m（cascade 3，~15 px/m），密度差 ~45 倍。在 PSSM 已经最优化像素预算分布的基础上叠加非均匀分辨率，既不能改善近处质量（PSSM 已给了最多像素），又会恶化远处质量（缩小远处 cascade 分辨率进一步降低本就偏低的密度）。真正影响近处阴影质量的参数是 `shadow_max_distance`（见「Shadow Max Distance 初始化」），而非 per-cascade 分辨率。

### Shadow Map 参数

| 参数 | 值 | 可调性 |
|------|-----|-------|
| Cascade 数量 | 4（默认） | 运行时可调（1/2/3/4），纯渲染参数（控制循环次数），不触发资源重建 |
| Shadow map 层数 | 4（固定 = MAX_SHADOW_CASCADES） | 不随 cascade 数量变化。未使用的层包含陈旧数据但不被采样 |
| 每 cascade 分辨率 | 2048²（所有 cascade 统一） | 运行时可调（512/1024/2048/4096），触发资源重建 |
| Depth 格式 | D32Sfloat | 固定（与主相机 depth 一致） |

所有 cascade 统一分辨率：Texture 2D Array 要求每 layer 同尺寸，且 PSSM 已通过覆盖范围差异提供非均匀 texel density（见上方分析），统一分辨率不构成浪费。4 × 2048² × 4B（D32Sfloat）= 64MB，桌面 8GB+ 显存下完全可接受。

Shadow map 层数固定为 4（MAX_SHADOW_CASCADES）：cascade_count 是纯渲染参数（控制 ShadowPass 循环次数 + shader 采样范围），不影响资源。这使得 cascade 数量切换为零开销操作（改一个整数，下一帧生效），避免了按需重建 image + views + descriptor 的复杂流程。`create_image()` 对 `array_layers = 4`（> 1 且 ≠ 6）自动推断 `VK_IMAGE_VIEW_TYPE_2D_ARRAY`，无需额外 flag。

运行时分辨率变更流程与 MSAA 切换统一模式：`vkQueueWaitIdle` → 重建资源（ShadowPass `on_resolution_changed()`）→ 更新 Set 2 descriptor。Cascade 数量变更不走此流程。

### Shadow Map 资源管理

**选择：ShadowPass 自管理（非 RG managed），每帧 import 到 RG**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 扩展 RGImageDesc 加 array_layers，作为 managed 资源 | RG 管理生命周期 |
| B. ShadowPass 自行创建/销毁，每帧 `import_image()` 到 RG | ShadowPass 完全拥有 |

**选择 B。** Shadow map 性质不同于屏幕尺寸 render target：Absolute 固定尺寸、array 纹理、需要 per-layer view。Managed 资源系统的核心价值（Relative 模式 + resize 自动重建）对 shadow map 无增值。方案 A 导致所有权分裂（主 image 归 RG，per-layer views 归 ShadowPass），config change 时需要协调两个管理者。方案 B 统一所有权——ShadowPass 同时管理主 image 和 per-layer views，创建/销毁/重建在一处完成。

每帧通过 `import_image("Shadow Map", shadow_image_, UNDEFINED, SHADER_READ_ONLY_OPTIMAL)` 导入 RG，与 swapchain image 的 import 模式一致。

### ShadowPass 类设计

**选择：单个 RG pass，内部循环 cascade**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 一个 RG pass，lambda 内循环 cascade | 单次 barrier |
| B. 每 cascade 一个 RG pass | 更细粒度 profiling |

**选择 A。** 方案 B 导致 4 个 pass 依次 Write 同一 shadow map RGResourceId，RG 在它们之间插入冗余 WAW barrier（RG 不追踪 subresource，无法区分不同 layer）。方案 A 一个 pass 一组 barrier，内部循环 cascade，per-cascade 用 `cmd.begin_debug_label("Cascade N")` 提供 RenderDoc 分组。

方法集：`setup()` / `record()` / `destroy()` / `on_resolution_changed()` / `rebuild_pipelines()`。不属于 MSAA 相关 pass（shadow map 始终 1x），无 `on_resize()`（Absolute 固定尺寸），无 `on_sample_count_changed()`。Cascade 数量为纯渲染参数，不需要专门方法。

ShadowPass 持有：shadow map ImageHandle、per-layer VkImageView 数组（通过 ResourceManager `create_layer_view()` 创建）、opaque pipeline、mask pipeline。提供 `shadow_map_image()` getter 供 Renderer 更新 Set 2 binding 5。

### Per-layer VkImageView 创建

**选择：扩展 ResourceManager，提供 `create_layer_view()` / `destroy_layer_view()` 工厂方法**

候选方案：

| 方案 | 说明 |
|------|------|
| A. ShadowPass 直接调用 Vulkan API 创建 VkImageView | 简单直接 |
| B. ResourceManager 新增 per-layer view 工厂方法 | 保持 RHI 抽象一致性 |

**选择 B。** 方案 A 让 Layer 2（passes）直接出现 `vkCreateImageView` 调用，破坏"上层不接触 Vulkan 类型"的分层约束。ResourceManager 已持有底层 `VkImage`、format、aspect 信息，创建 sub-resource view 是其天然职责。

`create_layer_view(ImageHandle, uint32_t layer)` 返回 `VkImageView`，调用方持有并通过 `destroy_layer_view()` 销毁——不侵入现有的 per-ImageHandle 资源池结构。未来 bloom mip chain（per-mip view）、reflection probe（per-face view）等场景也可复用此模式。

### Shadow Map Descriptor 写入时机

**选择：init + shadow resolution change，不参与 swapchain resize 和 MSAA 切换**

Set 2 binding 5（shadow map + comparison sampler）的写入时机：

| 触发 | 是否更新 binding 5 | 原因 |
|------|-------------------|------|
| `Renderer::init()` | 是 | ShadowPass setup 完成后首次写入 |
| `handle_shadow_resolution_changed()` | 是 | shadow map image 重建，view 变化 |
| swapchain resize | 否 | shadow map 为 Absolute 尺寸，不跟随窗口 |
| MSAA 切换 | 否 | shadow map 始终 1x |

与 `rt_hdr_color`（binding 0）不同——binding 0 在 resize 和 MSAA 切换时均需更新，binding 5 只在 shadow 资源重建时更新。两者独立，Renderer 在各自的 handler 中分别写入对应 binding。

### Alpha Mask 阴影

**选择：支持，从阶段四开始**

| 方案 | 说明 |
|------|------|
| A. 不处理，实心阴影 | 最简单 |
| B. 双 pipeline（opaque + mask） | 与 DepthPrePass 模式一致 |

**选择 B。** Sponza 场景有大量 alpha mask 植物，实心矩形阴影非常显眼。增量 ~50 行代码，模式复用 DepthPrePass 的 opaque/mask 分离。

- Opaque pipeline：仅 VS（`shadow.vert`），无 FS，深度由光栅器写入，完整 Early-Z
- Mask pipeline：VS + FS（`shadow_masked.frag`，alpha test + discard），Early-Z 可能被禁用
- 绘制顺序：先 opaque 后 mask（与 DepthPrePass 一致）

### Shadow Map 深度约定

**选择：Reverse-Z（与主相机一致）**

正交投影下 Reverse-Z 无精度优势（深度线性分布），选择纯粹基于一致性——项目全面 Reverse-Z，shadow map 特殊化引入认知负担。

- Clear value：0.0（far）
- Depth compare：GREATER
- Shadow comparison sampler：`GREATER_OR_EQUAL`（fragment depth ≥ shadow depth → lit）

### Cascade 分割策略

**选择：Practical（PSSM），lambda 默认 0.75，DebugUI 可调**

`C_i = λ × C_log + (1 - λ) × C_lin`

业界最常用方案（GPU Gems、NVIDIA、Unity/UE 均采用变体）。`shadow_max_distance` 限制分割范围（不一定等于相机 far plane），避免远距离浪费分辨率。

### Shadow Max Distance 初始化

**选择：场景加载时根据 scene AABB 自动初始化**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 固定默认值 200m | 简单，但对小场景（Sponza ~30m）浪费大量分辨率 |
| B. 场景加载时自动计算 | SceneLoader 暴露 scene AABB，Application 设置 `max_distance = diagonal × 1.5` |
| C. 运行时自动追踪 | 每帧根据可见物体动态调整 |

**选择 B。** `shadow_max_distance` 直接决定 PSSM 分割质量。以 Sponza（~30m 跨度）为例，固定 200m 时 cascade 0 的 texel density 仅 ~159 px/m（cascade 2-3 浪费在空旷区域），自动计算后（max_distance ≈ 55m）cascade 0 密度达 ~554 px/m，近处阴影质量提升 3.5 倍。

排除 A：不同场景尺度差异巨大（室内 30m vs 室外 500m），固定值对多数场景不是最优。排除 C：M1 静态场景，max_distance 不需要帧间变化。

**计算方式**：`max_distance = scene_aabb.diagonal_length() × 1.5`。乘 1.5 覆盖方向光在 ~60° 入射角下的阴影投射范围（此时阴影长度约为物体高度的 1.73 倍）。倍率硬编码，不作为可调参数——这是"合理默认值"的计算系数，不是用户关注点。

**退化防护**：Application 初始化 `max_distance = 100.0f`（退化 fallback）。场景 AABB 有效时覆盖为 `diagonal × 1.5`，退化时保持 100m。不设人为 clamp 范围——基于实际场景几何的计算结果对该场景总是合理的。

**实现位置**：SceneLoader 加载完成后计算场景 AABB（所有 mesh instance 的 `world_bounds` 求并集），暴露 `scene_bounds()` getter。Application 在场景加载后、首帧渲染前：`if (diagonal > epsilon) max_distance = diagonal × 1.5`，否则保持初始 100m。DebugUI 仍允许手动覆盖（对数滑条）。RenderFeatures 和 ShadowConfig 均无 struct 默认值，Application 使用 designated initializers 显式初始化全部字段。

### 相机自动定位与 F 键 Focus

**选择：Camera::compute_focus_position() 纯计算 + CameraController F 键触发**

两个需求共享同一核心计算——给定 AABB + 朝向 + FOV → 计算能看到整个场景的相机位置：

| 场景 | yaw/pitch | position |
|------|-----------|----------|
| 场景加载自动定位 | 设为 0° / -45° | 由 AABB 计算 |
| F 键 focus | 保持当前值不变 | 由 AABB 计算 |

**分层设计**：

- `Camera::compute_focus_position(const AABB& bounds) const`（framework 层）：纯几何计算，AABB 包围球半径 `r` → `distance = r / sin(fov/2)` → `center - forward() * distance`。不修改相机状态。退化 AABB 时返回当前 position。
- `CameraController::set_focus_target(const AABB* bounds)`（app 层）：持有 focus target 指针。`update()` 中检测 F 键单次按下（`ImGui::IsKeyPressed(ImGuiKey_F, false)`，在 `!io.WantTextInput` 保护下），调用 `compute_focus_position()` 更新 position。
- Application：场景加载后设置 `camera_controller_.set_focus_target(&scene_loader_.scene_bounds())`。自动定位时先设 pitch/yaw 再调用 `compute_focus_position()`。

**为什么不放在 Application 层检测 F 键**：键盘输入处理是 CameraController 的职责，Application 不应直接轮询按键。CameraController 通过指针获取 AABB 数据，不引入对 SceneLoader 的依赖。

### Cascade 混合策略

**选择：Lerp blend**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 硬切换 | 有可见接缝 |
| B. 相邻 cascade lerp | blend region 内 2x 采样 |
| C. Dithering + temporal | 无额外采样，需 temporal filtering |

**选择 B。** 排除 A：cascade 间质量差异在无过渡时产生可见接缝。不采用 C：dithering 依赖全画面 temporal accumulation（FSR/DLSS）平滑噪点，M1 不具备此基础设施。即使有 DLSS/FSR，disocclusion 时 dithering 噪声在 cascade 边界仍短暂可见（~1-2 帧，无法完全消除）。是否可接受需实测判断。

Lerp blend 每帧都产出干净结果，零 temporal 依赖，无 disocclusion 瑕疵。性能开销局限在 blend region（~10% 阴影像素 2x 采样），可接受。

**Dithering 切换实现约束**：shadow.glsl 中 cascade blend 逻辑封装为独立函数 `blend_cascade_shadow(shadow_current, shadow_next, blend_factor)`，forward.frag 通过此函数获取最终 shadow 值。切换 dithering 时只需修改此函数内部（从 `mix()` 改为 dither pattern 比较 + 单次采样），forward.frag 调用点不变。

#### Blend Width 默认值分析

`blend_width = 0.1`（cascade blend region 占 cascade 范围的 10%）。以 auto max_distance ≈ 55m 的 Sponza 场景为例分析绝对 blend 距离：

| 过渡 | cascade 范围 | blend 绝对宽度 | 过渡位置 | 相邻密度比 |
|------|-------------|---------------|---------|-----------|
| 0→1 | 3.7m | 0.37m | ~3.4m | 1.3:1 |
| 1→2 | 4.9m | 0.49m | ~8.2m | 2.1:1 |
| 2→3 | 10.5m | 1.05m | ~18.2m | 3.5:1 |

比例制的隐含优势：近处 cascade（密度比小、质量差异小）获得窄 blend，远处 cascade（密度比大但视距远、屏幕占比小）获得宽 blend，匹配人眼对不同距离的敏感度差异。0.1 作为默认值合理，DebugUI 可调。

**Distance fade 与 cascade blend 参数分离**：`distance_fade_width`（阴影远端淡出区间占 `max_distance` 的比例）独立于 `blend_width`（cascade 间过渡区间）。两者语义不同——blend_width 控制相邻 cascade 质量差异的平滑过渡，distance_fade_width 控制最远 cascade 边缘到无阴影的淡出。ShadowConfig 和 UBO 保留独立字段以备 M2+ 分离调参，当前 DebugUI 的 Blend Width 滑条同时驱动两者（两者本质都是"过渡平滑度"，M1 无需分别调参）。

### Shadow Bias 策略

**选择：三者组合（constant + slope 硬件 + normal offset shader）**

| Bias 类型 | 作用端 | 覆盖角度 |
|-----------|--------|---------|
| Constant | 渲染端（`vkCmdSetDepthBias`） | 正对光源 |
| Slope-scaled | 渲染端（`vkCmdSetDepthBias`） | 中等倾斜 |
| Normal offset | 采样端（`shadow.glsl`） | 极端掠射角 |

Normal offset 与 cascade texel 世界尺寸成正比——远处 cascade 的 texel 覆盖更大世界面积，需要更大偏移。texel world size 从 `cascade_view_proj` 矩阵提取（正交投影范围 / shadow map 分辨率），不需要额外 UBO 字段。

所有 bias 参数为全局值（非 per-cascade），DebugUI 可调。

### Shader 接口设计

**选择：分步函数，forward.frag 自行组装**

```glsl
// common/shadow.glsl
int select_cascade(float view_depth, out float blend_factor);
float sample_shadow_pcf(vec3 world_pos, vec3 world_normal, int cascade);
float blend_cascade_shadow(float shadow_current, float shadow_next, float blend_factor);
float shadow_distance_fade(float view_depth);
```

不做单一 `evaluate_shadow()` 包装——cascade index 被隐藏会妨碍 debug cascade 可视化。forward.frag 的 shadow 调用流程完全透明，方便调参和排查。`blend_cascade_shadow` 隔离 blend 策略（见「Cascade 混合策略 — Dithering 切换实现约束」）。

### GlobalUBO Shadow 数据布局

**选择：全部放入 GlobalUBO**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 扩展 GlobalUBO | 336 → 656 bytes |
| B. 独立 Shadow UBO（新增 Set 0 binding） | 清晰分离 |

**选择 A。** 656 bytes 远在 16KB 最低保证之下。新增 binding 需修改 Set 0 layout → 所有 pipeline layout 变更 → 全部 pipeline 重建，代价远超 UBO 多几百字节。

布局详见 `../milestone-1/m1-interfaces.md`「GlobalUniformData」。

### PushConstantData

**选择：仅 cascade_index，4 bytes**

Instancing 准备工作（阶段四准备工作 B）将 model + material_index 迁移到 per-instance SSBO 后，push constant 只剩 `cascade_index`（4 bytes）。Shadow VS 需要知道当前渲染的 cascade index 以从 GlobalUBO 读取对应的 `cascade_view_proj[cascade_index]`，每个 cascade 循环开头 push 一次，该 cascade 所有 draw call 共享。Forward/PrePass 不使用 push constant。

### Per-cascade 剔除与 Culling 模块重构

**选择：Step 1c 提前重构 Culling 模块为纯几何剔除，ShadowPass 暴力全画验证正确性后 Step 6 接入**

| 阶段 | 策略 |
|------|------|
| Step 1c | 重构 Culling 模块为纯几何接口 + 现有相机 cull 迁移（shadow 之前完成，隔离验证） |
| Step 2-5 | ShadowPass 暴力全画全部场景物体，GPU 自动裁剪 frustum 外几何。Sponza 规模下 ~2000 depth-only draw calls 无性能问题 |
| Step 6 | ShadowPass 接入 per-cascade 剔除（Culling 接口已就绪） |

**Culling 模块职责边界**：现有 `perform_culling()` 混合了三个职责——几何剔除（AABB-frustum 测试）、材质分桶（按 alpha_mode 分 opaque/transparent）、透明排序。Step 1c 拆分为：

| 职责 | 归属 | 理由 |
|------|------|------|
| 几何剔除 | `culling.h`（`Frustum` + `extract_frustum` + `cull_against_frustum`） | 纯几何，与材质无关，camera/shadow/probe 统一复用 |
| 材质分桶 | 调用方内联 | 不同消费者分桶标准不同（camera: opaque vs transparent，shadow: opaque vs mask，Blend 不投射阴影） |
| 透明排序 | 调用方内联 | 只有 camera 路径需要，shadow/probe 不需要 |

`CullResult` 继续在 `scene_data.h`——它是 camera cull + 分桶后的渲染合同，不属于 culling 模块。

**接口设计**：`cull_against_frustum` 使用调用方持有的预分配 buffer（`void` + `out_visible` 参数），调用方跨帧复用 `vector`（`clear()` 不释放内存，首帧分配后后续帧零分配），避免每帧 camera 1 次 + shadow 4 次 + 未来 probe 6 次的重复 vector 分配。接口定义见 `../milestone-1/m1-interfaces.md`「Culling 泛化接口」。

**Shadow cull 输入**：必须是全部场景物体（不是相机剔除子集）——相机 frustum 外的物体可能在 shadow map 中可见（如屏幕外的树投影到可见地面上）。Per-cascade 光空间剔除从全部物体中过滤，cull 结果再按 alpha_mode 分桶为 opaque/mask 列表供双 pipeline 绘制。

### PCSS（Percentage-Closer Soft Shadows）

从 M2 提前到阶段四 Step 7。在 PCF 基础上实现距离相关的软阴影（contact-hardening shadows）。

#### 算法选择

**选择：经典三步 PCSS（Fernando 2005），适配方向光正交投影**

PCSS 是业界最广泛部署的接触硬化软阴影算法（Unity HDRP、Godot、Bevy、GTA V、Far Cry 4、Dying Light 等），在现有 shadow mapping pipeline 中替换 shadow lookup 函数即可。

方向光（正交投影）相比透视投影的两个关键简化：
- **`LIGHT_SIZE_UV` 为常数**：正交投影无透视缩放，光源角尺寸在 UV 空间的投影不随深度变化。Blocker search 实际搜索半径取 `max(LIGHT_SIZE_UV, kMaxPenumbraTexels × texel_size)` 以确保覆盖最大半影范围——当半影宽度大于光源 UV 尺寸时（大 `depth_range` 或大角直径），仅用 `LIGHT_SIZE_UV` 会导致搜索边界处硬切边
- **半影公式去掉 `1/dBlocker` 除法**：`wPenumbra = (dReceiver - dBlocker) * lightSize`，因为正交投影中世界空间距离与 UV 距离的映射不依赖深度

不选 Variance Soft Shadow Mapping（VSSM）——虽然 blocker search 成本低 10×+，但有 light bleeding artifact，且项目当前不存在 PCSS 性能瓶颈。不选 Screen-Space PCSS——增加 pass 复杂度，收益在 M1 不明显。

#### Per-cascade 正交投影几何暴露

**选择：`ShadowCascadeResult` 新增 `cascade_width_x` / `cascade_width_y` / `cascade_depth_range`（vec4），由 Renderer 组装 PCSS UBO 字段**

PCSS 需要每个 cascade 的正交投影 X/Y 范围和 Z 深度范围来计算 `cascade_light_size_uv`、`cascade_pcss_scale`、`cascade_uv_scale_y`。这些值在 `compute_shadow_cascades` 的 per-cascade 循环中已经计算（`ls_max - ls_min`），但未暴露。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 扩展 result 存原始几何 | `compute_shadow_cascades` 只做几何，PCSS 组装留给 Renderer |
| B. 从 VP 矩阵反推 | 不改函数，从矩阵行向量长度提取宽度/深度范围 |
| C. 函数内直接算 PCSS 字段 | 传入 angular_diameter，内部一并计算 |

**选择 A。** `compute_shadow_cascades` 保持纯几何计算职责，不耦合 PCSS 参数。result 多 48 bytes 中间值（3 × vec4），仅为栈上临时结构体，无运行时开销。方案 B 不直观且依赖 `ortho_reverse_z` 内部编码；方案 C 在纯几何函数中引入光源物理参数，职责不清。

#### 双采样器绑定

**选择：Set 2 binding 5（比较采样器）+ binding 6（普通采样器），指向同一 image**

PCSS 的 Blocker Search 需要读取 shadow map 原始深度值（累加求平均），而 `sampler2DArrayShadow` 只返回比较结果（0/1），且 GLSL 规范不支持对 shadow sampler 调用 `texelFetch`。

唯一可行方案是新增一个非比较采样器 binding。两个 binding 指向同一 `VkImageView`、配不同 `VkSampler`，Vulkan 规范允许，零额外显存开销，仅一条额外 descriptor write。

#### Light Size 参数

**选择：物理角直径（angular diameter），弧度存储，DebugUI 度数显示**

方向光没有物理半径，光源大小以角直径描述——光盘从任意点看去所张的角。太阳角直径 ≈ 0.53°（0.00927 rad）。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 物理角直径 | `ShadowConfig.light_angular_diameter`（弧度），CPU 换算 per-cascade `LIGHT_SIZE_UV` |
| B. 抽象 soft shadow size | 单一 float，无物理含义 |

**选择 A。** 调参直觉清晰（"太阳 0.53°，更软就调大"），CPU 端换算 `LIGHT_SIZE_UV = 2 * tan(θ/2) / frustum_width` 仅一行代码。Bevy 早期用方案 B 后来也迁移到了物理参数。

Per-cascade `LIGHT_SIZE_UV` 必须每帧重算——各 cascade 覆盖不同世界空间范围（`frustum_width` 不同），同一角直径映射到不同 UV 大小。

#### UV 空间各向异性校正

**选择：以 width_x 为基准计算 `LIGHT_SIZE_UV`，额外传 per-cascade `cascade_uv_scale_y`（`width_x / width_y`）**

正交投影 tight-fit 相机子锥体后，X 和 Y 范围受相机 aspect ratio 和光照方向影响，可差 1.5-2×。同一世界空间圆形光盘在 shadow map UV 空间中是椭圆——1 个 UV 单位在 U 方向对应 `width_x` 米，在 V 方向对应 `width_y` 米。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 取 max(width_x, width_y) | 单一标量，Poisson Disk 画正圆。较窄维度搜索不足 |
| B. 分别传 U/V 两个缩放 | 两个 `vec4`（`cascade_light_size_u` + `cascade_light_size_v`），搜索/半影椭圆精确 |
| C. 以 width_x 为基准 + Y 缩放比 | 一个额外 `vec4`（`cascade_uv_scale_y = width_x / width_y`），shader 中 V 方向乘比值 |

**选择 C。** 精度等同方案 B，但只需一个额外 `vec4` UBO 字段（同时覆盖 Blocker Search 和 PCF 两处），因为 `cascade_pcss_scale` 的 V 方向同样乘以此比值即可。性能零损失——`vec2 * vec2` 与 `vec2 * scalar` 在 GPU 上均为单条 VMUL 指令，瓶颈在纹理采样（41 次/片元），不在 ALU。

#### 采样分布

**选择：Poisson Disk 硬编码 + per-pixel interleaved gradient noise 旋转**

候选方案：

| 方案 | 说明 |
|------|------|
| A. Poisson Disk | 预生成随机均匀分布的 2D 点集，shader 常量 |
| B. Vogel Disk | golden-angle spiral，shader 内公式生成，无需硬编码数据 |
| C. 规则网格 | 沿用现有 PCF 的 for 循环网格 |

**选择 A。** NVIDIA 原始 PCSS 论文和几乎所有生产实现的标准做法。相比 B 免去了运行时 `sqrt` + `sin` + `cos` per sample。相比 C 在圆形 kernel 中覆盖更均匀，无 moiré。

坐标由 `scripts/generate_poisson_disk.py` 生成（dart-throwing 算法，固定随机种子，单位圆内均匀分布），输出为可直接粘贴到 shader 的 GLSL `const vec2[]` 格式。脚本留在仓库中作为坐标的可复现来源。

Per-pixel 旋转使用 Jimenez 的 interleaved gradient noise（Call of Duty: Advanced Warfare），将结构化 banding 转化为高频噪声。后续 M2 加 TAA 时，temporal accumulation 天然消除此噪声。

#### 采样数量

**选择：运行时可配置，3 档预设（Low / Medium / High），通过 UBO 传入实际采样数**

Poisson Disk 硬编码最大档位的样本数（32 blocker + 49 PCF），shader 中通过 UBO 传入的 `pcss_blocker_samples` / `pcss_pcf_samples` 控制实际使用数量。循环上限来自 UBO uniform 值，wave 内所有线程执行相同次数，**不产生 warp 分支发散**（仅失去编译器循环展开优化，影响可忽略）。

| 档位 | Blocker | PCF | 总采样 | 有效比较 | 用途 |
|------|---------|-----|--------|---------|------|
| Low | 16 | 16 | 32 | 64 | M2 有 TAA 时的低开销配置 |
| Medium | 16 | 25 | 41 | 100 | 默认，业界 shipped titles 标配 |
| High | 32 | 49 | 81 | 196 | M1 无 TAA 想要干净半影 |

DebugUI 显示为 "PCSS Quality" 下拉（Low / Medium / High），PCSS 模式下可见。默认 Medium。

Poisson Disk 生成脚本输出 32 + 49 个样本（覆盖最大档位），shader 声明 `kBlockerSearchSamples[32]` 和 `kPCFSamples[49]`，Low/Medium 档位使用前 16/25 个。Poisson Disk 的特性保证前 N 个样本在子集内仍然分布均匀（dart-throwing 按顺序生成，先生成的点自然形成均匀子集）。

#### 半影宽度 clamp

**选择：shader 常量 `kMaxPenumbraTexels = 64`，双向 clamp `[shadow_texel_size, kMaxPenumbraTexels * shadow_texel_size]`**

下限 1 texel 防止极小半影退化为噪点（太阳角直径 0.53° 在近 cascade 可产生 sub-texel 半影）。

上限 64 texels 防御 PCSS 的 single-layer depth 根本局限：多层遮挡场景中 blocker search 混合近/远遮挡物深度，产生物理上无意义的巨大 `delta_ndc`，导致 kernel 爆炸——25 个 Poisson 样本分散到 shadow map 大片区域引起缓存失效和性能骤降，且结果为纯噪声。64 texels 对 25 样本 Poisson disk 已是稀疏采样的边界。

使用 texel 数（乘 `shadow_texel_size` 换算 UV）而非固定 UV 值，自动适配不同 shadow map 分辨率。不暴露为 DebugUI 参数——这是防御性 clamp 而非创意调参项。M2 加 TAA 后 temporal accumulation 可容忍更大 kernel，届时调常量即可。

#### Blocker Early-Out（多层遮挡漏光缓解）

**选择：blocker search 全命中时 early-out 返回 0.0，可通过 `pcss_flags` 运行时开关**

PCSS 的 single-layer depth 根本局限导致多层遮挡场景（室内、门洞、多层建筑）中可能出现漏光亮斑：blocker search 混合近/远遮挡物深度 → 平均出无意义的中间值 → 过大半影 → variable-width PCF 采到亮区。penumbra clamp（上限 64 texels）限制了严重程度但不能消除。

缓解措施：blocker search 完成后，若 `num_blockers >= BLOCKER_SEARCH_SAMPLES`（全部 16 个采样点均为 blocker），直接返回 0.0（纯阴影），跳过半影估算和 variable-width PCF。

**效果**：阴影深处（所有搜索样本均命中遮挡物）的漏光完全消除——这是最常见、最碍眼的情况（暗室中间出现亮斑）。剩余漏光仅在阴影边界附近的窄带内（宽度 ≈ blocker search 世界空间搜索半径，5° 角直径下 cascade 0 约 9cm），该位置本身就处于半影过渡区，视觉上是合理的。

**过渡平滑性**：从 16 blockers（返回 0.0）到 15 blockers（走 PCSS）的跳变极小——15/16 blocker 时平均深度很接近 receiver，算出的半影极窄，variable-width PCF 结果接近 0.0。

**零额外开销**——使用已有的 blocker search 计数，无额外纹理采样。

**运行时开关**：通过 `GlobalUniformData.pcss_flags` 的 bit 0（`PCSS_FLAG_BLOCKER_EARLY_OUT`）控制，DebugUI 显示为 "Blocker Early-Out" checkbox（PCSS 模式下可见，默认开启），便于对比开关前后的漏光差异。

`pcss_flags` 占用 `GlobalUniformData` offset 660 的原 padding 位（`_pcss_pad[3]` → `pcss_flags` + `_pcss_pad[2]`），UBO 总大小 720 bytes 不变。

#### Receiver Plane Depth Bias

**选择：在 PCSS 采样中引入 receiver plane depth bias，对抗 variable-width kernel 的 acne**

固定 PCF（Step 6）kernel 最大 11×11 texels，采样点离中心不远，现有 normal offset + 硬件 slope bias 足够。PCSS 的 variable-width PCF 在大半影（角直径 2-5°、大遮挡距离）时 kernel 可跨越几十 texels——normal offset 只作用于中心点，远离中心的采样点对倾斜表面的深度偏差无法补偿，导致 acne。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 不做额外处理 | 依赖现有 normal offset + slope bias，大 kernel 时可能 acne |
| B. Receiver plane depth bias | 从屏幕空间导数计算深度在 shadow UV 中的梯度，per-sample 线性调整比较深度 |

**选择 B。** 原理：`dFdx/dFdy` 算出 `ref_depth` 在 shadow UV 空间中的梯度 `(dz/du, dz/dv)`（2×2 矩阵求解），每个采样点按 UV 偏移线性调整比较深度：`adjusted = ref_depth + dz_du * offset.x + dz_dv * offset.y`。这让比较深度跟随接收面斜率变化，从根源消除大 kernel 的 acne。

性能：`dFdx/dFdy` 硬件免费，2×2 solve 每片元一次，per-sample 仅 2 mul + 2 add——相比 41 次纹理采样可忽略。

副作用防护：极端掠射角下梯度过大可能导致漏影（adjusted_depth 穿过 blocker），需 clamp 梯度到合理最大值。**`kMaxReceiverPlaneGradient = 0.01`**——45° 表面的梯度通常在 0.001~0.01 量级，0.01 允许中等倾斜表面的正确修正，同时截断掠射角（>80°）的极端梯度。截断后退化为固定 `ref_depth`，等价于无 receiver plane bias 的普通 PCF 行为，不会比 Step 6 的固定 PCF 更差。

此 bias 仅用于 PCSS 路径（blocker search + variable-width PCF）。固定 PCF 路径不需要（kernel 小，现有 bias 足够），保持两条路径独立。

#### Receiver Plane 梯度预计算（uniform flow 约束）

**选择：将 `dFdx/dFdy` 提取到独立函数 `prepare_shadow_proj()`，在 uniform control flow 中预计算，采样函数仅消费预计算数据**

GLSL 规范要求 `dFdx/dFdy` 在 uniform control flow 中执行（2×2 quad 内所有 invocation 必须执行同一指令）。`blend_cascade_shadow()` 的 cascade blend 分支（`if blend_factor > 0.0`）是 non-uniform control flow——quad 中部分像素在 blend 区域内、部分不在。如果 `blocker_search` 或 `sample_shadow_pcss` 在此分支内部调用 `dFdx/dFdy`，结果为未定义行为。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 接受 UB | 多数现代 GPU 通过 predicated execution 实际上仍能工作，但非可移植 |
| B. 预计算梯度到结构体 | 在分支外为当前和下一 cascade 各预计算 `ShadowProjData`，后续函数不调 `dFdx/dFdy` |
| C. 始终执行两路 PCSS 采样 | 移除 blend 分支，无条件对两个 cascade 完整采样，用 `blend_factor == 0` 时乘零抵消 |

**选择 B。** 引入 `ShadowProjData` 结构体（shadow_uv + ref_depth + dz_du + dz_dv）和 `prepare_shadow_proj()` 函数。`blend_cascade_shadow()` 在 PCSS 路径中为当前和 `min(cascade+1, max-1)` 各调用一次 `prepare_shadow_proj()`——两次调用均在 uniform flow 中（`shadow_mode == 1` 分支是 uniform，`shadow_mode` 来自 UBO）。后续 `blocker_search(ShadowProjData, ...)` 和 `sample_shadow_pcss(ShadowProjData, ...)` 仅读取预计算数据。

额外代价：每像素多一次 `prepare_shadow_proj()`（即使不在 blend 区域），约 25 ALU ops（1 次 mat4×vec4 + 4 次 dFdx/dFdy + 2×2 solve + clamp）。相比 41 次纹理采样可忽略。方案 C 无条件执行 41 次额外纹理采样，代价过高。方案 A 虽然实践中大多数桌面 GPU 能工作，但属于 spec 层面 UB，不可移植。

**已知局限与缓解**：cascade 边界处 2×2 quad 内像素可能使用不同 `cascade_view_proj` 矩阵，`dFdx(shadow_uv)` 混合不同投影空间的导数——数学上不精确（正常梯度量级 ~0.0005，跨 cascade 混合后可达 ~0.5，偏差 1000×）。2×2 solve 输出的 `dz_du/dz_dv` 为垃圾值。

**缓解措施：cascade 边界检测 + 梯度归零。** `prepare_shadow_proj()` 在计算梯度后，通过 `dFdx(float(cascade))` 检测 quad 内是否存在 cascade 不一致。若检测到不一致，将 `dz_du` 和 `dz_dv` 显式归零，而非依赖 `kMaxReceiverPlaneGradient` clamp 间接截断：

```glsl
float cascade_var = abs(dFdx(float(cascade))) + abs(dFdy(float(cascade)));
float cascade_ok = step(cascade_var, 0.5);  // 1.0 = uniform, 0.0 = boundary
dz_du *= cascade_ok;
dz_dv *= cascade_ok;
```

归零后这些像素失去 receiver plane bias 修正，退化为仅依赖 normal offset + slope bias——等价于固定 PCF 路径的 bias 质量。此情况仅影响 cascade 交界处 1-2 像素宽的窄带，且这些像素同时处于 cascade blend 区域（两个 cascade 的阴影值会被 lerp 混合），视觉上不可见。`kMaxReceiverPlaneGradient` clamp 作为第二层防护仍然保留，防御非 cascade 边界场景下的极端掠射角。

#### Cascade 边界半影连续性

**选择：世界空间统一半影估算**

候选方案：

| 方案 | 说明 |
|------|------|
| A. Per-cascade LIGHT_SIZE_UV + cascade blend lerp | 各 cascade 独立计算，blend region 内 lerp |
| B. 世界空间统一半影 | NDC depth → 世界空间距离后计算，所有 cascade 结果一致 |

**选择 B。** PSSM 分割下相邻 cascade 的 `LIGHT_SIZE_UV` 差异约 2-3×（典型 4 cascade 配置），方案 A 在 cascade 边界处半影宽度会跳变到不到一半。虽然 blend lerp 能平滑过渡，但跳变幅度大时在 blend region 内仍可感知半影收窄。

方案 B 在 CPU 端预计算 per-cascade `cascade_pcss_scale = depth_range × 2tan(θ/2) / width_x`，将 NDC→世界空间→UV 三步合并为 shader 中的单次乘法。此处 `depth_range` 是正交投影的 Z 深度范围（`far - near`，扩展到场景 AABB），**不是** XY 宽度——两者独立计算，近 cascade 的 Z/XY 比可达 20×。`depth_range` 在 NDC→世界→UV 链条中正确约掉，同一遮挡距离在所有 cascade 中产生完全一致的世界空间半影估计。

`cascade_pcss_scale` 以 `width_x` 为基准计算（U 方向），V 方向通过 `cascade_uv_scale_y`（`width_x / width_y`）校正，使半影在 UV 空间中为精确椭圆而非近似正圆（见「UV 空间各向异性校正」）。Shader 中：`penumbra = vec2(delta_ndc * pcss_scale, delta_ndc * pcss_scale * uv_scale_y)`。

#### GlobalUBO PCSS 字段

扩展现有 GlobalUBO（656 → 720 bytes），不新增 binding（理由同「GlobalUBO Shadow 数据布局」）。

新增字段：

| 字段 | 类型 | 用途 |
|------|------|------|
| `shadow_mode` | `uint` | 0=PCF, 1=PCSS，shader 分支选择 |
| `cascade_light_size_uv` | `vec4` | per-cascade `LIGHT_SIZE_UV`（光源角尺寸的 UV 投影，基于 `width_x`；Blocker search 取 `max(此值, kMaxPenumbraTexels × texel_size)` 作为搜索半径） |
| `cascade_pcss_scale` | `vec4` | per-cascade NDC 深度差→UV 半影宽度缩放因子（`depth_range × 2tan(θ/2) / width_x`，U 方向） |
| `cascade_uv_scale_y` | `vec4` | per-cascade UV 各向异性校正（`width_x / width_y`），V 方向乘此比值 |

#### Shadow Mode 运行时切换

`shadow_mode` 在 shader 中作为 `blend_cascade_shadow()` 内部的分支条件，选择调用 `sample_shadow_pcf()` 或 `sample_shadow_pcss()`。整个 PCSS 路径（blocker search + 半影估算 + variable-width PCF）封装在 `sample_shadow_pcss()` 一个函数中，与现有 PCF 路径完全独立——切换不影响 cascade 选择、blend、distance fade 逻辑。

保留 PCF 的价值：作为对照基准验证 PCSS 正确性，以及在 PCSS 不必要时（纯室内场景、低端 GPU）提供低开销替代。

---

## Instancing

### 背景

大型场景 hundreds of unique mesh 被实例化为数千 instance。当前每个 visible instance 独立 draw call（push constant + bind VB/IB + drawIndexed），密集区域数千 draw calls。瓶颈在 CPU 侧 draw call 提交开销。Phase 4 CSM shadow pass 将进一步增加 draw call（4 cascade × 全部场景物体）。

### 设计

**选择：CPU 侧 mesh_id 分组 + instanced draw + per-instance SSBO**

同一 mesh 的所有 instance 通常共享同一 material（glTF primitive 级别绑定），pipeline state 完全一致。按 `(mesh_id, alpha_mode, double_sided)` 分组后，每组一次 `vkCmdDrawIndexed(instanceCount=N)`，draw call 显著减少（unique mesh 上限，实际更少因为 frustum cull）。分组键包含 `alpha_mode` 和 `double_sided` 以确保同一 group 内 pipeline 和 cull mode 一致——即使同一几何体被不同材质引用也能正确处理。

**Per-instance SSBO 替代 push constant**：

```cpp
// Set 0, Binding 3
struct GPUInstanceData {              // 128 bytes, std430
    mat4 model;                       // 64 bytes
    mat3 normal_matrix;               // 48 bytes — transpose(inverse(mat3(model))), precomputed
    uint material_index;              //  4 bytes
    uint _padding[3];                 // 12 bytes
};
```

Shader 通过 `gl_InstanceIndex` 索引（`vkCmdDrawIndexed` 的 `firstInstance` 参数设置起始偏移）。Normal matrix 在 CPU 端 per-instance 预计算，避免每顶点 mat3 逆运算。Push constant 仅 4 bytes（`cascade_index`，shadow pass 用）。

**分组数据结构**（CPU 侧，不上 GPU）：

```cpp
struct MeshDrawGroup {
    uint32_t mesh_id;
    uint32_t first_instance;          // InstanceBuffer 偏移
    uint32_t instance_count;
    bool double_sided;                // 从 material 缓存
};
```

**帧流程**：
1. Culling 输出 `visible_opaque_indices`（flat list，不变）
2. Renderer 按 `(mesh_id, alpha_mode, double_sided)` 排序 → 构建 MeshDrawGroup + 填充 InstanceBuffer
3. Pass 迭代 draw groups：`set_cull_mode` → `bind_vb/ib` → `draw_indexed(instanceCount=N, firstInstance=offset)`

**InstanceBuffer 管理**：CpuToGpu 内存，per-frame buffer（2 frames in flight），固定大小（kMaxInstances=65536 × 128 bytes = 8 MB），每帧 memcpy 覆写。超出上限时 warn 并丢弃剩余 group。

**透明物体例外**：Blend 不做 instancing——数量少且需要 back-to-front 排序，排序破坏 mesh_id 分组。

**决策演进**：阶段二规划了 Push Constant 增长路径（阶段六 lightmap 数据 → M2 prev_model 超出 128 字节保证），原计划阶段六引入 per-instance SSBO 迁移。阶段四提前完成了此迁移并合并 M3 Instancing，解决大型场景性能痛点。阶段二的原始迁移计划不再适用。

---

## 运行时场景/HDR 加载

### 动机

M1 引入多个测试场景（DamagedHelmet、Sponza、Intel Sponza），频繁切换需要重新编译/重启。CLI11 命令行参数方式不灵活，且场景文件不应由 CMake 拷贝到 build 目录（资产体积大、路径耦合构建系统）。

### 设计

**选择：ImGui 运行时选择 + Windows 原生文件对话框 + JSON 配置持久化**

- **文件选择**：`GetOpenFileNameW` 原生对话框。零额外依赖，用户熟悉的 UI，过滤 .gltf/.glb 和 .hdr
- **配置位置**：`%LOCALAPPDATA%\himalaya\config.json`（Windows 用户数据标准目录，非可执行文件旁）
- **JSON 库**：nlohmann/json（vcpkg，header-only，C++ 事实标准）
- **CLI11 完全移除**：不保留命令行 override，所有配置走 config.json + ImGui

**启动流程**：
1. 读 `config.json` → 获取 `scene_path` 和 `env_path`
2. 分别尝试加载 scene 和 HDR（**独立失败**：scene 失败不影响 HDR，反之亦然）
3. Scene 失败 → 空场景（0 instance），仅 skybox
4. HDR 失败 → 灰色 fallback cubemap（IBL 已有 fallback 机制）
5. 首次运行（无 config）→ 空场景 + fallback cubemap

**运行时切换**：复用 MSAA 切换的 `vkQueueWaitIdle` 模式——`vkQueueWaitIdle` → destroy 旧资源 → load 新资源 → 更新 descriptors → 保存 config.json。

**DebugUI**：Scene / Environment 面板各显示当前文件路径 + "Load..." 按钮。加载失败时显示错误提示（不 abort）。

---

## 缓存基础设施

### 动机

阶段四引入两种缓存需求：BC 纹理压缩缓存（首次 CPU 压缩后缓存 KTX2）和 IBL 预计算缓存（首次 GPU 计算后缓存产物）。两者共享缓存目录解析、内容哈希、路径管理等逻辑。

### 设计

**选择：framework 层轻量缓存工具模块（`framework/cache.h`）**

```cpp
namespace himalaya::framework {
    std::filesystem::path cache_root();                              // %TEMP%\himalaya\
    std::string content_hash(const void* data, size_t size);         // XXH3_128 hex string
    std::string content_hash(const std::filesystem::path& file);     // 文件内容哈希
    std::filesystem::path cache_path(std::string_view category,      // root/category/hash.ext
                                     std::string_view hash,
                                     std::string_view extension);
}
```

模块只提供路径和哈希工具，不知道具体缓存格式（KTX2 / 二进制等）。消费者各自处理序列化。

**缓存位置**：`%TEMP%\himalaya\`（Windows `GetTempPath()`）。Disk Cleanup / Storage Sense 可自动清理，缓存丢了就重算，和 shader cache 同一性质。

**哈希算法**：XXH3_128（xxHash 库）。极快（>10 GB/s on modern CPU）、128 位低碰撞、BSD-2 许可。用 128 位十六进制字符串做文件名。

排除方案：
- `std::hash`：非确定性（跨编译器/运行不一致）
- CRC32：32 位碰撞率过高
- SHA-256：性能过剩（密码学级别不必要）

---

## 纹理压缩与缓存

### 背景

大型场景数百张纹理以 RGBA8 未压缩 + 全 mip chain 上传 GPU，纹理 VRAM 可达数 GB。BC 块压缩是 GPU 纹理的标准解决方案——4:1 压缩比、硬件原生解码、零性能开销。

### 压缩格式选择

**选择：BC7（通用）+ BC5（法线）**

| 纹理类别 | BC 格式 | VkFormat | 理由 |
|----------|---------|----------|------|
| baseColor | BC7 | `BC7_SRGB_BLOCK` | 高质量 4 通道，SRGB 颜色空间 |
| metalRough | BC7 | `BC7_UNORM_BLOCK` | 线性数据，G=roughness B=metallic |
| normal | BC5 | `BC5_UNORM_BLOCK` | 2 通道专用（RG），shader 重建 Z |
| emissive | BC7 | `BC7_SRGB_BLOCK` | SRGB 颜色 |

**为什么法线用 BC5 而非 BC7**：BC5 为 2 通道专用编码（每通道 8 bit），BC7 将精度分散到 4 通道（每通道 ~5-7 bit）。法线只需 RG 两通道，BC5 质量更高。shader 重建 `Z = sqrt(1 - R² - G²)` 改动极小。两者同为 1 byte/pixel，无大小差异。

**为什么不用 BC1**：BC1 是 0.5 byte/pixel（BC7 的一半），但每个 4×4 block 只有 4 个颜色，渐变和微妙色差出现可见 banding。质量代价不值得。

### 压缩编码器

**选择：bc7e.ispc（ISPC SIMD 向量化） + rgbcx（BC4/BC5）**

BC7 编码器使用 Binomial 的 bc7e.ispc（Apache 2.0），通过 ISPC 编译器生成 SSE2/SSE4/AVX2/AVX-512 四种 SIMD 变体，运行时自动选择最佳指令集。支持全部 8 种 BC7 mode，质量预设按构建类型区分：Release 使用 slowest（uber_level=4 + pbit search + 全 partition 搜索），Debug 使用 slow（uber_level=0 + pbit search）以加快迭代。BC4/BC5 编码沿用 rgbcx（算法简单，SIMD 加速无意义）。

ISPC 编译器作为构建依赖，CMake 4.1 原生支持 ISPC 语言（`project(... LANGUAGES CXX ISPC)`），通过 `CMAKE_ISPC_COMPILER` 指定路径。

**演进历史**：初始选择 bc7enc（纯 C++ 标量，4 mode），后因首次压缩耗时过长升级为 bc7e.ispc。bc7e 在最高质量设置下仍显著快于旧 bc7enc（SIMD 并行 + 更优算法），同时质量更高（8 mode 全支持）。

排除方案：
- **bc7enc**（原方案）：纯 C++ 标量实现，仅 4 mode，最高质量（uber_level=4）下速度慢
- **ISPCTextureCompressor**：Intel 已于 2024.9 归档停更
- **GPU compute BC7**：Vulkan 上比 CPU 慢（bc7e-on-gpu 实验结论），BC7 搜索空间大不适合 GPU
- **KTX-Software 内置 UASTC**：需每次加载 transcode，直接 BC7 缓存加载更快

### KTX2 缓存

**选择：首次压缩后缓存为 KTX2，后续直接加载 BC 数据**

```
首次: hash 源字节 → 缓存 miss → 解码 PNG → CPU mip → BC 压缩 → 上传 GPU + write_ktx2() 写缓存
缓存: hash 源字节 → 缓存 hit → read_ktx2() → upload_image_all_levels()（跳过解码和压缩）
```

KTX2 是 Khronos 官方纹理容器格式，天然支持 BC 格式 + 多 mip 存储。KTX2 读写通过自写最小模块（`framework/ktx2.h/cpp`）实现，不依赖 libktx。设计细节见下文「KTX2 读写模块」。Vulkan 上传走 `ResourceManager::upload_image_all_levels()`，设计细节见下文「多级上传 API」。

**缓存位置**：`%TEMP%\himalaya\textures\`。Windows Disk Cleanup / Storage Sense 可自动清理，不污染用户数据目录。缓存丢了就重新压缩，和 shader cache 同一性质。

**缓存 key**：源文件原始字节（JPEG/PNG 编码数据）的 XXH3_128 哈希 + 格式后缀（`_bc7s`/`_bc7u`/`_bc5u`）。基于源字节而非解码后像素，缓存命中时完全跳过图像解码。文件内容变化自动重新压缩。

### Mip 生成

**选择：CPU 端 stb_image_resize2 替代 GPU vkCmdBlitImage**

BC 纹理不支持 `vkCmdBlitImage`（GPU 不能逐像素写入 BC block）。CPU 端逐级缩放后每级独立压缩。stb_image_resize2 是 stb 系列头文件库，已有 stb_image 基础，集成成本为零。Color 纹理（BC7_SRGB）使用 `stbir_resize_uint8_srgb()` 做 gamma-correct 降采样，Linear/Normal 纹理使用 `stbir_resize_uint8_linear()`。

### 非 4 对齐纹理

**选择：resize 到 4 的倍数**

BC block 是 4×4 像素。宽或高不是 4 的倍数的纹理在压缩前 resize 到 `(dim + 3) & ~3`。UV 偏差 < 0.1%，肉眼不可见。大多数场景纹理恰好都是 4 对齐的，这是通用防护。

### 压缩并行度

**选择：纹理级 OpenMP 并行（`#pragma omp parallel for schedule(dynamic)`）**

多张纹理同时压缩，利用多核 CPU。`schedule(dynamic)` 适应纹理大小差异（大纹理比小纹理慢得多）。首次加载数百张纹理的压缩时间从分钟级降到可接受范围。实现简单，无需 block 级并行。

---

## KTX2 读写模块

### 背景

纹理压缩缓存（D-2）和 IBL 缓存（D-3）都需要将 GPU 纹理数据持久化为文件再读回。KTX2 是 Khronos 官方纹理容器格式，天然支持 BC 压缩 + cubemap + mip chain。

### 设计

**选择：自写最小 KTX2 读写模块（`framework/ktx2.h/cpp`），不依赖 libktx**

libktx（vcpkg `ktx` port）构建失败（GitHub 源码包 tar 解压已知问题：vcpkg#47514、#42381），且我们实际只需 6 种格式的 2D/cubemap + mip chain 读写，不需要 Basis Universal、supercompression、格式转码等 libktx 的大部分功能。自写模块约 200-300 行代码，零外部依赖，完全可控。

排除方案：
- **libktx（vcpkg）**：vcpkg 构建失败（tar 解压已知 bug），FetchContent 可能遇到相同构建问题
- **自定义二进制格式**：外部工具（RenderDoc、KTX viewer）无法检视缓存文件，不利于调试

**支持范围**：
- 6 种格式：BC7_SRGB、BC7_UNORM、BC5_UNORM、R16G16B16A16_SFLOAT、B10G11R11_UFLOAT_PACK32、R16G16_UNORM
- 2D 纹理（faceCount=1）和 cubemap（faceCount=6）
- 任意 mip level 数量
- 无 supercompression（scheme=0）
- 无纹理数组（layerCount=0）

**DFD（Data Format Descriptor）处理**：KTX2 规范要求每个文件包含有效 DFD。对 6 种支持格式通过 `build_dfd()` 按 Khronos Data Format Specification 参数化构建。读取时不解析 DFD——仅用 header 的 `vkFormat` 字段识别格式。DFD 的存在是为了让外部工具能正确识别文件格式。

**文件布局**（按 KTX2 规范）：identifier → header → index section → level index → DFD → mip data（从最小 mip 到最大 mip）。mip 数据间按 `lcm(block_bytes, 4)` 对齐。

**读取路径**：读整个文件 → 校验 identifier + header（`Ktx2FileHeader` packed struct） → 解析 level index → 提取 mip 数据到连续 buffer（剥离 KTX2 元数据） → 返回 `Ktx2Data`（仅含 mip 数据 + 每级 offset/size 索引），消费者直接传给 `upload_image_all_levels()`。

**写入路径**：write-to-temp + `std::filesystem::rename` 原子写入，防止崩溃产生半写文件。

---

## 多级上传 API

### 背景

现有 `upload_image()` 只上传 mip 0，然后 `generate_mips()` 通过 GPU blit 生成后续 mip。BC 纹理不支持 `vkCmdBlitImage`（GPU 无法逐像素写入 BC block），BC 和 IBL 缓存的 mip chain 是预建好的，需要一次性全部上传。

### 设计

**选择：`ResourceManager::upload_image_all_levels()`，单 staging buffer + 批量 `vkCmdCopyBufferToImage2`**

一个 staging buffer 装全部 mip 数据（memcpy 一次），对每个 mip level 构建一个 `VkBufferImageCopy2` region，`layerCount` 设为 image 的 `array_layers`（cubemap=6, 2D=1），单次 `vkCmdCopyBufferToImage2` 提交所有 region。

KTX2 的每级内数据布局（face 0, face 1, ..., face 5 连续排列）与 Vulkan 的 `layerCount > 1` buffer-to-image copy 约定天然一致——零数据重排。

排除方案：
- **多次 `upload_image()`**：每次创建独立 staging buffer + barrier + copy，N mip × 6 faces = 大量冗余同步
- **扩展现有 `upload_image()` 参数**：会使简单 case（mip 0 only）的 API 变复杂，保持两个函数各司其职更清晰

---

## IBL 缓存

### 背景

IBL 管线每次启动重新计算全部产物。主要耗时在 prefiltered environment map（512² × 6 faces × 10 mip × 1024 samples/texel），占启动时间约 95%。environment.hdr 89 MB 对应 2048 face cubemap。

### 设计

**选择：GPU 预计算后 readback 缓存为 KTX2，后续直接加载**

缓存分为两组，独立检查：

| 组 | 产物 | 格式 | 大小 | cache key |
|----|------|------|------|-----------|
| BRDF | BRDF LUT | R16G16_UNORM, 256² | ~0.25 MB | 固定 key（与 HDR 无关） |
| Cubemaps | Skybox cubemap | BC6H UFloat, 2048², 6 faces, mip 0 only | ~24 MB | HDR 内容哈希 |
| Cubemaps | Irradiance | R11G11B10F, 32², 6 faces | ~0.03 MB | HDR 内容哈希 |
| Cubemaps | Prefiltered | BC6H UFloat, 512², 6 faces + mips | ~2 MB | HDR 内容哈希 |

```
init() 流程:
  1. 哈希 HDR 文件 → hdr_hash
  2. 检查 BRDF 缓存（固定 key）和 Cubemaps 缓存（hdr_hash）
  3. 两组各自决定：
     命中 → read_ktx2(BC6H) + upload
     未命中 → compute → strip_skybox_mips → BC6H compress → readback + write_ktx2
  4. 注册 bindless

场景                           BRDF         Cubemaps
───────────────────────────────────────────────────────
首次启动（无缓存）              compute      compute
同 HDR 重启（全命中）           缓存加载     缓存加载
切换新 HDR（BRDF 命中）         缓存加载     compute
缓存被清除                      compute      compute
```

KTX2 天然支持 cubemap 存储（`numFaces = 6` + mip chain），与纹理缓存共用自写 KTX2 读写模块。

**Readback 方案**：compute 路径在 `end_immediate()` 之前，通过 per-product staging buffer + `vkCmdCopyImageToBuffer` 读回需要缓存的产物。immediate scope 内一次性开销，不影响运行时帧率。per-product 独立 staging buffer。BC6H 压缩后 readback 总量从 ~208 MB 降至 ~26 MB。

**BRDF LUT 永久缓存**：BRDF Integration LUT 仅依赖 GGX BRDF 公式，与输入 HDR 无关。使用固定 cache key，更换 HDR 时不重算——切换 HDR 只需重算 3 个 cubemap。

