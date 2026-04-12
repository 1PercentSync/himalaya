# M1 阶段七：PT 烘焙器 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：BC6H 压缩通用工具提取

- [ ] 新增 `framework/texture_compress.h`：`BC6HCompressInput` 结构体 + `compress_bc6h()` 函数声明
- [ ] 新增 `framework/texture_compress.cpp`：从 `ibl_compress.cpp` 迁移核心逻辑，泛化签名接受 `std::span<const BC6HCompressInput>`
- [ ] `ibl_compress.cpp`：`IBL::compress_cubemaps_bc6h()` 改为调用 `compress_bc6h()`（thin wrapper 或直接替换调用方）
- [ ] `framework/CMakeLists.txt`：添加 `texture_compress.cpp` 源文件

## Step 2：Cubemap Prefilter 通用工具提取

- [ ] 新增 `framework/cubemap_filter.h`：`prefilter_cubemap()` 函数声明
- [ ] 新增 `framework/cubemap_filter.cpp`：从 `ibl_compute.cpp` 迁移 prefilter dispatch 逻辑，泛化签名（src/dst cubemap + mip count）
- [ ] `ibl_compute.cpp`：`IBL::compute_prefiltered()` 改为调用 `prefilter_cubemap()`
- [ ] `framework/CMakeLists.txt`：添加 `cubemap_filter.cpp` 源文件

## Step 3：xatlas vcpkg 集成 + Lightmap UV 生成器

- [ ] `vcpkg.json`：添加 `xatlas` 依赖
- [ ] 新增 `framework/lightmap_uv.h`：`LightmapUVResult` 结构体 + `generate_lightmap_uv()` 函数声明
- [ ] 新增 `framework/lightmap_uv.cpp`：TEXCOORD_1 检测（uv1 全零判定）
- [ ] `lightmap_uv.cpp`：xatlas 调用（Create → AddMesh → Generate → 提取结果）
- [ ] `lightmap_uv.cpp`：缓存写入（header + lightmap UV 数组 + new index buffer + vertex remap table）
- [ ] `lightmap_uv.cpp`：缓存读取（命中时跳过 xatlas）
- [ ] `framework/CMakeLists.txt`：添加 `lightmap_uv.cpp` + link xatlas

## Step 4：Lightmap UV 拓扑应用

- [ ] `scene_loader.cpp`：load_meshes() 后对每个 mesh 调用 `generate_lightmap_uv()`
- [ ] `scene_loader.cpp`：返回 LightmapUVResult 时，根据 remap table 构建新 vertex 数组（uv1 写入 lightmap UV）+ 新 index 数组
- [ ] `scene_loader.cpp`：销毁原 GPU vertex/index buffer → 上传新 buffer → 更新 Mesh 字段
- [ ] `scene_loader.cpp`：同步更新 `cpu_vertices_` / `cpu_indices_`
- [ ] Lightmap 分辨率计算：per-instance 世界空间表面积 → `clamp(round(sqrt(area) * tpm), min, max)` → 对齐到 4
- [ ] 加载顺序确认：glTF → xatlas UV → rebuild VB/IB → build BLAS/TLAS → build emissive lights

## Step 5：BakeDenoiser

- [ ] 新增 `framework/bake_denoiser.h`：BakeDenoiser 类（init / denoise / destroy）
- [ ] 新增 `framework/bake_denoiser.cpp`：OIDN device 创建（GPU 优先 fallback CPU）+ RT filter + OIDNBuffer 管理
- [ ] `bake_denoiser.cpp`：`denoise()` 同步阻塞实现（memcpy → oidnExecuteFilter → memcpy → error check）
- [ ] `framework/CMakeLists.txt`：添加 `bake_denoiser.cpp`

## Step 6：PT Push Constants 扩展 + closesthit baker_mode

- [ ] RT shader PushConstants struct 追加 `uint baker_mode`（36B → 40B）+ `uint lightmap_width` + `uint lightmap_height`（40B → 48B）
- [ ] `closesthit.rchit`：OIDN aux imageStore 加条件 `if (baker_mode == 0u && payload.bounce == 0u)`
- [ ] `reference_view_pass.cpp`：push constant 填充追加 baker_mode=0, lightmap_width=0, lightmap_height=0
- [ ] `reference_view_pass.h`：push constant range 更新为 48B

