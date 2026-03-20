# 当前阶段：M1 阶段四 — 阴影

> 目标：在阶段三的 PBR 光照基础上，实现方向光 CSM 阴影 — 多级 cascade、PCF 软阴影、cascade 混合。
> 引入 RenderFeatures 运行时开关机制，为后续可选效果（SSAO、Contact Shadows 等）奠定基础。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

### 准备工作 A：测试场景

- 补充适合 CSM 阴影测试的室外场景（Intel Sponza）— **已完成**

---

### 准备工作 B：Instancing

> 大型场景 hundreds of unique mesh 被实例化为数千 instance。当前每个 visible instance 独立 draw call，密集区域数千 draw calls。
> 瓶颈在 CPU 侧 draw call 提交开销而非 GPU 三角处理。Phase 4 引入 CSM shadow pass 后 draw call 进一步翻倍。
> 同一 mesh 的所有 instance 通常共享同一 material（glTF primitive 级别绑定），instancing 无 state change 障碍（实现按 mesh_id + alpha_mode + double_sided 分组以处理边缘情况）。

- Per-instance SSBO（Set 0, Binding 3）替代 push constant 传递 model matrix + material_index
- Shader 通过 `gl_InstanceIndex` 索引 instance 数据，push constant 缩减为 `cascade_index`（4 bytes，shadow pass 用）
- 剔除后按 (mesh_id, alpha_mode, double_sided) 排序分组构建 MeshDrawGroup 列表 + 填充 InstanceBuffer（含 overflow guard）
- 各 pass draw loop 改为 group iteration：bind VB/IB 一次 → `draw_indexed(instanceCount=N, firstInstance=offset)`
- DepthPrePass opaque groups 和 mask groups 分别迭代
- **验证**：密集场景 draw call 显著减少，渲染结果一致

#### 设计要点

Instancing 设计见 `milestone-1/m1-design-decisions.md`「Instancing」。

关键设计：
- CullResult 不变（仍输出 flat visible indices），分组逻辑在 Renderer 侧——不同消费者可按需分组
- InstanceBuffer 使用 CpuToGpu 内存 per-frame 写入，固定大小（场景最大 instance 数），无需每帧重建 GPU buffer
- `vkCmdDrawIndexed` 的 `firstInstance` 参数作为 instance SSBO 的起始偏移，无需额外 push constant
- 透明物体（Blend）不做 instancing（数量少且需要 back-to-front 排序）
- 此变更提前了原计划阶段六的 per-instance SSBO 迁移，同时实现了 M3 规划的 Instancing

---

### 准备工作 C：运行时场景/HDR 加载 + 配置持久化

> 退役 CLI11 命令行参数，改为 ImGui 运行时选择 glTF 场景和 HDR 环境贴图。
> 配置持久化到 `%LOCALAPPDATA%\himalaya\config.json`，重启恢复上次文件。
> 加载失败时优雅降级：scene 失败 = 空场景仅 skybox，HDR 失败 = 灰色 fallback cubemap，两者独立。

- 移除 CLI11 依赖，添加 nlohmann/json（vcpkg）
- CMakeLists.txt 移除 scene/HDR 资产拷贝到 build 目录的规则
- `app/config.h/cpp` 新增 AppConfig + JSON load/save
- 启动流程：读配置 → 分别尝试加载 scene 和 HDR → 部分失败时另一项正常加载
- `switch_scene()` / `switch_environment()`：`vkQueueWaitIdle` → destroy → load → 更新 descriptors → 保存配置
- DebugUI：Scene/Environment 面板显示当前路径 + "Load..." 按钮（Windows `GetOpenFileNameW` 原生对话框）
- 加载失败时 DebugUI 显示错误提示，不 abort
- **验证**：运行时切换正常，配置持久化，重启恢复，文件丢失时 fallback 正确

#### 设计要点

运行时加载策略见 `milestone-1/m1-design-decisions.md`「运行时场景/HDR 加载」。

关键设计：
- Scene 和 HDR 独立加载/失败——不因一个失败影响另一个
- Windows 原生文件对话框（`GetOpenFileNameW`），零额外 UI 依赖
- 配置放在 `%LOCALAPPDATA%\himalaya\`（用户数据标准目录），不放在可执行文件旁
- `switch_scene()` 复用 MSAA 切换的 `vkQueueWaitIdle` 模式，SceneLoader 已有 destroy + load
- 空场景 = 0 mesh instances + 0 directional lights，skybox 正常渲染（如果 HDR 成功）

---

### 准备工作 D：缓存基础设施 + BC 纹理压缩 + IBL 缓存

> 三个子系统共享同一缓存模块：`framework/cache.h`（目录解析 + 内容哈希 + 路径拼接）。
> 纹理压缩解决大型场景 VRAM 问题，IBL 缓存解决启动慢问题。

**D-1：缓存模块 + KTX2 读写** — 共享基础设施，包含三部分：
- `framework/cache.h/cpp`：缓存工具函数（`cache_root()`、`content_hash()`、`cache_path()`）
- `framework/ktx2.h/cpp`：最小 KTX2 读写模块（自写，不依赖 libktx），支持 2D / cubemap + mip chain，DFD 按支持格式硬编码
- RHI 扩展：Format 枚举新增 BC 格式 + `from_vk_format()` + 格式工具函数 + `upload_image_all_levels()` 多级上传 API

**D-2：BC 纹理压缩 + KTX2 缓存** — 首次加载时 CPU 端 BC 压缩并缓存为 KTX2，后续直接加载 BC 数据：
- bc7enc（源文件集成）做 BC7/BC5 压缩，`write_ktx2()` 写缓存，`read_ktx2()` + `upload_image_all_levels()` 读缓存
- 法线 → BC5_UNORM（2 通道专用），其他 → BC7_SRGB / BC7_UNORM
- 非 4 对齐纹理 resize 到 4 的倍数，CPU mip 生成（stb_image_resize2，Color 用 sRGB-correct filtering）替代 GPU blit
- 纹理级 OpenMP 并行压缩，缓存路径 `%TEMP%\himalaya\textures\<hash>.ktx2`
- 缓存键基于源文件字节（JPEG/PNG）哈希，缓存命中时完全跳过图像解码
- 三函数拆分：`load_cached_texture()` 缓存查找 / `compress_texture()` 压缩+写缓存 / `prepare_texture()` 便捷包装
- **验证**：纹理 VRAM 显著降低（RGBA8 → BC 约 4:1 压缩比）

**D-3：IBL 缓存** — 首次 GPU 预计算后 readback 并缓存为 KTX2，后续直接加载：
- 缓存分两组独立检查：BRDF LUT（固定 key，永久缓存）+ 3 cubemaps（HDR 内容哈希 key）
- 每组各自决定路径：命中 → `read_ktx2()` + `upload_image_all_levels()` 直接加载；未命中 → GPU compute + readback + `write_ktx2()` 写缓存
- GPU → CPU readback（per-product staging buffer + `vkCmdCopyImageToBuffer`）读回未命中的产物
- 缓存路径 `%TEMP%\himalaya\ibl\<hash>.ktx2`，切换 HDR 时 BRDF 命中只重算 3 cubemaps
- **验证**：后续启动秒级加载，IBL 渲染结果一致

#### 设计要点

缓存基础设施和纹理压缩策略见 `milestone-1/m1-design-decisions.md`「缓存基础设施」「纹理压缩与缓存」「KTX2 读写模块」「多级上传 API」「IBL 缓存」。

关键设计：
- 缓存模块是纯工具层，不知道具体缓存格式——消费者各自处理序列化
- 缓存放在 `%TEMP%`（Windows Disk Cleanup / Storage Sense 可自动清理），缓存丢了就重算
- XXH3_128 哈希：极快（>10 GB/s）、低碰撞、128 位做文件名足够
- KTX2 读写自写（不依赖 libktx）：只需 6 种格式的 2D/cubemap + mip chain 读写，DFD 硬编码，读取只解析 header + level index
- `upload_image_all_levels()` 补充现有 `upload_image()`：单 staging buffer + 多 `VkBufferImageCopy2` region，KTX2 数据布局与 Vulkan buffer-to-image copy 天然兼容
- BC5 法线质量优于 BC7（2 通道专用编码），shader 重建 `Z = sqrt(1 - R² - G²)`
- IBL 缓存分两组独立检查：BRDF LUT（固定 key）和 3 cubemaps（HDR hash），每组各自决定 compute 或缓存加载
- 切换 HDR 时 BRDF 缓存命中直接加载，只重算 3 个 cubemap；readback 使用 per-product staging buffer 避免单次大分配

---

#### 依赖关系

```
Step 1a: RenderFeatures 基础设施
Step 1b: Shader 热重载
Step 1c: Culling 模块重构
    ↓ (1a)
