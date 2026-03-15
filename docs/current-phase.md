# 当前阶段：M1 阶段四 — 阴影

> 目标：在阶段三的 PBR 光照基础上，实现方向光 CSM 阴影 — 多级 cascade、PCF 软阴影、cascade 混合。
> 引入 RenderFeatures 运行时开关机制，为后续可选效果（SSAO、Contact Shadows 等）奠定基础。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

#### 依赖关系

```
Step 1: RenderFeatures 基础设施
    ↓
Step 2: Shadow 资源 + ShadowPass + 单 cascade
    ↓
Step 3: Forward 集成 + shadow.glsl + 硬阴影
    ↓
Step 4: 多 cascade + PSSM 分割策略
    ↓
Step 5: Texel snapping + cascade 可视化 + runtime config change
    ↓
Step 6: PCF + cascade blend + 剔除泛化 + 最终验证
```

#### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | RenderFeatures 基础设施 | Skybox 可通过 DebugUI 开关，无 validation 报错 |
| 2 | Shadow 资源 + ShadowPass + 单 cascade | RenderDoc 可检查 shadow map 深度内容正确 |
| 3 | Forward 集成 + 硬阴影 | 场景有可见硬阴影，Shadow toggle 可开关 |
| 4 | 多 cascade + PSSM 分割 | 近中远距离均有合理阴影覆盖 |
| 5 | Texel snapping + cascade 可视化 + runtime config | 相机移动时阴影边缘无闪烁，cascade 可视化正确，cascade 数/分辨率可运行时切换 |
| 6 | PCF + cascade blend + 剔除泛化 + 最终验证 | 阴影边缘柔和，cascade 过渡平滑，per-cascade 剔除生效 |

---

### Step 1：RenderFeatures 基础设施

- 在 `framework/scene_data.h` 新增 `RenderFeatures` 结构体（`skybox` + `shadows` 两个 bool，默认 true）
- 在 `framework/scene_data.h` 新增 `ShadowConfig` 结构体（split_lambda、max_distance、bias 参数、pcf_radius、blend_width），阶段四全部 shadow DebugUI 参数的集中定义
- GlobalUBO 新增 `feature_flags`（uint32_t bitmask），`bindings.glsl` 新增 `#define FEATURE_SHADOWS (1u << 0)` 常量
- FrameContext 新增 `features` 和 `shadow_config` 指针
- RenderInput 新增 `features` 和 `shadow_config` 引用
- Renderer 根据 `features.skybox` 条件调用 `skybox_pass_.record()`
- SceneLoader 计算并暴露场景 AABB（`scene_bounds()`：所有 mesh instance 的 `world_bounds` 求并集）
- Application 在场景加载后根据 scene AABB 初始化 `shadow_config.max_distance`（`diagonal × 1.5`，退化时 fallback 100m）
- DebugUI 新增 Features 面板（Skybox checkbox）
- Shader 热重载：各 pass 新增 `rebuild_pipelines()` 公开方法（调用已有 `create_pipelines()`），DebugUI 新增 "Reload Shaders" 按钮，Renderer 检测触发后 `vkQueueWaitIdle()` → 遍历所有 pass `rebuild_pipelines()`
- **验证**：Skybox 可通过 DebugUI 切换开/关，无 validation 报错；修改 shader 后点击 Reload 按钮生效

#### 设计要点

RenderFeatures + feature_flags 机制见 `milestone-1/m1-design-decisions.md`「Pass 运行时开关」。热重载机制见「Pipeline 创建与热重载预留」。

关键设计：
- RenderFeatures 控制 Renderer 是否调用 pass 的 `record()`，feature_flags 控制 shader 是否采样被禁用 pass 的输出
- Skybox 不需要 feature_flags（独立 RG pass，不调用 `record()` 即跳过，forward.frag 不采样 skybox 数据）
- Shadow 需要 feature_flags（forward.frag 采样 Set 2 binding 5，PARTIALLY_BOUND 未绑定 binding 为未定义行为）
- ShadowConfig 在此 Step 定义但部分字段在后续 Step 才使用（DebugUI 控件随 Step 逐步添加）
- Shadow max_distance 自动初始化见 `milestone-1/m1-design-decisions.md`「Shadow Max Distance 初始化」：SceneLoader 暴露 `scene_bounds()`，Application 设置 `max_distance = diagonal × 1.5`
- 热重载基于 `create_pipelines()` 预留结构，ShaderCompiler 缓存 key 基于源码文本，include 变化通过内容比对检测

