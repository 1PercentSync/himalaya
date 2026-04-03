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
    ↓
Step 11: Environment Map Importance Sampling（alias table 构建 + NEE 环境光 + MIS）
    ↓
Step 12: Area Light NEE（emissive alias table + NEE 面光源 + MIS）
    ↓
Step 13: Texture LOD（Ray Cones + lod_bias 调参）
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
| 11 | Env Map Importance Sampling | 高亮度 HDR 下 PT 前 60 帧收敛明显加速，无萤火虫噪点 |
| 12 | Area Light NEE | emissive 表面照亮周围，比纯 BRDF 命中收敛明显加速 |
| 13 | Texture LOD (Ray Cones) | RenderDoc: 远处表面纹理采样 mip > 0，带宽下降 |

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
  - `RTPipeline::destroy(VkDevice, VmaAllocator)`
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
  - `rt_supported = true` 时 Set 0 layout 新增 binding 4（`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`）+ binding 5（SSBO，GeometryInfoBuffer），descriptor pool 容量相应扩展（新增 AS + SSBO 描述符，per frame in flight）。Binding 4/5 使用 `PARTIALLY_BOUND` flag（Step 5 写入前无需有效 descriptor）
  - `rt_supported = true` 时 Set 0 bindings 0-2 的 stageFlags 追加 RT shader stages（`RAYGEN_BIT | CLOSEST_HIT_BIT | MISS_BIT | ANY_HIT_BIT`），binding 3（InstanceBuffer）不变（RT shader 不访问）
  - 新增 `write_set0_tlas(TLASHandle)` + `write_set0_buffer` 复用写 binding 5
  - `get_compute_set_layouts()` 重命名为 `get_dispatch_set_layouts()`（compute 和 RT pipeline 共用）
  - Set 1（bindless textures）layout binding stage flags 添加 `VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR`（RT shader 需要采样 bindless 纹理数组）
- `Mesh` 结构体新增 `group_id`（glTF source mesh index）+ `material_id`（primitive 固有材质，material_instances 索引）
- `CommandBuffer` 新增 RT command wrappers：`bind_rt_pipeline(const RTPipeline&)`、`bind_rt_descriptor_sets(VkPipelineLayout, first_set, sets, count)`、`push_rt_descriptor_set(VkPipelineLayout, set, writes)`（Step 7 ReferenceViewPass 录制需要完整的 RT 命令封装，避免通过 `handle()` 逃逸到原始 Vulkan）

**验证**：编译通过，新增类型和 API 可用但无调用方

---

### Step 5：Scene AS Builder + Renderer 集成

SceneLoader 变更 + SceneASBuilder 实现 + Renderer 集成 + bindings.glsl 扩展。

- `Context` 补充加载 `vkGetAccelerationStructureDeviceAddressKHR` 函数指针 + 新增 `pfn_get_as_device_address` 公有字段（SceneASBuilder 构建 TLAS instance 需要获取 BLAS device address）
- `SceneLoader::load()` 新增 `bool rt_supported` 参数：`true` 时 vertex/index buffer 创建额外加 `ShaderDeviceAddress` flag
- `SceneLoader::load_meshes()` 填充 `group_id` 和 `material_id`
- 新增 `framework/scene_as_builder.h`：
  - `SceneASBuilder` 类：
    - `build(Context&, ResourceManager&, AccelerationStructureManager&, std::span<const Mesh>, std::span<const MeshInstance>, std::span<const MaterialInstance>)`：
      1. 按 `Mesh::group_id` 分组 → 每组收集 BLASGeometry 列表（跳过 `vertex_count == 0` 或 `index_count < 3` 的 primitive——glTF 不保证所有 primitive 为有效三角形；根据 `Mesh::material_id` → `MaterialInstance::alpha_mode` 设置 `BLASGeometry::opaque`）→ 组装 BLASBuildInfo（multi-geometry）
      2. 调用 `AccelerationStructureManager::build_blas()` 批量构建（每 group 一个 BLAS）
      3. 构建 Geometry Info SSBO（按 group 连续排列，per-geometry：vertex/index buffer address、material buffer_offset）
      4. 按 `(group_id, transform)` 去重 mesh_instances → 组装 `VkAccelerationStructureInstanceKHR` 数组（customIndex = 该 group 在 Geometry Info 中的 base offset）
      5. 调用 `AccelerationStructureManager::build_tlas()`
    - `destroy()`：释放 BLAS、TLAS、Geometry Info buffer
    - `tlas_handle()` / `geometry_info_buffer()` getter