Step 2: Shadow 资源 + ShadowPass + 单 cascade
    ↓
Step 3: Forward 集成 + shadow.glsl + 硬阴影
    ↓
Step 4: 多 cascade + PSSM 分割策略
    ↓
Step 5: Texel snapping + cascade 可视化 + runtime config change
    ↓ (+ 1c)
Step 6: PCF + cascade blend + per-cascade 剔除 + 最终验证
    ↓
Step 7: PCSS（Percentage-Closer Soft Shadows）
```

#### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1a | RenderFeatures 基础设施 | Skybox 可通过 DebugUI 开关，无 validation 报错 |
| 1b | Shader 热重载 | 修改 shader 后点击 Reload 按钮生效 |
| 1c | Culling 模块重构 | 现有相机剔除行为不变，新接口编译通过 |
| 2 | Shadow 资源 + ShadowPass + 单 cascade | RenderDoc 可检查 shadow map 深度内容正确 |
| 3 | Forward 集成 + 硬阴影 | 场景有可见硬阴影，Shadow toggle 可开关 |
| 4 | 多 cascade + PSSM 分割 | 近中远距离均有合理阴影覆盖 |
| 5 | Texel snapping + cascade 可视化 + runtime config | 相机移动时阴影边缘无闪烁，cascade 可视化正确，cascade 数/分辨率可运行时切换 |
| 6 | PCF + cascade blend + per-cascade 剔除 + 最终验证 | 阴影边缘柔和，cascade 过渡平滑，per-cascade 剔除生效 |
| 7 | PCSS（Percentage-Closer Soft Shadows） | 阴影半影随遮挡距离变化（接触硬、远处软），PCF/PCSS 可运行时切换 |

---

### Step 1a：RenderFeatures 基础设施

- 在 `framework/scene_data.h` 新增 `RenderFeatures` 结构体（`skybox` + `shadows` 两个 bool，默认 true）
- 在 `framework/scene_data.h` 新增 `ShadowConfig` 结构体（cascade_count、split_lambda、max_distance、bias 参数、pcf_radius、blend_width、distance_fade_width），无默认值，Application 显式初始化
- GlobalUBO 新增 `feature_flags`（uint32_t bitmask），`bindings.glsl` 新增 `#define FEATURE_SHADOWS (1u << 0)` 常量
- FrameContext 新增 `features` 和 `shadow_config` 指针
- RenderInput 新增 `features` 和 `shadow_config` 引用
- Renderer 根据 `features.skybox` 条件调用 `skybox_pass_.record()`
- SceneLoader 计算并暴露场景 AABB（`scene_bounds()`：所有 mesh instance 的 `world_bounds` 求并集）
- Application 在场景加载后根据 scene AABB 初始化 `shadow_config.max_distance`（`diagonal × 1.5`，退化时保持默认 100m）
- Camera 新增 `compute_focus_position(AABB)` 纯计算方法（包围球半径 + FOV → 距离 → 位置）
- Application 在场景加载后自动定位相机（yaw=0, pitch=-45°, position 由 `compute_focus_position()` 计算，退化时 fallback 默认位置）
- CameraController 新增 `set_focus_target(AABB*)` + F 键 focus（保持朝向，移动到能看到整个场景的位置）
- DebugUI 新增 Features 面板（Skybox checkbox）
- **验证**：Skybox 可通过 DebugUI 切换开/关，F 键 focus 正确定位，无 validation 报错

#### 设计要点

RenderFeatures + feature_flags 机制见 `milestone-1/m1-design-decisions.md`「Pass 运行时开关」。

关键设计：
- RenderFeatures 控制 Renderer 是否调用 pass 的 `record()`，feature_flags 控制 shader 是否采样被禁用 pass 的输出
- Skybox 不需要 feature_flags（独立 RG pass，不调用 `record()` 即跳过，forward.frag 不采样 skybox 数据）
- Shadow 需要 feature_flags（forward.frag 采样 Set 2 binding 5，PARTIALLY_BOUND 未绑定 binding 为未定义行为）
- ShadowConfig 在此 Step 定义但部分字段在后续 Step 才使用（DebugUI 控件随 Step 逐步添加）
- Shadow max_distance 自动初始化见 `milestone-1/m1-design-decisions.md`「Shadow Max Distance 初始化」：Application 初始化 100m 作为退化 fallback，正常场景覆盖为 `diagonal × 1.5`
- 相机自动定位和 F 键 focus 共享 `Camera::compute_focus_position()` 纯计算，见 `milestone-1/m1-design-decisions.md`「相机自动定位与 F 键 Focus」

---

### Step 1b：Shader 热重载

- 各 pass 新增 `rebuild_pipelines()` 公开方法（调用已有 `create_pipelines()`）
- DebugUI 新增 "Reload Shaders" 按钮，Renderer 检测触发后 `vkQueueWaitIdle()` → 遍历所有 pass `rebuild_pipelines()`
- **验证**：修改 shader 后点击 Reload 按钮生效，无 validation 报错

#### 设计要点

热重载机制见 `milestone-1/m1-design-decisions.md`「Pipeline 创建与热重载预留」。

关键设计：
- 热重载基于 `create_pipelines()` 预留结构，ShaderCompiler 缓存 key 基于源码文本，include 变化通过内容比对检测

---

### Step 1c：Culling 模块重构

