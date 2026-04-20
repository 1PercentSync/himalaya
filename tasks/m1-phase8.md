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
- [ ] 延续到 Step 8.2：lightmap UV 同步加载 + bake 角度加载前 UV 就绪保证
- [x] `app/src/renderer_bake.cpp`：`compute_lightmap_keys()` 使用 post-xatlas 数据（含 UV hash）
- [x] `app/application.cpp`：`switch_scene()` / `switch_environment()` 中 `refresh_lightmap_keys()` 与 `trigger_bake_scan()` 调用顺序修正
- [x] `app/renderer.h`：移除 `RenderInput::indirect_lighting_mode` + 对应填充代码
- [x] `shaders/forward.frag`：修正注释（stored irradiance × surface albedo → stored irradiance, multiplied by surface albedo）
- [x] `framework/src/bake_data_manager.cpp`：`scan()` 中 `std::stoul` + `catch(...)` → `std::from_chars`

## Step 8.2：Lightmap UV 同步加载 + 流程简化

- [ ] `app/application.cpp`：场景加载后同步完成 xatlas 生成 + `apply_lightmap_uvs()` + BLAS/TLAS 重建（init / switch_scene）
- [ ] `app/application.cpp`：`start_bake_session()` 移除 UV 准备步骤（场景加载时已完成）
- [ ] `app/application.cpp`：移除 `ensure_lightmap_uvs()` （不再需要）
- [ ] `app/application.h`：移除 `uv_generator_` 成员 + 相关异步控制逻辑
- [ ] `app/debug_ui.h` + `app/debug_ui.cpp`：移除 Background UV Start/Stop/Progress 控件
- [ ] `app/debug_ui.h`：`DebugUIContext` 移除 `bg_uv_*` 字段
- [ ] `app/debug_ui.h`：`DebugUIActions` 移除 `bg_uv_*` 字段
- [ ] `app/application.h`：`AppConfig` 移除 `bg_uv_thread_count` / `bg_uv_auto_start`（或保留 thread_count 供同步生成用）
- [ ] `app/application.cpp`：`refresh_lightmap_keys()` 调整——场景加载时 UV 已 apply，直接用 post-xatlas 数据

## Step 8.5：Lightmap/Probe 独立开关 + UI 重排

- [ ] `framework/scene_data.h`：`RenderFeatures::lightmap_probe` → `use_lightmap` + `use_probe`
- [ ] `shaders/common/bindings.glsl`：`FEATURE_LIGHTMAP_PROBE` → `FEATURE_LIGHTMAP (1u << 3)` + `FEATURE_PROBE (1u << 4)`
- [ ] `shaders/forward.frag`：分别检查两个 bit（lightmap off → IBL diffuse，probe off → IBL specular）
- [ ] `app/renderer.cpp`：`fill_common_gpu_data()` 分别设置 bit 3/4
- [ ] `app/application.cpp`：模式切换时同步 `use_lightmap` / `use_probe`
- [ ] `app/debug_ui.cpp`：LP 模式下两个 checkbox + IBL 模式下隐藏
- [ ] `app/debug_ui.cpp`：Rendering 区移到 Camera 区之前

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