- Renderer：场景加载后调用 `SceneASBuilder::build()`，写入 Set 0 binding 4/5
- `bindings.glsl` 新增 `GeometryInfo` struct + Set 0 binding 4（`accelerationStructureEXT`）+ binding 5（`GeometryInfoBuffer`）
- `AppConfig` 新增 `log_level` 字段（`std::string`，默认空）+ `config.cpp` 序列化/反序列化（JSON key `"log_level"`，字符串格式如 `"warn"`、`"info"`；空/缺失 = 默认 `warn`，使用 `spdlog::level::to_string_view()` / `spdlog::level::from_str()` 转换）
- `Application::init()` 加载 config 后应用 log_level（非空时解析为 `spdlog::level::level_enum`，空时保持默认 `warn`），替代硬编码 `kLogLevel`
- DebugUI log level 变更持久化：`DebugUIActions` 新增 `log_level_changed` 标志 + `new_log_level`，`Application::update()` 检测变更后更新 `config_.log_level` 并调用 `save_config()`

**验证**：场景加载后日志输出 BLAS 数量（= unique group 数）+ TLAS instance 数量（= unique node 数）+ Geometry Info buffer 大小，无 validation 报错；DebugUI 修改 log level 后重启应用保持所选级别

#### 设计要点

`VkAccelerationStructureInstanceKHR::instanceCustomIndex` 设为该 BLAS 对应 group 在 Geometry Info buffer 中的 base offset（24 位，足够）。Closest-hit shader 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 vertex/index buffer address 和 material buffer_offset。变换矩阵通过 TLAS 内置 `gl_ObjectToWorldEXT` 获取。**RT shader 不访问 InstanceBuffer**。

TLAS instance 去重利用 `build_mesh_instances()` 的连续性保证：**SceneLoader 保证同一 node（glTF mesh）的所有 primitive 在 mesh_instances 数组中连续排列**（`build_mesh_instances` 按 node 遍历，内层循环 `prim_start..prim_end` 连续 push_back）。SceneASBuilder 预计算 `group_prim_count[group_id]`，迭代 mesh_instances 时每次步进 `count` 个条目，每步产生一个 TLAS instance。

Geometry Info buffer 是 GPU_ONLY，场景加载时通过 staging buffer 上传（不需要 ShaderDeviceAddress，通过 descriptor 绑定访问）。

**bindings.glsl RT 条件编译**：RT 专用绑定（binding 4 `accelerationStructureEXT` 需 `GL_EXT_ray_tracing`，binding 5 `GeometryInfo` 含 `uint64_t` 需 `GL_EXT_shader_explicit_arithmetic_types_int64`）用 `#ifdef HIMALAYA_RT` 守卫。光栅化 shader 不 define 此宏，RT shader 在 `#include "bindings.glsl"` 前 `#define HIMALAYA_RT`。

**Renderer 集成流程**：Application 场景加载后在新 immediate scope 中调用 `Renderer::build_scene_as(meshes, mesh_instances, materials)`。Renderer 内部创建 `AccelerationStructureManager`（init 时）和 `SceneASBuilder`（成员），`build_scene_as()` 调用 `SceneASBuilder::build()` 后写入 Set 0 binding 4/5。`SceneASBuilder::build()` 重复调用时自动 destroy 旧 AS 再重建（同 `MaterialSystem::upload_materials()` 模式），`switch_scene()` 无需显式 destroy。流程示例：

```
// Application 侧
context_.begin_immediate();
scene_loader_.load(..., context_.rt_supported);
context_.end_immediate();

if (context_.rt_supported) {
    context_.begin_immediate();
    renderer_.build_scene_as(meshes, mesh_instances, materials);
    context_.end_immediate();
}
```

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
  - Sobol 低差异序列生成（128 维 32-bit 方向数表嵌入 shader 常量数组，16 KB；超出 128 维时 fallback 到 PCG hash）+ Cranley-Patterson rotation（从 blue noise 纹理采样 per-pixel 偏移）
  - Blue noise 纹理采样：128×128 R8Unorm 单通道纹理，像素数据从 Calinou/free-blue-noise-textures（CC0）嵌入 `app/include/himalaya/app/blue_noise_data.h`（`constexpr uint8_t[]`），Renderer 初始化时上传 GPU 注册到 bindless 数组。不同采样维度通过空间偏移从同一张纹理派生
  - cosine-weighted hemisphere sampling
  - GGX VNDF importance sampling（Heitz 2018 可见法线分布采样，新增 `sample_ggx_vndf()` + `pdf_ggx_vndf()`；`#include "common/brdf.glsl"` 复用 `D_GGX`、`V_SmithGGX`、`F_Schlick` 做 BRDF 评估）
  - Russian Roulette（bounce ≥ 2 时按路径吞吐量概率终止）
  - MIS power heuristic（balance heuristic，Step 6 定义函数，方向光 NEE 权重恒为 1 不调用，Step 11 env sampling 开始使用）
  - 顶点属性插值工具（从 GeometryInfo 读 buffer address + gl_PrimitiveID 计算重心坐标，通过 buffer_reference 读取并插值 position/normal/tangent/UV）
  - Ray origin offset（Wächter & Binder，Ray Tracing Gems Ch.6）工具函数
  - Shading normal 一致性修正（clamp 到几何法线半球）
