# M1 阶段七：PT 烘焙器 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：BC6H 压缩通用工具提取

- [x] 新增 `framework/texture_compress.h`：`BC6HCompressInput` 结构体 + `compress_bc6h()` 函数声明
- [x] 新增 `framework/texture_compress.cpp`：从 `ibl_compress.cpp` 迁移核心逻辑，泛化签名接受 `std::span<const BC6HCompressInput>`
- [x] `ibl_compress.cpp`：`IBL::compress_cubemaps_bc6h()` 改为调用 `compress_bc6h()`（thin wrapper 或直接替换调用方）
- [x] `framework/CMakeLists.txt`：添加 `texture_compress.cpp` 源文件

## Step 2：Cubemap Prefilter 通用工具提取

- [ ] 新增 `framework/cubemap_filter.h`：`prefilter_cubemap()` 函数声明
- [ ] 新增 `framework/cubemap_filter.cpp`：从 `ibl_compute.cpp` 迁移 prefilter dispatch 逻辑，泛化签名（src/dst cubemap + mip count）
- [ ] `ibl_compute.cpp`：`IBL::compute_prefiltered()` 改为调用 `prefilter_cubemap()`
- [ ] `framework/CMakeLists.txt`：添加 `cubemap_filter.cpp` 源文件

## Step 3：xatlas 集成 + Lightmap UV 生成器

- [ ] 新增 `third_party/xatlas/`：复制 xatlas.h + xatlas.cpp（单文件库，MIT）
- [ ] `framework/CMakeLists.txt`：添加 xatlas.cpp 到源文件 + include path
- [ ] 新增 `framework/lightmap_uv.h`：`LightmapUVResult` 结构体 + `generate_lightmap_uv()` 函数声明
- [ ] 新增 `framework/lightmap_uv.cpp`：TEXCOORD_1 检测（接受 `bool has_lightmap_uv` flag，由 SceneLoader 通过 `findAttribute("TEXCOORD_1")` 检测传递）
- [ ] `lightmap_uv.cpp`：xatlas 调用（Create → AddMesh → Generate → 提取结果）
- [ ] `lightmap_uv.cpp`：缓存写入（header + lightmap UV 数组 + new index buffer + vertex remap table）
- [ ] `lightmap_uv.cpp`：缓存读取（命中时跳过 xatlas）
- [ ] `framework/CMakeLists.txt`：添加 `lightmap_uv.cpp`

## Step 4：Lightmap UV 拓扑应用

- [ ] `scene_loader.cpp`：load_meshes() 后对每个 mesh 调用 `generate_lightmap_uv()`
- [ ] `scene_loader.cpp`：返回 LightmapUVResult 时，根据 remap table 构建新 vertex 数组（uv1 写入 lightmap UV）+ 新 index 数组
- [ ] `scene_loader.cpp`：销毁原 GPU vertex/index buffer → 上传新 buffer → 更新 Mesh 字段
- [ ] `scene_loader.cpp`：同步更新 `cpu_vertices_` / `cpu_indices_`
- [ ] 加载顺序确认：glTF → xatlas UV → rebuild VB/IB → build BLAS/TLAS → build emissive lights

## Step 5：BakeDenoiser

- [ ] 新增 `framework/bake_denoiser.h`：BakeDenoiser 类（init / denoise / destroy）
- [ ] 新增 `framework/bake_denoiser.cpp`：OIDN device 创建（GPU 优先 fallback CPU）+ RT filter + OIDNBuffer 管理
- [ ] `bake_denoiser.cpp`：`denoise()` 同步阻塞实现（memcpy → oidnExecuteFilter → memcpy → error check）
- [ ] `framework/CMakeLists.txt`：添加 `bake_denoiser.cpp`

## Step 6：PT Push Constants 扩展 + 默认值修正

- [ ] RT shader PushConstants struct 追加 `uint lightmap_width` + `uint lightmap_height` + `float probe_pos_x/y/z` + `uint face_index`（36B → 60B）
- [ ] `pt_common.glsl` 新增 `build_orthonormal_basis(N, out T, out B)` 函数（从 closesthit:392-398 提取）
- [ ] `renderer.h`：`max_clamp_` 默认值 10.0f → 0.0f + `prev_max_clamp_` 同步改为 0.0f（firefly clamping 默认关闭）
- [ ] `reference_view_pass.cpp`：push constant 填充追加 baker 字段为 0
- [ ] `reference_view_pass.h`：push constant range 更新为 60B
- [ ] closesthit.rchit 不改动（aux imageStore 保持无条件执行）

## Step 7：Position/Normal Map 光栅化 pass

- [ ] 新增 `shaders/bake/pos_normal_map.vert`：uv1 → NDC 映射 + 世界空间 position/normal 输出
- [ ] 新增 `shaders/bake/pos_normal_map.frag`：写入两个 RGBA32F color attachment
- [ ] `renderer_bake.cpp`：position/normal map graphics pipeline 创建（Dynamic Rendering，2 color attachment，无 depth）
- [ ] `renderer_bake.cpp`：录制函数（给定 mesh + transform + lightmap 分辨率 → 创建 image + draw call）

## Step 8：Lightmap Baker Pass

