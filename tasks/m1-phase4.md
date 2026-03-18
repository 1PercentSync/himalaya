# Milestone 1 阶段四：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## 准备工作 A：测试场景

- [x] 补充适合 CSM 阴影测试的室外场景（Intel Sponza）

## 准备工作 B：Instancing

- [x] `framework/scene_data.h` 新增 `GPUInstanceData`（model mat4 + material_index uint, 80 bytes std430）和 `MeshDrawGroup`（mesh_id, first_instance, instance_count, double_sided）
- [x] `shaders/common/bindings.glsl` 新增 InstanceBuffer SSBO（Set 0, Binding 3），PushConstantData 缩减为 `cascade_index`（4 bytes）
- [x] `shaders/forward.vert` model 和 material_index 改为从 `instances[gl_InstanceIndex]` 读取
- [x] `shaders/forward.frag` material_index 改为从 `instances[gl_InstanceIndex]` 读取
- [x] `shaders/depth_prepass.vert` 同上
- [x] `shaders/depth_prepass.frag` 同上
- [x] `shaders/depth_prepass_masked.frag` 同上
- [x] `rhi/descriptors.h/cpp` Set 0 layout 新增 Binding 3（InstanceBuffer SSBO）
- [x] `app/renderer.h/cpp` 新增 per-frame InstanceBuffer（CpuToGpu, 固定大小），pipeline layout 更新（push constant 72→4 bytes）
- [x] `app/renderer.cpp` render() 新增 post-cull 分组逻辑：visible indices 按 (mesh_id, alpha_mode, double_sided) 排序 → 构建 MeshDrawGroup 列表 + 填充 InstanceBuffer（含 overflow guard）
- [x] `passes/forward_pass.cpp` draw loop 改为 MeshDrawGroup iteration + `draw_indexed(instanceCount=N, firstInstance=offset)`
- [x] `passes/depth_prepass.cpp` draw loop 改为 MeshDrawGroup iteration（opaque groups + mask groups 分别迭代）
- [x] 验证：密集场景 draw call 显著减少，渲染结果与 instancing 前一致，无 validation 报错

## 准备工作 C：运行时场景/HDR 加载 + 配置持久化

- [x] vcpkg.json + CMakeLists.txt 移除 CLI11，添加 nlohmann/json（需用户在 CLion 中确认构建配置）
- [x] CMakeLists.txt 移除 scene/HDR 资产拷贝到 build 目录的规则
- [x] `app/config.h/cpp` 新增 AppConfig 结构体（scene_path + env_path）+ JSON load/save（`%LOCALAPPDATA%\himalaya\config.json`）
- [x] `app/application.h/cpp` 移除 CLI11 命令行参数解析，改为读取 AppConfig
- [x] `app/application.cpp` 启动流程：读配置 → 分别尝试加载 scene/HDR → 部分失败时另一项正常加载（scene 失败 = 空场景仅 skybox，HDR 失败 = 灰色 fallback cubemap）
- [x] `app/application.cpp` 新增 `switch_scene(path)` / `switch_environment(path)`：`vkQueueWaitIdle` → destroy → load → 更新 descriptors → 保存配置
- [x] `app/debug_ui.cpp` 新增 Scene 面板：当前 scene path 显示 + "Load Scene..." 按钮（Windows `GetOpenFileNameW` 对话框，过滤 .gltf/.glb）
- [x] `app/debug_ui.cpp` 新增 Environment 面板：当前 HDR path 显示 + "Load HDR..." 按钮（过滤 .hdr）
- [x] 加载失败时 DebugUI 显示错误提示，不 abort
- [x] 验证：运行时切换 scene/HDR 正常，配置持久化，重启后恢复上次文件，文件丢失时 fallback 正确

## 准备工作 D：缓存基础设施 + BC 纹理压缩 + IBL 缓存

### D-1：缓存模块 + KTX2 读写（共享基础设施）