- 新增 `shaders/rt/reference_view.rgen`：
  - 从 GlobalUBO 的 `inv_view` 和 `inv_projection` 计算 primary ray（pixel → world ray direction）
  - **Subpixel jitter**：Sobol dims 0-1 像素偏移（PT 抗锯齿），per-bounce 维度从 dim 2 开始
  - 路径追踪主循环（max bounce 从 push constant 读取）
  - 每 bounce：设置 `payload.bounce = i` → traceRayEXT → closesthit/miss → 累积贡献
  - **Firefly clamping**：bounce > 0 的贡献 `min(contribution, max_clamp)`（bounce 0 不 clamp）
  - Running average 写入 accumulation buffer（`imageStore`，sample_count 加权）
- 新增 `shaders/rt/closesthit.rchit`：
  - 通过 `geometry_infos[gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT]` 获取当前 geometry 的 buffer address 和 material buffer_offset
  - 插值顶点属性（position、normal、**tangent**、UV，通过 buffer_reference 从 device address 读取）
  - **Normal mapping**：`#include "common/normal.glsl"` 复用 `get_shading_normal()`，应用 shading normal 一致性修正
  - 读取材质参数（GeometryInfo.material_buffer_offset → MaterialBuffer + bindless texture 采样）
  - **Emissive 贡献**：所有 bounce 命中时加 `emissive_tex × emissive_factor`（Step 12 补 MIS 权重）
  - **OIDN 辅助输出**：bounce == 0 时 `imageStore` 写入 aux albedo（`base_color × (1-metallic)`）+ aux normal（shading normal）
  - NEE 方向光：遍历 LightBuffer，发射 shadow ray（`traceRayEXT` flag `gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT`），权重 1.0（delta 分布）
  - **Multi-lobe BRDF 采样**：按 Fresnel 估计概率选 diffuse（cosine hemisphere）或 specular（GGX VNDF）lobe，PDF 补偿
  - **Ray origin offset**：next_origin 和 shadow ray 起点使用 Wächter & Binder 偏移
  - 写入 PrimaryPayload：color、next_origin、next_direction、throughput_update、hit_distance、bounce
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

Push constants（reference_view.rgen）：`max_bounces`（uint）、`sample_count`（uint）、`frame_seed`（uint）、`blue_noise_index`（uint）、`max_clamp`（float，firefly clamping 阈值，0 = 关闭），共 20 字节。相机逆矩阵（`inv_view`、`inv_projection`）从 GlobalUBO 读取，不放入 push constant（避免超出 128 字节保证限制，且与现有 compute pass 轻量 push constant 模式一致）。

Accumulation buffer 作为 `layout(set = 3, binding = 0, rgba32f) uniform image2D` 通过 push descriptor 绑定（与现有 compute pass Set 3 模式一致）。

NEE 采样：M1 光源少，暴力遍历 LightBuffer 中所有方向光（无 Light BVH）。发光面不在 M1 范围。

Ray payload 采用模式 A（closesthit 内完成全部着色计算）：closesthit 做顶点插值 + 材质采样 + BRDF 计算 + NEE shadow ray + 采样下一方向，通过 PrimaryPayload 返回计算结果。raygen 仅做循环累积。maxRecursionDepth = 1（raygen→closesthit 为 depth 0，closesthit→shadow_miss 为 depth 1）。

---

### Step 7：Reference View Pass

Pass 层实现 + accumulation 管理 + RG 扩展。

