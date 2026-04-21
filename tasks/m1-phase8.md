# M1 阶段八：间接光照集成 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：`indirect_intensity` 重命名

- [x] `framework/scene_data.h`：`GlobalUniformData::ibl_intensity` → `indirect_intensity` + `static_assert` 更新
- [x] `shaders/common/bindings.glsl`：`GlobalUBO.ibl_intensity` → `indirect_intensity`
- [x] `shaders/forward.frag`：4 处 `global.ibl_intensity` → `global.indirect_intensity`
- [x] `app/renderer.h`：`RenderInput::ibl_intensity` → `indirect_intensity` + `prev_ibl_intensity_` → `prev_indirect_intensity_`
- [x] `app/application.h` + `app/application.cpp`：`ibl_intensity_` → `indirect_intensity_` + RenderInput 填充
- [x] `app/renderer.cpp` + `app/renderer_pt.cpp`：UBO 填充 + PT 变化检测
- [x] `app/debug_ui.h` + `app/debug_ui.cpp`：`DebugUIContext` 字段 + slider 引用更新

## Step 2：IndirectLightingMode enum + FEATURE_LIGHTMAP_PROBE

- [x] `framework/scene_data.h`：新增 `IndirectLightingMode` enum + `RenderFeatures::lightmap_probe` bool
- [x] `shaders/common/bindings.glsl`：新增 `#define FEATURE_LIGHTMAP_PROBE (1u << 3)`
- [x] `app/application.h`：新增 `indirect_lighting_mode_` 成员
- [x] `app/renderer.h`：`RenderInput` 新增 `indirect_lighting_mode` 字段
- [x] `app/application.cpp` + `app/renderer.cpp`：RenderInput 填充 + `fill_common_gpu_data()` feature_flags 写入

## Step 3：GPUInstanceData 扩展 + instance_index 重构

- [x] `framework/scene_data.h` + `shaders/common/bindings.glsl`：GPUInstanceData `_padding[3]` → `lightmap_index` + `probe_index` + `_padding` + `static_assert` 更新
- [x] `app/renderer*.cpp`：InstanceBuffer 填充 lightmap_index / probe_index 为 `UINT32_MAX`（default member initializer 自动生效，无需修改填充代码）
- [x] `shaders/forward.vert` + `shaders/forward.frag`：`frag_material_index` → `frag_instance_index` + 新增 `frag_uv1`
- [x] `shaders/depth_prepass.vert` + `shaders/depth_prepass.frag` + `shaders/depth_prepass_masked.frag`：`frag_material_index` → `frag_instance_index`
- [x] `shaders/shadow.vert` + `shaders/shadow_masked.frag`：`frag_material_index` → `frag_instance_index`

## Step 4：GPUProbeData + ProbeBuffer SSBO

- [x] `framework/scene_data.h`：新增 `GPUProbeData` struct（48 bytes：position + aabb_min + aabb_max + cubemap_index）
- [x] `shaders/common/bindings.glsl`：新增 `GPUProbeData` struct + `layout(set = 0, binding = 9) readonly buffer ProbeBuffer`（非 RT-only，PARTIALLY_BOUND）
- [x] `rhi/descriptors.h` + `rhi/descriptors.cpp`：Set 0 layout 新增 binding 9 + `write_set0_probe_buffer()` 方法

## Step 5：BakeDataManager — 扫描与校验

- [x] 新增 `framework/bake_data_manager.h`：类声明（`init` / `destroy` / `scan` / `available_angles` / `has_bake_data`）
- [x] 新增 `framework/bake_data_manager.cpp`：`scan()` 实现（从 `debug_ui.cpp` 迁移扫描 + 校验逻辑）
- [x] `framework/CMakeLists.txt`：添加 `bake_data_manager.cpp`
- [x] `app/renderer.h`：新增 `BakeDataManager bake_data_manager_` 成员 + init/destroy 调用
- [x] `app/debug_ui.h` + `app/debug_ui.cpp`：移除本地扫描逻辑，改为从 DebugUIContext 读取 BakeDataManager 数据

## Step 6：BakeDataManager — 加载与卸载

- [x] `bake_data_manager.h` / `.cpp`：`load_angle()` — lightmap KTX2 加载 + bindless 注册 + per-instance `lightmap_indices_`
- [x] `bake_data_manager.h` / `.cpp`：`load_angle()` — probe KTX2 加载 + bindless cubemap 注册 + ProbeBuffer SSBO 填充
- [x] `bake_data_manager.h` / `.cpp`：`load_angle()` — CPU probe-to-instance 分配（最近 probe by AABB center 距离）+ per-instance `probe_indices_`
- [x] `bake_data_manager.h` / `.cpp`：`unload_angle()` — bindless 注销 + image 销毁 + 清空 indices
- [x] `app/renderer.h` / `.cpp`：新增 `switch_bake_angle()` 方法（GPU idle → unload → load）
- [x] `app/renderer*.cpp`：InstanceBuffer 填充时从 `bake_data_manager_` 查询 lightmap_index / probe_index

