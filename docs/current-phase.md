# 当前阶段：M1 阶段八点五 — 间接光照质量提升

> 目标：提升 Reflection Probe 系统的放置质量、选取精度和反射准确性。
> 三个核心改进：Per-pixel probe 选取 + blend、Probe 放置从"剔除"演进为"移动+重试"、PCC 视差校正。
>
> Phase 8 基础设施（BakeDataManager、ProbeBuffer SSBO、forward shader 间接光照分支）在此阶段重构和扩展。
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: GPUInstanceData 清理 + probe_count 引入（基础设施重构）
    ↓
Step 2: Manifest probe_spacing + BakeDataManager 读写更新
    ↓
Step 3: 3D Grid 空间索引（加载时构建，依赖 Step 2 的 probe_spacing）
    ↓
Step 4: Per-pixel probe 选取 + blend（shader 改造，依赖 Step 1/3）
    ↓
Step 5: Runtime 参数 + ImGui 面板
    ↓
Step 6: Probe relocation — pre-bake（修改 filter shader + placement 逻辑）
    ↓
Step 7: Probe relocation — post-bake（修改 bake 循环）
    ↓
Step 8: AABB 计算 + Manifest AABB 扩展（依赖 relocation 完成后的最终位置）
    ↓
Step 9: PCC 视差校正（shader 扩展，依赖 Step 4 的 probe 选取 + Step 8 的 AABB 数据）
    ↓
Step 10: 收尾
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | GPUInstanceData 清理 + probe_count | `probe_index` 字段移除，GlobalUBO 含 `probe_count`，BakeDataManager 无 CPU probe 分配逻辑 |
| 2 | Manifest probe_spacing | manifest 写入/读取含 `probe_spacing`，`scan()` 和 `load_angle()` 正确解析新格式 |
| 3 | 3D Grid 空间索引 | Grid buffer 上传 GPU，fragment shader 可通过世界坐标查询 cell 内 probe 列表 |
| 4 | Per-pixel probe 选取 + blend | 每个 fragment 从 5×5×5 邻域选取 top-2 probe，法线半球过滤 + roughness 平滑过渡 |
| 5 | Runtime 参数 + ImGui | `normal_bias`、`roughness_single/full`、`blend_curve` 可调，实时影响画面 |
| 6 | Probe relocation — pre-bake | 原本被 Rule 1/2 剔除的 probe 有机会移动后通过，survivor 数量上升 |
| 7 | Probe relocation — post-bake | 部分面全黑的 probe 被移动后重新 bake，post-bake reject 率显著下降 |
| 8 | AABB 计算 + Manifest AABB 扩展 | 每个 probe 有合理的 AABB，manifest 含 AABB 数据，load 后 GPUProbeData 非零 |
| 9 | PCC 视差校正 | 反射贴合房间墙壁，对比 PCC 开/关效果明显 |
| 10 | 收尾 | 全流程无 validation 报错，debug 模式可用 |

---

### Step 1：GPUInstanceData 清理 + probe_count 引入

#### 1a. GPUInstanceData::probe_index 移除

- `framework/scene_data.h`：`GPUInstanceData` 的 `probe_index` 改为 `_padding2`，struct 保持 128 bytes
- `shaders/common/bindings.glsl`：`GPUInstanceData` 对应更新
- `shaders/forward.frag`：移除 `inst.probe_index` 读取。specular indirect 分支临时改为 `probes[0]` + `global.probe_count > 0u` 守卫防越界

#### 1b. BakeDataManager probe 分配清理

- `bake_data_manager.cpp`：`load_angle()` 中移除 CPU probe-to-instance 分配逻辑（`probe_indices_` 赋值循环）
- `bake_data_manager.h`：移除 `probe_indices()` 访问器和 `probe_indices_` 成员
- `bake_data_manager.h`：新增 `uint32_t loaded_probe_count() const`（返回当前已加载的 probe 数量，未加载时返回 0）
- `bake_data_manager.h`：`load_angle()` 签名移除 `mesh_instances` 参数（不再需要做 probe-to-instance 分配），调用方同步更新
- `renderer_rasterization.cpp`：`build_draw_groups()` 移除 `probe_indices` 参数和相关填充逻辑

#### 1c. GlobalUBO 扩展

一次性新增所有 Phase 8.5 字段（避免 Step 4 shader 引用尚不存在的字段）：