- `RGStage` 枚举新增 `RayTracing`，RG barrier 计算映射到 `VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR` + `VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`
- 新增 `passes/reference_view_pass.h` / `.cpp`：
  - `ReferenceViewPass` 类：
    - `setup(Context&, ResourceManager&, DescriptorManager&, ShaderCompiler&)`：编译 RT shader、创建 RT pipeline、创建 accumulation buffer（RGBA32F，Relative 1.0x，Storage usage）、创建 OIDN 辅助 image（aux albedo + aux normal，RGBA32F，同尺寸）
    - `record(RenderGraph&, const FrameContext&)`：注册 accumulation buffer 到 RG，添加 RT pass（push descriptors 绑定 accumulation buffer + aux albedo + aux normal（Set 3 binding 0/1/2），trace_rays dispatch）
    - `reset_accumulation()`：清零 sample count（下一帧 shader 覆写而非累积）
    - `sample_count()` getter
    - `accumulation_image()` getter（供 tonemapping 和 OIDN 读取）
    - `aux_albedo_image()` / `aux_normal_image()` getter（供 OIDN 辅助通道读取）
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
    - `init(Context&, ResourceManager&)`：创建 OIDN device（GPU 优先，fallback CPU）、创建 filter（"RT" filter，beauty + albedo + normal 辅助输入）、分配 staging buffers（readback + upload，持久不重建；beauty + albedo + normal 各一组）
    - `denoise(ImageHandle accumulation, ImageHandle albedo, ImageHandle normal, ImageHandle output)`：
      1. Readback accumulation + albedo + normal → CPU staging buffers
      2. 设置 OIDN filter input/output 指针（beauty + albedo + normal 辅助通道）
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

Staging buffer 大小 = width × height × 16 bytes（RGBA32F），持久分配避免每次降噪重建（beauty + albedo + normal 各一组）。OIDN "RT" filter 配置 albedo + normal 辅助通道（Step 6 closesthit bounce 0 输出，Step 7 管理辅助 image），显著提升低采样数降噪质量。

OIDN 降噪是阻塞操作（GPU denoise + CPU readback），会造成一帧的卡顿。对参考视图可接受（非每帧执行）。

---

### Step 10：ImGui PT 面板

完善 PT 参考视图的 UI 控件。

- `DebugUIContext` 新增 PT 相关字段：
  - `render_mode`（读写）
  - `rt_supported`（只读，控制 UI 可见性）
  - `pt_sample_count`、`pt_target_samples`（读/写）
  - `pt_max_bounces`（读写，默认 8，范围 1-32）
  - `pt_max_clamp`（读写，默认 10.0，0 = 关闭 firefly clamping）
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
    - Firefly Clamp slider（0 = 关闭，默认 10.0）
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

### Step 11：Environment Map Importance Sampling

IBL alias table 构建 + closesthit NEE 环境光 + MIS 权重。架构决策见 `milestone-1/m1-rt-decisions.md`「Environment Map Importance Sampling」。

- IBL 新增 `build_env_alias_table()`：在 `load_equirect()` 的 stbi_loadf 数据上计算 luminance×sin(theta)，Vose's algorithm 构建半分辨率（1024×512）alias table，上传 SSBO（GPU_ONLY + TransferDst）。输出 `total_luminance`（float）。alias table 缓存（key = `hdr_hash + "_alias_table"`，二进制格式）
- IBL 新增 getter：`alias_table_buffer()`（BufferHandle）、`total_luminance()`（float）、`alias_table_width()`/`alias_table_height()`（uint32_t）
- Set 0 binding 6 新增：`EnvAliasTable` SSBO（`rt_supported` 时，`PARTIALLY_BOUND`），DescriptorManager 条件扩展 layout + descriptor pool 容量
- DescriptorManager 新增 `write_set0_env_alias_table(BufferHandle, uint64_t size)`
- Renderer：IBL init 后调用 `write_set0_env_alias_table()` 写入 binding 6
- bindings.glsl `#ifdef HIMALAYA_RT` 区域新增 binding 6 声明（`EnvAliasEntry` struct + `EnvAliasTable` buffer）
- pt_common.glsl 新增 `sample_env_alias_table()` 函数（2 个 uniform random → 像素索引 → equirect UV → 方向 → 应用 IBL rotation）
- pt_common.glsl 新增 `env_pdf()` 函数（方向 → IBL cubemap luminance → PDF）
- pt_common.glsl 新增 `mis_power_heuristic(float pdf_a, float pdf_b)` 函数
- closesthit.rchit 新增 NEE 环境光步骤：alias table 采样 → shadow ray → MIS 加权贡献
- closesthit.rchit BRDF 采样后：预计算 `env_mis_weight`，写入 PrimaryPayload
- PrimaryPayload 新增 `float env_mis_weight` 字段（56B → 60B）
- reference_view.rgen：miss 返回的 env color 乘以 payload 中的 `env_mis_weight`
- miss.rmiss：不变（仍返回 raw env color，MIS 权重由 raygen 应用）
- IBL fallback cubemap 时跳过 alias table 构建（无 HDR 环境，env importance sampling 退化为纯 BRDF miss）

**验证**：高亮度 HDR 环境（含太阳）下，PT 前 60 帧对比无 env sampling 版本收敛明显加速，无萤火虫噪点；低亮度均匀 HDR 下与无 env sampling 版本无可见差异

---

### Step 12：Area Light NEE

