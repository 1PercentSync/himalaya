# M1 阶段六：RT 基础设施 + PT 参考视图 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：RT 扩展检测 + 启用 + 特性激活

- [x] 新增 RT 扩展列表（acceleration_structure、ray_tracing_pipeline、ray_query、deferred_host_operations）
- [x] pick_physical_device() RT 支持作为加分项评分
- [x] Context 新增 `rt_supported` 公有字段
- [x] 日志输出选中设备 RT 支持状态
- [x] rt_supported 时 RT 扩展加入设备扩展列表
- [x] 启用设备特性：accelerationStructure、rayTracingPipeline、rayQuery、bufferDeviceAddress
- [x] rt_supported 时 VMA allocator 添加 BUFFER_DEVICE_ADDRESS flag
- [x] 查询并存储 RT pipeline 属性（shaderGroupHandleSize、shaderGroupBaseAlignment、shaderGroupHandleAlignment、maxRayRecursionDepth）+ AS 属性（minAccelerationStructureScratchOffsetAlignment）
- [x] Context 新增 RT 属性存储字段

## Step 2：AS 资源抽象

- [x] 新增 acceleration_structure.h：BLASHandle、TLASHandle、BLASGeometry、BLASBuildInfo（multi-geometry：`span<const BLASGeometry> geometries`）类型
- [x] AccelerationStructureManager：build_blas() 单次 vkCmdBuild 并行构建全部（PREFER_FAST_TRACE），每个 BLASBuildInfo 支持 1..N geometries
- [x] BLASGeometry 新增 `opaque` 字段 + build_blas() per-geometry opacity flag（`OPAQUE_BIT` / `NO_DUPLICATE_ANY_HIT_INVOCATION_BIT`）
- [x] AccelerationStructureManager：build_tlas()（TLAS instance geometry flags = 0，不覆盖 BLAS per-geometry 设置）
- [x] AccelerationStructureManager：destroy_blas()、destroy_tlas()
- [x] Scratch buffer 管理：分配大 scratch = 各 BLAS scratch 之和（对齐到 minAccelerationStructureScratchOffsetAlignment），构建完成后释放
- [x] 顶点格式硬编码：vertexFormat = R32G32B32_SFLOAT (offset 0)、indexType = UINT32

## Step 3：RT Pipeline + SBT + trace_rays

- [x] 新增 rt_pipeline.h：RTPipelineDesc（含可选 anyhit module）、RTPipeline 类型
- [x] create_rt_pipeline()：shader group 创建（hit group = closesthit + anyhit）+ vkCreateRayTracingPipelinesKHR
- [x] SBT 构建（vkGetRayTracingShaderGroupHandlesKHR → 对齐写入 SBT buffer，1 hit group entry 含 chit + ahit handle）
- [x] RTPipeline::destroy()
- [x] CommandBuffer 新增 trace_rays(const RTPipeline&, width, height)
- [x] RT 扩展函数通过 vkGetDeviceProcAddr 动态加载（Context 6 个 + CommandBuffer 1 个）
- [x] create_rt_pipeline() 签名简化为 (const Context&, const RTPipelineDesc&)

## Step 4：RHI/Framework RT 基础设施

- [x] BufferUsage 新增 ShaderDeviceAddress + ResourceManager 映射
- [x] ResourceManager 新增 get_buffer_device_address()
- [x] scene_data.h 新增 GPUGeometryInfo 结构体（std430 24B：vertex_buffer_address u64 + index_buffer_address u64 + material_buffer_offset u32 + _padding u32）+ static_assert 守卫
- [x] DescriptorManager::init() 从 context_->rt_supported 读取 RT 状态，Set 0 layout 条件扩展 binding 4/5（PARTIALLY_BOUND）+ descriptor pool 容量扩展（新增 AS + SSBO 描述符）
- [x] Set 0 bindings 0-2 stageFlags 条件追加 RT stages（RAYGEN | CLOSEST_HIT | MISS | ANY_HIT），binding 3 不变
- [x] get_global_set_layouts() 重命名为 get_graphics_set_layouts() + get_compute_set_layouts() 重命名为 get_dispatch_set_layouts()
- [x] DescriptorManager 新增 write_set0_tlas()
- [x] Set 1（bindless textures）layout binding stage flags 添加 `CLOSEST_HIT_BIT_KHR` + `ANY_HIT_BIT_KHR` + `MISS_BIT_KHR`（RT shader 需要采样纹理）
- [x] Mesh 结构体新增 group_id（glTF source mesh index）+ material_id（primitive 固有材质）
- [x] CommandBuffer 新增 RT command wrappers：bind_rt_pipeline()、bind_rt_descriptor_sets()、push_rt_descriptor_set()（Step 7 ReferenceViewPass 录制需要）