- `framework/culling.h` 重构为纯几何剔除：`Frustum` 结构体 + `extract_frustum(mat4 vp)` + `cull_against_frustum(instances, frustum, out_visible)`（预分配 buffer 版），删除混合了分桶和排序的旧 `perform_culling()`
- 现有相机 frustum cull 迁移到通用接口：`cull_against_frustum()` + 调用方内联分桶（opaque/transparent）+ 透明排序
- **验证**：现有相机剔除行为不变（渲染输出与重构前一致），无 validation 报错

#### 设计要点

剔除重构见 `milestone-1/m1-design-decisions.md`「Per-cascade 剔除与 Culling 模块重构」。

关键设计：
- Culling 模块只做 AABB-frustum 测试，材质分桶和排序在调用方——不同消费者分桶标准不同（camera: opaque vs transparent，shadow: opaque vs mask）
- `cull_against_frustum` 使用调用方预分配 buffer（跨帧复用零分配）
- 提前重构使 Step 2-5 的 ShadowPass 暴力全画时新接口已就绪，Step 6 直接使用

---

### Step 2 前置：RHI 基础设施扩展

> Step 2 需要 RHI 层若干基础设施扩展。这些是纯机制性变更，不改变现有行为。

- `types.h` 新增 `CompareOp` 自定义枚举（Never / Less / Equal / LessOrEqual / Greater / GreaterOrEqual）+ `to_vk_compare_op()` 转换函数，保持 RHI 层不暴露 Vulkan 类型的抽象一致性
- `SamplerDesc` 新增 `compare_enable` (bool) + `compare_op` (CompareOp)（无默认值，调用方显式初始化）；`create_sampler()` 据此设置 Vulkan 比较采样器字段。Shadow comparison sampler 需要此支持
- `create_image()` 自动推断 2D Array view type：当 `array_layers > 1 && != 6` 时创建 `VK_IMAGE_VIEW_TYPE_2D_ARRAY`（shadow map 始终 4 层，自然命中此条件）
- `GraphicsPipelineDesc` 支持无 FS：当 `fragment_shader == VK_NULL_HANDLE` 时仅包含 VS stage（`stageCount = 1`）。Shadow opaque pipeline 为 depth-only 渲染，无需 FS
- `passes/CMakeLists.txt` 新增 `shadow_pass.cpp` 构建条目

#### 设计要点

- `CompareOp` 自定义枚举而非直接使用 `VkCompareOp`：现有 `SamplerDesc` 全部使用自定义枚举（`Filter`、`SamplerWrapMode`），保持 RHI 抽象层不暴露 Vulkan 类型的一致风格
- `create_image` 2D Array 自动推断无歧义：`array_layers > 1 && != 6` 的非 cubemap 多层纹理在 Vulkan 中只有 `2D_ARRAY` 一种正确 view type。Shadow map 始终 4 层，自然命中此条件，无需额外 flag
- 无 FS pipeline 是 Vulkan 规范允许的 depth-only 渲染优化：无 fragment shader 开销，光栅器直接写入深度

---

### Step 2：Shadow 资源 + ShadowPass + 单 cascade 深度渲染

- Shadow map 2D Array 资源创建（D32Sfloat，Absolute 2048²，固定 4 layers = MAX_SHADOW_CASCADES），ShadowPass 自管理（非 RG managed），每帧 `import_image()` 到 RG
- Shadow comparison sampler 创建（Reverse-Z：`GREATER_OR_EQUAL` compare op），Renderer 持有
- Per-layer VkImageView 创建（ShadowPass 持有）
- Set 2 binding 5 descriptor 写入（shadow map image + comparison sampler）
- `bindings.glsl` 声明 `layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map`
- ShadowPass 类创建（`passes/shadow_pass.h/cpp`），方法集：`setup()` / `record()` / `destroy()` / `on_resolution_changed()` / `rebuild_pipelines()`
- Shadow pass 使用 Reverse-Z（clear 0.0，depth compare GREATER）
- 光空间正交投影矩阵计算：XY fit 相机 frustum（cascade=1 = 整个 frustum），Z 范围扩展到场景 AABB（RenderInput 传入 scene_bounds），避免 frustum 外的 shadow caster 被裁剪
- Opaque pipeline：仅 VS（`shadow.vert`），无 FS，depth-only 渲染
- Mask pipeline：VS（`shadow.vert`）+ FS（`shadow_masked.frag`），alpha test + discard
- Renderer `render()` 新增 shadow draw group 构建：全部 `mesh_instances` 排序分组 → 填充 InstanceBuffer 第二段（camera 区域之后）→ 构建 shadow opaque/mask draw groups；FrameContext 新增 `shadow_opaque_groups` / `shadow_mask_groups` span
- 绘制顺序：先 opaque 批次，再 mask 批次，暴力全画全部场景物体（通过 shadow draw groups 消费）
- GlobalUBO 新增 shadow 字段（`cascade_view_proj[4]`、`cascade_splits`、shadow 参数），`bindings.glsl` 同步更新
- ShadowPass pipeline layout 声明 push constant range（4 bytes `cascade_index`，shadow pass 专用）
- ShadowPass 提供 `shadow_map_image()` getter 供 Renderer 更新 Set 2
- **验证**：RenderDoc 检查 shadow map 内容——应看到从光源视角的场景深度图

#### 设计要点

Shadow map 资源管理策略见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统 — Shadow Map 资源管理」。

关键设计：
- Shadow map 始终创建 4 层（MAX_SHADOW_CASCADES），cascade_count 是纯渲染参数（控制循环次数和 shader 采样范围），不影响资源。未使用的层包含陈旧数据但不被采样。4 × 2048² × 4B = 64MB，桌面 GPU 可接受
- Shadow map 不走 RG managed（性质不同于屏幕尺寸 render target：Absolute 固定尺寸、array 纹理、需要 per-layer view），ShadowPass 完全拥有资源生命周期
- 每帧 `import_image()` 到 RG（`initial_layout = UNDEFINED`，`final_layout = SHADER_READ_ONLY_OPTIMAL`），RG 管理 barrier
- Shadow pass 注册为单个 RG pass，内部循环 cascade（cascade=1 时循环一次）。不拆为多个 RG pass 避免冗余 WAW barrier
- Opaque pipeline 无 FS（深度由光栅器直接写入），性能最优。Mask pipeline 有 FS 做 alpha test
- `shadow.vert` 输出 `gl_Position`（光空间变换）+ `uv0`（mask 用）：`gl_Position = global.cascade_vp[pc.cascade_index] * instances[gl_InstanceIndex].model * vec4(in_position, 1.0)`
- PushConstantData 4 bytes（仅 `cascade_index`），shadow pass pipeline 专用。Forward/PrePass 无 push constant（model + material_index 从 InstanceBuffer SSBO 读取）
- Shadow draw group 构建：Renderer 将全部 `mesh_instances`（非相机剔除子集）排序分组 → 填充 InstanceBuffer 第二段 → 通过 FrameContext 传递给 ShadowPass。相机 frustum 外的物体可能在 shadow map 中可见（屏幕外物体的阴影投射到可见表面）