Emissive 面光源 importance sampling + MIS。架构决策见 `milestone-1/m1-rt-decisions.md`「Area Light NEE」。

- 新增 `framework/emissive_light_builder.h` / `.cpp`：
  - `EmissiveLightBuilder` 类：
    - `build(Context&, ResourceManager&, meshes, mesh_instances, materials)`：遍历 mesh primitive 识别 emissive（`any(emissive_factor > 0)`），收集世界空间顶点/UV/面积，计算 `luminance(emissive_factor) × area` 功率，Vose's algorithm 构建 power-weighted alias table，上传 EmissiveTriangleBuffer SSBO + EmissiveAliasTable SSBO
    - `destroy()`、`emissive_count()`、`triangle_buffer()`、`alias_table_buffer()` getter
- GPUMaterialData `_padding`（offset 76）→ `uint double_sided`（从 glTF `doubleSided` 填充）
- forward.frag emissive 贡献对齐 doubleSided 检查
- DescriptorManager：Set 0 layout 条件新增 binding 7（EmissiveTriangleBuffer SSBO）+ binding 8（EmissiveAliasTable SSBO），`PARTIALLY_BOUND`，`rt_supported` 守卫
- Renderer：场景加载后调用 EmissiveLightBuilder::build() + 写入 Set 0 binding 7/8
- bindings.glsl `#ifdef HIMALAYA_RT` 新增 `EmissiveTriangle` struct + binding 7/8 声明
- Push constant 新增 `uint emissive_light_count`（20B → 24B，0 = 跳过 NEE emissive）
- pt_common.glsl 新增三角形均匀采样（重心坐标）+ emissive alias table 采样 + light PDF 计算
- closesthit.rchit 新增 NEE emissive 步骤：alias table 采样 → 三角形上均匀采样点 → shadow ray（tMax = `distance * (1 - 1e-4)`）→ MIS 加权贡献。Emissive 双面跟随 `double_sided` 标志
- closesthit.rchit BRDF 采样后写入 `payload.last_brdf_pdf`
- closesthit.rchit 命中 emissive 表面时（bounce > 0）：读 `payload.last_brdf_pdf` + 算 light_pdf → MIS 权重。Bounce 0 直视权重 1.0
- PrimaryPayload 新增 `float last_brdf_pdf` 字段（60B → 64B）

**验证**：含 emissive 灯罩/LED 的场景，PT 前 60 帧对比无 area light NEE 版本收敛明显加速，灯罩周围墙面照亮

---

### Step 13：Texture LOD（Ray Cones）

RT 纹理 mip 选择。架构决策见 `milestone-1/m1-rt-decisions.md`「Texture LOD / Ray Cones」。

- pt_common.glsl 新增 Ray Cone 工具函数（init_cone、propagate_cone、compute_lod）
- pt_common.glsl 新增 `compute_texel_density()`：从三角形顶点位置 + UV 运行时计算 world/UV 面积比
- reference_view.rgen：初始化 cone spread（`atan(2 × tan(fov/2) / screen_height)`）+ 循环内设 `payload.cone_spread`
- closesthit.rchit：propagate cone（`spread += hit_distance × spread_angle`），compute LOD，所有材质纹理 `texture()` → `textureLod(tex, uv, lod + lod_bias)`（~5-6 处）
- anyhit.rahit：alpha 纹理采样 `texture()` → `textureLod()`（~1 处）
- PrimaryPayload 新增 `float cone_spread` 字段（64B → 68B）
- Push constant 新增 `float lod_bias`（24B → 28B，默认 0.0）
- Step 10 ImGui 面板新增 LOD Bias slider
- 简化：忽略表面曲率（cone 只按距离扩散），纯累积无额外 bias

**验证**：RenderDoc 检查远处表面纹理采样 mip > 0，纹理带宽下降

---

## 阶段六帧流程

### 光栅化模式（不变）

与阶段五一致。

### PT 参考视图模式