---

### Step 2：Shadow 资源 + ShadowPass + 单 cascade 深度渲染

- Shadow map 2D Array 资源创建（D32Sfloat，Absolute 2048²，初始 1 layer），ShadowPass 自管理（非 RG managed），每帧 `import_image()` 到 RG
- Shadow comparison sampler 创建（Reverse-Z：`GREATER_OR_EQUAL` compare op），Renderer 持有
- Per-layer VkImageView 创建（ShadowPass 持有）
- Set 2 binding 5 descriptor 写入（shadow map image + comparison sampler）
- `bindings.glsl` 声明 `layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map`
- ShadowPass 类创建（`passes/shadow_pass.h/cpp`），方法集：`setup()` / `record()` / `destroy()` / `on_shadow_config_changed()`
- Shadow pass 使用 Reverse-Z（clear 0.0，depth compare GREATER）
- 光空间正交投影矩阵计算：fit 相机 frustum（cascade=1 = 整个 frustum）
- Opaque pipeline：仅 VS（`shadow.vert`），无 FS，depth-only 渲染
- Mask pipeline：VS（`shadow.vert`）+ FS（`shadow_masked.frag`），alpha test + discard
- 绘制顺序：先 opaque 批次，再 mask 批次，暴力全画全部场景物体
- GlobalUBO 新增 shadow 字段（`cascade_view_proj[4]`、`cascade_splits`、shadow 参数），`bindings.glsl` 同步更新
- PushConstantData 扩展为 72 bytes（新增 `cascade_index`）
- ShadowPass 提供 `shadow_map_image()` getter 供 Renderer 更新 Set 2
- **验证**：RenderDoc 检查 shadow map 内容——应看到从光源视角的场景深度图

#### 设计要点

Shadow map 资源管理策略见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统 — Shadow Map 资源管理」。

关键设计：
- Shadow map 不走 RG managed（性质不同于屏幕尺寸 render target：Absolute 固定尺寸、array 纹理、需要 per-layer view），ShadowPass 完全拥有资源生命周期
- 每帧 `import_image()` 到 RG（`initial_layout = UNDEFINED`，`final_layout = SHADER_READ_ONLY_OPTIMAL`），RG 管理 barrier
- Shadow pass 注册为单个 RG pass，内部循环 cascade（cascade=1 时循环一次）。不拆为多个 RG pass 避免冗余 WAW barrier
- Opaque pipeline 无 FS（深度由光栅器直接写入），性能最优。Mask pipeline 有 FS 做 alpha test
- `shadow.vert` 输出 `gl_Position`（光空间变换）+ `uv0`（mask 用）：`gl_Position = global.cascade_vp[push.cascade_index] * push.model * vec4(in_position, 1.0)`
- PushConstantData 从 68→72 bytes（新增 `cascade_index`），所有 pipeline 共享同一 layout。Forward/PrePass 不读 `cascade_index`

---

### Step 3：Forward 集成 + common/shadow.glsl + 硬阴影

- 创建 `shaders/common/shadow.glsl`：`select_cascade()`（暂返回 0）、`sample_shadow()`（单次硬件比较，无 PCF）、`shadow_distance_fade()`
- `forward.frag` 集成 shadow 采样，`feature_flags` 条件分支守护
- 硬件 depth bias（constant + slope-scaled）通过 `vkCmdSetDepthBias` 在 ShadowPass record 中设置
- Normal offset bias 在 `shadow.glsl` 的 `sample_shadow()` 中实现：沿法线偏移采样位置，偏移量与 cascade texel 世界尺寸成正比（从 `cascade_view_proj` 矩阵提取）
- DebugUI Shadow 面板：bias 滑条（constant、slope、normal offset）+ Shadow toggle（对应 `features.shadows`）
- **验证**：场景有可见硬阴影（锐利边缘），Shadow toggle 可开关，无明显 acne 或 peter panning

