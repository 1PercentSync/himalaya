# 当前阶段：M1 阶段八点五 — 间接光照质量提升

> 目标：提升 Reflection Probe 系统的放置质量、选取精度和反射准确性。
> 三个核心改进：Probe 放置从"剔除"演进为"移动+重试"、Per-pixel probe 选取 + blend、PCC 视差校正。
>
> Phase 8 基础设施（BakeDataManager、ProbeBuffer SSBO、forward shader 间接光照分支）在此阶段重构和扩展。
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: Probe relocation — pre-bake（修改 filter shader + placement 逻辑）
    ↓
Step 2: Probe relocation — post-bake（修改 bake 循环）
    ↓
Step 3: GPUInstanceData 清理 + probe_count 引入（基础设施重构）
    ↓
Step 4: 3D Grid 空间索引（加载时构建）
    ↓
Step 5: Per-pixel probe 选取 + blend（shader 改造，依赖 Step 3/4）
    ↓
Step 6: Runtime 参数 + ImGui 面板
    ↓
Step 7: AABB 计算（新 pass，依赖 relocation 完成后的最终位置）
    ↓
Step 8: Manifest 格式扩展 + AABB 写入（依赖 Step 7 产出）
    ↓
Step 9: PCC 视差校正（shader 扩展，依赖 Step 5 的 probe 选取 + Step 8 的 AABB 数据）
    ↓
Step 10: 收尾
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | Probe relocation — pre-bake | 原本被 Rule 1/2 剔除的 probe 有机会移动后通过，survivor 数量上升，日志输出 relocated 统计 |
| 2 | Probe relocation — post-bake | 部分面全黑的 probe 被移动后重新 bake，post-bake reject 率显著下降 |
| 3 | GPUInstanceData 清理 + probe_count | `probe_index` 字段移除，GlobalUBO 含 `probe_count`，BakeDataManager 无 CPU probe 分配逻辑 |
| 4 | 3D Grid 空间索引 | Grid buffer 上传 GPU，fragment shader 可通过世界坐标查询 cell 内 probe 列表 |
| 5 | Per-pixel probe 选取 + blend | 每个 fragment 从 5×5×5 邻域选取 top-2 probe，法线半球过滤 + roughness 平滑过渡 |
| 6 | Runtime 参数 + ImGui | `normal_bias`、`roughness_single/full`、`blend_curve` 可调，实时影响画面 |
| 7 | AABB 计算 | 每个 surviving probe 有合理的 AABB（非零，反映周围几何），日志输出 AABB 尺寸统计 |
| 8 | Manifest 格式扩展 | manifest 包含 position + AABB，BakeDataManager 正确读取并填入 GPUProbeData |
| 9 | PCC 视差校正 | 反射贴合房间墙壁，对比 PCC 开/关效果明显 |
| 10 | 收尾 | 全流程无 validation 报错，debug 模式可用 |

---

### Step 1：Probe Relocation — Pre-bake

两层测试失败的候选 probe 不再直接剔除，而是移动到更好的位置重试一次。

#### 1a. probe_filter.comp 扩展输出

- 当前输出 `uint result`（0/1/2）改为扩展结构：`uint result` + `vec3 nearest_hit_pos` + `vec3 nearest_hit_normal`
- 对于 result = 0（通过）的候选，hit 数据不使用
- 对于 result = 1 或 2（失败）的候选，记录所有射线中 `t_hit` 最小的命中：`hit_pos = origin + dir * t_hit`，几何法线通过 GeometryInfo buffer reference 读取三角形顶点重建面法线（数据路径与 closesthit 相同）
- Rule 1 的 early return 改为：记录当前命中信息后再写 result 并 return
- ResultBuffer 从 `uint[]` 改为结构化 buffer（每候选 32 bytes，使用 `GL_EXT_scalar_block_layout`（shader 已启用）避免 vec3 的 16B 对齐：`uint result`(4B) + `float hit_pos[3]`(12B) + `float hit_normal[3]`(12B) + `uint _pad`(4B) = 32B）
- `probe_placement.cpp`：result buffer 大小从 `total * 4` 改为 `total * 32`，readback 解析改为结构化读取