```
Reference View Pass (RT Pipeline)
  输入: TLAS (Set 0 binding 4), Geometry Info (Set 0 binding 5),
        Env Alias Table (Set 0 binding 6),
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
│   ├── denoiser.h                 # [Step 9] OIDN 降噪封装
│   └── emissive_light_builder.h   # [Step 12] Emissive 面光源采样构建
└── src/
    ├── scene_as_builder.cpp        # [Step 5]
    ├── denoiser.cpp                # [Step 9]
    └── emissive_light_builder.cpp  # [Step 12]
app/
└── include/himalaya/app/
    └── blue_noise_data.h           # [Step 6] 128×128 R8Unorm blue noise 像素数据（constexpr）
passes/
├── include/himalaya/passes/
│   └── reference_view_pass.h      # [Step 7] PT 参考视图
└── src/
    └── reference_view_pass.cpp     # [Step 7]
shaders/rt/
├── pt_common.glsl                 # [Step 6] PT 核心共享 ; [Step 11] env sampling + MIS ; [Step 12] emissive sampling ; [Step 13] ray cone
├── reference_view.rgen            # [Step 6] 参考视图 raygen ; [Step 11] env MIS ; [Step 13] cone init
├── closesthit.rchit               # [Step 6] 通用 closest-hit ; [Step 11] NEE env ; [Step 12] NEE emissive + MIS ; [Step 13] textureLod
├── miss.rmiss                     # [Step 6] 环境 miss
├── shadow_miss.rmiss              # [Step 6] Shadow miss
└── anyhit.rahit                   # [Step 6] Alpha test + stochastic alpha ; [Step 13] textureLod
```

### 修改文件