- `framework/scene_data.h`：`GlobalUniformData` 新增 5 个字段（offset 928 起）：
  - `uint32_t probe_count`（默认 0）
  - `float normal_bias`（默认 1.0）
  - `float roughness_single`（默认 0.15）
  - `float roughness_full`（默认 0.5）
  - `float blend_curve`（默认 1.0）
  - + 3 个 `uint32_t _phase85_pad`（12B padding 到 960B，16 的倍数）
  - 更新 `static_assert(sizeof == 960)` 和各字段 `offsetof` 校验
- `shaders/common/bindings.glsl`：`GlobalUBO` 对应更新（5 个新字段）
- `app/renderer.cpp`：`fill_common_gpu_data()` 填入 `probe_count`（从 `bake_data_manager_.loaded_probe_count()`）和 4 个 blend 参数（Step 5 引入 ImGui slider 前使用默认值）

**验证**：编译通过，渲染无回归，`probe_index` 无残留引用

---

### Step 2：Manifest probe_spacing

Manifest 格式引入 `probe_spacing` 字段，为 Step 3 的 3D Grid 构建提供 cell size。

#### 2a. Manifest 格式变更

- 当前格式：`uint32_t probe_count` + `vec3[probe_count] positions`
- 新格式：`uint32_t probe_count` + `float probe_spacing` + `vec3[probe_count] positions`（header 8B + 12B per probe）
- 旧 bake 数据由用户手动清除后重新 bake

#### 2b. 写入侧

- `renderer_bake.cpp`：`write_probe_manifest()` 在 `probe_count` 后写入 `probe_spacing`（值从 `bake_locked_config_.probe_spacing` 获取）

#### 2c. 读取侧

- `bake_data_manager.cpp`：`load_angle()` 从 manifest 读取 `probe_spacing`，存储为成员变量供 3D Grid 构建使用
- `bake_data_manager.cpp`：`scan()` 同步更新解析逻辑（offset 4 = `probe_spacing`，positions 从 offset 8 开始）

**验证**：bake 后 manifest 含 probe_spacing，load_angle 正确读取

---

### Step 3：3D Grid 空间索引

加载 bake 数据时构建世界空间 3D grid，供 fragment shader 快速查找附近 probe。

#### 3a. Grid 数据结构（CPU 侧）

- `bake_data_manager.h`：新增 `ProbeGrid3D` 内部结构
  - `glm::vec3 grid_origin`（所有 probe 位置的 AABB 最小角再减去 2 × cell_size，使边缘 probe 的 5×5×5 查询不越界）
  - `float cell_size`（从 manifest 读取的 `probe_spacing`）
  - `glm::uvec3 grid_dims`（各轴 cell 数量，= 原始 probe AABB 范围 / cell_size 向上取整 + 4（两端各扩展 2 个 cell））
  - `std::vector<uint32_t> cell_offsets`（`grid_dims.x * y * z + 1` 个元素，CSR 风格前缀和）
  - `std::vector<uint32_t> probe_indices`（flat 数组，cell_offsets 索引进来）
- `load_angle()` 加载 probe 后构建：遍历 probe positions → 计算 cell 坐标 → 统计 per-cell count → 前缀和 → 填充 indices
- `probe_count == 0` 时跳过 grid 构建，不创建 GPU buffer

#### 3b. GPU Buffer 上传

- 单个 SSBO，Set 0 binding 10（`ProbeGridBuffer`，非 RT-only，`PARTIALLY_BOUND`，`FRAGMENT` stage only——grid 查询仅在 fragment shader 中进行），布局：
  - Header (32B)：`vec4 grid_origin_and_cell_size`(16B) + `uvec4 grid_dims_and_pad`(16B)
  - `uint cell_offsets[cell_count + 1]`（CSR 前缀和，`cell_count = dims.x * dims.y * dims.z`）
  - `uint probe_indices[]`（flat probe index 数组）
- `rhi/descriptors.cpp`：`create_layouts()` 中 Set 0 新增 binding 10（RT 和非 RT 两条路径均需更新）
- `rhi/descriptors.h`：新增 `write_set0_probe_grid_buffer()` 方法

#### 3c. Shader 端查询

