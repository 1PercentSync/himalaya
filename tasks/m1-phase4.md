# Milestone 1 阶段四：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## 准备工作

- [ ] 补充适合 CSM 阴影测试的室外场景（需要方向光、不同深度的物体分布、开阔空间验证 cascade 分割）

## Step 1：RenderFeatures 基础设施

- [ ] `framework/scene_data.h` 新增 `RenderFeatures` 结构体（`skybox` bool 默认 true + `shadows` bool 默认 true）
- [ ] `framework/scene_data.h` 新增 `ShadowConfig` 结构体（split_lambda、max_distance、constant_bias、slope_bias、normal_offset、pcf_radius、blend_width，含默认值）
- [ ] GlobalUBO 新增 `feature_flags`（uint32_t，offset 324），`bindings.glsl` 新增 `#define FEATURE_SHADOWS (1u << 0)`
- [ ] FrameContext 新增 `RGResourceId shadow_map`（invalid if shadows disabled）+ `const RenderFeatures* features` + `const ShadowConfig* shadow_config`
- [ ] RenderInput 新增 `const RenderFeatures& features` + `const ShadowConfig& shadow_config`
- [ ] Renderer 根据 `features.skybox` 条件调用 `skybox_pass_.record()`
- [ ] Application 新增 `RenderFeatures` 和 `ShadowConfig` 成员，构造 RenderInput 时传入
- [ ] SceneLoader 计算并暴露场景 AABB（`scene_bounds()`：所有 mesh instance 的 `world_bounds` 求并集）
- [ ] Application 在场景加载后根据 scene AABB 初始化 `shadow_config.max_distance`（`diagonal × 1.5`，退化时 fallback 100m）
- [ ] DebugUI 新增 Features 面板（Skybox checkbox）
- [ ] Shader 热重载：各 pass 新增 `rebuild_pipelines()` 公开方法（调用已有 `create_pipelines()`）
- [ ] DebugUI 新增 "Reload Shaders" 按钮，Renderer 检测触发后 `vkQueueWaitIdle()` → 遍历所有 pass `rebuild_pipelines()`
- [ ] 验证：Skybox 可通过 DebugUI 切换开/关，修改 shader 后点击 Reload 生效，无 validation 报错

## Step 2：Shadow 资源 + ShadowPass + 单 cascade 深度渲染

- [ ] Shadow map 2D Array 资源创建（D32Sfloat，2048²，1 layer，`DEPTH_STENCIL_ATTACHMENT | SAMPLED`），ShadowPass 通过 ResourceManager 创建并持有
- [ ] Shadow comparison sampler 创建（`GREATER_OR_EQUAL` compare op、`LINEAR` mag/min filter、`CLAMP_TO_EDGE`），Renderer 持有
- [ ] Per-layer VkImageView 创建（ShadowPass 持有，每 layer 一个 view，`DEPTH` aspect）
- [ ] Set 2 binding 5 descriptor 写入（shadow map image view + comparison sampler）
- [ ] `bindings.glsl` 新增 `layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map`
- [ ] ShadowPass 类骨架（`passes/shadow_pass.h/cpp`）：`setup()` / `record()` / `destroy()` / `on_shadow_config_changed()` + `shadow_map_image()` getter
- [ ] 创建 `shaders/shadow.vert`（输出 `gl_Position` = `cascade_vp[cascade_index] * model * pos` + `uv0`）
- [ ] 创建 `shaders/shadow_masked.frag`（采样 `base_color_tex` alpha test + discard，无颜色输出）
- [ ] Opaque pipeline（仅 VS，无 FS）+ Mask pipeline（VS + masked FS）创建
- [ ] 光空间正交投影矩阵计算（fit 整个相机 frustum，cascade=1）
- [ ] ShadowPass `record()` 实现：`import_image()` 到 RG → `add_pass()` → lambda 内循环 cascade（=1）→ begin rendering(layer view) → set depth bias → draw opaque → draw mask → end rendering
- [ ] 绘制全部场景物体（暴力，iterate `mesh_instances`），先 opaque 后 mask
- [ ] GlobalUBO 新增 shadow 字段：`shadow_cascade_count`、`shadow_normal_offset`、`shadow_texel_size`、`shadow_max_distance`、`shadow_blend_width`、`shadow_pcf_radius`、`cascade_view_proj[4]`、`cascade_splits`；`bindings.glsl` GlobalUBO 同步更新
- [ ] PushConstantData 扩展为 72 bytes（新增 `uint cascade_index`），所有 pipeline layout 更新
- [ ] Renderer 在 `render()` 中填充 GlobalUBO shadow 字段（cascade VP、cascade splits、shadow params）
- [ ] 验证：RenderDoc 检查 shadow map 内容正确（从光源视角的场景深度图）

## Step 3：Forward 集成 + common/shadow.glsl + 硬阴影

