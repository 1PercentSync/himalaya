# 当前阶段：M1 阶段六 — RT 基础设施 + PT 参考视图

> 目标：引入 Vulkan RT 扩展和加速结构基础设施，实现路径追踪参考视图（accumulation + OIDN viewport denoising）。
> 验证整个 RT 技术栈端到端可用，为阶段七烘焙器和 M2 实时 PT 奠定基础。
>
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: RHI 层 RT 扩展（设备选择、扩展启用、特性查询）
    ↓
Step 2: RHI 层 AS 抽象（BLAS/TLAS 创建、构建、销毁）
    ↓
Step 3: RHI 层 RT Pipeline 抽象（SBT、trace_rays 命令）
    ↓
Step 4: RHI/Framework 层 RT 基础设施（BufferUsage、Descriptor 扩展、GPUGeometryInfo）
    ↓
Step 5: Scene AS Builder + Renderer 集成（SceneLoader 变更 + AS 构建 + bindings.glsl）
    ↓
Step 6: PT 核心 shader（raygen/miss/closesthit，共享 pt_common.glsl）
    ↓
Step 7: Reference View Pass + accumulation buffer
    ↓
Step 8: 独立渲染路径 + 模式切换
    ↓
Step 9: OIDN 集成
    ↓
Step 10: ImGui PT 面板 + 调参
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | RT 扩展检测 + 启用 + 特性激活 | 日志输出 RT 支持状态，无 validation 报错，RT 属性成功查询 |
| 2 | AS 资源抽象 | 编译通过，无调用方（纯 API 声明 + 实现） |
| 3 | RT Pipeline + SBT + trace_rays | 编译通过，无调用方 |
| 4 | RHI/Framework RT 基础设施 | 编译通过，新增类型和 API 可用但无调用方 |
| 5 | Scene AS Builder + Renderer 集成 | 场景加载后日志输出 BLAS/TLAS 构建信息，无 validation 报错 |
| 6 | PT 核心 shader | shader 编译通过（shaderc RT target），无运行时调用 |
| 7 | Reference View Pass | RenderDoc: accumulation buffer 逐帧亮度增加，静止时收敛 |
| 8 | 独立渲染路径 + 模式切换 | ImGui 切换光栅化 ↔ PT，两种模式正确显示，切换后缓存有效 |
| 9 | OIDN 集成 | 降噪后画面明显干净，自动/手动触发均工作 |
| 10 | ImGui PT 面板 | 所有控件功能正常，参数调整实时生效 |

---

### Step 1：RT 扩展检测 + 启用 + 特性激活

设备选择逻辑变更 + 条件启用 RT 扩展和设备特性。

- `context.cpp`：新增 RT 所需扩展列表（`VK_KHR_acceleration_structure`、`VK_KHR_ray_tracing_pipeline`、`VK_KHR_ray_query`、`VK_KHR_deferred_host_operations`）
- `pick_physical_device()`：对每个候选设备检测 RT 扩展可用性，RT 支持设备在评分中获得额外权重
- `Context` 新增 `bool rt_supported` 公有字段，设备选择完成后根据最终选中设备的 RT 能力设置
- 日志输出：选中设备名 + RT 支持状态
- `rt_supported = true` 时：将 RT 扩展加入设备扩展列表
- 启用设备特性：`accelerationStructure`、`rayTracingPipeline`、`rayQuery`、`bufferDeviceAddress`（Vulkan 1.2 核心，AS 构建需要）
- `rt_supported = true` 时：VMA allocator 添加 `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` flag（VMA 在分配内存时需要 `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`，否则 `vkGetBufferDeviceAddress` 返回无效地址）
- 查询并存储 RT 相关属性：
  - `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`：`shaderGroupHandleSize`、`shaderGroupBaseAlignment`、`shaderGroupHandleAlignment`、`maxRayRecursionDepth`
  - `VkPhysicalDeviceAccelerationStructurePropertiesKHR`：`minAccelerationStructureScratchOffsetAlignment`
- Context 新增存储字段
- `rt_supported = false` 时：跳过所有 RT 相关初始化

**验证**：有 RT 硬件时日志显示 `rt_supported = true`，无 validation 报错，RT 属性成功查询；无 RT 时 `rt_supported = false`，现有渲染不变

#### 设计要点

`bufferDeviceAddress` 是 Vulkan 1.2 核心特性，但需要显式启用。AS 构建需要 buffer device address 指定顶点/索引数据位置。`VK_KHR_spirv_1_4` 和 `VK_KHR_shader_float_controls` 是 RT 扩展的依赖，但已提升到 Vulkan 1.2 核心，项目目标 1.4 无需显式列出。

