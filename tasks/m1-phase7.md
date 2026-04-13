# M1 阶段七：PT 烘焙器 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：BC6H 压缩通用工具提取

- [x] 新增 `framework/texture_compress.h`：`BC6HCompressInput` 结构体 + `compress_bc6h()` 函数声明
- [x] 新增 `framework/texture_compress.cpp`：从 `ibl_compress.cpp` 迁移核心逻辑，泛化签名接受 `std::span<const BC6HCompressInput>`
- [x] `ibl_compress.cpp`：`IBL::compress_cubemaps_bc6h()` 改为调用 `compress_bc6h()`（thin wrapper 或直接替换调用方）
- [x] `framework/CMakeLists.txt`：添加 `texture_compress.cpp` 源文件

## Step 2：Cubemap Prefilter 通用工具提取

- [x] 新增 `framework/cubemap_filter.h`：`prefilter_cubemap()` 函数声明
- [x] 新增 `framework/cubemap_filter.cpp`：从 `ibl_compute.cpp` 迁移 prefilter dispatch 逻辑，泛化签名（src/dst cubemap + mip count）
- [x] `ibl_compute.cpp`：`IBL::compute_prefiltered()` 改为调用 `prefilter_cubemap()`
- [x] `framework/CMakeLists.txt`：添加 `cubemap_filter.cpp` 源文件

## Step 3：xatlas 集成 + Lightmap UV 生成器

- [x] 新增 `third_party/xatlas/`：复制 xatlas.h + xatlas.cpp（单文件库，MIT）
- [x] `framework/CMakeLists.txt`：添加 xatlas.cpp 到源文件 + include path
- [x] 新增 `framework/lightmap_uv.h`：`LightmapUVResult` 结构体 + `generate_lightmap_uv()` 函数声明
- [x] 新增 `framework/lightmap_uv.cpp`：xatlas 调用（Create → AddMesh → Generate → 提取结果）
- [x] `lightmap_uv.cpp`：缓存写入（header + lightmap UV array + new index buffer + vertex remap table）
- [x] `lightmap_uv.cpp`：缓存读取（命中时跳过 xatlas）
- [x] `framework/CMakeLists.txt`：添加 `lightmap_uv.cpp`

## Step 4：Lightmap UV 拓扑应用

- [x] `scene_loader.cpp`：load_meshes() 内部、GPU upload 前，对无 TEXCOORD_1 的 primitive 计算 mesh_hash + 调用 `generate_lightmap_uv()`
- [x] `scene_loader.cpp`：根据 remap table 构建新 vertex 数组（uv1 写入 lightmap UV）+ 新 index 数组，替换原 vertices/indices
- [x] `scene_loader.cpp`：日志输出每个 mesh 的 lightmap UV 来源（TEXCOORD_1 / xatlas / cache）
- [x] 加载顺序确认：glTF → vertex/index build → tangent → TEXCOORD_1 → xatlas UV → GPU upload → BLAS/TLAS → emissive lights

## Step 5：BakeDenoiser

- [x] 新增 `framework/bake_denoiser.h`：BakeDenoiser 类（init / denoise / destroy）
- [x] 新增 `framework/bake_denoiser.cpp`：OIDN device 创建（GPU 优先 fallback CPU）+ RT filter + OIDNBuffer 管理
- [x] `bake_denoiser.cpp`：`denoise()` 同步阻塞实现（memcpy → oidnExecuteFilter → memcpy → error check）
- [x] `framework/CMakeLists.txt`：添加 `bake_denoiser.cpp`

## Step 6：PT Push Constants 扩展 + 默认值修正

- [x] RT shader PushConstants struct 追加 `uint lightmap_width` + `uint lightmap_height` + `float probe_pos_x/y/z` + `uint face_index`（36B → 60B）
- [x] `pt_common.glsl` 新增 `build_orthonormal_basis(N, out T, out B)` 函数（从 closesthit:392-398 提取）
- [x] `renderer.h`：`max_clamp_` 默认值 10.0f → 0.0f + `prev_max_clamp_` 同步改为 0.0f（firefly clamping 默认关闭）
- [x] `reference_view_pass.cpp`：push constant 填充追加 baker 字段为 0
- [x] `reference_view_pass.h`：push constant range 更新为 60B（通过 sizeof(PTPushConstants) 自动生效）
- [x] closesthit.rchit 不改动（aux imageStore 保持无条件执行）

## Step 7：Position/Normal Map 光栅化 pass

- [x] 新增 `shaders/bake/pos_normal_map.vert`：uv1 → NDC 映射 + 世界空间 position/normal 输出
- [x] 新增 `shaders/bake/pos_normal_map.frag`：写入两个 RGBA32F color attachment
- [x] 新增 `passes/pos_normal_map_pass.h`：`PosNormalMapPass` 类声明
- [x] 新增 `passes/pos_normal_map_pass.cpp`：pipeline 创建（Dynamic Rendering，2×RGBA32F，无 depth）+ `record()` 录制函数
- [x] `passes/CMakeLists.txt`：添加 `pos_normal_map_pass.cpp`
- [x] `renderer.h` + `renderer_init.cpp`：持有 `PosNormalMapPass` 实例，setup/destroy/rebuild 统一调用