- `shaders/common/probe_grid.glsl`（新文件）：仅提供 CSR 数据访问辅助函数（`grid_cell_offset`、`grid_probe_index`、`grid_flat_index`），不包含收集函数
- 5×5×5 遍历 + 法线过滤 + 评分 + top-2 维护在 `forward.frag` 中内联完成（Step 4），原因：
  - Relocation（Step 6/7）后 probe 不再均匀分布，单 cell 可能有多个 probe，5×5×5 邻域总数可能超过 125
  - 只需 top-2 结果，无需收集全部候选到数组，边遍历边维护即可
  - 避免固定大小数组的溢出风险和寄存器压力
- 遍历伪代码（实际在 Step 4 的 forward.frag 中）：
  ```
  ivec3 center_cell = clamp(ivec3(floor((frag_world_pos - grid_origin) / cell_size)), ivec3(0), ivec3(grid_dims - 1))
  for dz = -2 to +2:
    for dy = -2 to +2:
      for dx = -2 to +2:
        cell = clamp(center_cell + ivec3(dx,dy,dz), ivec3(0), ivec3(grid_dims - 1))
        flat_index = grid_flat_index(cell, dims)
        for i in [grid_cell_offset(flat_index), grid_cell_offset(flat_index + 1)):
          pi = grid_probe_index(probe_base, i)
          // 法线半球过滤 + 评分 + top-2 维护（仅 2 个 index + 2 个 score）
  ```

#### 3d. unload 清理

- `unload_angle()`：销毁 grid buffer

**验证**：grid buffer 创建成功，shader 通过 grid 查询能找到正确的 probe

---

### Step 4：Per-pixel Probe 选取 + Blend

替换当前的临时 probe 采样，实现 per-pixel 选取和 top-2 blend。

> **当前状态**（Step 1a 遗留）：`forward.frag` specular indirect 分支使用 `probes[0]` 作为临时占位（所有 fragment 共用第一个 probe），入口条件为 `has_probe && global.probe_count > 0u`。Step 4 需要完全重写此分支。

#### 4a. forward.frag specular indirect 重写

- 移除临时的 `probes[0]` 占位逻辑
- 新的 probe 分支入口条件：`(feature_flags & FEATURE_PROBE) != 0u && global.probe_count > 0u`（`FEATURE_PROBE` flag 保留作为全局开关）
- 内联 Step 3 的 grid 5×5×5 遍历（复用 `probe_grid.glsl` 辅助函数，不使用收集函数）
- 遍��中逐 probe 执行：法线半球过滤 `dot(normalize(probe_pos - frag_world_pos), N) > 0.0` → 评分 `score = pow(normal_dot, normal_bias) / max(dist_sq, epsilon)` → 就地维护 top-2（2 个 index + 2 个 score，无数组）
- 同时维护距离最近的 probe index（忽略法线，用于 fallback）
- 无候选通过半球过滤 → fallback 到距离最近的 probe（忽略法线）
- 5×5×5 邻域内一个 probe 都没有（极端情况）→ 走 IBL 分支
- 只有 1 个候选通过 → `w0 = 1.0, w1 = 0.0`（不做 blend，避免 score 归一化除零）
- `probe_count == 0`（或 FEATURE_PROBE off）→ 走 IBL 分支（与当前行为一致）

#### 4b. Roughness 平滑过渡

- `t = clamp((roughness - roughness_single) / (roughness_full - roughness_single), 0.0, 1.0)`
- `blend_factor = pow(t, blend_curve)`
- `w1 *= blend_factor`，`w0 = 1.0 - w1`
- `roughness < roughness_single` → 只用 top-1
- `roughness > roughness_full` → 完整 blend

#### 4c. Cubemap 采样

- Top-1 和 top-2 各采样一次 cubemap（`textureLod` with roughness-based mip）
- 加权混合两个结果
- 乘以 `F0 * brdf_lut.x + brdf_lut.y`（Split-Sum 重建）

**验证**：
- 大物体不同位置使用不同 probe，反射有空间变化
- Probe 边界无硬切突变（blend 平滑过渡）
- 光滑表面（低 roughness）用单 probe 无重影
- 粗糙表面 blend 自然

---

### Step 5：Runtime 参数 + ImGui 面板

#### 5a. 参数结构

