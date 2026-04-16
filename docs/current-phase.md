# 当前阶段：M1 阶段八 — 间接光照集成

> 目标：将 Phase 7 烘焙器产出的 Lightmap 和 Reflection Probe 数据集成到光栅化渲染管线中，
> 使室内场景有间接光照（Lightmap），光滑表面反射周围环境（Probe）。
> 提供 IBL / Lightmap+Probe 两种间接光照模式切换，高完成度的 UI/UX。
>
> Phase 8.5（质量提升）范围：视差校正 Parallax Corrected Cubemap、多 probe 混合 + roughness 阈值、
> GPU per-pixel probe 网格查找、IBL 旋转拖拽距离-角度映射细化、lightmap 接缝处理（视情况）。
>
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: indirect_intensity 重命名（纯重构）
    ↓
Step 2: IndirectLightingMode enum + FEATURE_LIGHTMAP_PROBE
    ↓
Step 3: GPUInstanceData 扩展 + instance_index vert→frag 重构
    ↓
Step 4: GPUProbeData + ProbeBuffer SSBO（Set 0 binding 9）
    ↓
Step 5: BakeDataManager — 扫描与校验
    ↓
Step 6: BakeDataManager — 加载与卸载
    ↓
Step 7: Forward shader 间接光照集成（lightmap + probe 采样）
    ↓
Step 8: 模式切换 + 角度选择 UI
    ↓
Step 9: AO/SO 按模式自动预设
    ↓
Step 10: Lightmap/Probe 模式 IBL 旋转跳变
    ↓
Step 11: 边缘情况 + Debug 渲染模式 + 收尾
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | `indirect_intensity` 重命名 | grep 确认全代码库无 `ibl_intensity` 残留，渲染行为不变 |
| 2 | IndirectLightingMode enum + feature flag | 编译通过，feature_flags bit 3 默认 0，渲染不变 |
| 3 | GPUInstanceData 扩展 + instance_index 重构 | 三对 shader 改用 instance_index，渲染无回归（光栅化 + PT + Bake 三种模式） |
| 4 | GPUProbeData + ProbeBuffer SSBO | Set 0 binding 9 创建成功，无 validation 报错 |
| 5 | BakeDataManager 扫描与校验 | 已有 bake 数据时 UI 正确显示可用角度列表，无 bake 数据时列表为空 |
| 6 | BakeDataManager 加载与卸载 | 手动调用 load_angle() 后日志输出加载数量 + bindless 索引，unload_angle() 释放无泄漏 |
| 7 | Forward shader 间接光照集成 | 切换到 Lightmap/Probe 模式后场景可见烘焙间接光照 + probe 反射，IBL 模式无回归 |
| 8 | 模式切换 + 角度选择 UI | 模式 toggle 可用，角度点击切换正确加载/卸载，bake 完成后自动启用 |
| 9 | AO/SO 按模式自动预设 | 切换模式时 AO 参数自动切换，两模式各自记忆参数 |
| 10 | IBL 旋转跳变 | Lightmap/Probe 模式下拖拽 IBL 旋转超过阈值跳到下一个已 bake 角度 |
| 11 | 边缘情况 + debug 模式 | cache 清除回退 IBL，场景/HDR 重载触发重扫描，debug 视图正确 |

---

### Step 1：`indirect_intensity` 重命名

将 `ibl_intensity` 重命名为 `indirect_intensity`，纯代码重构，零行为变化。

- `framework/scene_data.h`：`GlobalUniformData::ibl_intensity` → `indirect_intensity`，更新 `static_assert` 偏移
- `shaders/common/bindings.glsl`：`GlobalUBO.ibl_intensity` → `indirect_intensity`
- `shaders/forward.frag`：4 处 `global.ibl_intensity` → `global.indirect_intensity`
- `app/renderer.h`：`RenderInput::ibl_intensity` → `indirect_intensity`
- `app/application.h`：`ibl_intensity_` → `indirect_intensity_`
- `app/application.cpp`：所有 `ibl_intensity` → `indirect_intensity`（RenderInput 填充等）
- `app/renderer.cpp`：`ubo_data.ibl_intensity` → `ubo_data.indirect_intensity`
- `app/renderer_pt.cpp`：`input.ibl_intensity` → `input.indirect_intensity`，`prev_ibl_intensity_` → `prev_indirect_intensity_`
- `app/debug_ui.h`：`DebugUIContext::ibl_intensity` → `indirect_intensity`
- `app/debug_ui.cpp`：slider label 暂时保持 `"IBL Intensity"`（Step 8 UI 阶段根据模式动态切换）