---

### Step 3 前置：Depth Bias 基础设施

> Step 3 引入硬件 depth bias 对抗 shadow acne，需要 RHI 层支持。

- `GraphicsPipelineDesc` 新增 `depth_bias_enable` (bool, 默认 false)；`create_graphics_pipeline()` 据此设置 `rasterization.depthBiasEnable`。动态状态列表新增 `VK_DYNAMIC_STATE_DEPTH_BIAS`（Vulkan 1.0 核心，对 `depthBiasEnable = false` 的 pipeline 无影响）
- `CommandBuffer` 新增 `set_depth_bias(float constant_factor, float clamp, float slope_factor)` 方法，wrap `vkCmdSetDepthBias`

#### 设计要点

- Depth bias 不在 Step 2 引入：Step 2 只渲染 shadow map 并用 RenderDoc 检查，不做阴影采样，shadow acne 不可见。Step 3 集成前向渲染采样后才需要 bias
- `VK_DYNAMIC_STATE_DEPTH_BIAS` 全局添加但仅在 `depthBiasEnable = true` 的 pipeline 生效，现有 pipeline 行为不变

---

### Step 3：Forward 集成 + common/shadow.glsl + 硬阴影

- 创建 `shaders/common/shadow.glsl`：`select_cascade()`（暂返回 0）、`sample_shadow()`（单次硬件比较，无 PCF）、`shadow_distance_fade()`（使用独立的 `shadow_distance_fade_width` UBO 字段，不复用 `shadow_blend_width`）
- `forward.frag` 集成 shadow 采样：M1 仅 1 盏方向光，shadow 直接乘到该光的直接光照贡献上；`feature_flags` 条件分支守护
- ShadowPass opaque/mask pipeline 启用 `depth_bias_enable = true`（rebuild pipelines）
- 硬件 depth bias（slope-scaled only）通过 `vkCmdSetDepthBias` 在 ShadowPass record 中设置（constant factor 对 D32Sfloat 无效，硬编码 0）
- Normal offset bias 在 `shadow.glsl` 的 `sample_shadow()` 中实现：沿法线偏移采样位置，偏移量与 cascade texel 世界尺寸成正比（从 `cascade_view_proj` 矩阵提取）
- DebugUI Shadow 面板：bias 滑条（slope、normal offset）+ Shadow toggle（对应 `features.shadows`）
- **验证**：场景有可见硬阴影（锐利边缘），Shadow toggle 可开关，无明显 acne 或 peter panning

#### 设计要点

shadow.glsl 采用分步函数设计见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统 — Shader 接口设计」。

关键设计：
- 分步函数（`select_cascade` / `sample_shadow` / `shadow_distance_fade`），forward.frag 自行组装调用流程，方便 debug 访问中间结果（如 cascade index）
- `shadow_distance_fade()` 使用独立 UBO 字段 `shadow_distance_fade_width`（而非复用 `shadow_blend_width`），两者语义不同：blend_width 控制 cascade 间过渡，distance_fade_width 控制远端淡出。当前默认值相同（0.1），后续可独立调参
- 不做单一 `evaluate_shadow()` 包装——cascade index 被隐藏会妨碍 debug cascade 可视化
- 硬件 bias（slope only，constant 对 D32Sfloat 无效）在渲染端，normal offset 在采样端，两者互补覆盖所有表面角度
- Normal offset 通过 `cascade_view_proj` 矩阵提取正交投影范围，自动计算 per-cascade texel world size，无需额外 UBO 字段

---

### Step 4：多 cascade + PSSM 分割策略

- Cascade 渲染数量从 1 提升到 4（shadow map 资源不变，始终 4 层）
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
- Shadow resolution runtime change：`Renderer::handle_shadow_resolution_changed()` 支持运行时切换分辨率（`vkQueueWaitIdle` → ShadowPass 重建资源 → 更新 Set 2）。Cascade 数量切换为纯渲染参数，不需要重建资源
- DebugUI 扩展：cascade count 下拉（1/2/3/4，纯渲染参数）、resolution 下拉（512/1024/2048/4096，触发资源重建）
- DebugUI Shadow 面板底部新增 cascade 统计信息：每个 cascade 的覆盖范围（近/远边界 m）和 texel density（px/m），辅助理解 max_distance / cascade count / resolution 三者交互
- **验证**：相机移动/旋转时阴影边缘稳定无闪烁，cascade 可视化显示正确分层，cascade 数量和分辨率可运行时切换

#### 设计要点

Texel snapping 是 CSM 的标准 artifact 防控措施。Runtime config change 沿用 MSAA 切换模式。

关键设计：
- Texel snapping 在 per-cascade 正交投影矩阵计算中实现：将投影边界 snap 到 texel 对齐的位置
- Runtime resolution change 流程：DebugUI → Application 检测 → `renderer_.handle_shadow_resolution_changed(new_resolution)` → `vkQueueWaitIdle` → ShadowPass `on_resolution_changed()` 重建 image（固定 4 层）+ views → Renderer 更新 Set 2 binding 5。与 `handle_msaa_change()` 统一模式。Cascade 数量切换不触发此流程（纯渲染参数，下一帧自动生效）
- cascade 可视化：`DEBUG_MODE_SHADOW_CASCADES` 追加到 passthrough 模式末尾（`>= DEBUG_MODE_PASSTHROUGH_START` 自动跳过 ACES），forward.frag 根据 `select_cascade()` 返回的 cascade index 输出对应颜色
- cascade 统计信息：与 cascade 可视化（颜色覆盖看空间分布）互补，提供数值指标帮助调参

---

### Step 6：PCF + cascade blend + per-cascade 剔除 + 最终验证

- `shadow.glsl` 新增 `sample_shadow_pcf()`：可配置 kernel，基于硬件 2×2 PCF 的多次采样，radius 由 `shadow_pcf_radius` 控制（0=off, 1=3×3, 2=5×5, ..., 5=11×11）
- Cascade blend：相邻 cascade 的 blend region 内线性插值两个 cascade 的 shadow 值，blend 逻辑封装为 `shadow.glsl` 独立函数 `blend_cascade_shadow()`（预留 dithering 切换）
- Distance fade：shadow max distance 边缘渐变到无阴影（1.0）
- Normal offset bias 与 PCF 配合调参
- ShadowPass per-cascade 调用 `cull_against_frustum()`（Step 1c 已就绪）做光空间剔除（输入全部场景物体），再按 alpha_mode 分桶为 opaque/mask 列表
- DebugUI 扩展：PCF radius 下拉（Off/3×3/5×5/7×7/9×9/11×11）、cascade blend width 滑条
- **最终验证**：阴影边缘柔和（PCF），cascade 过渡平滑（blend），各距离阴影质量合理，per-cascade 剔除生效（RenderDoc 对比 draw call 数量变化）

#### 设计要点

PCF 和 cascade blend 见 `milestone-1/m1-design-decisions.md`「CSM 阴影系统」。