- [x] vcpkg.json + CMakeLists.txt 添加 xxHash 依赖（需用户在 CLion 中确认构建配置）
- [x] `rhi/types.h` Format 枚举新增 BC 格式（Bc5UnormBlock / Bc7UnormBlock / Bc7SrgbBlock）+ `from_vk_format()` 反向转换 + 格式工具函数（`format_bytes_per_block` / `format_block_extent` / `format_is_block_compressed`）
- [x] `rhi/resources.h/cpp` 新增 `upload_image_all_levels()`：预建 mip chain + cubemap 一次性上传（单 staging buffer、多 VkBufferImageCopy2 region）
- [x] `framework/cache.h/cpp` 新增缓存模块：`cache_root()`（`%TEMP%\himalaya\`）+ `content_hash()`（XXH3_128）+ `cache_path(category, hash, ext)`
- [x] `framework/ktx2.h/cpp` 新增最小 KTX2 读写模块：`write_ktx2()`（2D / cubemap + mip chain）+ `read_ktx2()`（返回 format + mip offset 索引 + 数据 blob），DFD 按支持格式硬编码，读取时仅解析 header + level index

### D-2：BC 纹理压缩 + KTX2 缓存

- [x] 集成 bc7enc 源文件（bc7enc.h/cpp + rgbcx.h）到项目
- [x] 集成 stb_image_resize2 头文件（vcpkg stb 包已包含）
- [x] `framework/texture.h/cpp` 新增 CPU mip 生成（stb_image_resize2 逐级缩放，Color 用 sRGB-correct filtering，非 4 对齐纹理先 resize 到 4 的倍数）
- [x] `framework/texture.h/cpp` 新增 BC 压缩：BC7（SRGB/UNORM）+ BC5（UNORM，法线用）
- [x] `framework/texture.h/cpp` 新增 KTX2 缓存写入：BC 数据 + 所有 mip 级别通过 `write_ktx2()` 写入缓存（write-to-temp + rename 原子写入）
- [x] `framework/texture.h/cpp` 新增 KTX2 缓存读取：`read_ktx2()` + `upload_image_all_levels()` 直接上传 BC 数据（读取时剥离 KTX2 元数据，仅保留 mip 数据）
- [x] `framework/texture.h/cpp` 三函数拆分：`load_cached_texture()` 缓存查找 / `compress_texture()` 压缩+写缓存 / `prepare_texture()` 便捷包装
- [x] `app/scene_loader.cpp` 适配：TextureRole::Normal（BC5）+ 源文件字节哈希（缓存命中跳过解码）+ 4 阶段流水线（hash+check → decode misses → parallel compress → upload）
- [x] 纹理级 OpenMP 并行压缩（`schedule(dynamic)`，仅压缩缓存未命中的纹理）
- [x] 验证：纹理 VRAM 显著降低（RGBA8 → BC 约 4:1），缓存命中时跳过解码和压缩，画质无明显退化

### D-3：IBL 缓存

- [x] `framework/ibl.cpp` init() 末尾新增 GPU → CPU readback：staging buffer + `vkCmdCopyImageToBuffer` 读回 4 个 IBL 产物
- [x] `framework/ibl.cpp` 新增 KTX2 缓存写入：通过 `write_ktx2()` 将 readback 数据写为 KTX2 cubemap 缓存（irradiance / prefiltered / skybox cubemap）+ 2D 缓存（BRDF LUT）
- [x] `framework/ibl.cpp` init() 入口新增缓存检查：HDR 内容哈希 → 缓存命中时 `read_ktx2()` + `upload_image_all_levels()` 直接加载 + 注册 bindless，跳过全部 GPU compute
- [x] BRDF LUT 使用固定 cache key（与 HDR 无关，永久缓存）
- [x] 验证：首次启动完成 IBL 计算并写缓存，后续启动秒级加载，IBL 渲染结果与未缓存时一致

## Step 1a：RenderFeatures 基础设施

- [x] `framework/scene_data.h` 新增 `RenderFeatures` 结构体（`skybox` bool 默认 true + `shadows` bool 默认 true）
- [x] `framework/scene_data.h` 新增 `ShadowConfig` 结构体（split_lambda、max_distance、constant_bias、slope_bias、normal_offset、pcf_radius、blend_width，无默认值，调用方显式初始化）
- [x] GlobalUBO 新增 `feature_flags`（uint32_t，offset 324），`bindings.glsl` 新增 `#define FEATURE_SHADOWS (1u << 0)`
- [x] FrameContext 新增 `RGResourceId shadow_map`（invalid if shadows disabled）+ `const RenderFeatures* features` + `const ShadowConfig* shadow_config`
- [x] RenderInput 新增 `const RenderFeatures& features` + `const ShadowConfig& shadow_config`
- [ ] Renderer 根据 `features.skybox` 条件调用 `skybox_pass_.record()`
- [ ] Application 新增 `RenderFeatures` 和 `ShadowConfig` 成员，构造 RenderInput 时传入
- [ ] SceneLoader 计算并暴露场景 AABB（`scene_bounds()`：所有 mesh instance 的 `world_bounds` 求并集）
- [ ] Application 在场景加载后根据 scene AABB 初始化 `shadow_config.max_distance`（`diagonal × 1.5`，退化时保持默认 100m）
- [ ] `framework/camera.h/cpp` 新增 `Camera::compute_focus_position(const AABB&)` 纯计算方法（包围球半径 + FOV → 距离 → 位置，退化 AABB 返回当前 position）
- [ ] Application 在场景加载后自动定位相机（yaw=0, pitch=-45°, position 由 `compute_focus_position()` 计算，退化时 fallback 默认位置）
- [ ] `app/camera_controller.h/cpp` 新增 `set_focus_target(const AABB*)` + F 键 focus（`ImGui::IsKeyPressed(ImGuiKey_F, false)`，保持朝向，调用 `compute_focus_position()` 更新 position）
- [ ] Application 场景加载后调用 `camera_controller_.set_focus_target(&scene_loader_.scene_bounds())`
- [ ] DebugUI 新增 Features 面板（Skybox checkbox）
- [ ] 验证：Skybox 可通过 DebugUI 切换开/关，F 键 focus 正确定位，无 validation 报错

