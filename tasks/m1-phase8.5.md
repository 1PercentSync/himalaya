# M1 阶段八点五：间接光照质量提升 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：Probe Relocation — Pre-bake

- [ ] `shaders/bake/probe_filter.comp`：ResultBuffer 从 `uint[]` 改为结构化 buffer（32B/候选：result + nearest_hit_pos + nearest_hit_normal + padding）
- [ ] `shaders/bake/probe_filter.comp`：失败候选记录最近命中点位置和几何法线
- [ ] `framework/scene_data.h`：`BakeConfig` 新增 `probe_relocation_offset`（默认 0.1f）
- [ ] `framework/src/probe_placement.cpp`：Phase 3 扩展——失败候选计算 relocated position（hit_pos + normal × offset）
- [ ] `framework/src/probe_placement.cpp`：收集 relocated 候选，第二次 dispatch 同一 filter shader
- [ ] `framework/src/probe_placement.cpp`：合并两批 survivors + 日志统计（relocated N, passed M）

## Step 2：Probe Relocation — Post-bake

- [ ] `app/src/renderer_bake.cpp`：per-face luminance 检查改为记录黑面列表（不立即 reject）
- [ ] `app/src/renderer_bake.cpp`：计算移动方向（黑面反方向归一化合成，全黑直接剔除）
- [ ] `app/src/renderer_bake.cpp`：移动到新位置，重新执行完整 probe bake（6 faces × target SPP）
- [ ] `app/src/renderer_bake.cpp`：re-bake 后再次黑面检测，仍失败则最终剔除
- [ ] `app/src/renderer_bake.cpp`：更新 `bake_probe_positions_` 中 relocated probe 的位置
- [ ] `framework/include/himalaya/framework/render_progress.h`：新增 `probes_relocated` 计数
- [ ] `app/src/debug_ui.cpp`：显示 relocated vs rejected 统计

## Step 3：AABB 计算

- [ ] 新增 `shaders/bake/probe_aabb.comp`：Fibonacci 球面射线 + 6 轴射线，输出 per-probe per-ray 命中点
- [ ] `framework/src/probe_placement.cpp`（或 `renderer_bake.cpp`）：dispatch probe_aabb.comp + readback
- [ ] CPU 侧：per-probe 按主轴分组射线命中点，取中位数构建 AABB
- [ ] `framework/include/himalaya/framework/probe_placement.h`：`ProbeGrid` 新增 `std::vector<AABB> aabbs`
- [ ] 调用时机集成：probe bake 全部完成后、manifest 写入前执行 AABB 计算

## Step 4：Manifest 格式扩展 + AABB 写入

- [ ] `app/src/renderer_bake.cpp`：`write_probe_manifest()` 写入 v2 格式（version + probe_count + per-probe position/aabb_min/aabb_max）
- [ ] `framework/src/bake_data_manager.cpp`：`load_angle()` 读取 manifest v2，填入 GPUProbeData aabb_min/aabb_max
- [ ] `framework/src/bake_data_manager.cpp`：`scan()` 版本检测——v1 manifest 不通过完整性校验

## Step 5：GPUInstanceData 清理 + probe_count 引入

- [ ] `framework/scene_data.h`：`GPUInstanceData::probe_index` → `_padding2`
- [ ] `shaders/common/bindings.glsl`：`GPUInstanceData` 对应更新
- [ ] `shaders/forward.frag`：移除 `inst.probe_index` 读取
- [ ] `framework/src/bake_data_manager.cpp`：移除 CPU probe-to-instance 分配逻辑
- [ ] `framework/include/himalaya/framework/bake_data_manager.h`：移除 `probe_indices()` 和 `probe_indices_` 成员
- [ ] `app/src/renderer_rasterization.cpp`：移除 `probe_indices` 参数和填充逻辑
- [ ] `framework/scene_data.h`：`GlobalUniformData` 新增 `uint32_t probe_count` + `static_assert` 更新
- [ ] `shaders/common/bindings.glsl`：`GlobalUBO` 新增 `probe_count`
- [ ] `app/src/renderer.cpp`：`fill_common_gpu_data()` 填入 probe count

## Step 6：3D Grid 空间索引

- [ ] `framework/include/himalaya/framework/bake_data_manager.h`：新增 `ProbeGrid3D` 结构（grid_origin, cell_size, grid_dims, cell_offsets, probe_indices）
- [ ] `framework/src/bake_data_manager.cpp`：`load_angle()` 加载 probe 后构建 grid（遍历 positions → cell 坐标 → 前缀和 → flat array）
- [ ] `framework/src/bake_data_manager.cpp`：上传 grid_meta_buffer + grid_data_buffer 到 GPU
- [ ] `shaders/common/bindings.glsl` 或 `rhi/descriptors.cpp`：grid buffer binding 配置
- [ ] 新增 `shaders/common/probe_grid.glsl`：grid 查询函数（world pos → center cell → 5×5×5 邻域遍历）
- [ ] `framework/src/bake_data_manager.cpp`：`unload_angle()` 销毁 grid buffer

## Step 7：Per-pixel Probe 选取 + Blend

- [ ] `shaders/forward.frag`：移除 `has_probe` / `inst.probe_index` 逻辑
- [ ] `shaders/forward.frag`：引入 probe_grid.glsl，5×5×5 邻域遍历 + 法线半球过滤 + 评分 + top-2 维护
- [ ] `shaders/forward.frag`：无候选 fallback 到最近 probe
- [ ] `shaders/forward.frag`：roughness 平滑过渡（roughness_single/full/blend_curve）
- [ ] `shaders/forward.frag`：top-2 cubemap 采样 + 加权混合

## Step 8：PCC 视差校正

- [ ] `shaders/common/probe_grid.glsl`：新增 `parallax_correct()` 函数（ray-AABB intersection 校正反射向量）
- [ ] `shaders/forward.frag`：cubemap 采样前调用 parallax_correct()（aabb 有效时）
- [ ] `shaders/forward.frag`：AABB 无效时 fallback 用原始 R

## Step 9：Runtime 参数 + ImGui 面板

- [ ] `framework/scene_data.h`：`GlobalUniformData` 新增 `normal_bias` / `roughness_single` / `roughness_full` / `blend_curve`
- [ ] `shaders/common/bindings.glsl`：`GlobalUBO` 对应更新
- [ ] `app/src/renderer.cpp`：填充新 GlobalUBO 字段
- [ ] `app/src/debug_ui.cpp`：Rendering 区新增 4 个 slider（LP 模式下可见）
- [ ] `app/src/debug_ui.cpp`：Bake 面板新增 `probe_relocation_offset` slider
- [ ] `app/src/application.cpp`：`AppConfig` 新增字段 + JSON 持久化

## Step 10：收尾

- [ ] 新增 `DEBUG_MODE_PROBE_INDEX` debug 渲染模式（probe index 颜色编码可视化）
- [ ] 全模式切换路径 validation 验证
- [ ] Grid buffer 生命周期验证（load 创建、unload 销毁、场景切换清理）