- [ ] `pt_common.glsl`：`DIMS_PER_BOUNCE` 常量从 raygen/closesthit 迁移到此处
- [ ] `pt_common.glsl`：从 reference_view.rgen 提取共享 `trace_path()` 函数（`#ifdef RAYGEN_SHADER` 守卫，三个 raygen 共用）
- [ ] `reference_view.rgen`：重构为 `#define RAYGEN_SHADER` + 调用 `trace_path()`（行为不变）。push constant + payload 声明移到 `#include "rt/pt_common.glsl"` 之前（trace_path() 引用 `pc.xxx` 和 `payload`）
- [ ] 新增 `shaders/rt/lightmap_baker.rgen`：读 position/normal map → 逐 texel 发射射线 → 调用共享 bounce loop → accumulation
- [ ] 新增 `passes/lightmap_baker_pass.h`：LightmapBakerPass 类声明
- [ ] 新增 `passes/lightmap_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（RG pass + push descriptors + trace_rays）
- [ ] Set 3 layout：binding 0 accumulation + binding 1 aux albedo (lightmap 分辨率) + binding 2 aux normal (lightmap 分辨率) + binding 3 Sobol + binding 4 position map + binding 5 normal map

## Step 9：烘焙模式渲染路径 + Lightmap 端到端

- [ ] `scene_data.h`：RenderMode 新增 Baking
- [ ] `renderer.h`：BakeState 枚举 + 烘焙状态字段 + `render_baking()` 私有方法
- [ ] 新增 `renderer_bake.cpp`：烘焙状态机（BakingLightmaps → BakingProbes → Complete）
- [ ] `renderer_bake.cpp`：烘焙触发时计算 per-instance lightmap 分辨率（世界空间表面积 × texels_per_meter，对齐到 4）
- [ ] `renderer_bake.cpp`：per-instance lightmap 烘焙循环（position/normal map + aux image 创建 → accumulation → 目标采样数 → readback accumulation + aux → BakeDenoiser（含辅助通道）→ BC6H → KTX2）
- [ ] `renderer_bake.cpp`：per-instance `sample_count` 独立计数（从 0 到 target SPP），全局 `frame_seed` 单调递增不重置
- [ ] `renderer_bake.cpp`：baker push constant `max_clamp = 0`（禁用 firefly clamping）
- [ ] `renderer_bake.cpp`：每帧帧流程 `fill_common_gpu_data()` → RG import Set 0 + TLAS → baker RT pass → ImGui render pass → present
- [ ] `renderer.cpp`：render() switch 新增 Baking case
- [ ] `debug_ui.cpp`：RenderMode combo 新增 Baking 选项

## Step 10：Probe 自动放置

- [ ] 新增 `shaders/bake/probe_filter.comp`：rayQueryEXT 几何过滤 compute shader
- [ ] 新增 `framework/probe_placement.h`：`ProbeGrid` 结构体 + `generate_probe_grid()` 函数声明
- [ ] 新增 `framework/probe_placement.cpp`：均匀网格候选点生成（场景 AABB 内，grid spacing 间距）
- [ ] `probe_placement.cpp`：RT 几何过滤（compute shader + `rayQueryEXT`，6 轴对齐射线，>= 5/6 短距离命中 → 剔除）
- [ ] `framework/CMakeLists.txt`：添加 `probe_placement.cpp`

## Step 11：Probe Baker Pass

- [ ] 新增 `shaders/rt/probe_baker.rgen`：从 probe 位置向 6 面方向发射射线 + accumulation
- [ ] 新增 `passes/probe_baker_pass.h`：ProbeBakerPass 类声明
- [ ] 新增 `passes/probe_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（6 次 dispatch per frame，每 face 一次）
- [ ] Set 3 layout（per-dispatch）：binding 0 accumulation face view（cubemap 单层 2D view）+ binding 1 aux albedo（per-face 2D image）+ binding 2 aux normal（per-face 2D image）+ binding 3 Sobol
- [ ] Aux image 管理：6 对 aux 2D image（或 2 个 image2DArray per-layer 2D view），per-probe 创建/销毁
- [ ] 6 个 face 共享同一个 per-probe `sample_count`：每帧 6 次 dispatch 后 `sample_count` +1（不是 +6）

## Step 12：Probe 端到端流程

- [ ] `renderer_bake.cpp`：BakingProbes 状态扩展（probe_placement → 逐 probe 烘焙循环）
- [ ] 烘焙循环：accumulation + aux cubemap 创建 → dispatch → 目标采样数 → readback accumulation + aux → BakeDenoiser（6 face 逐面，含辅助通道）→ prefilter_cubemap() → compress_bc6h() → write_ktx2()
- [ ] 所有 probe 完成 → BakeState::Complete

## Step 13：ImGui 烘焙控制面板

- [ ] `debug_ui.cpp`：Baking collapsing header — 参数配置（texels_per_meter / min_res / max_res / lightmap SPP / probe face res / probe spacing / probe SPP）
- [ ] `debug_ui.cpp`：Start Bake / Cancel 按钮
- [ ] `debug_ui.cpp`：进度显示（阶段 + 当前项/总数 + 采样数/目标 + 百分比）
- [ ] `debug_ui.cpp`：ImGui::Image() accumulation 预览
- [ ] `renderer.h`：暴露烘焙状态给 DebugUIContext
- [ ] `config.cpp`：烘焙参数持久化