#### 设计要点

shadow.glsl 采用分步函数设计见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统 — Shader 接口设计」。

关键设计：
- 分步函数（`select_cascade` / `sample_shadow` / `shadow_distance_fade`），forward.frag 自行组装调用流程，方便 debug 访问中间结果（如 cascade index）
- 不做单一 `evaluate_shadow()` 包装——cascade index 被隐藏会妨碍 debug cascade 可视化
- 硬件 bias（constant + slope）在渲染端，normal offset 在采样端，三者互补覆盖所有表面角度
- Normal offset 通过 `cascade_view_proj` 矩阵提取正交投影范围，自动计算 per-cascade texel world size，无需额外 UBO 字段

---

### Step 4：多 cascade + PSSM 分割策略

- Cascade 数量从 1 提升到 4，shadow map array layers 对应调整
- Practical（PSSM）分割策略：`C_i = λ × C_log + (1 - λ) × C_lin`，lambda 默认 0.75
- Per-cascade 光空间 frustum tight fitting（正交投影紧密包围 cascade sub-frustum）
- `shadow.glsl` 的 `select_cascade()` 更新为基于 view-space depth 与 `cascade_splits` 比较
- ShadowPass `record()` 循环 4 次，每次渲染到不同 array layer，per-cascade debug label
- DebugUI 扩展：split lambda 滑条、shadow max distance 对数滑条
- **验证**：近中远距离均有合理阴影覆盖，各 cascade 范围合理

#### 设计要点

分割策略见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统 — Cascade 分割策略」。

关键设计：
- `shadow_max_distance` 在 Step 1 由 scene AABB 自动初始化，此处 DebugUI 对数滑条允许手动覆盖。对数刻度使低值区间更精细，适合不同场景尺度
- `cascade_splits` 以 view-space depth 存储在 GlobalUBO `vec4` 中（最多 4 cascade 恰好一个 vec4）
- Renderer 每帧根据 camera + light direction + ShadowConfig 计算 cascade_view_proj 和 cascade_splits，写入 GlobalUBO

---

### Step 5：Texel snapping + cascade 可视化 + runtime config change

- Texel snapping：cascade 正交投影的 min/max 对齐到 shadow map texel 网格，消除相机移动时的阴影边缘闪烁
- Debug render mode 新增 cascade index 可视化（`DEBUG_MODE_SHADOW_CASCADES`，每个 cascade 用不同颜色标注），追加到 passthrough 模式末尾，添加到 DebugUI 渲染模式下拉列表
- Shadow config runtime change：`Renderer::handle_shadow_config_changed()` 支持运行时切换 cascade 数量和分辨率（`vkQueueWaitIdle` → ShadowPass 重建资源 → 更新 Set 2）
- DebugUI 扩展：cascade 数量下拉（2/3/4）、分辨率下拉（512/1024/2048/4096）
- DebugUI Shadow 面板底部新增 cascade 统计信息：每个 cascade 的覆盖范围（近/远边界 m）和 texel density（px/m），辅助理解 max_distance / cascade count / resolution 三者交互
- **验证**：相机移动/旋转时阴影边缘稳定无闪烁，cascade 可视化显示正确分层，cascade 数量和分辨率可运行时切换

#### 设计要点

Texel snapping 是 CSM 的标准 artifact 防控措施。Runtime config change 沿用 MSAA 切换模式。

关键设计：
- Texel snapping 在 per-cascade 正交投影矩阵计算中实现：将投影边界 snap 到 texel 对齐的位置
- Runtime config change 流程：DebugUI → Application 检测 → `renderer_.handle_shadow_config_changed(new_count, new_resolution)` → `vkQueueWaitIdle` → ShadowPass `on_shadow_config_changed()` 重建 image + views → Renderer 更新 Set 2 binding 5。与 `handle_msaa_change()` 统一模式
- cascade 可视化：`DEBUG_MODE_SHADOW_CASCADES` 追加到 passthrough 模式末尾（`>= DEBUG_MODE_PASSTHROUGH_START` 自动跳过 ACES），forward.frag 根据 `select_cascade()` 返回的 cascade index 输出对应颜色
- cascade 统计信息：与 cascade 可视化（颜色覆盖看空间分布）互补，提供数值指标帮助调参