---

### Step 2：AS 资源抽象

RHI 层新增加速结构管理。

- 新增 `rhi/acceleration_structure.h`：
  - `BLASHandle`：BLAS 句柄（VkAccelerationStructureKHR + backing VkBuffer + VmaAllocation）
  - `TLASHandle`：TLAS 句柄（同上）
  - `BLASGeometry`：单个 geometry 的构建输入（vertex buffer address、index buffer address、vertex count、index count、vertex stride、opaque flag）
  - `BLASBuildInfo`：BLAS 构建输入，包含 `std::span<const BLASGeometry> geometries`（1..N geometries per BLAS，支持 multi-geometry BLAS）
  - `AccelerationStructureManager` 类：
    - `build_blas(std::span<const BLASBuildInfo> infos)` → `std::vector<BLASHandle>`：批量构建 BLAS（单次 `vkCmdBuildAccelerationStructuresKHR` 调用并行构建全部，PREFER_FAST_TRACE），每个 BLASBuildInfo 生成一个 BLAS
    - `build_tlas(std::span<const VkAccelerationStructureInstanceKHR> instances)` → `TLASHandle`：构建 TLAS
    - `destroy_blas(BLASHandle)`、`destroy_tlas(TLASHandle)`
    - 内部管理 scratch buffer（构建完成后释放）
- 新增 `rhi/acceleration_structure.cpp`：实现 AS 创建（`vkCreateAccelerationStructureKHR`）、构建（`vkCmdBuildAccelerationStructuresKHR`）、destroy

**验证**：编译通过，无调用方

#### 设计要点

BLAS 和 TLAS 的 backing buffer 需要 `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR` + `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`。Scratch buffer 需要 `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` + `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`。构建在 immediate command scope 中执行（场景加载时一次性构建）。

**Scratch buffer 策略**：单次 `vkCmdBuildAccelerationStructuresKHR` 调用传入全部 BLASBuildInfo，GPU 可并行构建。分配一个大 scratch buffer = 所有 BLAS scratch size 之和（每段对齐到 `minAccelerationStructureScratchOffsetAlignment`），每个 BLAS 使用 scratch 内的独立区域。

**顶点格式硬编码**：`VkAccelerationStructureGeometryTrianglesDataKHR` 的 `vertexFormat` 固定为 `VK_FORMAT_R32G32B32_SFLOAT`（Vertex::position 在 offset 0），`indexType` 固定为 `VK_INDEX_TYPE_UINT32`。项目使用统一顶点格式，无需参数化。

**Per-geometry opacity 标记**：`build_blas()` 根据 `BLASGeometry::opaque` 设置 geometry flag——`true` → `VK_GEOMETRY_OPAQUE_BIT_KHR`（硬件跳过 any-hit），`false` → `VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`（硬件调用 any-hit）。`build_tlas()` 的 TLAS instance geometry flags 设为 0，不覆盖 BLAS per-geometry 设置。详见 `m1-rt-decisions.md`「Per-geometry opacity 标记」和「Non-opaque 几何体处理」。

---

### Step 3：RT Pipeline + SBT + trace_rays

RHI 层新增 RT Pipeline 创建和命令录制。

- 新增 `rhi/rt_pipeline.h`：
  - `RTPipelineDesc`：RT pipeline 描述（raygen/miss/closesthit/anyhit shader modules、max recursion depth、descriptor set layouts、push constant ranges）
  - `RTPipeline`：持有 VkPipeline + VkPipelineLayout + SBT buffer（raygen/miss/hit regions）
  - `create_rt_pipeline(const Context&, const RTPipelineDesc&)` → `RTPipeline`
  - `RTPipeline::destroy(VkDevice)`
- `rt_pipeline.cpp`：实现 shader group 创建、`vkCreateRayTracingPipelinesKHR`、SBT 构建（`vkGetRayTracingShaderGroupHandlesKHR` → 对齐写入 SBT buffer）
- `commands.h` 新增 `trace_rays(const RTPipeline&, uint32_t width, uint32_t height)` 方法
- `commands.cpp`：实现 `vkCmdTraceRaysKHR`（从 RTPipeline 读取 SBT region 信息）

**验证**：编译通过，无调用方

#### 设计要点

SBT layout：raygen region（1 entry）+ miss region（2 entries：environment miss + shadow miss）+ hit region（1 entry：closest-hit + any-hit）。每个 entry 大小对齐到 `shaderGroupHandleAlignment`，region 起始对齐到 `shaderGroupBaseAlignment`（Step 1 查询的属性）。`maxRayRecursionDepth` 设为 1（不用递归，closest-hit 中 shadow ray 用 `traceRayEXT` 但 miss 不再发射射线）。

