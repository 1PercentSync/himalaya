# M1 阶段八：间接光照集成 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：`indirect_intensity` 重命名

- [ ] `framework/scene_data.h`：`GlobalUniformData::ibl_intensity` → `indirect_intensity` + `static_assert` 更新
- [ ] `shaders/common/bindings.glsl`：`GlobalUBO.ibl_intensity` → `indirect_intensity`
- [ ] `shaders/forward.frag`：4 处 `global.ibl_intensity` → `global.indirect_intensity`
- [ ] `app/renderer.h`：`RenderInput::ibl_intensity` → `indirect_intensity` + `prev_ibl_intensity_` → `prev_indirect_intensity_`
- [ ] `app/application.h` + `app/application.cpp`：`ibl_intensity_` → `indirect_intensity_` + RenderInput 填充
- [ ] `app/renderer.cpp` + `app/renderer_pt.cpp`：UBO 填充 + PT 变化检测
- [ ] `app/debug_ui.h` + `app/debug_ui.cpp`：`DebugUIContext` 字段 + slider 引用更新

## Step 2：IndirectLightingMode enum + FEATURE_LIGHTMAP_PROBE

- [ ] `framework/scene_data.h`：新增 `IndirectLightingMode` enum + `RenderFeatures::lightmap_probe` bool
- [ ] `shaders/common/bindings.glsl`：新增 `#define FEATURE_LIGHTMAP_PROBE (1u << 3)`
- [ ] `app/application.h`：新增 `indirect_lighting_mode_` 成员
- [ ] `app/renderer.h`：`RenderInput` 新增 `indirect_lighting_mode` 字段
- [ ] `app/application.cpp` + `app/renderer.cpp`：RenderInput 填充 + `fill_common_gpu_data()` feature_flags 写入

## Step 3：GPUInstanceData 扩展 + instance_index 重构

- [ ] `framework/scene_data.h` + `shaders/common/bindings.glsl`：GPUInstanceData `_padding[3]` → `lightmap_index` + `probe_index` + `_padding`
- [ ] `app/renderer*.cpp`：InstanceBuffer 填充 lightmap_index / probe_index 为 `UINT32_MAX`
- [ ] `shaders/forward.vert` + `shaders/forward.frag`：`frag_material_index` → `frag_instance_index` + 新增 `frag_uv1`
- [ ] `shaders/depth_prepass.vert` + `shaders/depth_prepass.frag` + `shaders/depth_prepass_masked.frag`：`frag_material_index` → `frag_instance_index`
- [ ] `shaders/shadow.vert` + `shaders/shadow_masked.frag`：`frag_material_index` → `frag_instance_index`

## Step 4：GPUProbeData + ProbeBuffer SSBO

- [ ] `framework/scene_data.h`：新增 `GPUProbeData` struct（48 bytes：position + aabb_min + aabb_max + cubemap_index）
- [ ] `shaders/common/bindings.glsl`：新增 `GPUProbeData` struct + `layout(set = 0, binding = 9) readonly buffer ProbeBuffer`（非 RT-only，PARTIALLY_BOUND）
- [ ] `rhi/descriptors.h` + `rhi/descriptors.cpp`：Set 0 layout 新增 binding 9 + `write_set0_probe_buffer()` 方法

## Step 5：BakeDataManager — 扫描与校验

- [ ] 新增 `framework/bake_data_manager.h`：类声明（`init` / `destroy` / `scan` / `available_angles` / `has_bake_data`）
- [ ] 新增 `framework/bake_data_manager.cpp`：`scan()` 实现（从 `debug_ui.cpp` 迁移扫描 + 校验逻辑）
- [ ] `framework/CMakeLists.txt`：添加 `bake_data_manager.cpp`
- [ ] `app/renderer.h`：新增 `BakeDataManager bake_data_manager_` 成员 + init/destroy 调用
- [ ] `app/debug_ui.h` + `app/debug_ui.cpp`：移除本地扫描逻辑，改为从 DebugUIContext 读取 BakeDataManager 数据