关键设计：
- PCF 基于 `sampler2DArrayShadow` 的硬件比较：每次 `texture()` 调用返回 2×2 双线性比较结果，多次偏移采样取平均。Radius 0-5 覆盖从硬阴影到极软阴影的调试范围，M2 PCSS 实现前也能近似评估不同软阴影程度
- Cascade blend 只在 blend region 内双重采样（~10% 阴影像素），性能开销有限。最后一级 cascade fade-out 复用 blend 逻辑
- Shadow cull 输入是全部场景物体（非相机剔除子集），cull 结果按 alpha_mode 分桶为 opaque/mask（Blend 跳过，不投射阴影）
- Cascade blend 使用 lerp（blend region 内双重采样），blend 逻辑封装为 `blend_cascade_shadow()` 独立函数预留 dithering 切换。见 `milestone-1/m1-design-decisions.md`「Cascade 混合策略」

---

### Step 7：PCSS（Percentage-Closer Soft Shadows）

> 从 M2 提前。在 Step 6 PCF 基础上实现 PCSS，产生距离相关的软阴影——接触点处硬阴影，遮挡距离增大时半影渐宽。
> PCF 和 PCSS 通过 `shadow_mode` 运行时切换，现有 PCF 路径完整保留。

#### 前置：Shadow Depth Sampler

- Renderer 新增 shadow depth sampler（`VK_FILTER_NEAREST`，无 compare，`CLAMP_TO_EDGE`）
- Set 2 新增 binding 6：绑定与 binding 5 同一张 shadow map image，使用 depth sampler（非比较采样器）
- `bindings.glsl` 新增 `layout(set = 2, binding = 6) uniform sampler2DArray rt_shadow_map_depth`
- Descriptor layout 更新（Set 2 新增 binding 6，`PARTIALLY_BOUND`），Renderer `init()` 写入 descriptor
- Shadow 分辨率变更时 `handle_shadow_resolution_changed()` 同步更新 binding 6

**为什么需要**：PCSS Blocker Search 需要读取 shadow map 的原始深度值来累加求平均。`sampler2DArrayShadow`（binding 5）是比较采样器，`texture()` 返回 0/1 比较结果而非深度值，且不支持 `texelFetch`。Binding 6 的普通 `sampler2DArray` 让 Blocker Search 能读取原始 float 深度，PCF 步骤继续使用 binding 5 的硬件比较采样。两个 binding 指向同一张 image、不同 sampler，零额外显存开销。

#### 前置：UBO + ShadowConfig 扩展

- `ShadowConfig` 新增 `shadow_mode`（uint32_t，0=PCF, 1=PCSS）、`light_angular_diameter`（float，弧度，Application 初始化为 0.00925f ≈ 0.53°——太阳角直径）、`pcss_flags`（uint32_t，bit 0: blocker early-out，默认开启）、`pcss_quality`（uint32_t，0=Low, 1=Medium, 2=High，默认 1）
- `GlobalUniformData` 新增字段（656 → 720 bytes）：
  - `uint shadow_mode`：0=PCF, 1=PCSS（shader 分支选择）
  - `uint pcss_flags`：bit 0: `PCSS_FLAG_BLOCKER_EARLY_OUT`（blocker search 全命中时 early-out 返回 0.0，缓解多层遮挡漏光）
  - `uint pcss_blocker_samples`：blocker search 采样数（Low=16, Medium=16, High=32）
  - `uint pcss_pcf_samples`：PCF 采样数（Low=16, Medium=25, High=49）
  - `vec4 cascade_light_size_uv`：per-cascade `LIGHT_SIZE_UV = 2 * tan(θ/2) / width_x`（CPU 预计算，Blocker Search U 方向搜索半径）
  - `vec4 cascade_pcss_scale`：per-cascade NDC 深度差→UV 半影宽度的预计算缩放因子（`depth_range * 2 * tan(θ/2) / width_x`，U 方向，将 NDC→世界→UV 三步合一）
  - `vec4 cascade_uv_scale_y`：per-cascade UV 各向异性校正（`width_x / width_y`），V 方向搜索半径和半影宽度乘此比值
- `bindings.glsl` GlobalUBO 同步新增上述字段

#### 前置：CPU 端 per-cascade 参数计算

- `ShadowCascadeResult` 新增 `cascade_width_x`（vec4）、`cascade_width_y`（vec4）、`cascade_depth_range`（vec4），在 `compute_shadow_cascades` 的 per-cascade 循环中从 `ls_max - ls_min` 存储
- Renderer 每帧计算 per-cascade `LIGHT_SIZE_UV`（以 `width_x` 为基准，U 方向）：
  - `cascade_light_size_uv[i] = 2.0f * tan(angular_diameter / 2.0f) / cascades.cascade_width_x[i]`
- Renderer 每帧计算 per-cascade `pcss_scale`（以 `width_x` 为基准，U 方向）：
  - `cascade_pcss_scale[i] = cascades.cascade_depth_range[i] * 2.0f * tan(angular_diameter / 2.0f) / cascades.cascade_width_x[i]`
  - 此标量将 NDC 深度差一步转换为 U 方向 UV 空间半影宽度（内含 NDC→世界空间→UV 三步合一，`depth_range` 在链条中正确约掉，保证 cascade 间一致性）
- Renderer 每帧计算 per-cascade `uv_scale_y`（UV 各向异性校正）：
  - `cascade_uv_scale_y[i] = cascades.cascade_width_x[i] / cascades.cascade_width_y[i]`
  - V 方向搜索半径和半影宽度 = U 方向值 × 此比值
- 上述值写入 GlobalUBO 的对应字段

#### 前置：Poisson Disk 生成脚本

- `scripts/generate_poisson_disk.py`：dart-throwing 算法生成单位圆内 Poisson Disk 样本点
  - 输入：样本数（32 / 49）、最小间距（由样本数自动推算：`0.7 / sqrt(N)`）、随机种子（固定值，保证可复现）
  - 算法：在单位圆内均匀随机采样候选点，拒绝与已有点距离小于最小间距的候选，直到达到目标数量
  - 输出：打印为 GLSL `const vec2 kBlockerSearchSamples[32]` / `const vec2 kPCFSamples[49]` 格式，可直接粘贴到 shader
  - Low/Medium 档位使用前 16/25 个样本（dart-throwing 按顺序生成，前 N 个自然形成均匀子集）
  - 脚本留在仓库中作为坐标的可复现来源

#### Shader：Poisson Disk 采样分布

- `shadow.glsl` 新增由 `scripts/generate_poisson_disk.py` 生成的 Poisson Disk 常量数组：32 个 `vec2` 用于 Blocker Search + 49 个 `vec2` 用于 PCF
- 样本分布在单位圆内，硬编码为 shader 常量（`const vec2 kBlockerSearchSamples[32]`、`const vec2 kPCFSamples[49]`）
- Shader 循环上限由 UBO `pcss_blocker_samples` / `pcss_pcf_samples` 控制（uniform 值，无 warp 分支发散）
- 新增 `interleaved_gradient_noise(vec2 screen_pos)` 函数：`fract(52.9829189 * fract(dot(screen_pos, vec2(0.06711056, 0.00583715))))`，返回 [0,1) 伪随机值
- 新增 `rotate_sample(vec2 sample, float angle)` 辅助函数：2D 旋转矩阵，per-pixel 旋转 Poisson Disk 样本消除结构化 banding
- Per-pixel 旋转角 = `interleaved_gradient_noise(gl_FragCoord.xy) * 2π`