#### 1b. probe_placement.cpp relocation 逻辑

- Phase 3（收集 survivors）扩展：对 result = 1 或 2 的候选，计算 relocated position = `nearest_hit_pos + nearest_hit_normal * probe_relocation_offset`
- `probe_relocation_offset` 为 bake-time 可调参数（默认 0.1m）
- 收集所有 relocated 候选，组成第二批 candidates
- 对第二批执行同一 filter shader（复用同一 pipeline + dispatch）
- 第二批通过的加入 survivors，仍失败的最终剔除
- 日志输出：`relocated N candidates, M passed retry`

#### 1c. BakeConfig 扩展

- `framework/scene_data.h`：`BakeConfig` 新增 `float probe_relocation_offset`（默认 0.1f，单位 meters）

**验证**：survivor 数量比不启用 relocation 时增加，relocated probe 位于几何表面外侧合法位置

---

### Step 2：Probe Relocation — Post-bake

Bake 后检测到部分面全黑的 probe，移动后重新 bake 一次。

#### 2a. 黑面方向分析

- `renderer_bake.cpp`：现有的 per-face luminance 检查扩展——不再立即 reject，而是记录哪些 face 低于阈值
- Cubemap face 方向映射：face 0-5 对应 +X/-X/+Y/-Y/+Z/-Z
- 计算移动方向：所有黑面反方向的归一化合成向量
- 全 6 面都黑 → 直接剔除（无处可移）

#### 2b. 移动 + 重新 bake

- 新位置 = 当前位置 + 移动方向 × `probe_relocation_offset`（复用 pre-bake 的参数）
- 在新位置重新执行完整的 probe bake（6 faces × target SPP）
- 重新 bake 后再次执行黑面检测
- 仍有黑面 → 最终剔除，已通过 → 接受新位置
- 更新 `bake_probe_positions_` 中该 probe 的位置为新位置

#### 2c. 进度统计

- `RenderProgress` 扩展：`probes_relocated` 计数（区别于 `probes_rejected`）
- DebugUI 显示 relocated vs rejected 统计

**验证**：post-bake reject 率相比 Phase 8 显著下降，relocated probe 的 cubemap 无全黑面

---

### Step 3：GPUInstanceData 清理 + probe_count 引入

#### 3a. GPUInstanceData::probe_index 移除

- `framework/scene_data.h`：`GPUInstanceData` 的 `probe_index` 改为 `_padding2`，struct 保持 128 bytes
- `shaders/common/bindings.glsl`：`GPUInstanceData` 对应更新
- `shaders/forward.frag`：移除 `inst.probe_index` 读取

#### 3b. BakeDataManager probe 分配清理

- `bake_data_manager.cpp`：`load_angle()` 中移除 CPU probe-to-instance 分配逻辑（`probe_indices_` 赋值循环）
- `bake_data_manager.h`：移除 `probe_indices()` 访问器和 `probe_indices_` 成员
- `bake_data_manager.h`：新增 `uint32_t loaded_probe_count() const`（返回当前已加载的 probe 数量，未加载时返回 0）
- `bake_data_manager.h`：`load_angle()` 签名移除 `mesh_instances` 参数（不再需要做 probe-to-instance 分配），调用方同步更新
- `renderer_rasterization.cpp`：`build_draw_groups()` 移除 `probe_indices` 参数和相关填充逻辑

#### 3c. GlobalUBO 新增 probe_count

- `framework/scene_data.h`：`GlobalUniformData` 新增 `uint32_t probe_count`（默认 0），更新 `static_assert` 偏移。注意 std140 对齐——当前结构大小 928B，Phase 8.5 最终新增 5 字段（1 uint + 4 float = 20B），总 948B 需 padding 到 960B（16 的倍数）。`probe_count` 在 Step 3 引入，其余 4 个 float 在 Step 6 引入，padding 在最后一次扩展时补齐
- `shaders/common/bindings.glsl`：`GlobalUBO` 对应更新
- `app/renderer.cpp`：`fill_common_gpu_data()` 中调用 `bake_data_manager_.loaded_probe_count()` 填入