## Step 5：Scene AS Builder + Renderer 集成

- [x] Context 补充加载 vkGetAccelerationStructureDeviceAddressKHR 函数指针（TLAS instance 构建需要获取 BLAS device address）
- [x] SceneLoader::load() 新增 rt_supported 参数，true 时 vertex/index buffer 额外加 ShaderDeviceAddress flag
- [x] SceneLoader::load_meshes() 填充 group_id 和 material_id
- [x] 新增 scene_as_builder.h：SceneASBuilder 类
- [x] SceneASBuilder::build()：按 group_id 分组构建 multi-geometry BLAS（根据材质 alpha_mode 设置 BLASGeometry::opaque）+ 按 (group_id, transform) 去重构建 TLAS + Geometry Info SSBO 构建（按 group 连续排列，customIndex = group base offset）。遍历 Mesh 时跳过 vertex_count == 0 或 index_count < 3 的 primitive（glTF 不保证所有 primitive 为有效三角形）
- [x] Renderer：场景加载后调用 SceneASBuilder::build() + 写入 Set 0 binding 4/5
- [x] bindings.glsl 新增 GeometryInfo struct（含 uint64_t，需 GL_EXT_shader_explicit_arithmetic_types_int64）+ Set 0 binding 4（accelerationStructureEXT）+ binding 5（GeometryInfoBuffer）
- [x] AppConfig 新增 `log_level` 字段（`std::string`，默认空）+ config.cpp 序列化/反序列化（JSON key `"log_level"`，字符串格式如 `"warn"`、`"info"`；空/缺失 = 默认 `warn`）
- [x] Application::init() 加载 config 后应用 log_level（非空时用 `spdlog::level::from_str()` 解析，空时保持默认 `warn`），替代硬编码 `kLogLevel` 作为唯一初始化来源
- [x] DebugUI log level 变更通知 Application：DebugUIActions 新增 `log_level_changed` 标志 + `new_log_level`，Application::update() 检测到变更后更新 `config_.log_level` 并调用 `save_config()`

## Step 6a：pt_common.glsl + C++ 基础设施

- [x] ShaderCompiler 扩展：支持 RT shader stage（raygen、closesthit、anyhit、miss），shaderc target vulkan_1_4
- [x] GlobalUniformData 新增 inv_view（mat4，offset 864），总大小 864→928 bytes + static_assert 更新
- [x] bindings.glsl GlobalUBO 新增 inv_view 字段
- [x] 新增 `app/include/himalaya/app/blue_noise_data.h`：128×128 R8Unorm blue noise 像素数据（`constexpr uint8_t[16384]`，从 `noise/HDR_L_0.png` 提取）+ Renderer 初始化时上传 GPU 注册到 bindless 数组
- [x] 新增 shaders/rt/pt_common.glsl：GLSL 扩展声明（GL_EXT_ray_tracing、GL_EXT_buffer_reference/2、GL_EXT_shader_explicit_arithmetic_types_int64、GL_EXT_nonuniform_qualifier）
- [x] pt_common.glsl：Ray Payload 定义（PrimaryPayload loc 0 56B 含 bounce 字段 + ShadowPayload loc 1）
- [x] pt_common.glsl：Vertex / Index buffer_reference layout 定义（匹配 Vertex 结构体 56B）
- [x] pt_common.glsl：顶点属性插值工具（GeometryInfo → buffer_reference 读取 → 重心坐标插值 position/normal/tangent/UV）
- [x] pt_common.glsl：Ray origin offset 工具函数（Wächter & Binder，Ray Tracing Gems Ch.6）
- [x] pt_common.glsl：Shading normal 一致性修正（clamp 到几何法线半球）
- [x] pt_common.glsl：Multi-lobe BRDF 选择（Fresnel 估计概率选 diffuse/specular lobe + PDF 补偿）
- [x] 新增 `app/include/himalaya/app/sobol_direction_data.h`：128 维 32-bit 方向数表（`constexpr uint32_t[4096]`，从 `noise/new-joe-kuo-6.21201` 生成）+ Renderer 初始化时上传 GPU SSBO
- [x] pt_common.glsl：Sobol SSBO 声明（Set 3 binding 3）+ sobol_sample() + pcg_hash() + rand_pt()（Cranley-Patterson rotation）
- [x] pt_common.glsl：cosine-weighted hemisphere sampling + GGX VNDF importance sampling（Heitz 2018，新增 `sample_ggx_vndf()` + `pdf_ggx_vndf()`；`#include "common/brdf.glsl"` 复用评估函数，不修改 brdf.glsl）
- [x] pt_common.glsl：Russian Roulette（bounce ≥ 2）+ MIS power heuristic（balance heuristic，Step 6a 定义函数，方向光不调用，Step 11 env sampling 使用）

