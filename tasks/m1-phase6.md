# M1 阶段六：RT 基础设施 + PT 参考视图 — 任务清单

> 详细设计和验证标准见 `docs/current-phase.md`。

---

## Step 1：RT 扩展检测与设备选择

- [ ] 新增 RT 扩展列表（acceleration_structure、ray_tracing_pipeline、ray_query、deferred_host_operations、spirv_1_4、shader_float_controls）
- [ ] pick_physical_device() RT 支持作为加分项评分
- [ ] Context 新增 `rt_supported` 公有字段
- [ ] 日志输出选中设备 RT 支持状态

## Step 2：RT 扩展启用与特性激活

- [ ] rt_supported 时 RT 扩展加入设备扩展列表
- [ ] 启用设备特性：accelerationStructure、rayTracingPipeline、rayQuery、bufferDeviceAddress
- [ ] 查询并存储 RT pipeline 属性（shaderGroupHandleSize、shaderGroupBaseAlignment、maxRayRecursionDepth 等）
- [ ] Context 新增 RT 属性存储字段

## Step 3：AS 资源抽象

- [ ] 新增 acceleration_structure.h：BLASHandle、TLASHandle、BLASBuildInfo 类型
- [ ] AccelerationStructureManager：build_blas() 批量构建（PREFER_FAST_TRACE）
- [ ] AccelerationStructureManager：build_tlas()
- [ ] AccelerationStructureManager：destroy_blas()、destroy_tlas()
- [ ] Scratch buffer 管理（构建完成后释放）

## Step 4：RT Pipeline + SBT + trace_rays

- [ ] 新增 rt_pipeline.h：RTPipelineDesc、RTPipeline 类型
- [ ] create_rt_pipeline()：shader group 创建 + vkCreateRayTracingPipelinesKHR
- [ ] SBT 构建（vkGetRayTracingShaderGroupHandlesKHR → 对齐写入 SBT buffer）
- [ ] RTPipeline::destroy()
- [ ] CommandBuffer 新增 trace_rays(const RTPipeline&, width, height)

## Step 5：Scene AS Builder + Set 0 扩展

- [ ] 新增 scene_as_builder.h：SceneASBuilder 类
- [ ] SceneASBuilder::build()：per-mesh BLAS 构建 + TLAS 构建 + Geometry Info SSBO 构建
- [ ] BufferUsage 新增 ShaderDeviceAddress + ResourceManager 映射
- [ ] ResourceManager 新增 get_buffer_device_address()
- [ ] DescriptorManager::init() 从 context_->rt_supported 读取 RT 状态，Set 0 layout 条件扩展 binding 4/5
- [ ] DescriptorManager 新增 write_set0_tlas()
- [ ] Renderer：场景加载后调用 SceneASBuilder::build() + 写入 Set 0 binding 4/5
- [ ] bindings.glsl 新增 Set 0 binding 4（accelerationStructureEXT）+ binding 5（GeometryInfoBuffer）

## Step 6：PT 核心 shader

- [ ] 新增 shaders/rt/pt_common.glsl：Sobol + Cranley-Patterson + blue noise 纹理采样 + hemisphere sampling + GGX sampling + Russian Roulette + MIS + 顶点插值
- [ ] 嵌入预生成 128×128 blue noise 纹理 + Renderer 初始化时注册到 bindless 数组
- [ ] 新增 shaders/rt/reference_view.rgen：primary ray 计算 + 路径追踪主循环 + accumulation 写入
- [ ] 新增 shaders/rt/closesthit.rchit：顶点插值 + 材质采样 + NEE + MIS + BRDF 采样
- [ ] 新增 shaders/rt/miss.rmiss：IBL cubemap 环境采样
- [ ] 新增 shaders/rt/shadow_miss.rmiss：shadow ray miss（标记未遮挡）
- [ ] ShaderCompiler 扩展：支持 RT shader stage（raygen、closesthit、miss）

## Step 7：Reference View Pass

- [ ] RGStage 枚举新增 RayTracing + RG barrier 映射（VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR）
- [ ] 新增 reference_view_pass.h/.cpp：setup / record / destroy / rebuild_pipelines / on_resize
- [ ] Accumulation buffer 创建（RGBA32F，Relative 1.0x，Storage）
- [ ] record()：RG pass 注册 + push descriptors + trace_rays dispatch
- [ ] Accumulation 逻辑：running average（mix(old, new, 1/(n+1))），sample_count=0 覆写
- [ ] reset_accumulation() + sample_count() getter + accumulation_image() getter

## Step 8：独立渲染路径 + 模式切换

- [ ] scene_data.h 新增 RenderMode 枚举（Rasterization、PathTracing）
- [ ] RenderInput 新增 render_mode 字段
- [ ] Renderer::render() 根据 render_mode 分叉（光栅化 / PT 路径）
- [ ] PT 路径：RG clear → accumulation + swapchain import → Reference View → Tonemapping → ImGui → present
- [ ] VP 矩阵比较 + reset_accumulation() 触发
- [ ] 模式切换不清零 accumulation（缓存保留）

## Step 9：OIDN 集成

- [ ] 手动集成 OIDN 预编译库（官方 release，含 CUDA GPU 支持）
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