**验证**：`grep -r "ibl_intensity" --include="*.h" --include="*.cpp" --include="*.glsl" --include="*.frag" --include="*.vert"` 返回零结果（归档文档除外），渲染画面不变

---

### Step 2：IndirectLightingMode enum + FEATURE_LIGHTMAP_PROBE

引入间接光照模式枚举和 shader feature flag，但不改变任何渲染行为。

- `framework/scene_data.h`：新增 `enum class IndirectLightingMode : uint8_t { IBL, LightmapProbe }`
- `shaders/common/bindings.glsl`：新增 `#define FEATURE_LIGHTMAP_PROBE (1u << 3)`
- `framework/scene_data.h`：`RenderFeatures` 新增 `bool lightmap_probe = false`
- `app/application.h`：新增 `IndirectLightingMode indirect_lighting_mode_ = IndirectLightingMode::IBL`
- `app/renderer.h`：`RenderInput` 新增 `IndirectLightingMode indirect_lighting_mode` 字段
- `app/application.cpp`：RenderInput 填充新字段
- `app/renderer.cpp`：`fill_common_gpu_data()` 中根据 `RenderFeatures::lightmap_probe` 设置 `feature_flags` bit 3

**验证**：编译通过，`feature_flags` bit 3 默认为 0，渲染行为不变

---

### Step 3：GPUInstanceData 扩展 + instance_index Vert→Frag 重构

扩展 per-instance 数据结构，并将三对 shader 从传递 `frag_material_index` 改为传递 `frag_instance_index`。

#### 3a. GPUInstanceData 扩展

- `framework/scene_data.h`：`GPUInstanceData` 的 `_padding[3]` 改为 `lightmap_index`（uint32）+ `probe_index`（uint32）+ `_padding`（uint32），struct 保持 128 bytes
- `shaders/common/bindings.glsl`：`GPUInstanceData` 对应更新
- `app/renderer.cpp` 或 `app/renderer_rasterization.cpp`：InstanceBuffer 填充时将 `lightmap_index` 和 `probe_index` 写为 `UINT32_MAX`（哨兵值）

#### 3b. instance_index 重构（forward）

- `shaders/forward.vert`：移除 `frag_material_index`（location 4），新增 `frag_instance_index`（location 4, flat uint = `gl_InstanceIndex`），新增 `frag_uv1`（location 5, vec2 = `in_uv1`）
- `shaders/forward.frag`：移除 `frag_material_index`（location 4），新增 `frag_instance_index`（location 4, flat uint），新增 `frag_uv1`（location 5, vec2）。通过 `instances[frag_instance_index]` 读取 `material_index`

#### 3c. instance_index 重构（depth_prepass）

- `shaders/depth_prepass.vert`：`frag_material_index` → `frag_instance_index`（location 3, flat uint = `gl_InstanceIndex`）
- `shaders/depth_prepass.frag`：通过 `instances[frag_instance_index].material_index` 读取
- `shaders/depth_prepass_masked.frag`：同上

#### 3d. instance_index 重构（shadow）

- `shaders/shadow.vert`：`frag_material_index` → `frag_instance_index`（location 1, flat uint = `gl_InstanceIndex`）
- `shaders/shadow_masked.frag`：通过 `instances[frag_instance_index].material_index` 读取

**验证**：光栅化渲染无回归（含 alpha mask 物体），PT 参考视图无回归，Bake 模式无回归。无 validation 报错

---

### Step 4：GPUProbeData + ProbeBuffer SSBO

新增 probe 数据结构和 Set 0 binding 9。

- `framework/scene_data.h`：新增 `GPUProbeData` 结构体（`glm::vec3 position` + pad + `glm::vec3 aabb_min` + pad + `glm::vec3 aabb_max` + `uint32_t cubemap_index`，48 bytes std430）
- `shaders/common/bindings.glsl`：新增 `GPUProbeData` struct 定义 + `layout(set = 0, binding = 9) readonly buffer ProbeBuffer { GPUProbeData probes[]; }`（不受 `HIMALAYA_RT` 守卫，`PARTIALLY_BOUND`）
- `rhi/descriptors.h`：（若需要）更新注释说明 binding 9
- `rhi/descriptors.cpp`：`create_layouts()` 中 Set 0 layout 新增 binding 9（`STORAGE_BUFFER`，`PARTIALLY_BOUND`，`VERTEX | FRAGMENT` stage，不受 `rt_supported` 条件守卫）
- `rhi/descriptors.h`：新增 `write_set0_probe_buffer()` 方法
- `rhi/descriptors.cpp`：实现 `write_set0_probe_buffer()`

