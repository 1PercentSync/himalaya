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
- [x] `application.cpp`：bake finalize 时机——`begin_frame()` fence wait 后、`render()` 前检查 flag，Application 驱动 `begin_immediate()` / `end_immediate()`；`bake_finalize()` 框架（清理当前 instance 图像 → 推进到下一个 instance 或标记 Complete）
- [x] `renderer_bake.cpp`：per-instance `sample_count` 独立计数（从 0 到 target SPP），全局 `frame_seed` 单调递增不重置
- [x] `renderer_bake.cpp`：baker push constant hardcode（`max_clamp = 0`、`lod_max_level = 0`）；`directional_lights = 0` 和 `ibl_intensity = 1.0` 由 Application 在 RenderInput 中设置
- [x] `renderer_bake.cpp`：baker 独立 push constant（`max_bounces` / `env_sampling` / `emissive_light_count` 从 BakeConfig 读取）
- [x] `application.cpp`：baker allow tearing（BakeConfig::allow_tearing，Application 层设置 present mode，沿用 PT allow tearing 模式）
- [x] `scene_loader.cpp`：纹理加载时复用已有的 `source_hashes`（纹理缓存 pipeline 已算），按 glTF 纹理索引顺序拼接 hash 为 `scene_textures_hash`，存储在 SceneLoader 上
- [x] `renderer_bake.cpp`：lightmap cache key（`scene + geometry + transform + hdr + scene_textures`）+ 文件 `<lm_hash>_rot<NNN>.ktx2`
- [x] `renderer_bake.cpp`：probe set cache key（`scene + hdr + scene_textures`）+ 文件 `<set_hash>_rot<NNN>_probe<III>.ktx2` + manifest.bin（probe_count + positions）
- [x] `renderer_bake.cpp`：进入 Baking 前调用 `abort_denoise()`（参考视图异步 Denoiser 归 Idle）
- [x] `renderer_bake.cpp`：完整性校验（逐角度检查所有 lightmap + manifest + probe 文件齐全）
- [x] `renderer_bake.cpp`：退化 instance（vertex_count=0/index_count<3）和透明 instance（AlphaMode::Blend）跳过 lightmap bake
- [x] `renderer_bake.cpp`：KTX2 / manifest 原子写入（write-to-temp + rename）
- [x] `renderer_bake.cpp`：`rotation_int = round(angle_deg) % 360`（0-359）
- [x] `renderer_bake.cpp`：每帧帧流程 `fill_common_gpu_data()` → RG import Set 0 + TLAS → baker RT pass → clear `managed_hdr_color_` + blit accumulation（居中保持宽高比）→ tonemapping → ImGui render pass → present
- [x] `renderer_bake.cpp`：进入 Baking 前记录当前 RenderMode，Cancel/Complete 后恢复
- [x] `renderer_bake.cpp`：Cancel 后显示信息（"Bake cancelled. N/M instances completed."）
- [x] `renderer.cpp`：render() switch 新增 Baking case

## Step 9.5：审查修复（Steps 1-9 回顾）