**验证**：编译通过，渲染无回归，`probe_index` 无残留引用

---

### Step 4：3D Grid 空间索引

加载 bake 数据时构建世界空间 3D grid，供 fragment shader 快速查找附近 probe。

#### 4a. Grid 数据结构（CPU 侧）

- `bake_data_manager.h`：新增 `ProbeGrid3D` 内部结构
  - `glm::vec3 grid_origin`（所有 probe 位置的 AABB 最小角再减去 2 × cell_size，使边缘 probe 的 5×5×5 查询不越界）
  - `float cell_size`（= bake config 的 grid_spacing）
  - `glm::uvec3 grid_dims`（各轴 cell 数量，= 原始 probe AABB 范围 / cell_size 向上取整 + 4（两端各扩展 2 个 cell））
  - `std::vector<uint32_t> cell_offsets`（`grid_dims.x * y * z + 1` 个元素，CSR 风格前缀和）
  - `std::vector<uint32_t> probe_indices`（flat 数组，cell_offsets 索引进来）
- `load_angle()` 加载 probe 后构建：遍历 probe positions → 计算 cell 坐标 → 统计 per-cell count → 前缀和 → 填充 indices
- `probe_count == 0` 时跳过 grid 构建，不创建 GPU buffer

#### 4b. GPU Buffer 上传

- 单个 SSBO，Set 0 binding 10（`ProbeGridBuffer`，非 RT-only，`PARTIALLY_BOUND`，`VERTEX | FRAGMENT` stage），布局：
  - Header (32B)：`vec4 grid_origin_and_cell_size`(16B) + `uvec4 grid_dims_and_pad`(16B)
  - `uint cell_offsets[cell_count + 1]`（CSR 前缀和，`cell_count = dims.x * dims.y * dims.z`）
  - `uint probe_indices[]`（flat probe index 数组）
- `rhi/descriptors.cpp`：`create_layouts()` 中 Set 0 新增 binding 10（RT 和非 RT 两条路径均需更新）
- `rhi/descriptors.h`：新增 `write_set0_probe_grid_buffer()` 方法

#### 4c. Shader 端查询

- `shaders/common/probe_grid.glsl`（新文件）：
  ```
  ivec3 center_cell = clamp(ivec3((frag_world_pos - grid_origin) / cell_size), ivec3(0), ivec3(grid_dims - 1))
  for dx in [-2, +2]:
    for dy in [-2, +2]:
      for dz in [-2, +2]:
        cell = clamp(center_cell + ivec3(dx,dy,dz), ivec3(0), ivec3(grid_dims - 1))
        flat_index = cell.x + cell.y * dims.x + cell.z * dims.x * dims.y
        for i in [cell_offsets[flat_index], cell_offsets[flat_index + 1]):
          probe_idx = probe_indices[i]
          // ... 法线半球过滤 + 评分 + top-2 维护
  ```

#### 4d. unload 清理

- `unload_angle()`：销毁 grid buffer

**验证**：grid buffer 创建成功，shader 通过 grid 查询能找到正确的 probe

---

### Step 5：Per-pixel Probe 选取 + Blend

替换当前的 per-instance probe 采样，实现 per-pixel 选取和 top-2 blend。

#### 5a. forward.frag specular indirect 重写

- 移除 `inst.probe_index` 读取和 `has_probe` 判断
- 新的 probe 分支入口条件：`(feature_flags & FEATURE_PROBE) != 0u && global.probe_count > 0u`（`FEATURE_PROBE` flag 保留作为全局开关）
- 通过 Step 4 的 grid 查询遍历附近 probe
- 法线半球过滤：`dot(normalize(probe_pos - frag_world_pos), N) > 0.0`
- 评分：`score = pow(normal_dot, normal_bias) / max(dist_sq, epsilon)`
- 维护 top-2（score 最高的两个 probe index + score）
- 无候选通过半球过滤 → fallback 到距离最近的 probe（忽略法线）
- 只有 1 个候选通过 → `w0 = 1.0, w1 = 0.0`（不做 blend，避免 score 归一化除零）
- `probe_count == 0`（或 FEATURE_PROBE off）→ 走 IBL 分支（与当前行为一致）