Runtime 参数（不影响 bake，ImGui 实时可调）：
- `float normal_bias`（默认 1.0，范围 0.0-4.0）— 法线-距离权重平衡
- `float roughness_single`（默认 0.15，范围 0.0-1.0）— 低于此值只用 top-1 probe
- `float roughness_full`（默认 0.5，范围 0.0-1.0）— 高于此值完整 blend
- `float blend_curve`（默认 1.0，范围 0.1-4.0）— 过渡曲线形状

Bake-time 参数（bake 面板中，下次 bake 生效）：
- `float probe_relocation_offset`（默认 0.1，范围 0.01-0.5，单位 meters）

#### 5b. GPU 传递

- GlobalUBO 字段已在 Step 1c 引入（`probe_count` + 4 个 blend float），此处只需确保 `fill_common_gpu_data()` 从 Application 层参数读取最新值（Step 1 中使用默认值，Step 5 后由 ImGui slider 控制）

#### 5c. ImGui 面板

- Rendering 区（LP 模式下可见）：`normal_bias`、`roughness_single`、`roughness_full`、`blend_curve` 四个 slider
- Bake 面板：`probe_relocation_offset` slider

**注意**：所有新参数（runtime 和 bake-time）均不做 JSON 持久化，每次启动使用默认值。与现有 AO/shadow 参数模式一致。`probe_relocation_offset` 影响 bake 结果，持久化后用户可能忘记曾修改过。

**验证**：slider 拖动实时影响 probe 选取和 blend 效果

---

### Step 6：Probe Relocation — Pre-bake

两层测试失败的候选 probe 不再直接剔除，而是移动到更好的位置重试**严格一次**（不循环）。

#### 6a. probe_filter.comp 扩展输出

- 当前输出 `uint result`（0/1/2）改为扩展结构：`uint result` + `vec3 nearest_hit_pos` + `vec3 nearest_hit_normal`
- 对于 result = 0（通过）的候选，hit 数据不使用
- 对于 result = 1 或 2（失败）的候选，记录所有射线中 `t_hit` 最小的命中：`hit_pos = origin + dir * t_hit`，几何法线通过 GeometryInfo buffer reference 读取三角形顶点重建面法线（数据路径与 closesthit 相同）
- 在射线循环中维护 running minimum `t_hit` 和对应的 `hit_pos`/`hit_normal`。Rule 1 触发时不再立即 return——先更新最近命中记录，再写 result 并 return。这样即使 Rule 1 在第一条射线就触发，也有该射线的命中数据可用于 relocation
- ResultBuffer 从 `uint[]` 改为结构化 buffer（每候选 32 bytes，使用 `GL_EXT_scalar_block_layout`（shader 已启用）避免 vec3 的 16B 对齐：`uint result`(4B) + `float hit_pos[3]`(12B) + `float hit_normal[3]`(12B) + `uint _pad`(4B) = 32B）
- `probe_placement.cpp`：result buffer 大小从 `total * 4` 改为 `total * 32`，readback 解析改为结构化读取

#### 6b. probe_placement.cpp relocation 逻辑

- Phase 3（收集 survivors）扩展：对 result = 1 或 2 的候选，计算 relocated position = `nearest_hit_pos + nearest_hit_normal * probe_relocation_offset`
- `probe_relocation_offset` 为 bake-time 可调参数（默认 0.1m）
- 收集所有 relocated 候选，组成第二批 candidates
- 对第二批执行同一 filter shader（复用同一 pipeline + dispatch）
- 第二批通过的加入 survivors，仍失败的最终剔除
- 日志输出：`relocated N candidates, M passed retry`

#### 6c. BakeConfig 扩展

- `framework/scene_data.h`：`BakeConfig` 新增 `float probe_relocation_offset`（默认 0.1f，单位 meters）

**验证**：survivor 数量比不启用 relocation 时增加，relocated probe 位于几何表面外侧合法位置

---

### Step 7：Probe Relocation — Post-bake

Bake 后检测到部分面全黑的 probe，移动后重新 bake **严格一次**（不循环）。在现有 probe bake 循环内部（`probe_bake_finalize()` 流程中）处理，不需要新增 `BakeState` 枚举值。

#### 7a. 黑面方向分析

- `renderer_bake.cpp`：现有的 per-face luminance 检查扩展——不再立即 reject，而是记录哪些 face 低于阈值
- Cubemap face 方向映射：face 0-5 对应 +X/-X/+Y/-Y/+Z/-Z
- 计算移动方向：所有黑面反方向的归一化合成向量
- 全 6 面都黑 → 直接剔除（无处可移）