- [x] `renderer.h`：删除未使用的 `bake_frame_seed_` 成员
- [x] `pos_normal_map_pass.cpp`：`record()` 入口检查 `pipeline_.pipeline != VK_NULL_HANDLE`，无效时 early return
- [x] `lightmap_baker_pass.cpp`：`record()` 入口检查 `rt_pipeline_.pipeline != VK_NULL_HANDLE`，无效时 early return
- [x] `renderer_bake.cpp`：lightmap cache key 的 `geometry_hash` 扩展为 vertices + indices 分别 hash 再拼接
- [x] `renderer_bake.cpp`：`start_bake()` 末尾调用 `begin_bake_instance(0)`（guard `bake_total_instances_ > 0`，0 时直接 Complete）；文档标注需 immediate scope
- [x] `renderer_bake.cpp`：`begin_bake_instance()` 中 accumulation / aux 创建后 barrier UNDEFINED→GENERAL + accumulation clear vec4(0)（aux 暂转 GENERAL，9.5c 改为 TRANSFER_DST + blit + GENERAL）
- [x] `closesthit.rchit`：aux imageStore 加 `if (pc.lightmap_width == 0u)` 守卫（lightmap baker 跳过，reference view / probe baker 保留）
- [x] `pos_normal_map.vert`：新增 `frag_uv0` 输出（pass through `in_uv0`）+ `material_index` push constant
- [x] `pos_normal_map.frag`：引入 `bindings.glsl`，从 MaterialBuffer 采样 base color，新增第三输出 `out_albedo`
- [x] `pos_normal_map_pass.h/.cpp`：`setup()` 新增 `DescriptorManager&`；push constants 新增 `material_index`（116B）；pipeline 3 color attachment（RGBA32F, RGBA32F, RGBA16F）；`record()` 新增 `material_index` + `frame_index` 参数，绑定 Set 0/1
- [x] `renderer.h`：新增 `bake_albedo_map_` 成员
- [x] `renderer_bake.cpp`：`begin_bake_instance()` blit albedo_map→aux_albedo + normal_map→aux_normal 预填充正确 aux
- [x] `lightmap_baker_pass.h/.cpp`：`record()` 接口改为接受外部 RG resource ID（5 个 baker 图像），移除内部 `import_image()` 调用
- [x] `renderer_bake.cpp`：`render_baking()` 统一 import baker 图像一次，传 RG resource ID 给 `lightmap_baker_pass_.record()` 和 blit pass
- [x] `renderer_bake.cpp`：`bake_finalize()` 实现完整管线（readback accumulation + aux_albedo + aux_normal → BakeDenoiser::denoise() → upload 降噪结果 → compress_bc6h() → readback BC6H → write_ktx2() → 释放 → 推进）

## Step 10：Probe 自动放置

- [x] 新增 `shaders/bake/probe_filter.comp`：蒙特卡洛球面采样 + `rayQueryEXT` 两级过滤 compute shader（Fibonacci lattice 方向生成 + MaterialBuffer double_sided 查询 + Rule 1 背面一票否决 + Rule 2 封闭体检测）
- [x] 新增 `framework/probe_placement.h`：`ProbeGrid` 结构体（positions + filter stats）+ `generate_probe_grid()` 函数声明
- [x] 新增 `framework/probe_placement.cpp`：均匀网格候选点生成（场景 AABB 内，grid spacing 间距）
- [x] `probe_placement.cpp`：两级 RT 几何过滤（compute shader dispatch + readback + Rule 1/Rule 2 统计日志）
- [x] `scene_data.h`：BakeConfig 新增 `filter_ray_count`(64) + `enclosure_threshold_factor`(0.05f)
- [x] `framework/CMakeLists.txt`：添加 `probe_placement.cpp`

## Step 11：Probe Baker Pass

- [x] 新增 `shaders/rt/probe_baker.rgen`：从 probe 位置向 6 面方向发射射线 + accumulation
- [x] 新增 `passes/probe_baker_pass.h`：ProbeBakerPass 类声明
- [x] 新增 `passes/probe_baker_pass.cpp`：setup（编译 rgen + 创建 RT pipeline）+ record（6 次 dispatch per frame，每 face 一次）
- [x] Set 3 layout（per-dispatch）：binding 0 accumulation face view（cubemap 单层 2D view）+ binding 1 aux albedo（per-face 2D image）+ binding 2 aux normal（per-face 2D image）+ binding 3 Sobol
- [x] Aux image 管理：2 个 RGBA16F image2DArray × 6 layer（albedo + normal）。Per-face 2D view per-probe 一次性创建（18 个 = 6 face × 3 images），所有 dispatch 复用，probe 完成时随 image 销毁
- [x] 6 个 face 共享同一个 per-probe `sample_count`：每帧 6 次 dispatch 后 `sample_count` +1（不是 +6）
- [x] `renderer.h` + `renderer_init.cpp`：持有 `ProbeBakerPass` 实例，setup/destroy/rebuild 统一调用（同时修复 LightmapBakerPass 遗漏的 setup 调用）