RT Pipeline layout 包含完整的 `{Set 0, Set 1, Set 2, Set 3(push)}`，与 graphics/compute pipeline 一致。PT shader 不引用 Set 2，但保持统一 layout 简化绑定路径。

---

### Step 4：RHI/Framework RT 基础设施

RT 所需的底层类型扩展和 Descriptor 扩展，为 Step 5 的 SceneASBuilder 集成做准备。

- `BufferUsage` 枚举新增 `ShaderDeviceAddress`，`ResourceManager` 在 buffer 创建时映射到 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- `ResourceManager` 新增 `get_buffer_device_address(BufferHandle)` 方法
- GPU 端新增 `GPUGeometryInfo` 结构体（std430，24 bytes）：
  - `uint64_t vertex_buffer_address`（offset 0）
  - `uint64_t index_buffer_address`（offset 8）
  - `uint32_t material_buffer_offset`（offset 16，MaterialBuffer SSBO 索引）
  - `uint32_t _padding`（offset 20）
- `DescriptorManager` 扩展：
  - `init()` 内部从 `context_->rt_supported` 读取 RT 状态（不加额外参数）
  - `rt_supported = true` 时 Set 0 layout 新增 binding 4（`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`）+ binding 5（SSBO，GeometryInfoBuffer），descriptor pool 容量相应扩展（新增 AS + SSBO 描述符，per frame in flight）
  - 新增 `write_set0_tlas(TLASHandle)` + `write_set0_buffer` 复用写 binding 5
  - `get_compute_set_layouts()` 重命名为 `get_dispatch_set_layouts()`（compute 和 RT pipeline 共用）
  - Set 1（bindless textures）layout binding stage flags 添加 `VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR`（RT shader 需要采样 bindless 纹理数组）
- `Mesh` 结构体新增 `group_id`（glTF source mesh index）+ `material_id`（primitive 固有材质，material_instances 索引）

**验证**：编译通过，新增类型和 API 可用但无调用方

---

### Step 5：Scene AS Builder + Renderer 集成

SceneLoader 变更 + SceneASBuilder 实现 + Renderer 集成 + bindings.glsl 扩展。

- `SceneLoader::load()` 新增 `bool rt_supported` 参数：`true` 时 vertex/index buffer 创建额外加 `ShaderDeviceAddress` flag
- `SceneLoader::load_meshes()` 填充 `group_id` 和 `material_id`
- 新增 `framework/scene_as_builder.h`：
  - `SceneASBuilder` 类：
    - `build(Context&, ResourceManager&, AccelerationStructureManager&, std::span<const Mesh>, std::span<const MeshInstance>, std::span<const MaterialInstance>)`：
      1. 按 `Mesh::group_id` 分组 → 每组收集 BLASGeometry 列表（根据 `Mesh::material_id` → `MaterialInstance::alpha_mode` 设置 `BLASGeometry::opaque`）→ 组装 BLASBuildInfo（multi-geometry）
      2. 调用 `AccelerationStructureManager::build_blas()` 批量构建（每 group 一个 BLAS）
      3. 构建 Geometry Info SSBO（按 group 连续排列，per-geometry：vertex/index buffer address、material buffer_offset）
      4. 按 `(group_id, transform)` 去重 mesh_instances → 组装 `VkAccelerationStructureInstanceKHR` 数组（customIndex = 该 group 在 Geometry Info 中的 base offset）
      5. 调用 `AccelerationStructureManager::build_tlas()`
    - `destroy()`：释放 BLAS、TLAS、Geometry Info buffer
    - `tlas_handle()` / `geometry_info_buffer()` getter
- Renderer：场景加载后调用 `SceneASBuilder::build()`，写入 Set 0 binding 4/5
- `bindings.glsl` 新增 `GeometryInfo` struct + Set 0 binding 4（`accelerationStructureEXT`）+ binding 5（`GeometryInfoBuffer`）

**验证**：场景加载后日志输出 BLAS 数量（= unique group 数）+ TLAS instance 数量（= unique node 数）+ Geometry Info buffer 大小，无 validation 报错

#### 设计要点

`VkAccelerationStructureInstanceKHR::instanceCustomIndex` 设为该 BLAS 对应 group 在 Geometry Info buffer 中的 base offset（24 位，足够）。Closest-hit shader 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 vertex/index buffer address 和 material buffer_offset。变换矩阵通过 TLAS 内置 `gl_ObjectToWorldEXT` 获取。**RT shader 不访问 InstanceBuffer**。