## Step 6b：RT shader 文件

- [x] 新增 shaders/rt/reference_view.rgen：从 GlobalUBO inv_view/inv_projection 计算 primary ray（Sobol dims 0-1 subpixel jitter）+ 路径追踪主循环（设 payload.bounce）+ firefly clamping（bounce > 0 min(contribution, max_clamp)）+ accumulation 写入。Push constant 20B：max_bounces + sample_count + frame_seed + blue_noise_index + max_clamp
- [x] 新增 shaders/rt/closesthit.rchit：geometry_infos 索引 + 顶点插值（含 tangent）+ normal mapping（`#include "common/normal.glsl"`）+ shading normal 一致性 + emissive 贡献（所有 bounce）+ OIDN aux 输出（bounce 0 imageStore albedo + normal）+ NEE 方向光（ray origin offset）+ multi-lobe BRDF 采样，写入 PrimaryPayload
- [x] 新增 shaders/rt/miss.rmiss：IBL cubemap 环境采样，写入 PrimaryPayload（color = 环境辐射度，hit_distance = -1）
- [x] 新增 shaders/rt/shadow_miss.rmiss：写入 ShadowPayload（visible = 1）
- [x] 新增 shaders/rt/anyhit.rahit：alpha test（Mask: texel_alpha < cutoff → ignoreIntersectionEXT）+ stochastic alpha（Blend: PCG hash rand() >= alpha → ignoreIntersectionEXT）

## Step 7：Reference View Pass

- [x] RGStage 枚举新增 RayTracing + RG barrier 映射（与 Compute 逻辑一致，stage 换为 RAY_TRACING_SHADER_BIT_KHR）
- [x] 新增 reference_view_pass.h/.cpp：setup / record / destroy / rebuild_pipelines + FrameContext 新增 pt_accumulation / pt_aux_albedo / pt_aux_normal
- [x] Accumulation buffer（RGBA32F）+ OIDN 辅助 image（aux albedo R8G8B8A8Unorm + aux normal R16G16B16A16Sfloat）创建（Renderer 侧 managed images，Relative 1.0x，Storage | Sampled）
- [x] record()：从 FrameContext 获取 RGResourceId，RG pass 注册（accumulation ReadWrite + aux Write，均 RayTracing stage）+ push descriptors（Set 3 binding 0/1/2/3，4 个 binding 一起 push）+ trace_rays dispatch
- [x] Accumulation 逻辑：running average（mix(old, new, 1/(n+1))），sample_count=0 覆写
- [x] reset_accumulation() + sample_count() getter

## Step 8：独立渲染路径 + 模式切换

- [x] scene_data.h 新增 RenderMode 枚举（Rasterization、PathTracing）（uint8_t）
- [x] RenderInput 新增 render_mode 字段
- [x] Renderer::render() 拆分为私有方法：fill_common_gpu_data() + render_rasterization() + render_path_tracing()，按 render_mode switch。renderer.cpp 拆分为四个文件：renderer.cpp（dispatch + GPU data fill）、renderer_init.cpp（lifecycle）、renderer_rasterization.cpp（光栅化路径）、renderer_pt.cpp（PT 路径）
- [x] PT 路径：RG clear → accumulation + swapchain import → Reference View → Tonemapping → ImGui → present
- [x] VP 矩阵比较 + reset_accumulation() 触发
- [x] 模式切换不清零 accumulation（缓存保留）
- [x] DebugUI Rendering section 新增 Render Mode combo（提前自 Step 10），Application 新增 `render_mode_` 状态并传入 RenderInput
- [x] 修复：bindings.glsl `#ifdef HIMALAYA_RT` 块补 `GL_EXT_ray_tracing` + `GL_EXT_shader_explicit_arithmetic_types_int64` 扩展声明
- [x] 修复：shadow_miss.rmiss 补 `#define HIMALAYA_RT` + `#include "common/bindings.glsl"`（统一 RT shader include 顺序）
- [x] 修复：context.cpp RT 启用时补 `shaderInt64` 设备特性（`uint64_t` SPIR-V Int64 capability）
- [x] 修复：descriptors.cpp Set 1 bindless stage flags 补 `RAYGEN_BIT_KHR`
- [x] 修复：reference_view.rgen NDC Y 轴翻转（Vulkan Y-down）
- [x] 修复：sync-shaders.sh CRLF → LF（WSL bash 兼容）
- [x] DebugUI：PT checkbox 移至面板顶部（VSync 旁），IBL rotation 变化触发 accumulation reset
- [x] DebugUI：Camera/Scene/Environment/Shadow/AO/Contact Shadows 栏默认收起

