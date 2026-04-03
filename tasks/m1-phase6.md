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
- [ ] CommandBuffer 新增 RT command wrappers：bind_rt_pipeline()、bind_rt_descriptor_sets()、push_rt_descriptor_set()（Step 7 ReferenceViewPass 录制需要）

## Step 5：Scene AS Builder + Renderer 集成

- [ ] Context 补充加载 vkGetAccelerationStructureDeviceAddressKHR 函数指针（TLAS instance 构建需要获取 BLAS device address）
- [ ] SceneLoader::load() 新增 rt_supported 参数，true 时 vertex/index buffer 额外加 ShaderDeviceAddress flag
- [ ] SceneLoader::load_meshes() 填充 group_id 和 material_id
- [ ] 新增 scene_as_builder.h：SceneASBuilder 类
- [ ] SceneASBuilder::build()：按 group_id 分组构建 multi-geometry BLAS（根据材质 alpha_mode 设置 BLASGeometry::opaque）+ 按 (group_id, transform) 去重构建 TLAS + Geometry Info SSBO 构建（按 group 连续排列，customIndex = group base offset）。遍历 Mesh 时跳过 vertex_count == 0 或 index_count < 3 的 primitive（glTF 不保证所有 primitive 为有效三角形）
- [ ] Renderer：场景加载后调用 SceneASBuilder::build() + 写入 Set 0 binding 4/5
- [ ] bindings.glsl 新增 GeometryInfo struct（含 uint64_t，需 GL_EXT_shader_explicit_arithmetic_types_int64）+ Set 0 binding 4（accelerationStructureEXT）+ binding 5（GeometryInfoBuffer）

## Step 6：PT 核心 shader

- [ ] 新增 shaders/rt/pt_common.glsl：GLSL 扩展声明（GL_EXT_ray_tracing、GL_EXT_buffer_reference/2、GL_EXT_shader_explicit_arithmetic_types_int64、GL_EXT_nonuniform_qualifier）
- [ ] pt_common.glsl：Ray Payload 定义（PrimaryPayload loc 0 + ShadowPayload loc 1）
- [ ] pt_common.glsl：Vertex / Index buffer_reference layout 定义（匹配 Vertex 结构体 56B）
- [ ] pt_common.glsl：顶点属性插值工具（GeometryInfo → buffer_reference 读取 → 重心坐标插值 position/normal/UV）
- [ ] pt_common.glsl：Sobol 低差异序列（预计算方向数表嵌入常量数组）+ Cranley-Patterson rotation
- [ ] pt_common.glsl：cosine-weighted hemisphere sampling + GGX importance sampling（复用 common/brdf.glsl）
- [ ] pt_common.glsl：Russian Roulette（bounce ≥ 2）+ MIS power heuristic（balance heuristic）
- [ ] 嵌入预生成 128×128 R8Unorm blue noise 纹理（公开数据集）+ Renderer 初始化时上传 GPU 注册到 bindless 数组
- [ ] GlobalUniformData 新增 inv_view（mat4，offset 864），总大小 864→928 bytes + static_assert 更新
- [ ] bindings.glsl GlobalUBO 新增 inv_view 字段
- [ ] 新增 shaders/rt/reference_view.rgen：从 GlobalUBO inv_view/inv_projection 计算 primary ray + 路径追踪主循环 + accumulation 写入。Push constant 12B：max_bounces + sample_count + frame_seed
- [ ] 新增 shaders/rt/closesthit.rchit：geometry_infos 索引 + 顶点插值 + 材质采样 + NEE + MIS + BRDF 采样，写入 PrimaryPayload
- [ ] 新增 shaders/rt/miss.rmiss：IBL cubemap 环境采样，写入 PrimaryPayload（color = 环境辐射度，hit_distance = -1）
- [ ] 新增 shaders/rt/shadow_miss.rmiss：写入 ShadowPayload（visible = 1）
- [ ] 新增 shaders/rt/anyhit.rahit：alpha test（Mask: texel_alpha < cutoff → ignoreIntersectionEXT）+ stochastic alpha（Blend: PCG hash rand() >= alpha → ignoreIntersectionEXT）
- [ ] ShaderCompiler 扩展：支持 RT shader stage（raygen、closesthit、anyhit、miss），shaderc target vulkan_1_4

## Step 7：Reference View Pass

- [ ] RGStage 枚举新增 RayTracing + RG barrier 映射（VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR）
- [ ] 新增 reference_view_pass.h/.cpp：setup / record / destroy / rebuild_pipelines / on_resize
- [ ] Accumulation buffer 创建（RGBA32F，Relative 1.0x，Storage）
- [ ] record()：RG pass 注册 + push descriptors + trace_rays dispatch
- [ ] Accumulation 逻辑：running average（mix(old, new, 1/(n+1))），sample_count=0 覆写
- [ ] reset_accumulation() + sample_count() getter + accumulation_image() getter

## Step 8：独立渲染路径 + 模式切换

- [ ] scene_data.h 新增 RenderMode 枚举（Rasterization、PathTracing）（uint8_t）
- [ ] RenderInput 新增 render_mode 字段
- [ ] Renderer::render() 拆分为私有方法：fill_common_gpu_data() + render_rasterization() + render_path_tracing()，按 render_mode switch
- [ ] PT 路径：RG clear → accumulation + swapchain import → Reference View → Tonemapping → ImGui → present
- [ ] VP 矩阵比较 + reset_accumulation() 触发
- [ ] 模式切换不清零 accumulation（缓存保留）

## Step 9：OIDN 集成

- [ ] 手动集成 OIDN 预编译库（官方 release，含 CUDA GPU 支持），修改 framework/CMakeLists.txt 链接库 + DLL 拷贝
- [ ] 新增 denoiser.h/.cpp：Denoiser 类（init / denoise / destroy / on_resize）
- [ ] OIDN device 创建（GPU 优先 fallback CPU）+ RT filter 创建
- [ ] 持久 staging buffer 分配（readback + upload）
- [ ] denoise()：Vulkan readback → OIDN filter execute → Vulkan upload
- [ ] Renderer 新增 denoised buffer（RGBA32F managed image）
- [ ] 降噪状态管理：denoise_enabled / auto_denoise / interval / last_denoised_sample_count / show_denoised
- [ ] 自动触发逻辑（每 N 采样）+ 手动触发（DebugUIActions）
- [ ] Tonemapping 输入切换（denoised buffer 或 accumulation buffer）

## Step 10：ImGui PT 面板

- [ ] DebugUIContext 新增 PT 字段（render_mode、rt_supported、sample_count、target_samples、max_bounces、elapsed_time、denoise 控件）
- [ ] DebugUIActions 新增 pt_reset_requested + pt_denoise_requested
- [ ] Rendering section 新增渲染模式 combo（仅 rt_supported 时显示 PT 选项）
- [ ] Path Tracing collapsing header：状态信息 + Max Bounces + Target Samples + Reset 按钮
- [ ] OIDN collapsing header：Denoise 开关 + Show Denoised/Raw 切换 + Auto Denoise + Interval + Denoise Now 按钮 + 上次降噪采样数
- [ ] Application 响应 PT actions