TLAS instance 去重利用 `build_mesh_instances()` 的连续性保证：**SceneLoader 保证同一 node（glTF mesh）的所有 primitive 在 mesh_instances 数组中连续排列**（`build_mesh_instances` 按 node 遍历，内层循环 `prim_start..prim_end` 连续 push_back）。SceneASBuilder 预计算 `group_prim_count[group_id]`，迭代 mesh_instances 时每次步进 `count` 个条目，每步产生一个 TLAS instance。

Geometry Info buffer 是 GPU_ONLY，场景加载时通过 staging buffer 上传（不需要 ShaderDeviceAddress，通过 descriptor 绑定访问）。

---

### Step 6：PT 核心 shader

编写路径追踪 shader 并验证编译。

- 新增 `shaders/rt/pt_common.glsl`：
  - 声明 RT shader 所需 GLSL 扩展（供其他 RT shader include）：
    - `GL_EXT_ray_tracing`：traceRayEXT、ray payload、RT 内置变量
    - `GL_EXT_buffer_reference` / `GL_EXT_buffer_reference2`：从 device address 读顶点数据
    - `GL_EXT_shader_explicit_arithmetic_types_int64`：GeometryInfo 中的 uint64_t
    - `GL_EXT_nonuniform_qualifier`：bindless 纹理索引（现有 forward.frag 已用）
  - Ray payload 结构定义：
    - `PrimaryPayload`（location 0）：`vec3 color`（本次 bounce 辐射度）、`vec3 next_origin`（下一条光线起点）、`vec3 next_direction`（下一条光线方向）、`vec3 throughput_update`（路径吞吐量乘数，含 Russian Roulette 补偿）、`float hit_distance`（命中距离，miss 时 -1 标记终止）
    - `ShadowPayload`（location 1）：`uint visible`（shadow_miss 设为 1，初始值 0 = 遮挡）
  - Vertex / Index buffer_reference layout 定义（匹配 Vertex 结构体：position vec3、normal vec3、uv0 vec2、tangent vec4、uv1 vec2，stride = 56 bytes）
  - Sobol 低差异序列生成（预计算方向数表嵌入 shader 常量数组）+ Cranley-Patterson rotation（从 blue noise 纹理采样 per-pixel 偏移）
  - Blue noise 纹理采样：128×128 R8Unorm 单通道纹理，像素数据从公开数据集嵌入代码，初始化时上传 GPU 注册到 bindless 数组。不同采样维度通过空间偏移从同一张纹理派生
  - cosine-weighted hemisphere sampling
  - GGX importance sampling（复用 `common/brdf.glsl` 中的 GGX 函数）
  - Russian Roulette（bounce ≥ 2 时按路径吞吐量概率终止）
  - MIS power heuristic（balance heuristic）
  - 顶点属性插值工具（从 GeometryInfo 读 buffer address + gl_PrimitiveID 计算重心坐标，通过 buffer_reference 读取并插值 position/normal/UV）
- 新增 `shaders/rt/reference_view.rgen`：
  - 从 GlobalUBO 的 `inv_view` 和 `inv_projection` 计算 primary ray（pixel → world ray direction）
  - 路径追踪主循环（max bounce 从 push constant 读取）
  - 每 bounce：traceRayEXT → closesthit/miss → 累积贡献
  - Running average 写入 accumulation buffer（`imageStore`，sample_count 加权）
- 新增 `shaders/rt/closesthit.rchit`：
  - 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 buffer address 和 material buffer_offset
  - 插值顶点属性（position、normal、UV，通过 buffer_reference 从 device address 读取）
  - 读取材质参数（GeometryInfo.material_buffer_offset → MaterialBuffer + bindless texture 采样）
  - NEE：随机选择光源，向光源发射 shadow ray（`traceRayEXT` flag `gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT`）
  - MIS 权重计算（BRDF pdf vs light pdf）
  - 计算 BRDF 贡献 + 采样下一个方向
  - 写入 PrimaryPayload：color、next_origin、next_direction、throughput_update、hit_distance
- 新增 `shaders/rt/miss.rmiss`：
  - 采样 IBL cubemap（GlobalUBO skybox_cubemap_index → cubemaps[] + IBL rotation）
  - 写入 PrimaryPayload：color = 环境辐射度，hit_distance = -1（标记路径终止）
- 新增 `shaders/rt/shadow_miss.rmiss`：
  - 写入 ShadowPayload：visible = 1（未被遮挡）
