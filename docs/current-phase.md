# 当前阶段：M1 阶段六 — RT 基础设施 + PT 参考视图

> 目标：引入 Vulkan RT 扩展和加速结构基础设施，实现路径追踪参考视图（accumulation + OIDN viewport denoising）。
> 验证整个 RT 技术栈端到端可用，为阶段七烘焙器和 M2 实时 PT 奠定基础。
>
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Steps 1-2: RHI 层 RT 扩展（设备选择、扩展启用、特性查询）
    ↓
Step 3: RHI 层 AS 抽象（BLAS/TLAS 创建、构建、销毁）
    ↓
Step 4: RHI 层 RT Pipeline 抽象（SBT、trace_rays 命令）
    ↓
Step 5: Framework 层 Scene AS Builder + Set 0 扩展（binding 4/5）
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
| 1 | RT 扩展检测与设备选择 | 日志输出 RT 支持状态，有 RT 硬件时 `rt_supported = true` |
| 2 | RT 扩展启用与特性激活 | 无 validation 报错，RT 设备特性成功启用 |
| 3 | AS 资源抽象 | 编译通过，无调用方（纯 API 声明 + 实现） |
| 4 | RT Pipeline + SBT + trace_rays | 编译通过，无调用方 |
| 5 | Scene AS Builder + Set 0 扩展 | 场景加载后日志输出 BLAS/TLAS 构建信息，无 validation 报错 |
| 6 | PT 核心 shader | shader 编译通过（shaderc RT target），无运行时调用 |
| 7 | Reference View Pass | RenderDoc: accumulation buffer 逐帧亮度增加，静止时收敛 |
| 8 | 独立渲染路径 + 模式切换 | ImGui 切换光栅化 ↔ PT，两种模式正确显示，切换后缓存有效 |
| 9 | OIDN 集成 | 降噪后画面明显干净，自动/手动触发均工作 |
| 10 | ImGui PT 面板 | 所有控件功能正常，参数调整实时生效 |

---

### Step 1：RT 扩展检测与设备选择

设备选择逻辑变更，RT 支持作为加分项。

- `context.cpp`：新增 RT 所需扩展列表（`VK_KHR_acceleration_structure`、`VK_KHR_ray_tracing_pipeline`、`VK_KHR_ray_query`、`VK_KHR_deferred_host_operations`、`VK_KHR_spirv_1_4`、`VK_KHR_shader_float_controls`）
- `pick_physical_device()`：对每个候选设备检测 RT 扩展可用性，RT 支持设备在评分中获得额外权重
- `Context` 新增 `bool rt_supported` 公有字段，设备选择完成后根据最终选中设备的 RT 能力设置
- 日志输出：选中设备名 + RT 支持状态

**验证**：有 RT 硬件时日志显示 `rt_supported = true`，无 RT 时 `rt_supported = false`，现有渲染不变

---

### Step 2：RT 扩展启用与特性激活

在 `create_device()` 中条件启用 RT 扩展和设备特性。

- `rt_supported = true` 时：将 RT 扩展加入设备扩展列表
- 启用设备特性：`accelerationStructure`、`rayTracingPipeline`、`rayQuery`、`bufferDeviceAddress`（Vulkan 1.2 核心，AS 构建需要）
- 查询并存储 RT 相关属性（`VkPhysicalDeviceRayTracingPipelinePropertiesKHR`：`shaderGroupHandleSize`、`shaderGroupBaseAlignment`、`maxRayRecursionDepth` 等），Context 新增存储字段
- `rt_supported = false` 时：跳过所有 RT 相关初始化

**验证**：无 validation 报错，`vkGetPhysicalDeviceProperties2` 成功获取 RT 属性

#### 设计要点

`bufferDeviceAddress` 是 Vulkan 1.2 核心特性，但需要显式启用。AS 构建需要 buffer device address 指定顶点/索引数据位置。`VK_KHR_spirv_1_4` 和 `VK_KHR_shader_float_controls` 是 `VK_KHR_ray_tracing_pipeline` 的依赖扩展。