---

### Step 6：PCF + cascade blend + 剔除泛化 + 最终验证

- `shadow.glsl` 新增 `sample_shadow_pcf()`：可配置 kernel（1/3/5/7），基于硬件 2×2 PCF 的多次采样
- Cascade blend：相邻 cascade 的 blend region 内线性插值两个 cascade 的 shadow 值，blend 逻辑封装为 `shadow.glsl` 独立函数 `blend_cascade_shadow()`（预留 dithering 切换）
- Distance fade：shadow max distance 边缘渐变到无阴影（1.0）
- Normal offset bias 与 PCF 配合调参
- Culling 模块重构：`framework/culling.h` 重构为纯几何剔除（`Frustum` + `extract_frustum` + `cull_against_frustum`，预分配 buffer），删除混合了分桶和排序的旧 `perform_culling()`
- ShadowPass per-cascade 调用 `cull_against_frustum()` 做光空间剔除（输入全部场景物体），再按 alpha_mode 分桶为 opaque/mask 列表
- 现有相机 frustum cull 迁移到通用接口：`cull_against_frustum()` + 调用方内联分桶（opaque/transparent）+ 透明排序
- DebugUI 扩展：PCF radius 下拉、cascade blend width 滑条
- **最终验证**：阴影边缘柔和（PCF），cascade 过渡平滑（blend），各距离阴影质量合理，per-cascade 剔除生效（RenderDoc 对比 draw call 数量变化）

#### 设计要点

PCF 和 cascade blend 见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统」。剔除重构见「Per-cascade 剔除与 Culling 模块重构」。

关键设计：
- PCF 基于 `sampler2DArrayShadow` 的硬件比较：每次 `texture()` 调用返回 2×2 双线性比较结果，多次偏移采样取平均
- Cascade blend 只在 blend region 内双重采样（~10% 阴影像素），性能开销有限。最后一级 cascade fade-out 复用 blend 逻辑
- Culling 重构为纯几何：`culling.h` 只做 AABB-frustum 测试，材质分桶和排序在调用方。`cull_against_frustum` 使用调用方预分配 buffer（跨帧复用零分配）
- Shadow cull 输入是全部场景物体（非相机剔除子集），cull 结果按 alpha_mode 分桶为 opaque/mask（Blend 跳过，不投射阴影）
- Cascade blend 使用 lerp（blend region 内双重采样），blend 逻辑封装为 `blend_cascade_shadow()` 独立函数预留 dithering 切换。见 `milestone-1/m1-design-decisions.md`「Cascade 混合策略」

---

## 阶段四帧流程

#### 阶段四结束状态帧流程

```
CSM Shadow Pass（单 RG pass，内部循环 4 cascade）
  输入: 全部场景物体（brute force → Step 6 per-cascade 剔除）
  输出: shadow_map (2D Array, 4 layers, D32Sfloat)
    ↓
DepthPrePass (MSAA)
  输出: msaa_depth, msaa_normal → resolve: depth, normal
    ↓
ForwardPass (MSAA, depth EQUAL write OFF)
  读: shadow_map (Set 2 binding 5, feature_flags 守护)
  输出: msaa_color → resolve: hdr_color
    ↓
SkyboxPass (1x, resolved buffer)
  输出: hdr_color (天空像素)
    ↓
TonemappingPass (1x)
  输出: swapchain
    ↓
ImGuiPass (1x)
    ↓
Present
```

---

## 阶段四文件清单

```
passes/
├── include/himalaya/passes/
│   └── shadow_pass.h            # [Step 2 新增] CSM 阴影 pass
└── src/
    └── shadow_pass.cpp          # [Step 2 新增]
shaders/
├── shadow.vert                  # [Step 2 新增] Shadow VS（position + uv0 输出）
├── shadow_masked.frag           # [Step 2 新增] Shadow mask FS（alpha test + discard）
└── common/shadow.glsl           # [Step 3 新增] CSM cascade 选择、PCF 采样、distance fade
```