**验证**：编译通过，Set 0 layout 创建成功，无 validation 报错。Binding 9 未写入时 shader 不访问（`PARTIALLY_BOUND` 保护）

---

### Step 5：BakeDataManager — 扫描与校验

Framework 层新建 `BakeDataManager` 类，迁移已有的 bake 角度扫描与完整性校验逻辑。

- 新增 `framework/bake_data_manager.h`：类声明，`init()` / `destroy()` / `scan()` / `available_angles()` / `has_bake_data()`
- 新增 `framework/bake_data_manager.cpp`：
  - `scan(lightmap_keys, probe_set_key)`：扫描 `cache_root()/bake/` 目录，从 manifest 文件名提取角度，逐角度校验（lightmap KTX2 完整 + probe KTX2 完整），构建 `available_angles_` 列表
  - 校验逻辑从 `debug_ui.cpp:1076-1162` 迁移
- `framework/CMakeLists.txt`：添加 `bake_data_manager.cpp`
- `app/renderer.h`：Renderer 新增 `BakeDataManager bake_data_manager_` 成员
- `app/renderer.cpp`（init/destroy）：调用 `bake_data_manager_.init()` / `bake_data_manager_.destroy()`
- `app/debug_ui.cpp`：移除 `baked_angles_` 本地列表和扫描逻辑，改为从 `DebugUIContext` 中读取 BakeDataManager 提供的可用角度列表
- `app/debug_ui.h`：`DebugUIContext` 新增 `std::span<const BakeDataManager::AngleInfo> available_angles` 字段，移除 `bake_angles_dirty`

**验证**：有已 bake 数据时 UI 角度列表显示正确（与重构前一致），无已 bake 数据时列表为空。扫描时机与重构前一致

---

### Step 6：BakeDataManager — 加载与卸载

BakeDataManager 实现角度加载（KTX2 → GPU → bindless）和卸载。

#### 6a. Lightmap 加载

- `bake_data_manager.h`：`load_angle(rotation_int, lightmap_keys, mesh_instances)` / `unload_angle()`
- `bake_data_manager.cpp`：
  - 遍历 lightmap_keys，构建文件路径 `cache_path("bake", key + "_rot" + NNN, ".ktx2")`
  - 使用已有 KTX2 加载工具读取 BC6H 数据 → 创建 GPU image → 注册 `register_texture(image, linear_clamp_sampler)`
  - 存储 per-instance `lightmap_indices_`（parallel to mesh_instances，无 lightmap 的 instance 填 `UINT32_MAX`）

#### 6b. Probe 加载 + 分配

- `bake_data_manager.cpp`：
  - 读取 manifest 文件 → probe_count + positions
  - 逐 probe 加载 KTX2 cubemap → 创建 GPU cubemap image → 注册 `register_cubemap(image, default_sampler)`
  - 填充 `GPUProbeData` 数组（position、aabb_min/max 暂填零、cubemap_index）→ 上传 ProbeBuffer SSBO → `write_set0_probe_buffer()`
  - CPU probe-to-instance 分配：每个 instance 的世界空间 AABB 中心 → 找最近 probe → 存储 `probe_indices_`

#### 6c. 卸载

- `bake_data_manager.cpp`：`unload_angle()` 注销 bindless（`unregister_texture` / `unregister_cubemap`）、销毁 GPU image、清空 `lightmap_indices_` / `probe_indices_`

#### 6d. Renderer 集成

- `app/renderer.h`：新增 `switch_bake_angle(uint32_t rotation_int)` 方法
- `app/renderer.cpp`：`switch_bake_angle()` 实现——`vkQueueWaitIdle` → `bake_data_manager_.unload_angle()` → immediate scope 内 `bake_data_manager_.load_angle()` → `end_immediate()`
- `app/renderer.cpp`：InstanceBuffer 填充逻辑从 `bake_data_manager_` 查询 `lightmap_index` / `probe_index`（当前无已加载角度时全部为 `UINT32_MAX`）