---

### Step 3：AS 资源抽象

RHI 层新增加速结构管理。

- 新增 `rhi/acceleration_structure.h`：
  - `BLASHandle`：BLAS 句柄（VkAccelerationStructureKHR + backing VkBuffer + VmaAllocation）
  - `TLASHandle`：TLAS 句柄（同上）
  - `BLASBuildInfo`：BLAS 构建输入（vertex buffer address、index buffer address、vertex count、index count、vertex stride、transform）
  - `AccelerationStructureManager` 类：
    - `build_blas(std::span<const BLASBuildInfo> infos)` → `std::vector<BLASHandle>`：批量构建 BLAS（单次 command buffer 提交，PREFER_FAST_TRACE）
    - `build_tlas(std::span<const VkAccelerationStructureInstanceKHR> instances)` → `TLASHandle`：构建 TLAS
    - `destroy_blas(BLASHandle)`、`destroy_tlas(TLASHandle)`
    - 内部管理 scratch buffer（构建完成后释放）
- 新增 `rhi/acceleration_structure.cpp`：实现 AS 创建（`vkCreateAccelerationStructureKHR`）、构建（`vkCmdBuildAccelerationStructuresKHR`）、destroy

**验证**：编译通过，无调用方

#### 设计要点

BLAS 和 TLAS 的 backing buffer 需要 `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR` + `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`。Scratch buffer 需要 `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` + `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`。构建在 immediate command scope 中执行（场景加载时一次性构建）。

---

### Step 4：RT Pipeline + SBT + trace_rays

RHI 层新增 RT Pipeline 创建和命令录制。

- 新增 `rhi/rt_pipeline.h`：
  - `RTPipelineDesc`：RT pipeline 描述（raygen/miss/closesthit shader modules、max recursion depth、descriptor set layouts、push constant ranges）
  - `RTPipeline`：持有 VkPipeline + VkPipelineLayout + SBT buffer（raygen/miss/hit regions）
  - `create_rt_pipeline(VkDevice, const RTPipelineDesc&)` → `RTPipeline`
  - `RTPipeline::destroy(VkDevice)`
- `rt_pipeline.cpp`：实现 shader group 创建、`vkCreateRayTracingPipelinesKHR`、SBT 构建（`vkGetRayTracingShaderGroupHandlesKHR` → 对齐写入 SBT buffer）
- `commands.h` 新增 `trace_rays(const RTPipeline&, uint32_t width, uint32_t height)` 方法
- `commands.cpp`：实现 `vkCmdTraceRaysKHR`（从 RTPipeline 读取 SBT region 信息）

**验证**：编译通过，无调用方

#### 设计要点

SBT layout：raygen region（1 entry）+ miss region（2 entries：environment miss + shadow miss）+ hit region（1 entry：closest-hit）。每个 entry 大小对齐到 `shaderGroupHandleAlignment`，region 起始对齐到 `shaderGroupBaseAlignment`（Step 2 查询的属性）。`maxRayRecursionDepth` 设为 1（不用递归，closest-hit 中 shadow ray 用 `traceRayEXT` 但 miss 不再发射射线）。

---

### Step 5：Scene AS Builder + Set 0 扩展

Framework 层场景加速结构构建 + Descriptor 扩展。

- 新增 `framework/scene_as_builder.h`：
  - `SceneASBuilder` 类：
    - `build(Context&, ResourceManager&, AccelerationStructureManager&, std::span<const Mesh>, std::span<const MeshInstance>)`：
      1. 为每个 unique mesh 收集 BLASBuildInfo（vertex/index buffer device address）
      2. 调用 `AccelerationStructureManager::build_blas()` 批量构建
      3. 组装 `VkAccelerationStructureInstanceKHR` 数组（transform、customIndex = instance index、BLAS reference）
      4. 调用 `AccelerationStructureManager::build_tlas()`
      5. 构建 Geometry Info SSBO（per-mesh：vertex buffer address、index buffer address、vertex stride、index type、material_id）
    - `destroy()`：释放 BLAS、TLAS、Geometry Info buffer
    - `tlas_handle()` / `geometry_info_buffer()` getter