- 新增 `shaders/rt/anyhit.rahit`：
  - 通过 GeometryInfo → MaterialBuffer 读取 alpha 参数（alpha_mode、alpha_cutoff、base_color_factor.a、base_color_tex）
  - 插值 UV（同 closesthit 的顶点插值工具，复用 pt_common.glsl）
  - 采样 base_color_tex 获取纹素 alpha
  - Mask：`texel_alpha < alpha_cutoff` → `ignoreIntersectionEXT`
  - Blend：PCG hash 生成随机数 `r`，`r >= texel_alpha` → `ignoreIntersectionEXT`（stochastic alpha）
  - PCG hash 种子：`gl_LaunchIDEXT` + `frame_seed`（push constant）+ `gl_PrimitiveID` + `gl_GeometryIndexEXT`
- `ShaderCompiler` 扩展：支持 RT shader stage（raygen、closesthit、anyhit、miss），shaderc target 设为 `shaderc_env_version_vulkan_1_4`

**验证**：所有 RT shader 通过 shaderc 编译，无编译错误

#### 设计要点

Push constants（reference_view.rgen）：`max_bounces`（uint）、`sample_count`（uint，当前累积数）、`frame_seed`（uint，蓝噪声时序偏移），共 12 字节。相机逆矩阵（`inv_view`、`inv_projection`）从 GlobalUBO 读取，不放入 push constant（避免超出 128 字节保证限制，且与现有 compute pass 轻量 push constant 模式一致）。

Accumulation buffer 作为 `layout(set = 3, binding = 0, rgba32f) uniform image2D` 通过 push descriptor 绑定（与现有 compute pass Set 3 模式一致）。

NEE 采样：M1 光源少，暴力遍历 LightBuffer 中所有方向光（无 Light BVH）。发光面不在 M1 范围。

Ray payload 采用模式 A（closesthit 内完成全部着色计算）：closesthit 做顶点插值 + 材质采样 + BRDF 计算 + NEE shadow ray + 采样下一方向，通过 PrimaryPayload 返回计算结果。raygen 仅做循环累积。maxRecursionDepth = 1（raygen→closesthit 为 depth 0，closesthit→shadow_miss 为 depth 1）。

---

### Step 7：Reference View Pass

Pass 层实现 + accumulation 管理 + RG 扩展。

- `RGStage` 枚举新增 `RayTracing`，RG barrier 计算映射到 `VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR` + `VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`
- 新增 `passes/reference_view_pass.h` / `.cpp`：
  - `ReferenceViewPass` 类：
    - `setup(Context&, ResourceManager&, DescriptorManager&, ShaderCompiler&)`：编译 RT shader、创建 RT pipeline、创建 accumulation buffer（RGBA32F，Relative 1.0x，Storage usage）
    - `record(RenderGraph&, const FrameContext&)`：注册 accumulation buffer 到 RG，添加 RT pass（push descriptors 绑定 accumulation buffer，trace_rays dispatch）
    - `reset_accumulation()`：清零 sample count（下一帧 shader 覆写而非累积）
    - `sample_count()` getter
    - `accumulation_image()` getter（供 tonemapping 和 OIDN 读取）
    - `rebuild_pipelines()`、`destroy()`、`on_resize()`
  - Accumulation 逻辑：shader 端 `imageLoad` 读取旧值，`new_color = mix(old_color, current_sample, 1.0 / (sample_count + 1))`，`imageStore` 写回
  - Sample count 由 CPU 侧 pass 维护，通过 push constant 传给 shader
  - `reset_accumulation()` 将 sample count 归零（shader 端 `sample_count == 0` 时直接覆写）

**验证**：RenderDoc 检查 accumulation buffer——逐帧亮度/质量增加，相机静止 10+ 帧后画面明显收敛

---

### Step 8：独立渲染路径 + 模式切换

Renderer 分叉为光栅化/PT 两条路径。

- `scene_data.h` 新增 `RenderMode` 枚举（`Rasterization`、`PathTracing`）
- `RenderInput` 新增 `RenderMode render_mode` 字段
- `Renderer::render()` 拆分为私有方法：
  - `fill_common_gpu_data()`：共享的 GPU buffer 填充（GlobalUBO 核心字段、LightBuffer）
  - `render_rasterization()`：现有完整光栅化管线（不变）
  - `render_path_tracing()`：RG clear → import accumulation buffer + swapchain → Reference View Pass → Tonemapping Pass → ImGui Pass → present
  - `render()` 调用 `fill_common_gpu_data()` 后按 `render_mode` switch 到对应私有方法