**设计要点**：
- Lightmap 纹理注册到 Set 1 binding 0（`textures[]`），使用 linear clamp sampler（Renderer 已有 `linear_clamp_sampler_`，BakeDataManager 通过 init 参数接收）
- Probe cubemap 注册到 Set 1 binding 1（`cubemaps[]`），使用 default sampler（linear repeat）
- BakeDataManager 不依赖 Renderer 具体实现——通过 init 接收 `ResourceManager*`、`DescriptorManager*`、`SamplerHandle` 引用

**验证**：手动触发 `load_angle()` 后日志输出加载的 lightmap 数量 + probe 数量 + bindless 索引。`unload_angle()` 后无资源泄漏。无 validation 报错

---

### Step 7：Forward Shader 间接光照集成

在 `forward.frag` 中实现 Lightmap/Probe 采样，替代 IBL 间接光照。

- `shaders/forward.frag`：重构间接光照计算部分：
  - BRDF LUT 查找提到条件分支之前（两种模式共用）
  - 读取 `GPUInstanceData inst = instances[frag_instance_index]`
  - `bool use_lightmap = (feature_flags & FEATURE_LIGHTMAP_PROBE) != 0u && inst.lightmap_index != 0xFFFFFFFFu`
  - **Lightmap 分支**（`use_lightmap == true`）：
    - Diffuse：`texture(textures[nonuniformEXT(inst.lightmap_index)], frag_uv1).rgb * diffuse_color`（lightmap 存储入射辐照度，需乘表面漫反射色）
    - Specular：若 `inst.probe_index != 0xFFFFFFFFu`，从 ProbeBuffer 读 probe data，`textureLod(cubemaps[probe.cubemap_index], R, roughness * (textureQueryLevels(...) - 1))`；否则回退 IBL specular
  - **IBL 分支**（`use_lightmap == false`）：保持现有 IBL 采样逻辑（含 IBL rotation）
  - AO/SO 应用于两种模式的 indirect 结果（与决策一致）
  - `indirect_intensity` 乘数应用于两种模式的 indirect 结果
- `shaders/forward.frag`：更新 debug render mode `DEBUG_MODE_IBL_ONLY` → `DEBUG_MODE_INDIRECT_ONLY`（显示当前激活的间接光照）
- `shaders/common/bindings.glsl`：`#define DEBUG_MODE_IBL_ONLY 3` → `#define DEBUG_MODE_INDIRECT_ONLY 3`

**设计要点**：
- Lightmap 分支中不做 IBL rotation（probe cubemap 已在世界空间正确方向）
- IBL 回退分支中使用 `ibl_rotation_sin/cos`（Application 在 Lightmap/Probe 模式下设置为已 bake 角度的旋转值）
- Probe specular 不需要 IBL rotation（probe 在 bake 时已包含正确的环境光方向）
- `textureQueryLevels()` 返回 probe cubemap 的 mip 级数，用于 roughness-based LOD
- 分支在 per-instance 级别不发散（同一 draw call 的所有 fragment 走同一分支）

**验证**：
- IBL 模式：渲染完全不变（feature flag 为 0，走 IBL 分支）
- 手动设置 feature flag + 加载 bake 数据后：Lightmap/Probe 模式下场景可见间接光照 + probe 反射
- 切换模式对比效果合理

---

### Step 8：模式切换 + 角度选择 UI

实现完整的 UI 交互流程：模式切换、角度选择、Bake 后自动启用。

- `app/debug_ui.h`：`DebugUIContext` 新增 `IndirectLightingMode& indirect_lighting_mode` 引用、`bool has_bake_data`（可用角度非空）
- `app/debug_ui.cpp`：
  - 模式 toggle：`has_bake_data == false` 时 Lightmap/Probe 选项灰显
  - 已 bake 角度列表：每项可点击，点击触发 `DebugUIActions` 中的角度切换请求
  - `"IBL Intensity"` slider label 根据 `indirect_lighting_mode` 动态显示：IBL 模式 → `"IBL Intensity"`，Lightmap/Probe 模式 → `"Indirect Intensity"`
  - Bake 期间模式 toggle 灰显