#### 5b. Roughness 平滑过渡

- `t = clamp((roughness - roughness_single) / (roughness_full - roughness_single), 0.0, 1.0)`
- `blend_factor = pow(t, blend_curve)`
- `w1 *= blend_factor`，`w0 = 1.0 - w1`
- `roughness < roughness_single` → 只用 top-1
- `roughness > roughness_full` → 完整 blend

#### 5c. Cubemap 采样

- Top-1 和 top-2 各采样一次 cubemap（`textureLod` with roughness-based mip）
- 加权混合两个结果
- 乘以 `F0 * brdf_lut.x + brdf_lut.y`（Split-Sum 重建）

**验证**：
- 大物体不同位置使用不同 probe，反射有空间变化
- Probe 边界无硬切突变（blend 平滑过渡）
- 光滑表面（低 roughness）用单 probe 无重影
- 粗糙表面 blend 自然

---

### Step 6：Runtime 参数 + ImGui 面板

#### 6a. 参数结构

Runtime 参数（不影响 bake，ImGui 实时可调）：
- `float normal_bias`（默认 1.0，范围 0.0-4.0）— 法线-距离权重平衡
- `float roughness_single`（默认 0.15，范围 0.0-1.0）— 低于此值只用 top-1 probe
- `float roughness_full`（默认 0.5，范围 0.0-1.0）— 高于此值完整 blend
- `float blend_curve`（默认 1.0，范围 0.1-4.0）— 过渡曲线形状

Bake-time 参数（bake 面板中，下次 bake 生效）：
- `float probe_relocation_offset`（默认 0.1，范围 0.01-0.5，单位 meters）

#### 6b. GPU 传递

- Runtime 参数通过 GlobalUBO 新增字段传入 shader
- `framework/scene_data.h`：`GlobalUniformData` 新增 4 个 float + `probe_count` uint

#### 6c. ImGui 面板

- Rendering 区（LP 模式下可见）：`normal_bias`、`roughness_single`、`roughness_full`、`blend_curve` 四个 slider
- Bake 面板：`probe_relocation_offset` slider

**验证**：slider 拖动实时影响 probe 选取和 blend 效果

---

### Step 7：AABB 计算

所有 relocation 完成后，为每个 surviving probe 计算 PCC 用的 AABB。

#### 7a. AABB 估算 shader

- 新增 `shaders/bake/probe_aabb.comp`：对每个 probe 发射 Fibonacci 球面射线（复用 `probe_ray_count`，原名 `filter_ray_count`）+ 6 条轴对齐射线
- 重命名：`BakeConfig::filter_ray_count` → `probe_ray_count`，同步更新代码、文档、ImGui label（该参数同时服务于 placement filter 和 AABB 估算）
- 每条射线记录命中点世界坐标
- Push constants：probe positions buffer address、probe count、ray count（Fibonacci）、ray max distance

#### 7b. CPU 侧 AABB 构建

- GPU 输出每个 probe 的所有射线命中点（per-probe × (fibonacci_count + 6) 条射线 → hit position vec3）
- CPU readback 后对每个 probe：
  - 每条命中射线按主轴方向（direction 分量绝对值最大的轴）分入 6 组（±X/±Y/±Z）
  - 每组取命中点在该轴上投影坐标的中位数
  - 6 个中位数构成 AABB 的 ±X/±Y/±Z 边界
- Miss 的射线排除不参与分组。某方向分组全部 miss 时（如朝天空的方向无几何体），该方向 AABB 边界用 `ray_max_distance`（场景 AABB 对角线），使 PCC 在该方向几乎无校正效果（正确行为：开放方向上环境确实在远处）
- 70 条射线 × 2000 probe ≈ 1.6MB readback，CPU 中位数计算瞬时完成