- VP 矩阵比较逻辑：Renderer 缓存上一帧 PT 模式的 VP 矩阵，每帧比较，不同则调用 `reset_accumulation()`
- Accumulation 缓存：模式切换不清零 accumulation buffer 和 sample count，VP 矩阵不变时继续累积
- Tonemapping 复用：PT 路径中将 accumulation buffer（或 denoised buffer，Step 9）导入为 hdr_color RGResourceId，TonemappingPass 照常读取

**验证**：ImGui 切换光栅化 ↔ PT 两种模式正确显示；PT 模式下相机静止画面逐帧收敛；切到光栅化再切回，同位置不重新渲染（sample count 保持）；移动相机后切回 PT，accumulation 重置

#### 设计要点

PT 路径的 RG 编排极简：仅 Reference View Pass + Tonemapping Pass + ImGui Pass。不注册 shadow/depth/AO 等光栅化 pass。两条路径共享同一个 RenderGraph 实例（每帧 clear → 重新注册当前模式的 pass），共享 swapchain image handles。

模式切换由 Application 层控制（键盘快捷键或 ImGui），通过 RenderInput.render_mode 传递。

---

### Step 9：OIDN 集成

集成 Intel Open Image Denoise GPU 降噪。

- 手动集成 OIDN 预编译库（官方 release 包含 headers + lib + dll，CUDA GPU 支持内置；vcpkg 无端口）。需修改 `framework/CMakeLists.txt` 添加头文件路径和链接库，OIDN DLL 拷贝到构建输出目录
- 新增 `framework/denoiser.h` / `.cpp`：
  - `Denoiser` 类：
    - `init(Context&, ResourceManager&)`：创建 OIDN device（GPU 优先，fallback CPU）、创建 filter（"RT" filter，beauty + albedo + normal 可选辅助输入）、分配 staging buffers（readback + upload，持久不重建）
    - `denoise(ImageHandle accumulation, ImageHandle output)`：
      1. Readback accumulation → CPU staging buffer（`vkCmdCopyImageToBuffer` + fence wait）
      2. 设置 OIDN filter input/output 指针
      3. `oidnExecuteFilter()`（OIDN 内部 upload 到 GPU 执行降噪）
      4. 读取 OIDN 输出到 CPU staging
      5. Upload CPU staging → output image（`vkCmdCopyBufferToImage`）
    - `destroy()`：释放 OIDN device、filter、staging buffers
    - `on_resize()`：重建 staging buffers
- Renderer 新增 denoised buffer（RGBA32F managed image，与 accumulation buffer 同尺寸）
- 降噪状态管理：
  - `denoise_enabled_`（降噪功能开关）
  - `auto_denoise_`（自动触发开关）
  - `auto_denoise_interval_`（每 N 个采样触发）
  - `last_denoised_sample_count_`（上次降噪时的采样数）
  - `show_denoised_`（显示降噪结果还是原始累积）
- 降噪触发逻辑（Renderer 每帧检查）：
  - 自动：`auto_denoise_ && (sample_count - last_denoised >= interval)` → 触发
  - 手动：通过 DebugUIActions 标记触发
- Tonemapping 输入切换：`show_denoised_ && denoise_enabled_` 时导入 denoised buffer 为 hdr_color，否则导入 accumulation buffer

**验证**：降噪后画面明显干净（对比原始累积），自动触发按间隔正常工作，手动按钮即时触发，resize 后降噪正常

#### 设计要点

Staging buffer 大小 = width × height × 16 bytes（RGBA32F），持久分配避免每次降噪重建。OIDN "RT" filter 是通用路径追踪降噪器。辅助通道（albedo、normal）可选——先不传（仅 beauty input），后续可添加以提升降噪质量。

OIDN 降噪是阻塞操作（GPU denoise + CPU readback），会造成一帧的卡顿。对参考视图可接受（非每帧执行）。

---

### Step 10：ImGui PT 面板

完善 PT 参考视图的 UI 控件。

- `DebugUIContext` 新增 PT 相关字段：
  - `render_mode`（读写）
  - `rt_supported`（只读，控制 UI 可见性）
  - `pt_sample_count`、`pt_target_samples`（读/写）
  - `pt_max_bounces`（读写，默认 8，范围 1-32）
  - `pt_elapsed_time`（只读）
  - `denoise_enabled`、`auto_denoise`、`auto_denoise_interval`（读写）
  - `show_denoised`（读写）
  - `last_denoised_sample_count`（只读）
- `DebugUIActions` 新增：
  - `pt_reset_requested`（重置按钮）
  - `pt_denoise_requested`（手动降噪按钮）