## Step 12：Probe 端到端流程

- [x] 前置重构：`bake_finalize()` → `lightmap_bake_finalize()`，`bake_finalize_pending_` → `lightmap_finalize_pending_`（方法名 + 成员名 + header doc + Application 调用侧）
- [x] `renderer.h`：新增 probe bake 状态字段（`bake_probe_positions_` / `bake_probe_total_` / `bake_current_probe_` / `bake_probe_accumulation_` / `bake_probe_aux_albedo_` / `bake_probe_aux_normal_` / `bake_probe_finalize_pending_` / `bake_probe_placement_pending_`）
- [x] `renderer_bake.cpp`：状态机转换——BakingLightmaps 完成或 `bake_total_instances_ == 0` 时设 `bake_state_ = BakingProbes` + `bake_probe_placement_pending_ = true`（不在 immediate scope 内调用 placement）
- [x] `renderer_bake.cpp`：`render_baking()` BakingProbes 首帧——检测 `placement_pending` → `generate_probe_grid()` → manifest.bin 原子写入 → `begin_immediate()` → `begin_probe_bake_instance(0)` → `end_immediate()` → 清除 pending
- [x] `renderer_bake.cpp`：`begin_probe_bake_instance()`——创建 3 个 CUBE_COMPATIBLE cubemap（accumulation RGBA32F + aux RGBA16F × 2）→ barrier UNDEFINED→GENERAL → clear accumulation → `probe_baker_pass_.set_probe_images()` + `set_probe_position()` + `reset_accumulation()`
- [x] `renderer_bake.cpp`：`render_baking()` BakingProbes 后续帧——dispatch `probe_baker_pass_.record()` → 达到 `probe_spp` 时设 `bake_probe_finalize_pending_`
- [x] `renderer_bake.cpp`：`render_baking()` BakingProbes RG 预览——十字展开（4×3 grid，6 次 `vkCmdBlitImage` via `srcSubresource.baseArrayLayer`）缩放居中到 hdr_color
- [x] `application.cpp`：`begin_frame()` 后检测 `bake_probe_finalize_pending_` → 调用 `probe_bake_finalize()`
- [x] `renderer_bake.cpp`：`probe_bake_finalize()` Scope 1——readback accumulation + aux（layerCount=6，一次 copy）
- [x] `renderer_bake.cpp`：`probe_bake_finalize()` CPU——OIDN × 6 face 逐面降噪
- [x] `renderer_bake.cpp`：`probe_bake_finalize()` Scope 2——upload 降噪结果 → `prefilter_cubemap()`（创建 RGBA16F dst cubemap with mip chain）→ `compress_bc6h()`（cubemap 6 face × N mip）→ readback BC6H
- [x] `renderer_bake.cpp`：`probe_bake_finalize()` 写入 `<set_hash>_rot<NNN>_probe<III>.ktx2`（`face_count=6`，`levels=N`）→ cleanup → advance 或 Complete
- [x] `renderer_bake.cpp`：`destroy_probe_bake_instance_images()`——`probe_baker_pass_.destroy_face_views()` + 销毁 3 个 cubemap。`cancel_bake()` 和 `Renderer::destroy()` 中调用
- [x] `renderer_bake.cpp`：manifest.bin 写入（`uint32 probe_count` + `vec3[N] positions`，原子写入）

## Step 12.5：延迟 Lightmap UV 生成