#### 7c. 集成调用时机

- 在 `renderer_bake.cpp` 中 probe bake 全部完成（含 post-bake relocation）、写入 manifest 之前执行
- 需要 TLAS 和 RT pipeline 仍可用（bake 模式结束前）

**验证**：每个 probe 的 AABB 非零且合理（不远大于 grid spacing 的数倍），反映周围几何边界

---

### Step 8：Manifest 格式扩展 + AABB 写入

#### 8a. Manifest 格式 v2

- 当前格式：`uint32_t probe_count` + `vec3[probe_count] positions`
- 新格式：`uint32_t version`(= 2) + `uint32_t probe_count` + per-probe stride: `vec3 position` + `vec3 aabb_min` + `vec3 aabb_max`（36 bytes per probe）
- `renderer_bake.cpp`：`write_probe_manifest()` 写入 v2 格式

#### 8b. BakeDataManager 读取 AABB

- `bake_data_manager.cpp`：`load_angle()` 读取 manifest v2，将 `aabb_min`/`aabb_max` 填入 `GPUProbeData`（替代当前的填零）
- `bake_data_manager.cpp`：`scan()` 也需同步更新 v2 解析逻辑（当前直接从 offset 0 读 `uint32_t` 作为 `probe_count`，v2 的 offset 0 是 `version`）

**验证**：bake 后 manifest 文件含 AABB 数据，load_angle 后 GPUProbeData 的 aabb_min/max 非零

---

### Step 9：PCC 视差校正

在 probe cubemap 采样前校正反射向量。

#### 9a. PCC 函数

- `shaders/common/probe_grid.glsl` 内新增：
  ```glsl
  vec3 parallax_correct(vec3 R, vec3 frag_pos, vec3 probe_pos, vec3 aabb_min, vec3 aabb_max) {
      vec3 first_plane = (aabb_max - frag_pos) / R;
      vec3 second_plane = (aabb_min - frag_pos) / R;
      vec3 furthest_plane = max(first_plane, second_plane);
      float t = min(min(furthest_plane.x, furthest_plane.y), furthest_plane.z);
      vec3 intersection = frag_pos + R * t;
      return normalize(intersection - probe_pos);
  }
  ```
- Top-1 和 top-2 分别校正后采样各自的 cubemap

#### 9b. 集成

- Step 5 的 cubemap 采样前插入 `parallax_correct()` 调用
- 仅当 `aabb_min != aabb_max`（AABB 有效）时执行校正，否则用原始 R
- `parallax_correct()` 中 `R` 分量为 0 时 GLSL 产生 `inf`，后续 `min()` 自然选有限值，无需特殊处理。`t < 0`（fragment 在 AABB 外部）时校正结果无意义，但 Phase 8.5 的 per-pixel 选取保证 fragment 附近有 probe（probe 在 AABB 内），实际不会触发此情况

**验证**：
- 光滑地板反射墙壁位置正确（贴合房间几何）
- 对比 PCC 开/关效果明显
- 无 AABB 数据时 graceful fallback

---

### Step 10：收尾

#### 10a. Debug 渲染模式

- 新增 `DEBUG_MODE_PROBE_INDEX = 12`（passthrough 类型，>= 4）：可视化每个 fragment 选中的 top-1 probe index（颜色编码）
- `shaders/common/bindings.glsl` 新增 `#define`，`app/debug_ui.cpp` 新增选项

#### 10b. Phase 8.5 预留注释清理

- `shaders/common/bindings.glsl:154`：移除 "reserved for Phase 8.5"
- `framework/include/himalaya/framework/scene_data.h:541,547,549`：移除 "Phase 8.5, filled with zeros in Phase 8" 注释，改为描述实际用途

#### 10c. 全面验证

- 所有模式切换路径（IBL ↔ LP、角度切换、bake、场景/HDR 切换）无 validation 报错
- Probe relocation 统计日志完整
- Grid buffer 生命周期正确（load 创建、unload 销毁、probe_count == 0 时不创建）