- DebugUI 面板：
  - Rendering section 新增渲染模式切换（Rasterization / Path Tracing combo，仅 `rt_supported` 时显示 PT 选项）
  - PT 激活时显示 "Path Tracing" collapsing header：
    - 状态：`Samples: 128 / 1024`（当前/目标）、`Time: 3.2s`
    - Max Bounces slider（1-32）
    - Target Samples input（0 = unlimited）
    - Reset 按钮
  - OIDN collapsing header（PT 激活时显示）：
    - Denoise checkbox
    - Show Denoised / Show Raw toggle
    - Auto Denoise checkbox + Interval input
    - Denoise Now 按钮（自动关闭时可用）
    - `Last denoised at: 64 samples`
- Application 响应 actions：`pt_reset_requested` → `reference_view_pass_.reset_accumulation()`，`pt_denoise_requested` → 触发降噪

**验证**：所有控件功能正常，参数调整实时生效（bounce 数变更触发重置，target samples 到达时停止累积）

---

## 阶段六帧流程

### 光栅化模式（不变）

与阶段五一致。

### PT 参考视图模式

```
Reference View Pass (RT Pipeline)
  输入: TLAS (Set 0 binding 4), Geometry Info (Set 0 binding 5),
        GlobalUBO, LightBuffer, MaterialBuffer (Set 0 binding 0-2),
        Bindless textures/cubemaps (Set 1),
        Accumulation Buffer (Set 3 push descriptor, storage image)
  输出: Accumulation Buffer (running average 累积)
    ↓
[可选] OIDN Denoise (CPU 中转)
  输入: Accumulation Buffer (readback)
  输出: Denoised Buffer (upload)
    ↓
Tonemapping Pass
  输入: Denoised Buffer 或 Accumulation Buffer (作为 hdr_color 导入)
  输出: Swapchain Image
    ↓
ImGui Pass → Present
```

---

## 阶段六文件清单

### 新增文件

```
rhi/
├── include/himalaya/rhi/
│   ├── acceleration_structure.h   # [Step 2] BLAS/TLAS 管理
│   └── rt_pipeline.h              # [Step 3] RT Pipeline + SBT
└── src/
    ├── acceleration_structure.cpp  # [Step 2]
    └── rt_pipeline.cpp             # [Step 3]
framework/
├── include/himalaya/framework/
│   ├── scene_as_builder.h         # [Step 5] 场景加速结构构建
│   └── denoiser.h                 # [Step 9] OIDN 降噪封装
└── src/
    ├── scene_as_builder.cpp        # [Step 5]
    └── denoiser.cpp                # [Step 9]
passes/
├── include/himalaya/passes/
│   └── reference_view_pass.h      # [Step 7] PT 参考视图
└── src/
    └── reference_view_pass.cpp     # [Step 7]
shaders/rt/
├── pt_common.glsl                 # [Step 6] PT 核心共享
├── reference_view.rgen            # [Step 6] 参考视图 raygen
├── closesthit.rchit               # [Step 6] 通用 closest-hit
├── miss.rmiss                     # [Step 6] 环境 miss
└── shadow_miss.rmiss              # [Step 6] Shadow miss
```

### 修改文件