## Step 1b：Shader 热重载

- [ ] 各 pass 新增 `rebuild_pipelines()` 公开方法（调用已有 `create_pipelines()`）
- [ ] DebugUI 新增 "Reload Shaders" 按钮，Renderer 检测触发后 `vkQueueWaitIdle()` → 遍历所有 pass `rebuild_pipelines()`
- [ ] 验证：修改 shader 后点击 Reload 生效，无 validation 报错

## Step 1c：Culling 模块重构

- [ ] `framework/culling.h` 重构为纯几何剔除：`Frustum` 结构体 + `extract_frustum(mat4 vp)` + `cull_against_frustum(instances, frustum, out_visible)`（预分配 buffer 版），删除旧 `perform_culling()`
- [ ] 现有相机 frustum cull 迁移到通用接口：`cull_against_frustum()` + 调用方内联分桶（opaque/transparent）+ 透明排序
- [ ] 验证：现有相机剔除行为不变（渲染输出与重构前一致），无 validation 报错

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
- [ ] ShadowPass pipeline layout 声明 push constant range（4 bytes `cascade_index`，shadow pass 专用）
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
- [ ] DebugUI Shadow 面板扩展：cascade count 下拉（1/2/3/4）+ resolution 下拉（512/1024/2048/4096）
- [ ] DebugUI Shadow 面板底部新增 cascade 统计信息：每个 cascade 的覆盖范围（近/远边界 m）和 texel density（px/m）
- [ ] 验证：相机移动/旋转时阴影边缘无闪烁，cascade 可视化显示正确分层，cascade 数量和分辨率可运行时切换

## Step 6：PCF + cascade blend + per-cascade 剔除 + 最终验证

- [ ] `shadow.glsl` 新增 `sample_shadow_pcf()`：基于硬件 2×2 比较的多次偏移采样，kernel 由 `shadow_pcf_radius` 控制（0=off, 1=3×3, 2=5×5, ..., 5=11×11）
- [ ] `shadow.glsl` `select_cascade()` 输出 `blend_factor`，`shadow.glsl` 新增 `blend_cascade_shadow()` 封装 blend 逻辑（预留 dithering 切换），forward.frag 通过此函数获取最终 shadow 值
- [ ] Distance fade：最后一级 cascade 远端 blend 到 1.0（无阴影），复用 blend 逻辑
- [ ] ShadowPass per-cascade 调用 `cull_against_frustum()`（Step 1c 已就绪，输入全部场景物体）替代暴力全画，cull 结果按 alpha_mode 分桶为 opaque/mask 列表
- [ ] DebugUI Shadow 面板扩展：PCF radius 下拉（Off/3×3/5×5/7×7/9×9/11×11）+ blend width 滑条
- [ ] 最终验证：阴影边缘柔和（PCF），cascade 过渡平滑（blend），per-cascade 剔除生效（RenderDoc 对比 draw call 数减少）