修改文件（随 Step 推进）：

```
framework/
├── include/himalaya/framework/
│   ├── scene_data.h             # [Step 1] RenderFeatures + ShadowConfig
│   ├── frame_context.h          # [Step 1] shadow_map + features + shadow_config
│   └── culling.h                # [Step 6] 通用 frustum 剔除
app/
├── include/himalaya/app/
│   └── renderer.h               # [Step 1-2] RenderInput 扩展 + handle_shadow_config_changed
├── src/
│   ├── renderer.cpp             # [Step 2-6]
│   ├── application.cpp          # [Step 1-5] ShadowConfig + RenderFeatures
│   └── debug_ui.cpp             # [Step 1-6] Features 面板 + Shadow 面板
shaders/
├── common/bindings.glsl         # [Step 1-2] feature_flags + shadow UBO 字段
└── forward.frag                 # [Step 3] shadow 采样集成
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| Shadow map 所有权 | ShadowPass 自管理（非 RG managed），每帧 `import_image()` 到 RG。与 IBL 自管理 cubemap 的模式一致，原因是 shadow map 性质不同于屏幕尺寸 render target |
| Shadow map 深度约定 | Reverse-Z（与主相机一致），clear 0.0，depth compare GREATER |
| Texture 2D Array | 每 cascade 一个 layer，所有 cascade 同分辨率。`sampler2DArrayShadow` 硬件 PCF + 无边界溢出问题 |
| RG pass 结构 | 单个 RG pass 内部循环 cascade。不拆多个 RG pass，避免冗余 WAW barrier（RG 不追踪 subresource） |
| PushConstantData | 72 bytes（新增 `cascade_index`），所有 pipeline 共享。Forward/PrePass 不读 `cascade_index` |
| 硬件 bias vs shader bias | Constant + slope-scaled 通过 `vkCmdSetDepthBias`（渲染端），normal offset 在 `shadow.glsl`（采样端），三者互补 |
| Alpha mask 阴影 | ShadowPass 双 pipeline（opaque 无 FS + mask 有 FS discard），与 DepthPrePass 模式一致 |
| Cascade blend | Lerp blend（blend region 内双重采样），`blend_cascade_shadow()` 隔离 blend 策略预留 dithering 切换 |
| Per-cascade 剔除 | Step 2-5 暴力全画（正确性优先），Step 6 泛化 Culling 模块支持任意 frustum 剔除 |
| RenderFeatures | Step 1 引入骨架（skybox + shadows），阶段五扩展 ssao + contact_shadows |
| feature_flags | GlobalUBO uint bitmask，shader 动态分支守护采样。Skybox 不需要（独立 pass 跳过即可），Shadow 需要（forward.frag 采样 Set 2） |
| ShadowConfig 位置 | `framework/scene_data.h`，与 RenderFeatures 同位。Application 持有实例，DebugUI 直接操作 |
| Shadow max_distance 初始化 | SceneLoader 暴露 `scene_bounds()`，Application 加载后设 `max_distance = diagonal × 1.5`（退化时 fallback 100m）。DebugUI 对数滑条可覆盖 |
| Cascade 统计信息 | DebugUI Shadow 面板底部显示每个 cascade 的范围（近/远 m）和 texel density（px/m），辅助理解 max_distance / cascade count / resolution 的交互 |
| Runtime config change | Cascade 数量和分辨率可运行时调整，沿用 MSAA 切换模式（`vkQueueWaitIdle` → 重建资源 → 更新 descriptor） |
| Cascade 可视化 | `DEBUG_MODE_SHADOW_CASCADES` 追加到 passthrough 模式末尾，`>= DEBUG_MODE_PASSTHROUGH_START` 自动跳过 ACES |
| GlobalUBO 增长 | 336 → 624 bytes（+288），仍远小于 16KB 最低保证 |
| Pass 虚基类 | 阶段四不引入，M1 全程具体类。M2 评估 |