#### 7b. 移动 + 重新 bake

- 新位置 = 当前位置 + 移动方向 × `probe_relocation_offset`（复用 pre-bake 的参数）
- 在新位置重新执行完整的 probe bake（6 faces × target SPP）：在 `probe_bake_finalize()` 中检测到需要 relocate 时，更新 `bake_probe_positions_` 中该 probe 的位置，设置 per-probe `relocated` 标记（防止二次 relocate），销毁当前累积 images，重新调用 `begin_probe_bake_instance(bake_current_probe_)` 重置累积状态（不递增 `bake_current_probe_`），当前 probe 重新进入累积循环
- 重新 bake 后再次执行黑面检测
- 仍有黑面 → 最终剔除，已通过 → 接受新位置
- 已 relocate 但最终被 reject 的 probe 的 KTX2 文件残留在磁盘上，不影响正确性（manifest 的 probe_count 决定加载哪些），Clear Bake Cache 时统一清除

#### 7c. 进度统计

- `BakeProgress` 扩展：`probes_relocated` 计数（区别于 `probes_rejected`）
- DebugUI 显示 relocated vs rejected 统计
- 进度影响：relocated probe 的 re-bake 使 `probe_sample_count` 从 0 重新累积，当前 probe 进度条回退。`bake_probe_total_` 不变（仍是候选数），总进度百分比通过 `(completed + current_fraction) / total` 计算，relocated probe 等同于在同一 slot 烘焙两次——总进度会短暂倒退但最终追上。ETA 受影响但可接受

#### 7d. Phase 8.5 后的完整 probe bake 流程

```
BakingProbes 阶段内部：
  for each probe candidate:
    accumulate SPP → finalize:
      per-face luminance check:
        all 6 faces OK → accept（write KTX2 + prefilter）
        all 6 faces black → reject
        partial black + not yet relocated → relocate:
          update position → reset accumulation → re-enter accumulate loop
        partial black + already relocated → reject
  ↓
  AABB compute dispatch（Step 8，所有 accepted probes 的最终位置）
  ↓
  write manifest（Step 8，positions + AABBs + probe_spacing）
  ↓
  BakeState → Complete
```

**验证**：post-bake reject 率相比 Phase 8 显著下降，relocated probe 的 cubemap 无全黑面

---

### Step 8：AABB 计算 + Manifest AABB 扩展

所有 relocation 完成后，为每个 surviving probe 计算 PCC 用的 AABB，并扩展 manifest 格式存储。

#### 8a. AABB 估算 shader

- 新增 `shaders/bake/probe_aabb.comp`：使用 `rayQueryEXT`（与 `probe_filter.comp` 相同），需要 TLAS（Set 0 binding 4）。Pipeline 创建复用 `probe_filter.comp` 的 descriptor layout 模式
- 对每个 probe 发射 Fibonacci 球面射线（复用 `probe_ray_count`，原名 `filter_ray_count`）+ 6 条轴对齐射线
- 重命名：`BakeConfig::filter_ray_count` → `probe_ray_count`，同步更新代码、文档、ImGui label（该参数同时服务于 placement filter 和 AABB 估算）
- 每条射线记录命中点世界坐标
- Push constants：probe positions buffer address、result buffer address、probe count、ray count（Fibonacci）、ray max distance
- 输出 buffer 布局（per-probe × `(ray_count + 6)` 条射线，scalar block layout）：`vec3 hit_pos`(12B) + `uint hit`(4B，1 = hit / 0 = miss) = 16B per ray。miss 的射线 `hit = 0`，CPU 侧排除不参与分组

#### 8b. CPU 侧 AABB 构建

- GPU 输出每个 probe 的所有射线命中点（per-probe × (fibonacci_count + 6) 条射线 → hit position vec3）
- CPU readback 后对每个 probe：
  - 每条命中射线按主轴方向（direction 分量绝对值最大的轴）分入 6 组（±X/±Y/±Z）
  - 每组取命中点在该轴上投影坐标的中位数
  - 6 个中位数构成 AABB 的 ±X/±Y/±Z 边界