- `app/application.cpp`：
  - 检测 DebugUI 的角度切换请求 → 调用 `renderer_.switch_bake_angle(rotation)`
  - 检测模式切换 → IBL → LightmapProbe：调用 `switch_bake_angle()` 加载首个可用角度（或上次选中角度）；LightmapProbe → IBL：调用 `unload_angle()` 释放资源
  - Bake 完成后（`BakeState::Complete` 检测）：调用 `bake_data_manager_.scan()` 刷新角度列表，自动切换到 LightmapProbe 模式 + 加载刚 bake 的角度
- `app/application.cpp`：`IndirectLightingMode` 与 `RenderFeatures::lightmap_probe` 同步（模式为 LightmapProbe 时 flag = true）
- `app/debug_ui.cpp`：Lightmap/Probe 模式下 Start Bake 按钮灰显（Bake 操作仅在 IBL 模式下可触发）

**验证**：
- 无 bake 数据时 Lightmap/Probe 选项灰显不可选
- 有 bake 数据时可切换模式，画面正确切换（IBL ↔ Lightmap/Probe）
- 角度列表点击切换正确加载新数据
- Bake 完成后自动启用 Lightmap/Probe 模式 + 加载对应角度
- Clear Bake Cache 后自动回退 IBL 模式

---

### Step 9：AO/SO 按模式自动预设

模式切换时自动应用该模式的 AO 参数预设，两种模式各自记忆用户的调整。

- `app/application.h`：新增 `AOConfig ao_config_ibl_` 和 `AOConfig ao_config_lightmap_probe_`（后者默认较小 radius + 较低 intensity）
- `app/application.cpp`：模式切换时保存当前 AOConfig 到旧模式 → 加载新模式的 AOConfig
- `app/application.cpp`：`ao_config_lightmap_probe_` 默认值：radius 适当缩小、intensity 适当降低（具体值实测调整）

**验证**：
- IBL → Lightmap/Probe：AO slider 自动跳到 Lightmap/Probe 预设值
- Lightmap/Probe 下手动调 AO → 切回 IBL → 切回 Lightmap/Probe：手动调整值被记忆
- IBL 模式 AO 预设不受影响

---

### Step 10：Lightmap/Probe 模式 IBL 旋转跳变

Lightmap/Probe 模式下 IBL 旋转不能自由拖动，改为拖过阈值后跳到下一个已 bake 角度。

- `app/application.cpp`（或相关 input 处理代码）：Lightmap/Probe 模式下拦截 IBL 旋转拖拽
  - 累积拖拽角度偏移量
  - 超过固定阈值（如 15°）→ 在已 bake 角度列表中找到下一个方向的角度 → 触发 `switch_bake_angle()`
  - 重置累积偏移
- IBL 模式下保持自由旋转行为不变

**设计要点**：
- Phase 8 使用固定阈值（如 15°），Phase 8.5 细化为与角度差成比例的映射
- 已 bake 角度按度数排序，拖拽方向决定是找下一个还是上一个
- 只有 1 个已 bake 角度时拖拽无效果

**验证**：Lightmap/Probe 模式下拖拽 IBL 旋转超过阈值后跳到下一个角度，画面正确更新

---

### Step 11：边缘情况 + Debug 渲染模式 + 收尾

处理边缘情况，添加 debug 视图，完成收尾。

- **Clear Bake Cache**：清除后若当前处于 Lightmap/Probe 模式 → `unload_angle()` → 自动回退 IBL 模式 → 刷新角度列表（空）
- **场景重载 / HDR 重载**：重新调用 `bake_data_manager_.scan()` 刷新角度列表。若当前角度不在新列表中 → 卸载 → 回退 IBL 模式
- **Debug 渲染模式**：新增 `DEBUG_MODE_LIGHTMAP_ONLY`（passthrough，仅显示 lightmap 采样值）。在 Lightmap/Probe 模式下可用，IBL 模式下显示黑色
- **Bake 完成后角度列表刷新**：确认 `scan()` 在 bake 完成时被调用
- **Validation 全面检查**：所有模式切换路径无 validation 报错

**验证**：
- Clear Bake Cache → 自动回退 IBL，UI 角度列表清空
- 加载新场景 → 旧 bake 数据正确处理（角度列表更新或清空）
- Debug 模式 `LIGHTMAP_ONLY` 正确显示 lightmap UV 空间颜色
- 全流程无 validation 报错