- [ ] 创建 `shaders/common/shadow.glsl`：`select_cascade(float view_depth, out float blend_factor)` 暂返回 cascade 0 + `sample_shadow(vec3 world_pos, vec3 world_normal, int cascade)` 单次硬件比较 + `shadow_distance_fade(float view_depth)`
- [ ] `forward.frag` 集成 shadow 采样：`#include "common/shadow.glsl"` + `feature_flags & FEATURE_SHADOWS` 条件分支 + `Lo_direct *= shadow`
- [ ] Normal offset bias 实现：`shadow.glsl` 的 `sample_shadow()` 内部沿法线偏移采样位置，从 `cascade_view_proj` 矩阵提取 texel world size
- [ ] ShadowPass `record()` lambda 中调用 `cmd.set_depth_bias(constant_factor, 0.0f, slope_factor)` 设置硬件 bias
- [ ] DebugUI Shadow 面板：constant bias / slope bias / normal offset 滑条 + Shadows checkbox（操作 `features.shadows`）
- [ ] 验证：场景有可见硬阴影（锐利边缘），Shadow toggle 可开关，无明显 acne 或 peter panning

## Step 4：多 cascade + PSSM 分割策略

- [ ] Cascade 数量从 1 提升到 4，shadow map array 重建为 4 layers
- [ ] PSSM 分割实现：`C_i = λ × C_log + (1 - λ) × C_lin`，lambda 从 ShadowConfig 读取（默认 0.75）
- [ ] Per-cascade 光空间 frustum tight fitting（正交投影紧密包围 cascade sub-frustum 的 8 个角点）
- [ ] `shadow.glsl` `select_cascade()` 更新：view-space depth 与 `cascade_splits` 比较，返回正确 cascade index
- [ ] ShadowPass `record()` 循环 4 次，per-cascade `cmd.begin_debug_label("Cascade N")`
- [ ] DebugUI Shadow 面板扩展：split lambda 滑条 + max distance 对数滑条
- [ ] 验证：近中远距离均有合理阴影覆盖，各 cascade 范围无明显间隙

## Step 5：Texel snapping + cascade 可视化 + runtime config change

- [ ] Texel snapping：per-cascade 正交投影边界 snap 到 texel 对齐位置
- [ ] Debug render mode 追加 `DEBUG_MODE_SHADOW_CASCADES`（passthrough 模式末尾，每 cascade 不同颜色），forward.frag 新增对应分支
- [ ] DebugUI 渲染模式下拉列表追加 "Shadow Cascades"
- [ ] `Renderer::handle_shadow_config_changed(uint32_t new_cascade_count, uint32_t new_resolution)`：`vkQueueWaitIdle` → `shadow_pass_.on_shadow_config_changed()` 重建 image + views → 更新 Set 2 binding 5
- [ ] ShadowPass `on_shadow_config_changed()` 实现：销毁旧 image + views → 创建新 image（new layers / new resolution）+ 新 views
- [ ] DebugUI Shadow 面板扩展：cascade count 下拉（2/3/4）+ resolution 下拉（512/1024/2048/4096）
- [ ] DebugUI Shadow 面板底部新增 cascade 统计信息：每个 cascade 的覆盖范围（近/远边界 m）和 texel density（px/m）
- [ ] 验证：相机移动/旋转时阴影边缘无闪烁，cascade 可视化显示正确分层，cascade 数量和分辨率可运行时切换

## Step 6：PCF + cascade blend + 剔除泛化 + 最终验证

- [ ] `shadow.glsl` 新增 `sample_shadow_pcf()`：基于硬件 2×2 比较的多次偏移采样，kernel 由 `shadow_pcf_radius` 控制（1=3×3, 2=5×5, 3=7×7）
- [ ] `shadow.glsl` `select_cascade()` 输出 `blend_factor`，`shadow.glsl` 新增 `blend_cascade_shadow()` 封装 blend 逻辑（预留 dithering 切换），forward.frag 通过此函数获取最终 shadow 值
- [ ] Distance fade：最后一级 cascade 远端 blend 到 1.0（无阴影），复用 blend 逻辑
- [ ] `framework/culling.h` 重构为纯几何剔除：`Frustum` 结构体 + `extract_frustum(mat4 vp)` + `cull_against_frustum(instances, frustum, out_visible)`（预分配 buffer 版），删除旧 `perform_culling()`
- [ ] ShadowPass per-cascade 调用 `cull_against_frustum()`（输入全部场景物体）替代暴力全画，cull 结果按 alpha_mode 分桶为 opaque/mask 列表
- [ ] 现有相机 frustum cull 迁移到通用接口：`cull_against_frustum()` + 调用方内联分桶（opaque/transparent）+ 透明排序
- [ ] DebugUI Shadow 面板扩展：PCF radius 下拉（Off/3×3/5×5/7×7）+ blend width 滑条
- [ ] 最终验证：阴影边缘柔和（PCF），cascade 过渡平滑（blend），per-cascade 剔除生效（RenderDoc 对比 draw call 数减少）