```
rhi/
├── include/himalaya/rhi/
│   ├── context.h                  # [Step 1] rt_supported + RT 属性存储
│   ├── resources.h                # [Step 4] BufferUsage::ShaderDeviceAddress
│   ├── commands.h                 # [Step 3] trace_rays()
│   ├── descriptors.h              # [Step 4] Set 0 layout 条件扩展 + write_set0_tlas()
│   └── shader.h                   # [Step 6] RT shader stage 支持
├── src/
│   ├── context.cpp                # [Step 1] 设备选择 + RT 扩展启用
│   ├── resources.cpp              # [Step 4] ShaderDeviceAddress 映射 + get_buffer_device_address
│   ├── commands.cpp               # [Step 3] trace_rays 实现
│   ├── descriptors.cpp            # [Step 4] Set 0 layout 条件扩展 + TLAS descriptor 写入
│   └── shader.cpp                 # [Step 6] RT stage 编译支持
framework/
├── include/himalaya/framework/
│   ├── mesh.h                     # [Step 4] Mesh 新增 group_id + material_id
│   ├── render_graph.h             # [Step 7] RGStage::RayTracing 枚举
│   └── scene_data.h               # [Step 4+8] GPUGeometryInfo + RenderMode 枚举
├── src/
│   └── render_graph.cpp           # [Step 7] RayTracing barrier 映射
app/
├── include/himalaya/app/
│   ├── scene_loader.h             # [Step 5] load() 新增 rt_supported 参数
│   ├── renderer.h                 # [Step 5-9] SceneASBuilder + ReferenceViewPass + Denoiser 成员
│   ├── debug_ui.h                 # [Step 10] DebugUIContext PT 字段 + DebugUIActions PT 动作
│   └── application.h              # [Step 8] render_mode 状态
├── src/
│   ├── scene_loader.cpp           # [Step 5] load_meshes() 填充 group_id/material_id + buffer flags
│   ├── renderer.cpp               # [Step 5-9] AS 构建 + 渲染路径私有方法拆分 + OIDN 触发
│   ├── debug_ui.cpp               # [Step 10] PT 面板绘制
│   └── application.cpp            # [Step 8-10] 模式切换 + PT actions 响应
shaders/
└── common/bindings.glsl           # [Step 5] Set 0 binding 4/5 声明
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| GlobalUBO inv_view | 阶段六新增 `inv_view`（mat4，view 矩阵的逆）供 PT raygen shader 计算 primary ray。Renderer `fill_common_gpu_data()` 填充。GlobalUniformData 总大小从 864 增至 928 bytes（+64 bytes inv_view，_phase5_pad 后追加） |
| Multi-geometry BLAS | 同一 glTF mesh 的 primitive 合并为一个 BLAS 的多个 geometry。Mesh.group_id 标识分组，instanceCustomIndex = geometry info base offset，closesthit 用 `geometry_infos[customIndex + geometryIndex]` 索引 |
| Accumulation buffer | RGBA32F，Relative 1.0x，Storage usage。running average：`new = mix(old, sample, 1/(n+1))`。sample_count=0 时直接覆写 |
| SBT layout | raygen(1) + miss(2: env + shadow) + hit(1: closesthit)。entry 对齐到 `shaderGroupHandleAlignment`，region 对齐到 `shaderGroupBaseAlignment` |
| NEE | 暴力遍历 LightBuffer 方向光，无 Light BVH。Shadow ray 使用 `gl_RayFlagsTerminateOnFirstHitEXT \| gl_RayFlagsSkipClosestHitShaderEXT` |
| Russian Roulette | bounce ≥ 2 时启用，终止概率 = `1 - max(throughput.r, throughput.g, throughput.b)`，存活时 throughput 除以存活概率 |
| OIDN staging | 持久分配 readback + upload staging buffer（width × height × 16 bytes），避免每次降噪重分配 |
| 模式切换缓存 | accumulation buffer 和 sample count 在模式切换时不清零。VP 矩阵变化触发重置，模式切换不触发 |
| 渲染路径组织 | render() 拆分为 fill_common_gpu_data() + render_rasterization() + render_path_tracing() 私有方法，不引入新抽象 |
| BLAS 批量构建 | 单次 `vkCmdBuildAccelerationStructuresKHR` 调用传入全部 BLAS，GPU 并行构建。Scratch buffer = 所有 BLAS scratch size 之和（各段对齐到 `minAccelerationStructureScratchOffsetAlignment`），构建后释放 |
| 顶点格式硬编码 | `VkAccelerationStructureGeometryTrianglesDataKHR` 的 `vertexFormat` = `R32G32B32_SFLOAT`（Vertex::position offset 0），`indexType` = `UINT32`，Vertex stride = 56 bytes。统一顶点格式，AccelerationStructureManager 内部硬编码 |
| Sobol + Blue Noise | Sobol 低差异序列（预计算方向数表嵌入 shader 常量）+ Cranley-Patterson rotation（per-pixel blue noise 偏移）。Blue noise 128×128 R8Unorm 单通道，像素数据从公开数据集嵌入代码，不同维度通过空间偏移派生 |
| Ray Payload | 模式 A：closesthit 完成全部着色计算。PrimaryPayload(loc 0): color + next_origin + next_direction + throughput_update + hit_distance（52B）。ShadowPayload(loc 1): visible uint（4B） |
| GeometryInfo | GPUGeometryInfo std430 24B：vertex_buffer_address(u64) + index_buffer_address(u64) + material_buffer_offset(u32) + _padding(u32)。Shader 端 GLSL 同布局 |
| RT shader 热重载 | RTPipeline 封装绑定 pipeline + SBT 生命周期，rebuild_pipelines() 走 destroy + create 全量重建 |
| RT 函数加载 | vulkan-1.lib 不导出 KHR RT/AS 符号。Context 通过 `vkGetDeviceProcAddr` 加载 6 个 device-level 函数指针（create/destroy/build AS、create RT pipeline、get shader group handles），CommandBuffer 同模式加载 `vkCmdTraceRaysKHR`（匿名命名空间，`init_rt_functions(VkDevice)` 初始化） |