## Step 7：Position/Normal Map 光栅化 pass

- [ ] 新增 `shaders/bake/pos_normal_map.vert`：uv1 → NDC 映射 + 世界空间 position/normal 输出
- [ ] 新增 `shaders/bake/pos_normal_map.frag`：写入两个 RGBA32F color attachment
- [ ] `renderer_bake.cpp`：position/normal map graphics pipeline 创建（Dynamic Rendering，2 color attachment，无 depth）
- [ ] `renderer_bake.cpp`：录制函数（给定 mesh + transform + lightmap 分辨率 → 创建 image + draw call）

## Step 8：Lightmap Baker Pass

- [ ] 新增 `shaders/rt/lightmap_baker.rgen`：读 position/normal map → 逐 texel 发射射线 → accumulation
- [ ] 新增 `passes/lightmap_baker_pass.h`：LightmapBakerPass 类声明
- [ ] 新增 `passes/lightmap_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（RG pass + push descriptors + trace_rays）
- [ ] Set 3 layout：binding 0 accumulation + binding 1/2 dummy aux + binding 3 Sobol + binding 4 position map + binding 5 normal map
- [ ] Dummy aux images（1×1 RGBA16F storage）创建（Renderer init）

## Step 9：烘焙模式渲染路径 + Lightmap 端到端

- [ ] `scene_data.h`：RenderMode 新增 Baking
- [ ] `renderer.h`：BakeState 枚举 + 烘焙状态字段 + `render_baking()` 私有方法
- [ ] 新增 `renderer_bake.cpp`：烘焙状态机（BakingLightmaps → BakingProbes → Complete）
- [ ] `renderer_bake.cpp`：per-instance lightmap 烘焙循环（position/normal map → accumulation → 目标采样数 → denoise → BC6H → KTX2）
- [ ] `renderer.cpp`：render() switch 新增 Baking case
- [ ] `debug_ui.cpp`：RenderMode combo 新增 Baking 选项

## Step 10：Probe 自动放置

- [ ] 新增 `framework/probe_placement.h`：`ProbeGrid` 结构体 + `generate_probe_grid()` 函数声明
- [ ] 新增 `framework/probe_placement.cpp`：均匀网格候选点生成（场景 AABB 内，grid spacing 间距）
- [ ] `probe_placement.cpp`：RT 几何过滤（6 轴对齐射线，>= 5/6 短距离命中 → 剔除）
- [ ] `framework/CMakeLists.txt`：添加 `probe_placement.cpp`

## Step 11：Probe Baker Pass

- [ ] 新增 `shaders/rt/probe_baker.rgen`：从 probe 位置向 6 面方向发射射线 + accumulation
- [ ] 新增 `passes/probe_baker_pass.h`：ProbeBakerPass 类声明
- [ ] 新增 `passes/probe_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（RG pass + trace_rays）
- [ ] Set 3 layout：binding 0 accumulation cubemap（image2DArray storage）+ binding 1/2 dummy aux + binding 3 Sobol

## Step 12：Probe 端到端流程

- [ ] `renderer_bake.cpp`：BakingProbes 状态扩展（probe_placement → 逐 probe 烘焙循环）
- [ ] 烘焙循环：accumulation cubemap 创建 → dispatch → 目标采样数 → BakeDenoiser（6 face 逐面）→ prefilter_cubemap() → compress_bc6h() → write_ktx2()
- [ ] 所有 probe 完成 → BakeState::Complete

## Step 13：ImGui 烘焙控制面板

- [ ] `debug_ui.cpp`：Baking collapsing header — 参数配置（texels_per_meter / min_res / max_res / lightmap SPP / probe face res / probe spacing / probe SPP）
- [ ] `debug_ui.cpp`：Start Bake / Cancel 按钮
- [ ] `debug_ui.cpp`：进度显示（阶段 + 当前项/总数 + 采样数/目标 + 百分比）
- [ ] `debug_ui.cpp`：ImGui::Image() accumulation 预览
- [ ] `renderer.h`：暴露烘焙状态给 DebugUIContext
- [ ] `config.cpp`：烘焙参数持久化