#### Shader：ShadowProjData + prepare_shadow_proj

- `shadow.glsl` 新增 `ShadowProjData` 结构体，封装单个 cascade 的投影结果和 receiver plane 梯度：
  - `vec2 shadow_uv`：光空间 UV
  - `float ref_depth`：光空间 NDC 深度
  - `float dz_du`、`float dz_dv`：深度对 shadow UV 的梯度（receiver plane depth bias 用）
- `shadow.glsl` 新增 `prepare_shadow_proj(vec3 world_pos, vec3 world_normal, int cascade)` → `ShadowProjData`：
  - 法线偏移与投影（复用 `sample_shadow` 的法线偏移 + 光空间投影逻辑）
  - `dFdx/dFdy` 计算 `shadow_uv` 和 `ref_depth` 的屏幕空间导数
  - 2×2 线性系统求解 `(dz_du, dz_dv)` 梯度，clamp 到 `kMaxReceiverPlaneGradient = 0.01` 防止极端掠射角漏影
  - Cascade 边界检测：通过 `dFdx(float(cascade))` + `dFdy(float(cascade))` 检测 quad 内 cascade 不一致，不一致时将 `dz_du`/`dz_dv` 归零（`step` 无分支），避免跨投影空间的垃圾梯度传播到采样中
  - **必须在 uniform control flow 中调用**——`dFdx/dFdy` 要求 quad 内所有 invocation 执行同一指令

**为什么独立为 `prepare_shadow_proj`**：`blend_cascade_shadow()` 在 blend region 内对下一 cascade 采样的分支是 non-uniform control flow（quad 中部分像素在 blend 区域内、部分不在）。`dFdx/dFdy` 在 non-uniform 分支内是 UB（GLSL 规范要求 uniform control flow）。将投影和梯度计算提取到分支外，在 uniform flow 中为当前和下一 cascade 预计算 `ShadowProjData`，后续 `blocker_search` 和 `sample_shadow_pcss` 仅消费预计算数据，不再调用 `dFdx/dFdy`。额外开销约 25 ALU ops（1 次矩阵乘 + 4 次导数 + 2×2 solve），相比 41 次纹理采样可忽略。

**已知局限与缓解**：cascade 边界处 2×2 quad 内像素可能使用不同 `cascade_view_proj` 矩阵，导致 `dFdx(shadow_uv)` 混合不同投影空间的导数——数学上不精确。通过 `dFdx(float(cascade))` 主动检测此情况并将梯度归零，使这些像素退化为仅依赖 normal offset + slope bias（等价于固定 PCF 路径的 bias 质量）。此情况仅影响 1-2 像素宽的窄带，且同时处于 cascade blend 区域，视觉上不可见。

#### Shader：Blocker Search

- `shadow.glsl` 新增 `blocker_search(ShadowProjData proj, int cascade, out float avg_blocker_depth, out float num_blockers)`：
  - 从 `proj` 读取 `shadow_uv`、`ref_depth`、`dz_du`、`dz_dv`（不再内部调用 `dFdx/dFdy`）
  - 搜索半径 = `vec2(cascade_light_size_uv[cascade], cascade_light_size_uv[cascade] * cascade_uv_scale_y[cascade])`（U/V 椭圆搜索，精确匹配世界空间圆形光源）
  - 遍历 `kBlockerSearchSamples[16]`，每个样本施加 per-pixel 旋转后乘以搜索半径偏移 UV
  - Per-sample 深度调整：`adjusted_depth = ref_depth + dz_du * offset.x + dz_dv * offset.y`
  - 通过 `rt_shadow_map_depth`（binding 6）读取原始深度值：`texture(rt_shadow_map_depth, vec3(sample_uv, float(cascade))).r`
  - Reverse-Z 下 blocker 判定：`sampled_depth > adjusted_depth`（blocker 离光源更近，在 Reverse-Z 中深度值更大）
  - 累加 blocker 深度和计数
  - 输出 `avg_blocker_depth`（累加和 / 计数）和 `num_blockers`

#### Shader：半影估算（世界空间统一）

- 半影估算通过 CPU 预计算的 `cascade_pcss_scale` + `cascade_uv_scale_y` 完成，消除 cascade 边界处的半影宽度跳变：
  - `delta_ndc = abs(blocker_ndc - receiver_ndc)`
  - `penumbra_u = delta_ndc * cascade_pcss_scale[cascade]`
  - `penumbra_v = penumbra_u * cascade_uv_scale_y[cascade]`
  - `cascade_pcss_scale` 由 CPU 每帧预计算：`depth_range * 2 * tan(θ/2) / width_x`（U 方向），将 NDC→世界空间→UV 三步合一为单次乘法
  - V 方向乘 `cascade_uv_scale_y`（`width_x / width_y`）校正各向异性，使世界空间圆形半影精确映射到 UV 椭圆
  - 方向光简化公式：去掉了点光源/聚光灯的 `1/dBlocker` 除法（正交投影无透视缩放）
  - Clamp 到 `[shadow_texel_size, kMaxPenumbraTexels * shadow_texel_size]`：下限 1 texel 防止极小半影退化为噪点；上限 `kMaxPenumbraTexels = 64` 防止多层遮挡场景中混合深度导致 kernel 爆炸（性能骤降 + 物理上无意义的极宽半影）。64 texels 对 25 个 Poisson 样本已是稀疏采样的边界，再大结果为噪声。使用 texel 数而非固定 UV 值，自动适配不同 shadow map 分辨率
- 将 clamp 后的 `vec2(penumbra_u, penumbra_v)` 传递给 variable-width PCF 步骤

**为什么需要 `depth_range`**：正交投影的 XY 范围（紧贴 cascade 子锥体）和 Z 范围（扩展到场景 AABB）是独立计算的，通常差异巨大（近 cascade Z/XY 比可达 20×）。NDC 深度差→世界空间深度差的转换必须使用 Z 深度范围（`far - near`），而非 XY 宽度。`cascade_pcss_scale` 在 CPU 端将正确的 `depth_range` 与 `width_x` 和角直径合并为单一标量，shader 中一次乘法即得 U 方向 UV 半影宽度。

**Cascade 一致性证明**：对于同一世界空间深度差 Δz，在任意 cascade 上 `penumbra_u = (Δz / depth_range) × (depth_range × 2tan(θ/2) / width_x) = Δz × 2tan(θ/2) / width_x`。`depth_range` 在链条中约掉，世界空间半影 `= penumbra_u × width_x = Δz × 2tan(θ/2)` 在所有 cascade 中一致。

#### Shader：Variable-Width PCF