- `DescriptorManager` 扩展：
  - `init()` 接收 `bool rt_supported` 参数
  - `rt_supported = true` 时 Set 0 layout 新增 binding 4（`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`）+ binding 5（SSBO）
  - 新增 `write_set0_tlas(TLASHandle)` + `write_set0_buffer` 复用写 binding 5
- `BufferUsage` 枚举新增 `ShaderDeviceAddress`，`ResourceManager` 在 buffer 创建时映射到 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- `ResourceManager` 新增 `get_buffer_device_address(BufferHandle)` 方法
- Renderer：场景加载后调用 `SceneASBuilder::build()`，写入 Set 0 binding 4/5

**验证**：场景加载后日志输出 BLAS 数量 + TLAS instance 数量 + Geometry Info buffer 大小，无 validation 报错

#### 设计要点

`VkAccelerationStructureInstanceKHR::instanceCustomIndex` 设为实例在 mesh_instances 数组中的索引（24 位，足够）。Closest-hit shader 通过 `gl_InstanceCustomIndexEXT` 查 InstanceBuffer 获取 material_index，再通过 Geometry Info 获取该 mesh 的 vertex/index buffer address 计算顶点属性（position、normal、UV）。

Geometry Info buffer 是 GPU_ONLY + ShaderDeviceAddress，场景加载时通过 staging buffer 上传。

---

### Step 6：PT 核心 shader

编写路径追踪 shader 并验证编译。

- 新增 `shaders/rt/pt_common.glsl`：
  - Sobol / Halton 低差异序列生成
  - Blue noise 纹理采样偏移
  - cosine-weighted hemisphere sampling
  - GGX importance sampling（复用 `common/brdf.glsl` 中的 GGX 函数）
  - Russian Roulette（bounce ≥ 2 时按路径吞吐量概率终止）
  - MIS power heuristic（balance heuristic）
  - 顶点属性插值工具（从 Geometry Info 读 buffer address + gl_PrimitiveID 计算重心坐标插值 position/normal/UV）
- 新增 `shaders/rt/reference_view.rgen`：
  - 从相机矩阵计算 primary ray（pixel → world ray direction）
  - 路径追踪主循环（max bounce 从 push constant 读取）
  - 每 bounce：traceRayEXT → closesthit/miss → 累积贡献
  - Running average 写入 accumulation buffer（`imageStore`，sample_count 加权）
- 新增 `shaders/rt/closesthit.rchit`：
  - 插值顶点属性（position、normal、UV）
  - 读取材质参数（MaterialBuffer + bindless texture 采样）
  - NEE：随机选择光源，向光源发射 shadow ray（`traceRayEXT` flag `gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipClosestHitShaderEXT`）
  - MIS 权重计算（BRDF pdf vs light pdf）
  - 计算 BRDF 贡献 + 采样下一个方向
- 新增 `shaders/rt/miss.rmiss`：
  - 采样 IBL cubemap（GlobalUBO skybox_cubemap_index → cubemaps[] + IBL rotation）
  - 返回环境辐射度
- 新增 `shaders/rt/shadow_miss.rmiss`：
  - 空 miss shader（shadow ray miss = 未被遮挡，payload 标记 visible = true）
- `ShaderCompiler` 扩展：支持 RT shader stage（raygen、closesthit、miss），shaderc target 设为 `shaderc_env_version_vulkan_1_4`

**验证**：所有 RT shader 通过 shaderc 编译，无编译错误

#### 设计要点

Push constants（reference_view.rgen）：`inv_view`、`inv_projection`、`max_bounces`、`sample_count`（当前累积数）、`frame_seed`（蓝噪声时序偏移）。