## Step 6：BakeDataManager — 加载与卸载

- [ ] `bake_data_manager.h` / `.cpp`：`load_angle()` — lightmap KTX2 加载 + bindless 注册 + per-instance `lightmap_indices_`
- [ ] `bake_data_manager.h` / `.cpp`：`load_angle()` — probe KTX2 加载 + bindless cubemap 注册 + ProbeBuffer SSBO 填充
- [ ] `bake_data_manager.h` / `.cpp`：`load_angle()` — CPU probe-to-instance 分配（最近 probe by AABB center 距离）+ per-instance `probe_indices_`
- [ ] `bake_data_manager.h` / `.cpp`：`unload_angle()` — bindless 注销 + image 销毁 + 清空 indices
- [ ] `app/renderer.h` / `.cpp`：新增 `switch_bake_angle()` 方法（GPU idle → unload → load）
- [ ] `app/renderer*.cpp`：InstanceBuffer 填充时从 `bake_data_manager_` 查询 lightmap_index / probe_index

## Step 7：Forward Shader 间接光照集成

- [ ] `shaders/forward.frag`：BRDF LUT 查找移到分支前（两模式共用）
- [ ] `shaders/forward.frag`：Lightmap 分支 — lightmap 采样 × diffuse_color
- [ ] `shaders/forward.frag`：Probe 分支 — probe cubemap 采样（roughness-based mip via `textureQueryLevels()`），无 probe 时回退 IBL specular
- [ ] `shaders/forward.frag`：IBL 分支 — 保持现有逻辑
- [ ] `shaders/forward.frag`：indirect_intensity 乘数 + AO/SO 应用于两种模式
- [ ] `shaders/common/bindings.glsl`：`DEBUG_MODE_IBL_ONLY` → `DEBUG_MODE_INDIRECT_ONLY`

## Step 8：模式切换 + 角度选择 UI

- [ ] `app/debug_ui.h` + `.cpp`：IndirectLightingMode toggle（无 bake 数据时灰显 Lightmap/Probe）
- [ ] `app/debug_ui.cpp`：Indirect Intensity slider label 根据模式动态显示
- [ ] `app/debug_ui.cpp`：已 bake 角度列表可点击切换 + 角度切换 action
- [ ] `app/application.cpp`：检测角度切换 action → 调用 `switch_bake_angle()`
- [ ] `app/application.cpp`：模式切换 → IBL↔LightmapProbe 加载/卸载
- [ ] `app/application.cpp`：Bake 完成后自动切换 LightmapProbe + 加载角度
- [ ] `app/debug_ui.cpp`：Lightmap/Probe 模式下 Start Bake 灰显

## Step 9：AO/SO 按模式自动预设

- [ ] `app/application.h`：per-mode AOConfig 存储（`ao_config_ibl_` + `ao_config_lightmap_probe_`）
- [ ] `app/application.cpp`：模式切换时保存旧 → 加载新 AOConfig
- [ ] `ao_config_lightmap_probe_` 默认值调优（实测确定 radius / intensity）

## Step 10：Lightmap/Probe 模式 IBL 旋转跳变

- [ ] `app/application.cpp`：Lightmap/Probe 模式拦截 IBL 旋转拖拽
- [ ] `app/application.cpp`：累积偏移超阈值 → 在已 bake 角度列表中找下一个 → `switch_bake_angle()`
- [ ] IBL 模式保持自由旋转不变

## Step 11：边缘情况 + Debug 渲染模式 + 收尾

- [ ] Clear Bake Cache → unload_angle + 回退 IBL + 刷新角度列表
- [ ] 场景/HDR 重载 → 重新 scan + 当前角度失效处理
- [ ] 新增 `DEBUG_MODE_LIGHTMAP_ONLY` passthrough 渲染模式
- [ ] 全流程 validation 检查（所有模式切换路径）