## Step 8：Lightmap Baker Pass

- [x] `pt_common.glsl`：`DIMS_PER_BOUNCE` 常量从 raygen/closesthit 迁移到此处
- [x] `pt_common.glsl`：从 reference_view.rgen 提取共享 `trace_path()` 函数（`#ifdef RAYGEN_SHADER` 守卫，三个 raygen 共用）
- [x] `reference_view.rgen`：重构为 `#define RAYGEN_SHADER` + 调用 `trace_path()`（行为不变）。push constant + payload 声明由 pt_common.glsl `#ifdef RAYGEN_SHADER` 区域统一管理
- [x] 新增 `shaders/rt/lightmap_baker.rgen`：读 position/normal map → 逐 texel 发射射线 → 调用共享 bounce loop → accumulation
- [x] 新增 `passes/lightmap_baker_pass.h`：LightmapBakerPass 类声明
- [x] 新增 `passes/lightmap_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（RG pass + push descriptors + trace_rays）
- [x] Set 3 layout：binding 0 accumulation + binding 1 aux albedo (lightmap 分辨率) + binding 2 aux normal (lightmap 分辨率) + binding 3 Sobol + binding 4 position map + binding 5 normal map
- [x] `lightmap_baker_pass.h`：`lod_max_level_` 默认值从 4 改为 0，移除 `set_lod_max_level()` 接口（baker 始终全分辨率纹理采样，hardcode）
- [x] `lightmap_baker_pass.h`：`max_bounces_` 默认值从 8 改为 32（与 baker 面板默认值一致）
- [x] `lightmap_baker_pass.h/.cpp`：移除 `set_directional_lights()` 接口和 `directional_lights_` 成员，record() 中 hardcode `directional_lights = 0`（与 `max_clamp = 0.0f` 一致的 hardcode 策略）
- [x] `pos_normal_map_pass.cpp`：`rebuild_pipelines()` 去掉双重编译，直接调 `create_pipeline()`（与 ReferenceViewPass/LightmapBakerPass 一致）
- [x] `pos_normal_map_pass.h`：`destroy()` 去掉 const（与所有其他 Pass 一致）
- [x] `reference_view_pass.cpp`：删除未使用的 `kDefaultMaxBounces` / `kDefaultMaxClamp` 常量
- [x] 新增 `passes/include/himalaya/passes/pt_push_constants.h`：PTPushConstants 共享定义，reference_view_pass.cpp 和 lightmap_baker_pass.cpp 删除各自的重复定义
- [x] `bake_denoiser.h/.cpp`：`denoise()` 的 albedo/normal 参数移除 nullable 语义，入口 assert 非空

## Step 9：烘焙模式渲染路径 + Lightmap 端到端

- [x] 重构：PT 参数从 Renderer 迁移到 Application（新增 `scene_data.h` PTConfig 结构体，沿用 ShadowConfig 模式；RenderInput 新增 `const PTConfig&`；移除 Renderer 上的 mutable reference accessors）
- [x] 新增 `scene_data.h`：BakeConfig 结构体（texels_per_meter / min_res / max_res / lightmap_spp / probe_face_res / probe_spacing / probe_spp / max_bounces / env_sampling / emissive_nee / allow_tearing）+ RenderInput 新增 `const BakeConfig&`
- [x] `scene_data.h`：RenderMode 新增 Baking
- [x] `renderer.h`：BakeState 枚举 + 烘焙状态字段 + `render_baking()` 私有方法
- [x] 新增 `renderer_bake.cpp`：烘焙状态机（BakingLightmaps → BakingProbes → Complete）
- [x] `renderer_bake.cpp`：烘焙触发时计算 per-instance lightmap 分辨率（世界空间表面积 × texels_per_meter，对齐到 4）
- [x] `renderer_bake.cpp`：per-instance lightmap 烘焙循环（position/normal map + aux image 创建 → accumulation → 目标采样数 → 设 finalize pending flag）
- [x] `application.cpp`：bake finalize 时机——`begin_frame()` fence wait 后、`render()` 前检查 flag，Application 驱动 `begin_immediate()` / `end_immediate()`（readback → BakeDenoiser → upload → BC6H → readback → KTX2 → 释放资源 → 推进到下一个 instance）
- [x] `renderer_bake.cpp`：per-instance `sample_count` 独立计数（从 0 到 target SPP），全局 `frame_seed` 单调递增不重置
- [x] `renderer_bake.cpp`：baker push constant hardcode（`max_clamp = 0`、`lod_max_level = 0`）；`directional_lights = 0` 和 `ibl_intensity = 1.0` 由 Application 在 RenderInput 中设置
- [x] `renderer_bake.cpp`：baker 独立 push constant（`max_bounces` / `env_sampling` / `emissive_light_count` 从 BakeConfig 读取）
- [x] `application.cpp`：baker allow tearing（BakeConfig::allow_tearing，Application 层设置 present mode，沿用 PT allow tearing 模式）
- [x] `scene_loader.cpp`：纹理加载时复用已有的 `source_hashes`（纹理缓存 pipeline 已算），按 glTF 纹理索引顺序拼接 hash 为 `scene_textures_hash`，存储在 SceneLoader 上
- [x] `renderer_bake.cpp`：lightmap cache key（`scene + geometry + transform + hdr + scene_textures`）+ 文件 `<lm_hash>_rot<NNN>.ktx2`
- [x] `renderer_bake.cpp`：probe set cache key（`scene + hdr + scene_textures`）+ 文件 `<set_hash>_rot<NNN>_probe<III>.ktx2` + manifest.bin（probe_count + positions）
- [ ] `renderer_bake.cpp`：进入 Baking 前调用 `abort_denoise()`（参考视图异步 Denoiser 归 Idle）
- [ ] `renderer_bake.cpp`：完整性校验（逐角度检查所有 lightmap + manifest + probe 文件齐全）
- [ ] `renderer_bake.cpp`：退化 instance（vertex_count=0/index_count<3）和透明 instance（AlphaMode::Blend）跳过 lightmap bake
- [ ] `renderer_bake.cpp`：KTX2 / manifest 原子写入（write-to-temp + rename）
- [ ] `renderer_bake.cpp`：`rotation_int = round(angle_deg) % 360`（0-359）
- [ ] `renderer_bake.cpp`：每帧帧流程 `fill_common_gpu_data()` → RG import Set 0 + TLAS → baker RT pass → clear `managed_hdr_color_` + blit accumulation（居中保持宽高比）→ tonemapping → ImGui render pass → present
- [ ] `renderer_bake.cpp`：进入 Baking 前记录当前 RenderMode，Cancel/Complete 后恢复
- [ ] `renderer_bake.cpp`：Cancel 后显示信息（"Bake cancelled. N/M instances completed."）
- [ ] `renderer.cpp`：render() switch 新增 Baking case

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
- [ ] Aux image 管理：2 个 RGBA16F image2DArray × 6 layer（albedo + normal）。Per-face 2D view per-probe 一次性创建（18 个 = 6 face × 3 images），所有 dispatch 复用，probe 完成时随 image 销毁
- [ ] 6 个 face 共享同一个 per-probe `sample_count`：每帧 6 次 dispatch 后 `sample_count` +1（不是 +6）

## Step 12：Probe 端到端流程

- [ ] `renderer_bake.cpp`：BakingProbes 状态扩展（probe_placement → 写入 manifest.bin → 逐 probe 烘焙循环）
- [ ] manifest.bin 写入：`uint32 probe_count` + `vec3[probe_count] positions`（probe position 是 bake 产物）
- [ ] 烘焙循环：accumulation + aux cubemap 创建 → dispatch → 目标采样数 → readback accumulation + aux → BakeDenoiser（6 face 逐面，含辅助通道）→ prefilter_cubemap() → compress_bc6h() → write_ktx2()（文件名 `<set_hash>_rot<NNN>_probe<III>.ktx2`）
- [ ] 所有 probe 完成 → BakeState::Complete

## Step 13：ImGui 烘焙控制面板

- [ ] `debug_ui.cpp`：Baking collapsing header（始终显示，默认折叠）— 参数配置（texels_per_meter / min_res / max_res / lightmap SPP / probe face res / probe spacing / probe SPP / baker max_bounces / baker env_sampling / baker emissive_nee / baker allow_tearing）
- [ ] `debug_ui.cpp`：Start Bake 按钮（唯一入口，旁显当前角度 + tooltip）+ Cancel 按钮（恢复原 RenderMode + 显示取消信息）
- [ ] `debug_ui.cpp`：Bake 期间 UI 锁定（bake 参数 slider + Load Scene + Load HDR + Reload Shaders + PT checkbox 全部灰显，PT 面板不显示）
- [ ] `debug_ui.cpp`：进度显示（阶段 + 当前项/总数 + 采样数/目标 + 吞吐量 SPP/s + 当前项耗时 + 总进度百分比 + 总耗时）
- [ ] `debug_ui.cpp`：已 bake 角度列表（目录扫描 `<hash>_rot*.ktx2`，显示角度 + lightmap/probe 数量，点击切换）
- [ ] `debug_ui.cpp`：Cache 面板新增 Clear Bake Cache 按钮
- [ ] `renderer.h`：暴露烘焙状态（BakeState、current index、sample count、吞吐量、耗时等）给 DebugUIContext
- [ ] `config.cpp`：烘焙参数持久化（texels_per_meter、probe spacing、baker max_bounces、env_sampling、emissive_nee、allow_tearing）