Accumulation buffer 作为 `layout(set = 3, binding = 0, rgba32f) uniform image2D` 通过 push descriptor 绑定（与现有 compute pass Set 3 模式一致）。

NEE 采样：M1 光源少，暴力遍历 LightBuffer 中所有方向光（无 Light BVH）。发光面不在 M1 范围。

---

### Step 7：Reference View Pass

Pass 层实现 + accumulation 管理。

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
- `Renderer::render()` 根据 `render_mode` 分叉：
  - `Rasterization`：现有完整光栅化管线（不变）
  - `PathTracing`：RG clear → import accumulation buffer + swapchain → Reference View Pass → Tonemapping Pass → ImGui Pass → present
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

- vcpkg.json 新增 OIDN 依赖（需确认 vcpkg 端口可用性，备选：手动集成预编译库）
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
        GlobalUBO, LightBuffer, MaterialBuffer, InstanceBuffer (Set 0),
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
│   ├── acceleration_structure.h   # [Step 3] BLAS/TLAS 管理
│   └── rt_pipeline.h              # [Step 4] RT Pipeline + SBT
└── src/
    ├── acceleration_structure.cpp  # [Step 3]
    └── rt_pipeline.cpp             # [Step 4]
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
│   ├── context.h                  # [Step 1-2] rt_supported + RT 属性存储
│   ├── resources.h                # [Step 5] BufferUsage::ShaderDeviceAddress
│   ├── commands.h                 # [Step 4] trace_rays()
│   ├── descriptors.h              # [Step 5] init(rt_supported), write_set0_tlas()
│   └── shader.h                   # [Step 6] RT shader stage 支持
├── src/
│   ├── context.cpp                # [Step 1-2] 设备选择 + RT 扩展启用
│   ├── resources.cpp              # [Step 5] ShaderDeviceAddress 映射 + get_buffer_device_address
│   ├── commands.cpp               # [Step 4] trace_rays 实现
│   ├── descriptors.cpp            # [Step 5] Set 0 layout 扩展 + TLAS descriptor 写入
│   └── shader.cpp                 # [Step 6] RT stage 编译支持
framework/
├── include/himalaya/framework/
│   └── scene_data.h               # [Step 8] RenderMode 枚举
app/
├── include/himalaya/app/
│   ├── renderer.h                 # [Step 5-9] SceneASBuilder + ReferenceViewPass + Denoiser 成员
│   ├── debug_ui.h                 # [Step 10] DebugUIContext PT 字段 + DebugUIActions PT 动作
│   └── application.h              # [Step 8] render_mode 状态
├── src/
│   ├── renderer.cpp               # [Step 5-9] AS 构建 + 双路径 render + OIDN 触发
│   ├── debug_ui.cpp               # [Step 10] PT 面板绘制
│   └── application.cpp            # [Step 8-10] 模式切换 + PT actions 响应
shaders/
└── common/bindings.glsl           # [Step 5] Set 0 binding 4/5 声明
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| Accumulation buffer | RGBA32F，Relative 1.0x，Storage usage。running average：`new = mix(old, sample, 1/(n+1))`。sample_count=0 时直接覆写 |
| SBT layout | raygen(1) + miss(2: env + shadow) + hit(1: closesthit)。entry 对齐到 `shaderGroupHandleAlignment`，region 对齐到 `shaderGroupBaseAlignment` |
| NEE | 暴力遍历 LightBuffer 方向光，无 Light BVH。Shadow ray 使用 `gl_RayFlagsTerminateOnFirstHitEXT \| gl_RayFlagsSkipClosestHitShaderEXT` |
| Russian Roulette | bounce ≥ 2 时启用，终止概率 = `1 - max(throughput.r, throughput.g, throughput.b)`，存活时 throughput 除以存活概率 |
| OIDN staging | 持久分配 readback + upload staging buffer（width × height × 16 bytes），避免每次降噪重分配 |
| 模式切换缓存 | accumulation buffer 和 sample count 在模式切换时不清零。VP 矩阵变化触发重置，模式切换不触发 |