- [x] `lightmap_uv.h`：新增 `LightmapUVQuality` 枚举（Fast / Production）+ `kDefaultLightmapUVQuality` 编译期常量（函数签名不变，内部使用常量）
- [x] `lightmap_uv.cpp`：quality 分支（Fast: maxIterations=1 bruteForce=false，Production: maxIterations=4 bruteForce=true）+ 缓存 category 隔离（`lightmap_uv_debug` / `lightmap_uv_release`）
- [x] `lightmap_uv_generator.h`：新建 `LightmapUVGenerator` 类声明（Request struct + start/cancel/running/completed/total）
- [x] `lightmap_uv_generator.cpp`：新建线程池实现（jthread + atomic next_task/completed/cancel）
- [x] `framework/CMakeLists.txt`：添加 `lightmap_uv_generator.cpp`
- [x] `lightmap_uv_generator.h/.cpp` 重构：新增 `wait()` 方法（join 不 cancel），`cancel()` 复用 `wait()`，新增 `~LightmapUVGenerator()` 调用 `cancel()`（防止析构时 worker 未响应 jthread stop_token 导致卡住）
- [x] `scene_loader.h`：修正已提交的声明（删除 `has_pending_uvs()`，`apply_lightmap_uvs()` 文档更新为不清空 pending）+ 新增 `uv_pending_prims_` / `uv_pending_hashes_` 数据成员 + `prepare_uv_requests()` / `apply_lightmap_uvs()` 方法声明
- [x] `scene_loader.cpp`：`load_meshes()` 去除 xatlas 阻塞（无 TEXCOORD_1 的 mesh 上传 uv1=0，记录 pending）
- [x] `scene_loader.cpp`：`prepare_uv_requests()` 实现（从 pending + cpu data 构造 Request 列表）
- [x] `lightmap_uv.h/.cpp` 重构：`LightmapUVResult` 新增 `bool cache_hit` 字段，`generate_lightmap_uv()` 设置该字段
- [x] `scene_loader.cpp`：`apply_lightmap_uvs()` 实现（全量重建 VB/IB：pending 走 xatlas 缓存 + 检查 `cache_hit` miss 时 warn log，其余原样，需 immediate scope，不清空 pending）
- [x] `config.h`：`AppConfig` 新增 `bg_uv_auto_start`（bool）+ `bg_uv_thread_count`（uint32_t）
- [x] `config.cpp`：JSON 读写新增两个字段
- [x] `debug_ui.h`：`DebugUIContext` 新增 bg_uv 字段（thread_count&、auto_start&、running、completed、total、max_thread_count）+ `DebugUIActions` 新增 bg_uv_start_requested / bg_uv_stop_requested / bg_uv_config_changed
- [x] `debug_ui.cpp`：Baking header 内新增 Lightmap UV Generation 子面板（Auto-start checkbox + Threads slider + Start/Stop 按钮 + Status 文本）
- [x] `application.cpp`：持有 `LightmapUVGenerator`，场景加载后 auto-start 逻辑，DebugUIActions 处理（start/stop/config save），线程数首次解析并持久化
- [x] `scene_loader.h/.cpp` 修复：新增 `uv_original_vertices_` / `uv_original_indices_`（原始数据备份），`load_meshes()` 记录 pending 时同时保存副本，`prepare_uv_requests()` 和 `apply_lightmap_uvs()` 从原始数据读取，`destroy()` 清理 pending 数据
- [x] `application.cpp`：Start Bake 流程前置步骤（始终：if !running → start; wait → apply_lightmap_uvs → build_scene_rt → start_bake）

## Step 12.6：审查修复（Steps 12-12.5 正确性 + 全项目 barrier helper 重构）

### 12.6a：Vulkan 正确性修复

- [x] `renderer_bake.cpp`：`bake_normal_map_` 创建时 usage 补 `TransferSrc`
- [x] `renderer_bake.cpp`：`lightmap_bake_finalize()` 全部 readback buffer 补 `vmaInvalidateAllocation()`，upload buffer 补 `vmaFlushAllocation()`
- [x] `renderer_bake.cpp`：`probe_bake_finalize()` 全部 readback/upload buffer 补 VMA coherence 操作
- [x] `renderer_bake.cpp`：`lightmap_bake_finalize()` aux barrier 修正为 `srcStageMask = RT_SHADER` + `srcAccessMask = NONE`
- [x] `application.cpp`：`start_bake_session()` 开头补 `vkQueueWaitIdle(context_.graphics_queue)`

