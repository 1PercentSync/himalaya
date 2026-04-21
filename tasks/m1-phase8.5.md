# M1 阶段八点五：间接光照质量提升 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：GPUInstanceData 清理 + probe_count 引入

- [x] `framework/scene_data.h`：`GPUInstanceData::probe_index` → `_padding2`
- [x] `shaders/common/bindings.glsl`：`GPUInstanceData` 对应更新
- [x] `shaders/forward.frag`：移除 `inst.probe_index` 读取
- [x] `framework/src/bake_data_manager.cpp`：移除 CPU probe-to-instance 分配逻辑
- [x] `framework/include/himalaya/framework/bake_data_manager.h`：移除 `probe_indices()` 和 `probe_indices_` 成员
- [x] `framework/include/himalaya/framework/bake_data_manager.h`：新增 `loaded_probe_count()` 访问器
- [x] `framework/include/himalaya/framework/bake_data_manager.h`：`load_angle()` 签名移除 `mesh_instances` 参数 + 调用方同步
- [x] `app/src/renderer_rasterization.cpp`：移除 `probe_indices` 参数和填充逻辑
- [x] `framework/scene_data.h`：`GlobalUniformData` 新增 `probe_count` + `normal_bias` + `roughness_single` + `roughness_full` + `blend_curve` + padding 到 960B + `static_assert` 更新
- [x] `shaders/common/bindings.glsl`：`GlobalUBO` 新增 5 个字段
- [x] `app/src/renderer.cpp`：`fill_common_gpu_data()` 填入 probe count + 4 个 blend 参数（默认值）

## Step 2：Manifest probe_spacing

- [x] `app/src/renderer_bake.cpp`：`write_probe_manifest()` 在 `probe_count` 后写入 `probe_spacing`
- [x] `framework/src/bake_data_manager.cpp`：`load_angle()` 读取 manifest `probe_spacing`，存储为成员变量
- [x] `framework/src/bake_data_manager.cpp`：`scan()` 同步更新解析逻辑（positions 从 offset 8 开始）

## Step 3：3D Grid 空间索引

- [x] `framework/include/himalaya/framework/bake_data_manager.h`：新增 `ProbeGrid3D` 结构（grid_origin, cell_size, grid_dims, cell_offsets, probe_indices）
- [x] `framework/src/bake_data_manager.cpp`：`load_angle()` 加载 probe 后构建 grid（遍历 positions → cell 坐标 → 前缀和 → flat array）
- [x] `framework/src/bake_data_manager.cpp`：上传单个 ProbeGridBuffer SSBO（header + cell_offsets + probe_indices）
- [x] `rhi/descriptors.cpp`：Set 0 新增 binding 10（RT 和非 RT 两条路径）+ `write_set0_probe_grid_buffer()`
- [x] `shaders/common/bindings.glsl`：新增 `layout(set = 0, binding = 10) readonly buffer ProbeGridBuffer`
- [x] 新增 `shaders/common/probe_grid.glsl`：CSR 辅助函数（grid_cell_offset、grid_probe_index、grid_flat_index）
- [x] `framework/src/bake_data_manager.cpp`：`unload_angle()` 销毁 grid buffer

## Step 4：Per-pixel Probe 选取 + Blend

- [x] `shaders/forward.frag`：移除 `has_probe` / `inst.probe_index` 逻辑
- [x] `shaders/forward.frag`：引入 probe_grid.glsl，内联 5×5×5 遍历 + 法线半球过滤 + 评分 + 就地 top-2 维护（无数组）
- [x] `shaders/forward.frag`：无候选 fallback 到最近 probe；5×5×5 全空走 IBL
- [x] `shaders/forward.frag`：roughness 平滑过渡（roughness_single/full/blend_curve）
- [x] `shaders/forward.frag`：top-2 cubemap 采样 + 加权混合

## Step 5：Runtime 参数 + ImGui 面板

- [x] `app/src/renderer.cpp`：`fill_common_gpu_data()` 改为从 Application 层参数读取 blend 值（不再使用默认值）
- [x] `app/src/debug_ui.cpp`：Rendering 区新增 4 个 slider（LP 模式下可见）

## Step 5.1：LP 模式正确性修复

- [x] `app/src/renderer_bake.cpp`：probe 累积 cubemap 生成 mip chain + prefilter 前 `generate_mips()`（修复 prefilter 油感）
- [x] `shaders/forward.frag`：primary light CSM shadow 提取到 direct light loop 前
- [x] `shaders/forward.frag`：LP 模式 diffuse 合成改为 `max(direct, indirect)` 替代相加
- [x] `shaders/forward.frag`：LP 模式 specular 合成改为 `probe × SO × max(shadow, floor)`，不叠加 direct_specular
- [x] `shaders/forward.frag`：四个 debug mode 全部跟随 LP 合成公式
- [ ] `framework/include/himalaya/framework/ibl.h`：声明 `sample_hdr_pixel` 静态方法
- [ ] `framework/src/ibl.cpp`：实现 `sample_hdr_pixel`（stbi_loadf 读像素后释放）
- [ ] `app/include/himalaya/app/application.h`：新增 `hdr_sun_auto_`（默认 true，不持久化）
- [ ] `app/src/application.cpp`：Auto 模式下调用 `sample_hdr_pixel` → 分解 color/intensity → 填入 `hdr_sun_light_`
- [ ] `app/src/application.cpp`：切换到 HdrSun 模式时触发采样更新
- [ ] `app/src/debug_ui.cpp`：HdrSun 区新增 `Auto from HDR` checkbox + 灰显逻辑