- `shadow.glsl` 新增 `sample_shadow_pcss(ShadowProjData proj, int cascade)`：
  - 调用 `blocker_search(proj, ...)` 获取平均 blocker 深度和 blocker 计数
  - 无 blocker（`num_blockers < 1.0`）时 early-out 返回 1.0（全亮）
  - 全 blocker（`num_blockers >= BLOCKER_SEARCH_SAMPLES` 且 `pcss_flags & PCSS_FLAG_BLOCKER_EARLY_OUT`）时 early-out 返回 0.0（纯阴影，缓解多层遮挡漏光）
  - 半影估算（见上）得到 `vec2(penumbra_u, penumbra_v)`，已含双向 clamp（见半影估算小节）
  - 遍历 `kPCFSamples[25]`，每个样本施加 per-pixel 旋转后乘以 `vec2(penumbra_u, penumbra_v)` 偏移 UV（椭圆 kernel）
  - Per-sample receiver plane depth bias：`adjusted_depth = proj.ref_depth + proj.dz_du * offset.x + proj.dz_dv * offset.y`（消费 `prepare_shadow_proj` 预计算的梯度）
  - 通过 `rt_shadow_map`（binding 5，比较采样器）做硬件 2×2 比较采样：`texture(rt_shadow_map, vec4(sample_uv, float(cascade), adjusted_depth))`
  - 25 次硬件比较采样 × 2×2 双线性 = 100 次有效深度比较
  - 返回平均值

#### Shader：blend_cascade_shadow 适配

- `blend_cascade_shadow()` 根据 `global.shadow_mode` 分支：
  - `shadow_mode == 0`（PCF）：调用现有 `sample_shadow_pcf()`（行为不变，无导数依赖）
  - `shadow_mode == 1`（PCSS）：在 blend 分支外预计算当前和下一 cascade 的 `ShadowProjData`（下一 cascade 索引 clamp 为 `min(cascade+1, shadow_cascade_count-1)` 防越界，保证 `dFdx/dFdy` 在 uniform flow 中执行），然后调用 `sample_shadow_pcss(proj, cascade)`；blend region 内使用预计算的 `next_proj` 采样下一 cascade
- `shadow_mode` 是 UBO uniform 值，`if (shadow_mode == 0)` 分支本身是 uniform control flow，两条路径互不影响
- Cascade blend 逻辑不变：blend region 内对当前和下一个 cascade 分别采样后 lerp
- Distance fade 逻辑不变

#### DebugUI 适配

- Shadow 面板顶部（Cascades 下拉之前）新增 **Shadow Mode 下拉**（"PCF" / "PCSS"，对应 `shadow_config.shadow_mode` 0/1）
- 当 `shadow_mode == 1`（PCSS）时：
  - **隐藏** PCF Radius 下拉（PCSS 自动计算 kernel size）
  - **显示** Angular Diameter 滑条（单位度，范围 0.1°~5.0°，默认 0.53°，内部转换为弧度存入 `shadow_config.light_angular_diameter`）+ hover tooltip 提示："Sun ~0.53 deg (subtle). Use 2-5 deg for visible soft shadows."
  - **显示** PCSS Quality 下拉（Low / Medium / High，对应 `shadow_config.pcss_quality` 0/1/2，默认 Medium）
  - **显示** Blocker Early-Out checkbox（对应 `shadow_config.pcss_flags` bit 0，默认开启）——便于对比多层遮挡场景下的漏光差异
- 当 `shadow_mode == 0`（PCF）时：保持现有全部控件不变，隐藏 Angular Diameter / Quality / Blocker Early-Out
- 其余控件（Cascades、Resolution、Split Lambda、Max Distance、Slope Bias、Normal Offset、Blend Width、Cascade 统计）两种模式均显示

#### 验证

- PCSS 模式下阴影具有接触硬化效果：物体与地面接触处阴影边缘锐利，远离遮挡物时阴影边缘渐软
- 切换 PCF/PCSS 模式时画面正确切换，无 validation 报错
- Cascade 边界处半影过渡平滑（世界空间统一半影 + cascade blend 双重保障）
- Angular Diameter 滑条调节有明显视觉效果（增大→阴影更软，减小→阴影更硬）
- 性能在可接受范围（16 blocker + 25 PCF = 41 次纹理采样/像素，现代桌面 GPU 可承受）

#### 设计要点

PCSS 设计决策见 `milestone-1/m1-design-decisions.md`「PCSS（Percentage-Closer Soft Shadows）」。