## Step 7：Forward Shader 间接光照集成

- [x] `shaders/forward.frag`：BRDF LUT 查找移到分支前（两模式共用）
- [x] `shaders/forward.frag`：Lightmap 分支 — lightmap 采样 × diffuse_color
- [x] `shaders/forward.frag`：Probe 分支 — probe cubemap 采样（roughness-based mip via `textureQueryLevels()`），无 probe 时回退 IBL specular
- [x] `shaders/forward.frag`：IBL 分支 — 保持现有逻辑
- [x] `shaders/forward.frag`：indirect_intensity 乘数 + AO/SO 应用于两种模式
- [x] `shaders/common/bindings.glsl`：`DEBUG_MODE_IBL_ONLY` → `DEBUG_MODE_INDIRECT_ONLY`
- [x] `app/debug_ui.cpp`：Debug View 标签 `"IBL Only"` → `"Indirect Only"`

## Step 8：模式切换 + 角度选择 UI

- [x] `app/debug_ui.h` + `.cpp`：IndirectLightingMode toggle（无 bake 数据时灰显 Lightmap/Probe）
- [x] `app/debug_ui.cpp`：Indirect Intensity slider label 根据模式动态显示
- [x] `app/debug_ui.h`：`DebugUIActions` 新增 `bool angle_switch_requested` + `uint32_t new_angle_rotation`
- [x] `app/debug_ui.cpp`：已 bake 角度列表可点击切换（当前角度高亮）
- [x] `app/application.cpp`：检测角度切换 action → 调用 `switch_bake_angle()`
- [x] `app/application.cpp`：模式切换 → IBL↔LightmapProbe 加载/卸载 + IBL 旋转同步到已加载角度
- [x] `app/application.cpp`：Bake 完成后自动切换 LightmapProbe + 加载角度
- [x] `app/debug_ui.cpp`：Lightmap/Probe 模式下 Start Bake 灰显

## Step 8.1：Bake 数据生命周期修复

- [x] `app/application.cpp`：`switch_scene()` — LP 模式时先 unload bake + 回退 IBL
- [x] `app/application.cpp`：`switch_environment()` — 同上
- [x] `app/application.cpp`：`clear_bake_cache` / `clear_all_cache` — 同上
- [x] ~~bake 角度加载前 UV 就绪保证~~ → 延续到 Step 8.2（UV 同步加载）
- [x] `app/src/renderer_bake.cpp`：`compute_lightmap_keys()` 使用 post-xatlas 数据（含 UV hash）
- [x] `app/application.cpp`：`switch_scene()` / `switch_environment()` 中 `refresh_lightmap_keys()` 与 `trigger_bake_scan()` 调用顺序修正
- [x] `app/renderer.h`：移除 `RenderInput::indirect_lighting_mode` + 对应填充代码
- [x] `shaders/forward.frag`：修正注释（stored irradiance × surface albedo → stored irradiance, multiplied by surface albedo）
- [x] `framework/src/bake_data_manager.cpp`：`scan()` 中 `std::stoul` + `catch(...)` → `std::from_chars`

## Step 8.2：Lightmap UV 同步加载 + 流程简化

- [x] `app/application.cpp`：场景加载后同步完成 xatlas 生成 + `apply_lightmap_uvs()` + BLAS/TLAS 重建（init / switch_scene）
- [x] `app/application.cpp`：`start_bake_session()` 移除 UV 准备步骤（场景加载时已完成）
- [x] `app/application.cpp`：移除 `ensure_lightmap_uvs()` （不再需要，从未合入）
- [x] `app/application.h`：`uv_generator_` 保留为成员（同步使用），移除异步控制逻辑
- [x] `app/debug_ui.h` + `app/debug_ui.cpp`：移除 Background UV Start/Stop/Progress 控件
- [x] `app/debug_ui.h`：`DebugUIContext` 移除 `bg_uv_*` 字段
- [x] `app/debug_ui.h`：`DebugUIActions` 移除 `bg_uv_*` 字段
- [x] `app/config.h` + `config.cpp`：移除 `bg_uv_thread_count` / `bg_uv_auto_start`，线程数改为运行时 `hardware_concurrency()`
- [x] `app/application.h`：移除 `resolve_thread_count()`
- [x] `app/application.cpp`：`refresh_lightmap_keys()` 已在 8.1c 中改用 post-xatlas 数据