## Step 9：OIDN 集成（异步降噪）

- [x] PT managed images 补 TransferSrc usage（accumulation + aux albedo + aux normal），readback 需要 VK_IMAGE_USAGE_TRANSFER_SRC_BIT
- [x] Aux albedo 格式升级：R8G8B8A8Unorm → R16G16B16A16Sfloat（配合 OIDN HALF3 pixelByteStride=8 零 CPU 格式转换）。修改 renderer_init.cpp managed image format + closesthit.rchit image layout qualifier（rgba8 → rgba16f）
- [x] 手动集成 OIDN 预编译库（官方 release，多厂商 GPU 支持：CUDA/HIP/SYCL + CPU fallback），修改 framework/CMakeLists.txt 链接库 + DLL 拷贝
- [x] OIDN 运行时 DLL 拷贝到可执行文件目录（app/CMakeLists.txt POST_BUILD，已配置 glob 全部 DLL）
- [x] 新增 denoiser.h/.cpp：DenoiseState 枚举（Idle / ReadbackPending / Processing / UploadPending）+ Denoiser 类
- [x] Denoiser::init()：OIDN device 创建（GPU 优先 fallback CPU）+ RT filter（hdr, cleanAux, High）+ 持久 staging buffers（readback beauty/albedo/normal + upload）+ timeline semaphore 创建
- [x] Denoiser::request_denoise(generation)：记录 trigger_generation，状态 → ReadbackPending
- [x] Denoiser::launch_processing()：启动 std::jthread 后台线程（vkWaitSemaphores → OIDN write → execute+sync → error check → OIDN read → 状态 → UploadPending），状态 → Processing
- [x] Denoiser::poll_upload_ready(current_generation)：UploadPending + generation 匹配 → true；不匹配 → 丢弃，状态 → Idle
- [x] Denoiser::complete_upload()：状态 → Idle
- [x] Denoiser::on_resize()：join 线程 + 强制 state→Idle + 重建 staging buffers + OIDN buffers
- [x] Denoiser::destroy()：join 线程 + 强制 state→Idle + 释放所有资源（OIDN device/filter、staging buffers、timeline semaphore）
- [x] Denoiser::abort()：join 线程 + 强制 state→Idle（场景加载前调用）
- [x] 后台线程 oidnExecuteFilter 失败处理：spdlog::error + state→Idle（跳过 upload）
- [x] Renderer 新增 denoised buffer（RGBA32F managed image，TransferDst | Sampled）+ Denoiser 实例 + init/destroy/on_resize 集成
- [x] Renderer 新增 accumulation_generation_（uint32_t，accumulation 重置时 +1）+ denoised_generation_（初始 UINT32_MAX）
- [x] 降噪状态管理（Renderer 侧）：denoise_enabled / auto_denoise / interval / last_denoised_sample_count（触发时值）/ show_denoised
- [x] 降噪触发守卫：state==Idle && denoise_enabled && show_denoised && sample_count>0 && (自动间隔 || 手动请求)
- [x] render_path_tracing() RG 编排：ReadbackPending → 注册 Readback Copy Pass + signal timeline semaphore + launch_processing()
- [x] render_path_tracing() RG 编排：poll_upload_ready() → 注册 Upload Pass + complete_upload() + 更新 denoised_generation_
- [x] Tonemapping 输入切换：show_denoised && denoise_enabled && denoised_generation==accumulation_generation → denoised buffer，否则 accumulation
- [x] Renderer::pending_denoise_signal() getter + Application submit 前追加到 signalSemaphoreInfos
- [x] accumulation 重置触发点补充 generation++：相机移动、IBL 旋转、resize、环境重载（max_bounces/firefly_clamp 变更待 Step 10 DebugUI）
- [x] 场景加载前调用 renderer.abort_denoise()（join + Idle + reset accumulation + generation++）
- [x] ~~Renderer::pending_denoise_signal()~~ （已合并到 RG 编排条目）