关键设计：
- PCSS 是 PCF 的超集——三步流程（Blocker Search → 半影估算 → Variable-Width PCF）中第三步就是 PCF，区别在于 kernel 大小从固定值变为动态计算。通过 `shadow_mode` 运行时切换，保留 PCF 作为对照和低开销 fallback
- Blocker Search 必须读取原始深度值，需要独立的非比较采样器（binding 6）。两个 binding 指向同一 image 不同 sampler，Vulkan 规范允许且零额外显存
- 方向光（正交投影）下 Blocker Search 搜索半径是常数 `LIGHT_SIZE_UV`（无透视缩放），半影公式去掉 `1/dBlocker` 除法——这是方向光 PCSS 相比点光源/聚光灯的两个关键简化
- 半影估算通过 CPU 预计算的 `cascade_pcss_scale`（`depth_range × 2tan(θ/2) / width_x`）一步完成：NDC 深度差 × 此标量 = U 方向 UV 半影宽度。`depth_range` 在链条中约掉，保证 cascade 间世界空间半影一致，消除边界处 2-3× 的跳变
- UV 各向异性校正：`cascade_uv_scale_y`（`width_x / width_y`）将 U 方向值转换为 V 方向值，使搜索区域和半影 kernel 在 UV 空间为精确椭圆，匹配世界空间圆形光源
- `ShadowCascadeResult` 新增 `cascade_width_x` / `cascade_width_y` / `cascade_depth_range`（per-cascade 正交投影几何），`compute_shadow_cascades` 保持纯几何计算职责，PCSS 参数在 Renderer 中从这些值组装
- Receiver plane depth bias：`prepare_shadow_proj()` 在 uniform flow 中通过 `dFdx/dFdy` 预计算深度在 shadow UV 空间的梯度，`blocker_search` 和 `sample_shadow_pcss` 消费预计算数据做 per-sample 线性调整比较深度，对抗 variable-width kernel 的 acne（仅 PCSS 路径，固定 PCF 不需要）
- Poisson Disk 硬编码 + per-pixel interleaved gradient noise 旋转：消除结构化 banding，将 artifact 转化为高频噪声。后续 M2 加 TAA 时天然配合（temporal accumulation 消除高频噪声）
- 采样预算 16 blocker + 25 PCF 是业界生产配置（NVIDIA PCSS 论文、多个 shipped titles），M1 无 TAA 需要足够的单帧质量
- Light size 使用物理参数（角直径），默认 0.53° = 太阳。用户调参直觉清晰，per-cascade `LIGHT_SIZE_UV` 由 CPU 每帧预计算

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
  读: shadow_map (Set 2 binding 5 比较采样 + binding 6 深度读取, feature_flags 守护)
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
└── common/shadow.glsl           # [Step 3 新增, Step 7 扩展] CSM cascade 选择、PCF/PCSS 采样、distance fade
```

修改文件（随 Step 推进）：

```
framework/
├── include/himalaya/framework/
│   ├── scene_data.h             # [Step 1a] RenderFeatures + ShadowConfig
│   ├── frame_context.h          # [Step 1a] shadow_map + features + shadow_config
│   ├── camera.h                 # [Step 1a] compute_focus_position()
│   └── culling.h                # [Step 1c] 通用 frustum 剔除
app/
├── include/himalaya/app/
│   ├── renderer.h               # [Step 1a-2] RenderInput 扩展 + handle_shadow_resolution_changed
│   ├── camera_controller.h      # [Step 1a] set_focus_target() + F 键 focus
│   └── debug_ui.h               # [Step 1a] DebugUIContext + RenderFeatures
├── src/
│   ├── renderer.cpp             # [Step 2-7]
│   ├── application.cpp          # [Step 1a-5] ShadowConfig + RenderFeatures + 相机自动定位
│   ├── camera_controller.cpp    # [Step 1a] F 键 focus 处理
│   └── debug_ui.cpp             # [Step 1a-7] Features 面板 + Shadow 面板
shaders/
├── common/bindings.glsl         # [Step 1a-2, Step 7] feature_flags + shadow UBO 字段 + PCSS 字段
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
| PushConstantData | 4 bytes（仅 `cascade_index`，shadow pass 专用）。model + material_index 已移至 InstanceBuffer SSBO。Forward/PrePass 无 push constant |
| 硬件 bias vs shader bias | Constant + slope-scaled 通过 `vkCmdSetDepthBias`（渲染端），normal offset 在 `shadow.glsl`（采样端），三者互补 |
| Alpha mask 阴影 | ShadowPass 双 pipeline（opaque 无 FS + mask 有 FS discard），与 DepthPrePass 模式一致 |
| Cascade blend | Lerp blend（blend region 内双重采样），`blend_cascade_shadow()` 隔离 blend 策略预留 dithering 切换 |
| Per-cascade 剔除 | Step 1c Culling 模块重构为通用接口，Step 2-5 暴力全画（正确性优先），Step 6 ShadowPass 接入 per-cascade 剔除 |
| RenderFeatures | Step 1a 引入骨架（skybox + shadows），阶段五扩展 ssao + contact_shadows |
| feature_flags | GlobalUBO uint bitmask，shader 动态分支守护采样。Skybox 不需要（独立 pass 跳过即可），Shadow 需要（forward.frag 采样 Set 2） |
| ShadowConfig 位置 | `framework/scene_data.h`，与 RenderFeatures 同位。Application 持有实例，DebugUI 直接操作 |
| Shadow max_distance 初始化 | Application 初始化 100m 作为退化 fallback，正常场景覆盖为 `diagonal × 1.5`。DebugUI 对数滑条可覆盖 |
| 相机自动定位 + F 键 Focus | `Camera::compute_focus_position(AABB)` 纯计算共享。场景加载设 pitch=-45° 后调用；F 键保持朝向直接调用。CameraController 持有 focus target 指针 |
| Cascade 统计信息 | DebugUI Shadow 面板底部显示每个 cascade 的范围（近/远 m）和 texel density（px/m），辅助理解 max_distance / cascade count / resolution 的交互 |
| Runtime config change | 分辨率变更沿用 MSAA 切换模式（`vkQueueWaitIdle` → 重建资源 → 更新 descriptor）。Cascade 数量为纯渲染参数，无需重建资源 |
| Cascade 可视化 | `DEBUG_MODE_SHADOW_CASCADES` 追加到 passthrough 模式末尾，`>= DEBUG_MODE_PASSTHROUGH_START` 自动跳过 ACES |
| GlobalUBO 增长 | 336 → 656 bytes（+320, Step 2-6）→ 720 bytes（+64, Step 7 PCSS），仍远小于 16KB 最低保证 |
| Pass 虚基类 | 阶段四不引入，M1 全程具体类。M2 评估 |
| PCSS 双采样器 | Binding 5（`sampler2DArrayShadow`）用于 PCF 硬件比较采样，binding 6（`sampler2DArray`）用于 Blocker Search 读取原始深度。同一 image 不同 sampler，零额外显存 |
| PCSS 采样策略 | Poisson Disk 硬编码（最大 32 blocker + 49 PCF），3 档预设（Low 16+16 / Medium 16+25 / High 32+49），UBO 传入实际采样数（uniform 循环上限，无 warp 发散）。per-pixel interleaved gradient noise 旋转消除 banding |
| PCSS 半影估算 | CPU 预计算 `cascade_pcss_scale`（`depth_range × 2tan(θ/2) / width_x`，U 方向），shader 中 NDC 深度差 × 此标量 = U 方向 UV 半影宽度。`depth_range` 在链条中约掉，cascade 间世界空间半影一致，消除边界处 2-3× 跳变 |
| PCSS UV 各向异性 | `cascade_uv_scale_y`（`width_x / width_y`）将 U 方向值转换为 V 方向值。搜索和半影 kernel 在 UV 空间为椭圆，精确匹配世界空间圆形光源。一个额外 `vec4` UBO 字段同时覆盖 blocker search 和 PCF |
| PCSS Receiver Plane Bias | `prepare_shadow_proj()` 在 uniform flow 中通过 `dFdx/dFdy` 预计算梯度，`blocker_search` 和 `sample_shadow_pcss` 消费预计算数据做 per-sample 线性调整。梯度 clamp 到最大值防止掠射角漏影。仅 PCSS 路径使用 |
| PCSS 正交几何暴露 | `ShadowCascadeResult` 新增 `cascade_width_x` / `cascade_width_y` / `cascade_depth_range`，`compute_shadow_cascades` 保持纯几何职责，PCSS UBO 字段在 Renderer 中从这些值组装 |
| PCSS 导数预计算 | `prepare_shadow_proj()` 在 uniform flow 中预计算投影+梯度为 `ShadowProjData`，`blocker_search` / `sample_shadow_pcss` 不调 `dFdx/dFdy`。`blend_cascade_shadow()` PCSS 路径在 blend 分支外为当前和下一 cascade 各预计算一份，额外约 25 ALU ops 相比 41 次纹理采样可忽略。Cascade 边界处跨投影梯度混合是已知局限，退化为固定 PCF 路径的 bias 质量 |
| PCSS 方向光简化 | 正交投影下搜索半径为常数 `LIGHT_SIZE_UV`（无透视缩放），半影公式去掉 `1/dBlocker` 除法 |
| Shadow Mode 切换 | `shadow_mode` UBO 字段（0=PCF, 1=PCSS），`blend_cascade_shadow()` 内分支选择采样函数，保留 PCF 作为对照和低开销 fallback |
| PCSS Blocker Early-Out | `pcss_flags` bit 0 控制：blocker search 全命中时 early-out 返回 0.0，缓解多层遮挡漏光。阴影深处的漏光完全消除，剩余漏光仅在阴影边界窄带内。零额外采样开销，DebugUI checkbox 可运行时对比 |