## Step 8.3：Baking 守卫 + 文档修正 + RT 残留清理

- [x] `app/application.h`：`uv_generator_` 注释 "Background" → "Synchronous"
- [x] `app/application.h`：`start_bake_session()` Doxygen 更新（移除已删除的 UV 准备描述）
- [x] `framework/include/himalaya/framework/lightmap_uv_generator.h`：`@file` / 类文档中 "Background" → "Parallel"（保留多线程事实，去掉异步含义）
- [x] `app/debug_ui.cpp`：Baking 期间灰显场景加载 / HDR 加载按钮（已有守卫，无需修改）
- [x] `app/debug_ui.cpp`：Baking 期间灰显 IBL/LP radio button + Baked Angle List
- [x] `app/application.cpp`：`update_drag_input()` Baking 期间阻止 IBL 旋转拖拽
- [x] `app/application.cpp`：`switch_scene()` 中 `scene_loader_.destroy()` 后无条件清理 RT 数据（BLAS/TLAS/emissive SSBOs）

## Step 8.4：LightmapUVGenerator 内联重构

- [x] `framework/src/lightmap_uv_generator.cpp` + `framework/include/himalaya/framework/lightmap_uv_generator.h`：删除整个类
- [x] `framework/CMakeLists.txt`：移除 `lightmap_uv_generator.cpp`
- [x] `app/src/scene_loader.cpp` + `app/include/himalaya/app/scene_loader.h`：`apply_lightmap_uvs(uint32_t thread_count)` 内部并行化 xatlas 生成（Phase 1 并行 + Phase 2 顺序 GPU 上传），删除 `prepare_uv_requests()`
- [x] `app/include/himalaya/app/application.h`：移除 `uv_generator_` 成员 + `lightmap_uv_generator.h` include
- [x] `app/src/application.cpp`：`init()` / `switch_scene()` 调用改为 `scene_loader_.apply_lightmap_uvs(thread_count)`（合并原三行为一行）

## Step 8.45：xatlas 重构 — UV 生成移入烘焙管线

- [x] 8.45a. 场景加载精简：`load_meshes()` 移除 TEXCOORD_1 读取 / mesh_hash / `uv_pending_*` 记录
- [x] 8.45a. `scene_loader.h` 移除 `apply_lightmap_uvs()` / `original_cpu_vertices/indices()` / `uv_pending_*` / `original_cpu_*` 成员
- [x] 8.45a. `scene_loader.cpp` 删除 `apply_lightmap_uvs()` 实现 + `destroy()` 中对应 clear
- [x] 8.45a. `application.cpp` 移除 `init()` / `switch_scene()` 中的 `apply_lightmap_uvs()` 调用
- [x] 8.45a. `lightmap_uv.cpp` 移除 `read_cache()` / `write_cache()` / `CacheHeader` / `kCacheCategory` / `kPackResolution`
- [x] 8.45a. `generate_lightmap_uv()` 签名改为 `(vertices, indices, pack_resolution)` → 返回 `LightmapUVResult`，移除 `cache_hit` 字段
- [x] 8.45b. `compute_lightmap_keys()` 改用原始 geometry hash（positions + indices），不依赖 post-xatlas 数据
- [x] 8.45b. `refresh_lightmap_keys()` 移到场景加载后立即调用（不再等待 xatlas）
- [x] 8.45c. `start_bake()` 新增 xatlas 阶段：per-mesh 分组取 max resolution → 并行 xatlas → 顺序 VB/IB 重建 → BLAS/TLAS 重建
- [x] 8.45d. `lightmap_bake_finalize()` 新增 UV 数据写入（`<key>_rot<NNN>_uv.bin`，atomic_write_file）
- [x] 8.45e. `bake_data_manager.cpp`：`read_angle_uv_data()` + `scan()` UV bin 校验；`switch_bake_angle()` 读 UV → per-mesh 去重 → VB/IB 重建 → BLAS/TLAS 重建
- [x] 8.45f. 死代码清理：确认所有已移除的声明/实现无残留引用

## Step 8.5：Baker 质量改进 + Lightmap/Probe 独立开关 + UI 重排

### 8.5a. Baker Firefly Clamp

- [x] `passes/lightmap_baker_pass.h`：新增 `set_max_clamp(float)` 接口
- [x] `passes/lightmap_baker_pass.cpp`：`max_clamp_` 字段 + push constants 填充
- [x] `passes/probe_baker_pass.h`：新增 `set_max_clamp(float)` 接口
- [x] `passes/probe_baker_pass.cpp`：`max_clamp_` 字段 + push constants 填充
- [x] `framework/scene_data.h`：`BakeConfig` 新增 `float baker_clamp = 100.0f`
- [x] `app/renderer_bake.cpp`：`begin_bake_instance()` / `begin_probe_bake_instance()` 传入 locked_config clamp
- [x] `app/debug_ui.cpp`：Bake 面板新增 Baker Clamp slider（0 = disabled，范围 0~1000）