## Step 10：ImGui PT 面板

- [x] DebugUIContext 新增 PT 字段（render_mode、rt_supported、sample_count、target_samples、max_bounces、max_clamp、elapsed_time、denoise_state、denoise 控件）
- [x] DebugUIActions 新增 pt_reset_requested + pt_denoise_requested
- [x] Rendering section 新增渲染模式 combo（仅 rt_supported 时显示 PT 选项）（Step 8 已提前实现）
- [x] Path Tracing collapsing header：状态信息 + Max Bounces + Firefly Clamp slider（0=关闭，默认 10.0）+ Target Samples + Reset 按钮
- [x] OIDN collapsing header：Denoise 开关 + Show Denoised/Raw 切换（Show Raw 暂停降噪）+ Auto Denoise + Interval（最小值 16，默认 64）+ Denoise Now 按钮（`!denoise_enabled || state!=Idle || sample_count==0 || !show_denoised` 时灰掉）+ 上次降噪采样数 + 降噪状态文字（Idle/Denoising...）
- [x] Application 响应 PT actions

## Step 10.5：Shader 磁盘缓存（SPIR-V 持久化）

- [x] `rhi/shader.h` 重构：添加 `virtual ~ShaderCompiler() = default;`，`compile_from_file()` 加 `virtual`
- [x] `rhi/shader.h` 重构：`CacheEntry` 结构体和 `compile()` 从 private 移到 protected
- [x] `rhi/shader.h` 重构：新增 protected 访问器 `include_path()` + `find_cache_entry(source, stage)`
- [x] `rhi/shader.cpp`：实现 `find_cache_entry()`（通过 `make_cache_key` 查 `cache_` map）
- [x] 新增 `framework/cached_shader_compiler.h`：`CachedShaderCompiler : public rhi::ShaderCompiler`，`set_cache_category()` + override `compile_from_file()`
- [x] 新增 `framework/cached_shader_compiler.cpp`：磁盘缓存加载（读 .meta JSON 验证 include hash + 读 .spv binary）
- [x] `framework/cached_shader_compiler.cpp`：磁盘缓存写入（编译后通过 `find_cache_entry()` 取 include 列表，写 .spv + .meta）
- [x] `framework/CMakeLists.txt`：添加 `cached_shader_compiler.cpp` 源文件（需用户确认构建配置）
- [x] `app/renderer.h`：成员类型 `rhi::ShaderCompiler` → `framework::CachedShaderCompiler`
- [x] `app/renderer_init.cpp`：`set_include_path()` 后添加 `set_cache_category()`（Debug: `"shader_debug"`，Release: `"shader_release"`）

## Step 11：Environment Map Importance Sampling

- [x] `load_equirect()` 重构：`stbi_image_free` 移至 `init()` 调用侧，返回 raw `float*` 数据供 alias table 构建
- [x] IBL 新增 `build_env_alias_table(float* rgb_data, int w, int h)`：半分辨率下采样（源宽高各除 2）luminance×sin(theta) + Vose's algorithm 构建 alias table（CPU O(N)），上传 SSBO（GPU_ONLY + TransferDst），计算 total_luminance
- [x] IBL alias table 二进制缓存（key = `hdr_hash + "_alias_table"`，独立于 cubemap 缓存）：缓存命中直接加载 SSBO；cubemap 缓存命中但 alias table 未缓存时单独 `stbi_loadf` 构建
- [x] IBL 新增 getter：alias_table_buffer()、total_luminance()、alias_table_width()、alias_table_height()
- [x] IBL fallback 时跳过 alias table 构建（无 HDR 环境）
- [x] DescriptorManager：Set 0 layout 条件新增 binding 6（SSBO，`PARTIALLY_BOUND`，RT stages，`rt_supported` 守卫）+ descriptor pool 容量扩展
- [x] DescriptorManager 新增 `write_set0_env_alias_table(BufferHandle, uint64_t size)`
- [x] Renderer：IBL init 后调用 write_set0_env_alias_table() 写入 binding 6
- [x] bindings.glsl `#ifdef HIMALAYA_RT` 新增 `EnvAliasEntry` struct + `EnvAliasTable` buffer（binding 6）
- [x] pt_common.glsl 新增 `sample_env_alias_table()` 函数（2 rand → pixel index → equirect UV → 方向 → IBL rotation）
- [x] pt_common.glsl 新增 `env_pdf()` 函数（方向 → IBL cubemap luminance → PDF）
- [x] closesthit.rchit + reference_view.rgen 调用 `mis_power_heuristic()`（Step 6 已定义）计算 env MIS 权重
- [x] PrimaryPayload 新增 `float env_mis_weight` 字段（56B → 60B）
- [x] closesthit.rchit 新增 NEE 环境光：alias table 采样 → shadow ray → MIS 加权贡献
- [x] closesthit.rchit BRDF 采样后预计算 env_mis_weight 写入 PrimaryPayload
- [x] reference_view.rgen：miss 返回 env color × payload.env_mis_weight