## Step 6：Probe Relocation — Pre-bake

- [ ] `shaders/bake/probe_filter.comp`：ResultBuffer 从 `uint[]` 改为结构化 buffer（32B/候选，scalar layout）
- [ ] `shaders/bake/probe_filter.comp`：射线循环维护 running minimum t_hit + hit_pos + hit_normal
- [ ] `shaders/bake/probe_filter.comp`：Rule 1 改为先记录命中信息再写 result 并 return
- [ ] `framework/src/probe_placement.cpp`：result buffer 大小 `total * 32`，readback 结构化解析
- [ ] `framework/scene_data.h`：`BakeConfig` 新增 `probe_relocation_offset`（默认 0.1f）
- [ ] `framework/src/probe_placement.cpp`：失败候选计算 relocated position（hit_pos + normal × offset）
- [ ] `framework/src/probe_placement.cpp`：收集 relocated 候选，第二次 dispatch 同一 filter shader
- [ ] `framework/src/probe_placement.cpp`：合并两批 survivors + 日志统计（relocated N, passed M）

## Step 7：Probe Relocation — Post-bake

- [ ] `app/src/renderer_bake.cpp`：per-face luminance 检查改为记录黑面列表（不立即 reject）
- [ ] `app/src/renderer_bake.cpp`：计算移动方向（黑面反方向归一化合成，全黑直接剔除）
- [ ] `app/src/renderer_bake.cpp`：新增 per-probe `relocated` 标记（防止二次 relocate）
- [ ] `app/src/renderer_bake.cpp`：relocate 时更新位置 + 销毁累积 images + 重新 begin_probe_bake_instance（不递增 probe index）
- [ ] `app/src/renderer_bake.cpp`：re-bake 后再次黑面检测，仍失败则最终剔除
- [ ] `framework/include/himalaya/framework/render_progress.h`：`BakeProgress` 新增 `probes_relocated` 计数
- [ ] `app/src/debug_ui.cpp`：显示 relocated vs rejected 统计
- [ ] `app/src/debug_ui.cpp`：Bake 面板新增 `probe_relocation_offset` slider

## Step 8：AABB 计算 + Manifest AABB 扩展

- [ ] 重命名 `BakeConfig::filter_ray_count` → `probe_ray_count`（代码 + 文档 + ImGui label 全部同步）
- [ ] 新增 `shaders/bake/probe_aabb.comp`：rayQueryEXT + Fibonacci 射线 + 6 轴射线，输出 per-probe per-ray 命中点(16B: vec3 + hit flag)
- [ ] `app/src/renderer_bake.cpp`（或 `framework/src/probe_placement.cpp`）：dispatch probe_aabb.comp + readback
- [ ] CPU 侧：per-probe 按主轴分组射线命中点，取中位数构建 AABB（miss 排除，全 miss 用 ray_max_distance）
- [ ] `app/src/renderer_bake.cpp`：probe bake 完成后、manifest 写入前插入 AABB compute dispatch
- [ ] `app/src/renderer_bake.cpp`：`write_probe_manifest()` 签名扩展，per-probe 写入 position + aabb_min + aabb_max（36B）
- [ ] `framework/src/bake_data_manager.cpp`：`load_angle()` 读取 per-probe AABB，填入 GPUProbeData
- [ ] `framework/src/bake_data_manager.cpp`：`scan()` 更新解析逻辑（per-probe stride 12B → 36B）

## Step 9：PCC 视差校正

- [ ] `shaders/common/probe_grid.glsl`：新增 `parallax_correct()` 函数（ray-AABB intersection + out_t 返回）
- [ ] `shaders/forward.frag`：cubemap 采样前调用 parallax_correct()（aabb 有效 && t > 0 时）
- [ ] `shaders/forward.frag`：AABB 无效或 t < 0 时 fallback 用原始 R

## Step 10：收尾

- [ ] 新增 `DEBUG_MODE_PROBE_INDEX = 12` debug 渲染模式：`bindings.glsl` 新增 `#define`，`debug_ui.cpp` 新增选项，`forward.frag` passthrough 分支新增 case 12（index-to-color 映射）
- [ ] 清理 Phase 8.5 预留注释（bindings.glsl、scene_data.h）
- [ ] 全模式切换路径 validation 验证
- [ ] Grid buffer 生命周期验证（load 创建、unload 销毁、probe_count == 0 不创建）