- Miss 的射线排除不参与分组。某方向分组全部 miss 时（如朝天空的方向无几何体），该方向 AABB 边界用 `ray_max_distance`（场景 AABB 对角线），使 PCC 在该方向几乎无校正效果（正确行为：开放方向上环境确实在远处）
- 70 条射线 × 2000 probe ≈ 1.6MB readback，CPU 中位数计算瞬时完成

#### 8c. Manifest 格式扩展

- Step 2 的 manifest 格式（header 8B + 12B per probe）扩展 per-probe 为 36B：

```
uint32_t probe_count        // offset 0
float    probe_spacing       // offset 4
per-probe (36B):
  vec3 position             // 12B
  vec3 aabb_min             // 12B（新增）
  vec3 aabb_max             // 12B（新增）
```

- `renderer_bake.cpp`：`write_probe_manifest()` 签名扩展以接收 AABB 数据，写入新格式
- `bake_data_manager.cpp`：`load_angle()` 读取 per-probe AABB，填入 `GPUProbeData::aabb_min`/`aabb_max`
- `bake_data_manager.cpp`：`scan()` 同步更新解析逻辑（per-probe stride 从 12B 变为 36B）
- 旧 bake 数据由用户手动清除后重新 bake

#### 8d. 集成调用时机

- 在 `renderer_bake.cpp` 中 probe bake 全部完成（含 post-bake relocation）、写入 manifest 之前插入 AABB compute dispatch
- 需要 TLAS 和 RT pipeline 仍可用（bake 模式结束前）

**验证**：每个 probe 的 AABB 非零且合理，manifest 含 AABB 数据，load 后 GPUProbeData 的 aabb_min/max 非零

---

### Step 9：PCC 视差校正

在 probe cubemap 采样前校正反射向量。

#### 9a. PCC 函数

- `shaders/common/probe_grid.glsl` 内新增：
  ```glsl
  // Returns corrected direction. out_t < 0 means fragment outside AABB — caller should use original R.
  vec3 parallax_correct(vec3 R, vec3 frag_pos, vec3 probe_pos, vec3 aabb_min, vec3 aabb_max, out float out_t) {
      vec3 first_plane = (aabb_max - frag_pos) / R;
      vec3 second_plane = (aabb_min - frag_pos) / R;
      vec3 furthest_plane = max(first_plane, second_plane);
      out_t = min(min(furthest_plane.x, furthest_plane.y), furthest_plane.z);
      vec3 intersection = frag_pos + R * out_t;
      return normalize(intersection - probe_pos);
  }
  ```
- Top-1 和 top-2 分别校正后采样各自的 cubemap

#### 9b. 集成

- Step 4 的 cubemap 采样前插入 `parallax_correct()` 调用
- 仅当 `aabb_min != aabb_max`（AABB 有效）时执行校正，否则用原始 R
- `parallax_correct()` 中 `R` 分量为 0 时 GLSL 产生 `inf`，后续 `min()` 自然选有限值，无需特殊处理。`t < 0` 时 fragment 在 AABB 外部（blend 的 top-2 probe 的 AABB 不一定包含当前 fragment），此时跳过校正用原始 R。调用方检查：`t > 0.0` 时用校正后方向，否则用原始 R

**验证**：
- 光滑地板反射墙壁位置正确（贴合房间几何）
- 对比 PCC 开/关效果明显
- 无 AABB 数据时 graceful fallback

---

### Step 10：收尾

#### 10a. Debug 渲染模式

- 新增 `DEBUG_MODE_PROBE_INDEX = 12`（passthrough 类型，>= 4）：可视化每个 fragment 选中的 top-1 probe index（颜色编码）
- `shaders/common/bindings.glsl` 新增 `#define`，`app/debug_ui.cpp` 新增选项，`forward.frag` passthrough 分支新增 case 12（index-to-color 映射）

#### 10b. Phase 8.5 预留注释清理

- `shaders/common/bindings.glsl:154`：移除 "reserved for Phase 8.5"
- `framework/include/himalaya/framework/scene_data.h:541,547,549`：移除 "Phase 8.5, filled with zeros in Phase 8" 注释，改为描述实际用途

#### 10c. 全面验证

- 所有模式切换路径（IBL ↔ LP、角度切换、bake、场景/HDR 切换）无 validation 报错
- Probe relocation 统计日志完整
- Grid buffer 生命周期正确（load 创建、unload 销毁、probe_count == 0 时不创建）