### 8.5b. Baker Denoise 开关

- [x] `framework/scene_data.h`：`BakeConfig` 新增 `bool denoise = true`
- [x] `app/renderer_bake.cpp`：`lightmap_bake_finalize()` 根据开关跳过 OIDN（直接用 noisy 数据）
- [x] `app/renderer_bake.cpp`：`probe_bake_finalize()` 根据开关跳过 per-face OIDN
- [x] `app/debug_ui.cpp`：Bake 面板新增 Denoise checkbox（baking 期间锁定）

### 8.5c. Lightmap/Probe 独立开关 + UI 重排

`lightmap_probe` 保留为 LP 模式总闸（`IndirectLightingMode` 不变）。
`use_lightmap` / `use_probe` 为 LP 模式内部的调试子开关，控制 shader 中
lightmap 和 probe 贡献是否生效。IBL 模式下隐藏，不影响模式切换逻辑。
切换回 LP 模式时不重置子开关（字段保持原值）。

- [x] `framework/scene_data.h`：`RenderFeatures` 新增 `use_lightmap = true` + `use_probe = true`（保留 `lightmap_probe`）
- [x] `shaders/common/bindings.glsl`：`FEATURE_LIGHTMAP_PROBE` → `FEATURE_LIGHTMAP (1u << 3)` + `FEATURE_PROBE (1u << 4)`
- [x] `shaders/forward.frag`：拆分为独立 diffuse/specular 分支（两个 bit 分别判断）
- [x] `app/renderer.cpp`：`fill_common_gpu_data()` — `lightmap_probe && use_lightmap` → bit 3，`lightmap_probe && use_probe` → bit 4
- [x] `app/debug_ui.cpp`：LP 模式下显示两个 checkbox（IBL 模式下隐藏）
- [x] `app/debug_ui.cpp`：Rendering 区移到 Camera 区之前

## Step 8.6：Lightmap Dilation（chart 接缝黑块修复）

- [x] `app/src/renderer_bake.cpp`：新增 `dilate_lightmap()` static 函数 — CPU 朴素 4 次全图迭代
- [x] `app/src/renderer_bake.cpp`：`lightmap_bake_finalize()` 中 OIDN 之后、upload flush 之前调用 dilation

## Step 8.7：独立 Bake Lightmap / Probe / All

- [ ] `framework/scene_data.h`：新增 `BakeMode` enum（`All`, `Lightmap`, `Probe`）
- [ ] `app/include/himalaya/app/debug_ui.h`：`DebugUIActions::bake_start_requested` → `bake_start_mode`（`std::optional<BakeMode>`）
- [ ] `app/src/debug_ui.cpp`：3 按钮替换 1 按钮 + enable 逻辑（Lightmap/Probe 仅当前角度已有完整数据时可用）
- [ ] `app/include/himalaya/app/renderer.h`：`start_bake()` 签名新增 `BakeMode` 参数 + `bake_mode_` 成员
- [ ] `app/src/renderer_bake.cpp`：Lightmap 模式完成后直接 Complete（跳过 probe）；Probe 模式跳过 xatlas/lightmap 直接进 BakingProbes
- [ ] `app/include/himalaya/app/application.h`：`start_bake_session()` 新增 `BakeMode` 参数
- [ ] `app/src/application.cpp`：分发 `bake_start_mode` 到 `start_bake_session(mode)`

## Step 9：AO/SO 按模式自动预设

- [ ] `app/application.h`：per-mode AOConfig 存储（`ao_config_ibl_` + `ao_config_lightmap_probe_`）
- [ ] `app/application.cpp`：模式切换时保存旧 → 加载新 AOConfig
- [ ] `ao_config_lightmap_probe_` 默认值调优（实测确定 radius / intensity）

## Step 10：Lightmap/Probe 模式 IBL 旋转跳变

- [ ] `app/application.cpp`：Lightmap/Probe 模式拦截 IBL 旋转拖拽
- [ ] `app/application.cpp`：累积偏移超阈值 → 在已 bake 角度列表中找下一个 → `switch_bake_angle()`
- [ ] IBL 模式保持自由旋转不变

## Step 11：Debug 渲染模式 + 收尾

- [ ] 新增 `DEBUG_MODE_LIGHTMAP_ONLY` passthrough 渲染模式
- [ ] 全流程 validation 检查（所有模式切换路径）