## Step 12a：EmissiveLightBuilder + 基础设施

- [x] GPUMaterialData `_padding`（offset 76）→ `uint double_sided`，SceneLoader 填充 doubleSided
- [x] closesthit.rchit：single-sided 材质背面透传（命中背面且 `double_sided == 0` 时消耗一个 bounce 穿过表面：`color = 0`，`throughput_update = vec3(1.0)`，`hit_distance = gl_HitTEXT`，`next_origin/next_direction` 沿原 ray 方向继续）
- [ ] max_bounces 默认值从 8 调至 16（配合背面透传消耗 bounce 的方案）
- [ ] 新增 emissive_light_builder.h/.cpp：EmissiveLightBuilder 类（build / destroy / getters）
- [ ] build()：遍历 mesh primitive 识别 emissive（`any(emissive_factor > 0)`），收集世界空间顶点/UV/面积/emission，计算 power = luminance(emissive_factor) × area
- [ ] Power-weighted alias table 构建（Vose's algorithm，复用 Step 11 逻辑）
- [ ] EmissiveTriangleBuffer SSBO 上传（96B/entry：v0/v1/v2 + emission + area + material_index + uv0/uv1/uv2）
- [ ] EmissiveAliasTable SSBO 上传（header + `{float prob, uint alias}` 8B/entry）
- [ ] 无 emissive 场景跳过构建（emissive_count() 返回 0）
- [ ] DescriptorManager：Set 0 binding 7（EmissiveTriangleBuffer）+ binding 8（EmissiveAliasTable），`PARTIALLY_BOUND`，`rt_supported` 守卫
- [ ] Renderer：场景加载后调用 EmissiveLightBuilder::build() + 写入 Set 0 binding 7/8

## Step 12b：Shader NEE Emissive + MIS

- [ ] bindings.glsl `#ifdef HIMALAYA_RT` 新增 `EmissiveTriangle` struct + binding 7/8 声明
- [ ] Push constant 新增 `uint emissive_light_count`（20B → 24B，0 = 跳过 NEE emissive）
- [ ] pt_common.glsl 新增三角形均匀采样（重心坐标）+ emissive alias table 采样 + light PDF 计算
- [ ] closesthit.rchit 新增 NEE emissive：alias table 采样 → 三角形采样点 → shadow ray（tMax = `distance * (1-1e-4)`）→ MIS 加权。Emissive 双面跟随 double_sided
- [ ] closesthit.rchit BRDF 采样后写入 `payload.last_brdf_pdf`
- [ ] closesthit.rchit 命中 emissive 表面时（bounce > 0）：读 last_brdf_pdf + 算 light_pdf → MIS 权重。Bounce 0 直视权重 1.0
- [ ] PrimaryPayload 新增 `float last_brdf_pdf` 字段（60B → 64B）

## Step 13：Texture LOD（Ray Cones）

- [ ] pt_common.glsl 新增 Ray Cone 工具函数（init_cone、propagate_cone、compute_lod）
- [ ] pt_common.glsl 新增 `compute_texel_density()`：从三角形顶点位置 + UV 运行时算 world/UV 面积比
- [ ] reference_view.rgen：初始化 cone spread（`atan(2 × tan(fov/2) / screen_height)`）+ 循环内设 `payload.cone_spread`
- [ ] closesthit.rchit：propagate cone + compute LOD + 所有材质纹理 `texture()` → `textureLod(tex, uv, lod + lod_bias)`（~5-6 处）
- [ ] anyhit.rahit：alpha 纹理 `texture()` → `textureLod()`（~1 处）
- [ ] PrimaryPayload 新增 `float cone_spread` 字段（64B → 68B）
- [ ] Push constant 新增 `float lod_bias`（24B → 28B，默认 0.0）
- [ ] Step 10 ImGui 面板新增 LOD Bias slider