```
rhi/
├── include/himalaya/rhi/
│   ├── context.h                  # [Step 1] rt_supported + RT 属性存储 + RT 函数指针
│   ├── resources.h                # [Step 4] BufferUsage::ShaderDeviceAddress
│   ├── commands.h                 # [Step 3] trace_rays() + init_rt_functions() ; [Step 4] RT bind/descriptor/push wrappers
│   ├── descriptors.h              # [Step 4] Set 0 layout 条件扩展 + write_set0_tlas() ; [Step 11] write_set0_env_alias_table() ; [Step 12] binding 7/8
│   └── shader.h                   # [Step 6] RT shader stage 支持
├── src/
│   ├── context.cpp                # [Step 1] 设备选择 + RT 扩展启用 + RT 函数指针加载 ; [Step 5] 补充 pfn_get_as_device_address
│   ├── resources.cpp              # [Step 4] ShaderDeviceAddress 映射 + get_buffer_device_address
│   ├── commands.cpp               # [Step 3] trace_rays 实现 ; [Step 4] RT bind/descriptor/push 实现
│   ├── descriptors.cpp            # [Step 4] Set 0 layout 条件扩展 + TLAS descriptor 写入 ; [Step 11] binding 6 ; [Step 12] binding 7/8
│   └── shader.cpp                 # [Step 6] RT stage 编译支持
framework/
├── include/himalaya/framework/
│   ├── mesh.h                     # [Step 4] Mesh 新增 group_id + material_id
│   ├── render_graph.h             # [Step 7] RGStage::RayTracing 枚举
│   ├── scene_data.h               # [Step 4+8] GPUGeometryInfo + RenderMode 枚举 ; [Step 12] EmissiveTriangle struct
│   └── material_system.h          # [Step 12] GPUMaterialData _padding → double_sided
├── src/
│   └── render_graph.cpp           # [Step 7] RayTracing barrier 映射
│   ├── ibl.h                      # [Step 11] build_env_alias_table() + alias table getter
│   └── ibl.cpp                    # [Step 11] Vose's algorithm 构建 + 缓存 + SSBO 上传
app/
├── include/himalaya/app/
│   ├── scene_loader.h             # [Step 5] load() 新增 rt_supported 参数
│   ├── renderer.h                 # [Step 5-9] SceneASBuilder + ReferenceViewPass + Denoiser ; [Step 12] EmissiveLightBuilder
│   ├── config.h                   # [Step 5] AppConfig 新增 log_level 字段
│   ├── debug_ui.h                 # [Step 5] DebugUIActions log_level_changed ; [Step 10] DebugUIContext PT 字段 + DebugUIActions PT 动作
│   └── application.h              # [Step 8] render_mode 状态
├── src/
│   ├── scene_loader.cpp           # [Step 5] load_meshes() 填充 group_id/material_id + buffer flags
│   ├── renderer.cpp               # [Step 5-9] AS 构建 + 渲染路径拆分 + OIDN ; [Step 11] env alias table ; [Step 12] emissive light builder
│   ├── config.cpp                 # [Step 5] log_level 序列化/反序列化
│   ├── debug_ui.cpp               # [Step 5] log level 变更返回 action ; [Step 10] PT 面板绘制
│   └── application.cpp            # [Step 5] log level 加载 + 变更持久化 ; [Step 8-10] 模式切换 + PT actions 响应
shaders/
├── common/bindings.glsl           # [Step 5] binding 4/5 ; [Step 11] binding 6 ; [Step 12] binding 7/8 + double_sided
└── forward.frag                   # [Step 12] emissive doubleSided 检查
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| GlobalUBO inv_view | 阶段六新增 `inv_view`（mat4，view 矩阵的逆）供 PT raygen shader 计算 primary ray。Renderer `fill_common_gpu_data()` 填充。GlobalUniformData 总大小从 864 增至 928 bytes（+64 bytes inv_view，_phase5_pad 后追加） |
| Multi-geometry BLAS | 同一 glTF mesh 的 primitive 合并为一个 BLAS 的多个 geometry。Mesh.group_id 标识分组，instanceCustomIndex = geometry info base offset，closesthit 用 `geometry_infos[customIndex + geometryIndex]` 索引 |
| Accumulation buffer | RGBA32F，Relative 1.0x，Storage usage。running average：`new = mix(old, sample, 1/(n+1))`。sample_count=0 时直接覆写 |
| SBT layout | raygen(1) + miss(2: env + shadow) + hit(1: closesthit)。entry 对齐到 `shaderGroupHandleAlignment`，region 对齐到 `shaderGroupBaseAlignment` |
| NEE 方向光 | 暴力遍历 LightBuffer 方向光，无 Light BVH。方向光为 delta 分布，MIS 权重恒为 1，不需要 MIS。Shadow ray 使用 `gl_RayFlagsTerminateOnFirstHitEXT \| gl_RayFlagsSkipClosestHitShaderEXT` |
| NEE 环境光 | Alias table importance sampling（1024×512，`{float32 prob, uint32 alias}`，Set 0 binding 6 SSBO）。total_luminance 嵌入 SSBO 头部。env-local 空间构建，shader 端旋转。MIS power heuristic 与 BRDF 采样组合 |
| Russian Roulette | bounce ≥ 2 时启用，终止概率 = `1 - max(throughput.r, throughput.g, throughput.b)`，存活时 throughput 除以存活概率 |
| OIDN staging | 持久分配 readback + upload staging buffer（width × height × 16 bytes），避免每次降噪重分配 |
| 模式切换缓存 | accumulation buffer 和 sample count 在模式切换时不清零。VP 矩阵变化触发重置，模式切换不触发 |
| 渲染路径组织 | render() 拆分为 fill_common_gpu_data() + render_rasterization() + render_path_tracing() 私有方法，不引入新抽象 |
| BLAS 批量构建 | 单次 `vkCmdBuildAccelerationStructuresKHR` 调用传入全部 BLAS，GPU 并行构建。Scratch buffer = 所有 BLAS scratch size 之和（各段对齐到 `minAccelerationStructureScratchOffsetAlignment`），构建后释放 |
| 顶点格式硬编码 | `VkAccelerationStructureGeometryTrianglesDataKHR` 的 `vertexFormat` = `R32G32B32_SFLOAT`（Vertex::position offset 0），`indexType` = `UINT32`，Vertex stride = 56 bytes。统一顶点格式，AccelerationStructureManager 内部硬编码 |
| Sobol + Blue Noise | Sobol 128 维 32-bit 方向数表（16 KB shader 常量），超出 128 维 fallback 到 PCG hash。Cranley-Patterson rotation（per-pixel blue noise 偏移）。Blue noise 128×128 R8Unorm 单通道，数据源 Calinou/free-blue-noise-textures（CC0），像素嵌入 `app/include/himalaya/app/blue_noise_data.h`（`constexpr uint8_t[]`），不同维度通过空间偏移派生。bindless index 通过 push constant 传递 |
| GGX 采样 | VNDF sampling（Heitz 2018），pt_common.glsl 新增 `sample_ggx_vndf()` + `pdf_ggx_vndf()`，`#include "common/brdf.glsl"` 复用评估函数，不修改 brdf.glsl |
| RT shader include 组织 | 各 RT shader 负责 `#define HIMALAYA_RT` + `#include "common/bindings.glsl"`，然后 `#include "rt/pt_common.glsl"`。pt_common.glsl 不 include bindings.glsl（由调用方负责），但 include `common/brdf.glsl`（GGX 评估需要） |
| Ray Payload | 模式 A：closesthit 完成全部着色计算。PrimaryPayload(loc 0) 演进：Step 6 56B（+bounce）→ Step 11 60B（+env_mis_weight）→ Step 12 64B（+last_brdf_pdf）→ Step 13 68B（+cone_spread）。ShadowPayload(loc 1): visible uint（4B） |
| GeometryInfo | GPUGeometryInfo std430 24B：vertex_buffer_address(u64) + index_buffer_address(u64) + material_buffer_offset(u32) + _padding(u32)。Shader 端 GLSL 同布局 |
| Set 0 RT stage flags | `rt_supported = true` 时 bindings 0-2 追加 `RAYGEN_BIT \| CLOSEST_HIT_BIT \| MISS_BIT \| ANY_HIT_BIT`（RT shader 访问 GlobalUBO/LightBuffer/MaterialBuffer）。Binding 3（InstanceBuffer）不追加（RT shader 不访问）。Binding 4/5 新增时直接带 RT stages |
| Set 0 binding 4/5 PARTIALLY_BOUND | 新增的 AS（binding 4）和 SSBO（binding 5）使用 `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT`，允许 Step 5 写入前 descriptor 未初始化。不需要 `UPDATE_AFTER_BIND` pool flag，`PARTIALLY_BOUND` 仅需 Vulkan 1.2 核心 `descriptorBindingPartiallyBound` feature |
| Set 1 RT stage flags | `rt_supported = true` 时 bindless 纹理/cubemap 的 stageFlags 追加 `CLOSEST_HIT_BIT \| ANY_HIT_BIT \| MISS_BIT`（closesthit/anyhit 采样材质纹理，miss 采样 IBL cubemap） |
| RT shader 热重载 | RTPipeline 封装绑定 pipeline + SBT 生命周期，rebuild_pipelines() 走 destroy + create 全量重建 |
| RT 函数加载 | vulkan-1.lib 不导出 KHR RT/AS 符号。Context 通过 `vkGetDeviceProcAddr` 加载 6 个 device-level 函数指针（create/destroy/build AS、create RT pipeline、get shader group handles），CommandBuffer 同模式加载 `vkCmdTraceRaysKHR`（匿名命名空间，`init_rt_functions(VkDevice)` 初始化） |
| PT Push Constants | 演进：Step 6 20B（max_bounces + sample_count + frame_seed + blue_noise_index + max_clamp）→ Step 12 24B（+emissive_light_count）→ Step 13 28B（+lod_bias）。相机逆矩阵从 GlobalUBO 读取 |
| Env Alias Table | 1024×512 半分辨率，`{float32 prob, uint32 alias}` 8B/entry，total_luminance + entry_count 嵌入 SSBO 头部。IBL load_equirect() 中从原始 float32 RGB 构建（Vose O(N)），随 IBL 产物缓存。Set 0 binding 6，PARTIALLY_BOUND，rt_supported 守卫 |
| Ray Origin Offset | Wächter & Binder（Ray Tracing Gems Ch.6）：float 位整数偏移，全尺度鲁棒无 epsilon。pt_common.glsl 工具函数，closesthit 的 shadow ray 和 next_origin 共用 |
| Normal Mapping (RT) | closesthit 通过 buffer_reference 读 tangent（vec4），`#include "common/normal.glsl"` 复用 `get_shading_normal()` + shading normal 一致性修正（clamp 到几何法线半球） |
| Subpixel Jitter | Sobol dims 0-1 像素偏移，per-bounce 维度从 dim 2 开始。无亚像素抖动则 PT 几何边缘永远锯齿 |
| Emissive 策略 | 所有 bounce 加 emissive（物理正确）。Step 12 前无 MIS（直接贡献），Step 12 后 BRDF 命中 emissive 加 MIS 权重 |
| Multi-lobe BRDF | Fresnel 估计概率（`luminance(F_Schlick(NdotV, F0))`）选 diffuse(cosine) / specular(GGX VNDF)。PDF 除以选择概率补偿 |
| Firefly Clamping | 仅 bounce > 0（间接贡献）。`min(contribution, max_clamp)`。bounce 0 不产生萤火虫，直视 emissive 不应被压暗。max_clamp 默认 10.0，ImGui slider |
| OIDN Aux Buffers | bounce 0 closesthit imageStore aux albedo（base_color × (1-metallic)，不含 emissive）+ aux normal（shading normal）。RGBA32F。Set 3 binding 1/2 push descriptor。Step 9 OIDN filter 配置辅助通道 |
| Area Light NEE | EmissiveLightBuilder 构建 emissive triangle list + power-weighted alias table。Set 0 binding 7/8。MIS：NEE emissive + BRDF 命中 emissive 用 power heuristic 平衡。push constant `emissive_light_count`（0=skip）。双面跟随 glTF doubleSided |
| EmissiveTriangle | std430 96B：v0/v1/v2(vec3) + emission(vec3, raw factor) + area(float) + material_index(uint) + uv0/uv1/uv2(vec2)。存 UV 以精确采样纹理 emissive |
| Texture LOD | Ray Cones（Akenine-Möller 2021）。忽略表面曲率，运行时算纹理密度（不改 GeometryInfo），纯累积无 bias。`lod_bias` push constant 调参（默认 0.0）。PrimaryPayload `cone_spread` 跨 bounce 传递 |
| Set 0 binding 7/8 | EmissiveTriangleBuffer(binding 7) + EmissiveAliasTable(binding 8)，`PARTIALLY_BOUND`，`rt_supported` 守卫。无 emissive 场景时不写入 |