### 12.6b：逻辑 bug 修复

- [x] `renderer_bake.cpp`：`bake_rotation_int_` 负角度修正（fmod + 负值修正 + round + %360）
- [x] `renderer_bake.cpp` + `application.cpp`：`BakeState::Complete` 自动恢复 `bake_pre_mode_`

### 12.6c：lightmap UV 缓存健壮性

- [x] `lightmap_uv.h`：`LightmapUVResult` 新增 `bool is_fallback`
- [x] `lightmap_uv.h`：`CacheHeader` 新增 `uint32_t flags`（8→12 字节）
- [x] `lightmap_uv.cpp`：`write_cache()` / `read_cache()` 适配新 flags 字段
- [x] `lightmap_uv.cpp`：`AddMesh` 失败分流——mesh 固有错误缓存 fallback，`Error` 类型不缓存
- [ ] `lightmap_uv.cpp`：`Generate` 后 0×0 atlas 标记 `is_fallback=true`
- [ ] `scene_loader.cpp`：`apply_lightmap_uvs()` 检测 `is_fallback` 时 warn
- [ ] `scene_loader.cpp`：`prepare_uv_requests()` 按 `mesh_hash` 去重

### 12.6d：线程安全文档修正

- [ ] `lightmap_uv_generator.h`：修正线程安全注释（`running()` / `total()` 主线程限定）

### 12.6e：RHI barrier helper + 全项目重构

- [ ] `rhi/commands.h`：新增 `image_barrier()` static 构建函数
- [ ] 全项目替换手写 `VkImageMemoryBarrier2` 初始化（逐文件重构 + 审查 stage/access 精确性）

### 12.6f：`end_immediate()` 跨 submit memory dependency

- [ ] `context.cpp`：`end_immediate()` command buffer 末尾插入 full pipeline barrier

## Step 13：ImGui 烘焙控制面板

- [ ] `debug_ui.cpp`：Baking collapsing header（始终显示，默认折叠）— Lightmap UV Generation 子面板（Step 12.5 已实现）+ 参数配置（texels_per_meter / min_res / max_res / lightmap SPP / probe face res / probe spacing / filter ray count / enclosure threshold factor + 绝对阈值显示 / probe SPP / baker max_bounces / baker env_sampling / baker emissive_nee / baker allow_tearing）
- [ ] `debug_ui.cpp`：Start Bake 按钮（唯一入口，旁显当前角度 + tooltip，点击调用 `Application::start_bake_session()`（Step 12.5 已实现））+ Cancel 按钮（恢复原 RenderMode + 显示取消信息）
- [ ] `debug_ui.cpp`：Bake 期间 UI 锁定（bake 参数 slider + Load Scene + Load HDR + Reload Shaders + PT checkbox 全部灰显，PT 面板不显示，UV Generation Start 灰显）
- [ ] `debug_ui.cpp`：进度显示（阶段 + 当前项/总数 + 采样数/目标 + 吞吐量 SPP/s + 当前项耗时 + 总进度百分比 + 总耗时）
- [ ] `debug_ui.cpp`：已 bake 角度列表（目录扫描 `<hash>_rot*.ktx2`，显示角度 + lightmap/probe 数量，点击切换）
- [ ] `debug_ui.cpp`：Cache 面板新增 Clear Bake Cache 按钮 + Clear Lightmap UV Cache 按钮
- [ ] `renderer.h`：暴露烘焙状态（BakeState、current index、sample count、吞吐量、耗时等）给 DebugUIContext
- [ ] `config.cpp`：烘焙参数持久化（texels_per_meter、probe spacing、filter_ray_count、enclosure_threshold_factor、baker max_bounces、env_sampling、emissive_nee、allow_tearing、bg_uv_auto_start、bg_uv_thread_count）
