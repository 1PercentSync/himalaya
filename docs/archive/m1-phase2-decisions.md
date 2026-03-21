# M1 阶段二设计决策：基础渲染管线

> 阶段二：Bindless descriptor、Mesh/纹理加载、材质系统、相机、场景接口、glTF 加载、视锥剔除、Render Graph、App 层拆分。
> 核心决策摘要见 `../milestone-1/m1-design-decisions-core.md`。

---

## 决策总览

| 组件 | 实现级别 | 核心理由 |
|------|----------|----------|
| Render Graph | 手动编排 + barrier 辅助（Sync2 API） | 减痛点但不过度投资 |
| 资源 / 描述符 | Bindless（2D 4096 + Cubemap 256）+ 传统 Descriptor Set + Push Constant | Bindless 早期投资值得 |
| 顶点格式 | 统一格式（position + normal + uv0 + tangent + uv1） | 简化 pipeline 管理 |
| 材质系统 | 代码定义 + 固定数据结构 + 全局 SSBO | 简单直接 |
| Shader 系统 | 运行时编译 + 热重载 | 开发效率优先 |
| 场景数据 | 渲染列表（完整结构一次定义，按需填充） | 简单解耦 |
| 调参 | ImGui + 配置结构体 | 能调就行 |
| 深度缓冲 | D32Sfloat + Reverse-Z | 远处精度提升 |
| Sampler | Per-texture sampler | glTF sampler 忠实还原 |
| Default 纹理 | 3 个 1×1（white / flat normal / black） | shader 无需特判 |
| 纹理格式 | 按角色选（SRGB / UNORM） | gamma 正确 |
| GPU 上传 | 批量 immediate command scope | 一次 submit |

---

## Render Graph

**选择：手动编排的 Pass 列表 + barrier 自动插入**

Pass 的执行顺序由代码手动定义（一个有序列表），但每个 Pass 声明自己的输入输出资源，系统根据这些声明自动插入 barrier 和管理资源状态。不做自动拓扑排序，不做资源别名优化。

**为什么不做完整自动化 Render Graph：** 完整系统（依赖图拓扑排序、资源别名分析、barrier 合并优化）开发量大，在还没有任何 pass 跑起来之前要先花相当长时间在基础设施上。

**为什么不完全手动管理：** 每个 pass 自己管理 barrier 和资源状态，后续维护痛苦会指数增长。

**M1 的优势：** pass 数量约 10 个，手动排序完全可控，同时获得了 barrier 自动化这个最大的减痛点。

**升级路径：** 每个 pass 已声明输入输出，后续加入拓扑排序和资源别名分析时，已有的 pass 声明格式不需要修改。

### 渐进式能力建设

Render Graph 的功能按需引入，不提前建设未使用的能力：

| 能力 | 引入阶段 | 理由 |
|------|---------|------|
| Barrier 自动插入 | 阶段二 | 核心价值，从第一天就需要 |
| 资源导入（import） | 阶段二 | 外部创建的资源（swapchain image、vertex buffer 等）导入 RG 追踪状态 |

阶段二的 RG 只管 barrier 和状态追踪，资源由外部创建后导入。这不影响后续扩展——pass 声明输入输出的接口从阶段二就固定下来，后续增量添加 managed/temporal 等能力时已有 pass 不需要修改。

### 帧间生命周期

RG 每帧重建：`clear()` → `import_*()` → `add_pass()` → `compile()` → `execute()`。这是**长期方案**，不是过渡性设计。每帧重建确保 pass 列表和资源引用始终与当前帧状态一致，避免帧间残留状态带来的 bug。

### Command Buffer 传递

`execute(CommandBuffer& cmd)` 接收外部传入的 command buffer，RG 不持有任何 Vulkan 同步资源（fence、semaphore、command pool）。同步由外部帧循环管理，RG 专注于 pass 编排和 barrier 插入。

### import_image 初始 layout

`import_image()` 增加 `VkImageLayout initial_layout` 参数，调用方显式传入资源在导入时的当前 layout。RG 以此为起点计算 layout transition。

### import_image 最终 layout

`import_image()` 同时接受 `final_layout` 参数（必填，无默认值）。RG 在 `execute()` 结束后，为每个 imported image 插入从「最后使用 layout」到 `final_layout` 的 barrier。

**设计理由：** 帧间存活的 imported resource（swapchain image、depth buffer 等）需要在帧末处于确定的 layout，以对应下一帧 import 时的 `initial_layout`。`final_layout` 让 RG 自动管理这个转换，避免调用方手动插入 barrier。

**为什么不用默认值：** 所有 imported image 都是帧间存活的（阶段二没有 transient 资源），强制指定 `final_layout` 避免遗漏导致的 layout 不匹配 bug。

**用法示例：** swapchain image 传 `PRESENT_SRC_KHR`，depth buffer 传 `DEPTH_ATTACHMENT_OPTIMAL`。

### Barrier 粒度

阶段二只处理 image layout transition。Buffer barrier 在阶段三有 GPU 写 buffer 需求时再加——扩展只需在 barrier emit 处加一个 if/else 分支，约 10 行代码。

### Barrier 计算策略

`compile()` 根据 pass 声明的 `RGResourceUsage` 推导 barrier 参数。从 `(RGAccessType, RGStage)` 到 `(VkImageLayout, VkPipelineStageFlags2, VkAccessFlags2)` 的映射以函数形式封装，按需实现实际用到的组合，未实现的组合 assert 拦截。后续阶段加新组合只需加 case。

**Hazard 覆盖**：barrier 在 layout 变化或存在数据竞争时发出。四种 hazard 类型中，RAR（连续读取）无需 barrier，其余三种均处理：RAW/WAW 通过检测上一次 access 包含写 flag 触发；WAR 通过检测当前 access 包含写 flag 且资源此前已被访问触发。这确保 M2 引入 compute pass（storage image 在 GENERAL layout 下先读后写）时不会遗漏 execution dependency。

**RG 不管 loadOp/storeOp。** Pass 在 execute 回调中自己构造 `VkRenderingInfo` 时决定 loadOp/storeOp。RG 只管 layout transition 和内存依赖，保持职责单一。

### Debug Utils 集成

RG `execute()` 自动为每个 pass 插入 `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT`，使用 pass 注册时的名称。RenderDoc 和 GPU profiler 中按 pass 名称分组显示 GPU 工作。

CommandBuffer 新增 `begin_debug_label(name, color)` / `end_debug_label()` 方法封装此功能。

### 资源引用方式

Pass 声明资源使用时通过 typed handle（`RGResourceId`）而非字符串引用资源。`declare_resource()` / `import_resource()` 返回 `RGResourceId`，pass 持有并传递这些 ID。每个资源使用声明（`RGResourceUsage`）包含 handle、access type（READ / WRITE / READ_WRITE）和 stage，合并为单个列表传入 `add_pass()`，不区分 inputs/outputs 参数。资源名称仅用于调试（debug name）。

**为什么不用字符串：** 拼写错误不被编译期捕获，且字符串查找不够优雅。typed handle 提供编译期类型安全和零运行时查找开销。

### READ_WRITE 语义

`READ_WRITE` 表示**同一张 image 在同一帧内同时读写**，典型场景是 depth attachment（depth test 读 + depth write 写）。RG 为此生成的 barrier 使用 `DEPTH_ATTACHMENT_OPTIMAL`（同时允许读写）。

### Render Graph 接管范围

Render Graph 接管 acquire image 和 present 之间的**所有** GPU 工作。帧循环变为：acquire image → CPU 准备 → RG compile → RG execute → submit → present。Swapchain image 作为 imported resource 导入 RG，RG 管理其渲染期间的 layout transition。

### ImGui 作为 Render Graph Pass

ImGui 注册为 Render Graph 中的最后一个 pass（独立 pass），声明对最终 color attachment 的读写。`begin_frame()` 在 CPU 准备阶段调用（RG execute 之前），`render()` 在 pass 的 execute 回调中调用。

**为什么不在其他 pass 内部附加：** ImGui 作为独立 pass 更透明，RG 统一管理所有渲染工作。

### Swapchain Image 导入 RG

Swapchain image 由 `VkSwapchainKHR` 持有，不在 ResourceManager 的资源池中。为保持 `import_image()` 接口统一使用 `ImageHandle`，ResourceManager 新增**外部 image 注册**能力：

- `register_external_image(VkImage, VkImageView, ImageDesc)` → `ImageHandle`：分配一个 slot，记录 VkImage/VkImageView/desc，`allocation` 设为 null（不持有 VMA 内存）
- `unregister_external_image(ImageHandle)`：释放 slot，递增 generation，**不调用** `vmaDestroyImage`
- `destroy_image()` 检查 `allocation` 是否为 null，防止误销毁外部资源

Swapchain 初始化时注册所有 swapchain image 获得 `ImageHandle` 数组，每帧用 `swapchain_handles_[image_index]` 调用 `import_image()`。Resize 时先 unregister 后重新注册。

**为什么不给 RG 加 VkImage 重载：** `get_image()` 返回 `ImageHandle`，pass lambda 通过 `resource_manager.get_image(handle)` 统一获取 VkImage/VkImageView。如果 swapchain image 没有 ImageHandle，`get_image()` 接口断裂，pass lambda 需要两条代码路径。外部注册保持了整个 Handle 体系的一致性。

### RG 与 ResourceManager 的关系

RG 在 `compile()` 时需要 `VkImage` 构造 `VkImageMemoryBarrier2`，在 `compile()` / `execute()` 时通过 `ResourceManager::get_image(handle)` 解析。因此 RG 构造/初始化时接收 `ResourceManager*` 引用。

依赖方向正确：RG（framework 层）→ ResourceManager（rhi 层）。这与 ResourceManager 自身接收 `Context*` 是同一模式。

### RG Barrier 的 Image Aspect 推导

RG 插入 `VkImageMemoryBarrier2` 时需要 `subresourceRange.aspectMask`（depth image 用 `DEPTH_BIT`，color image 用 `COLOR_BIT`）。RG 通过 `resource_manager.get_image(handle).desc.format` 获取资源格式，再由 `aspect_from_format()` 推导 aspect。

`to_vk_format(Format)` 和 `aspect_from_format(Format)` 从 `resources.cpp` 的 static 函数提取为 `rhi/types.h` 中的公共 inline 函数，与 `Format` 枚举定义在同一文件中，framework 层通过 `#include <himalaya/rhi/types.h>` 使用。

### Framework 层 Vulkan 类型使用

Render Graph 和 ImGui Backend 是 framework 层中允许直接使用 Vulkan 类型（VkImageLayout 等）的**明确例外**，不是暂时性的实现。

**理由：** RG 本质是 Vulkan barrier 管理器，用 VkImageLayout 比自造枚举再映射更直观且不易出错。ImGui Backend 的 Vulkan backend 天然需要 Vulkan 类型。其他 framework 模块的公开接口仍然不使用 Vulkan 类型。

详见 `../project/architecture.md` 的「Framework 层 Vulkan 类型例外」章节。

---

## 资源管理与描述符

**选择：Bindless 纹理 + Push Constant 混合方案**

- **材质纹理**：Bindless（创建全局 descriptor array，所有纹理注册进去用 index 访问）
- **Per-draw 动态数据**：Push Constant（模型矩阵、材质 index 等每次绘制变化的数据）

不使用传统的 per-material descriptor set。

**为什么 Bindless 从一开始做：** 这是一个早期投资但回报贯穿整个项目——材质系统简洁，不需要 per-material descriptor set 的分配和管理，以后做 Instancing、Shadow Atlas 等动态访问不同纹理的功能时无缝支持。Vulkan 对 bindless 支持好（VK_EXT_descriptor_indexing）。

**为什么不用传统 per-material descriptor set：** descriptor set 的分配和管理会随材质数量增长变得繁琐，频繁切换有 CPU/GPU 开销，以后做 Instancing 需要改造。

**注意事项：** 需要想清楚 descriptor 更新策略、纹理加载后注册到 array 的流程、shader 里的间接访问模式。对 GPU 调试不太友好（所有东西都是 index），需配合 debug name 使用。

### Descriptor Set 三层架构

三个 Descriptor Set 按资源生命周期分层，每层有清晰的语义边界：

| Set | 内容 | 生命周期 | 更新频率 |
|-----|------|---------|---------|
| 0 | 全局 Buffer（GlobalUBO + LightBuffer + MaterialBuffer） | per-frame 双缓冲 | 每帧 memcpy |
| 1 | 持久纹理资产（bindless 材质纹理 + cubemap） | 场景加载 → 卸载 | 加载时写入 |
| 2 | 帧内 Render Target（后处理 / 屏幕空间效果的中间产物） | init → destroy | resize / MSAA 切换时更新 |

均使用传统 Descriptor Set 分配（非 Push Descriptors）。Set 0 per-frame 分配 2 个（2 frames in flight），Set 1 分配 1 个长期持有，Set 2 分配 1 个。共 4 个 descriptor set：Set 0 × 2 + Set 1 × 1 + Set 2 × 1。

所有 graphics pipeline 使用统一的 pipeline layout `{Set 0, Set 1, Set 2}`，确保切换 pipeline 时所有 set 绑定保持有效。后处理 pass 不使用 Set 1 但 layout 中包含以保持兼容性。

### Descriptor Pool 分离

三个 Set 使用**独立的 Descriptor Pool**：

- **Set 0 Pool**（普通 pool）：容纳 2 UBO + 4 SSBO，maxSets = 2。分配 Set 0 × 2（per-frame）
- **Set 1 Pool**（`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`）：容纳 4352 COMBINED_IMAGE_SAMPLER（2D 纹理 4096 + Cubemap 256），maxSets = 1。分配 Set 1 × 1
- **Set 2 Pool**（普通 pool）：容纳 8 COMBINED_IMAGE_SAMPLER（M1 预留 8 个 render target binding），maxSets = 1。分配 Set 2 × 1

**为什么分离：** `UPDATE_AFTER_BIND_BIT` 加在 pool 上会影响从该 pool 分配的所有 set。Set 1 需要此 flag（bindless 纹理在使用时更新），Set 0 和 Set 2 不需要，分离后职责隔离更清晰。

Set 2 不需要 `UPDATE_AFTER_BIND`——只在 resize/MSAA 切换时更新（有 `vkQueueWaitIdle()` 保障）。

ImGui 专用 Descriptor Pool 独立于上述三个 pool（已在阶段一实现）。

### Set 0 Per-frame Buffer 策略

- **GlobalUBO × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **LightBuffer × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **MaterialBuffer × 1**：场景加载后一次性创建

Set 0 descriptor 初始化时通过 `vkUpdateDescriptorSets` 写一次（per-frame set 各指向自己的 buffer），之后每帧只 `memcpy` buffer 内容，不需要再 update descriptor。

**Push Descriptors 评估后不采用：** Vulkan 1.4 核心的 Push Descriptors 可以省去 Set 0 的 descriptor pool 分配（约 30 行初始化代码），但每帧需要构造 VkWriteDescriptorSet（约 15 行），总代码量几乎打平。且每个 pipeline layout 只能有一个 push descriptor set，引入了第二种描述符管理模式，增加认知复杂度。收益不足以抵消一致性的损失。

### DescriptorManager 职责

`DescriptorManager`（`rhi/descriptors.h/cpp`）集中管理所有 descriptor 相关资源：

- **持有全局 Descriptor Set Layout**（Set 0、Set 1、Set 2），提供 `get_global_set_layouts()` 方法供 pipeline 创建时使用，确保所有 pipeline 使用一致的 layout
- **管理 Descriptor Pool** 和 **Descriptor Set 分配**（三个 pool 各自分配对应的 set）
- **管理 Bindless 数组**（Set 1：2D 纹理和 Cubemap 的注册、注销、slot 分配）
- **管理 Render Target Descriptor**（Set 2：render target binding 的写入和更新）

### Descriptor Set Layout 与 Pipeline Layout 的关系

DescriptorManager 持有全局 set layout，Pipeline 创建时通过 `GraphicsPipelineDesc::descriptor_set_layouts` 传入。每个 Pipeline 各自创建 `VkPipelineLayout`。虽然产生重复的 layout 对象（轻量元数据，开销忽略不计），但保留了灵活性——未来 compute pipeline 或特殊 pass 可能需要不同的 layout 组合。

`get_global_set_layouts()` 方法将一致性风险集中在 DescriptorManager 一处，各 pass 统一调用而非自己构造 layout 数组。

### 资源 Debug Name

`create_image()`、`create_buffer()`、`create_sampler()` 全部新增**必选** `debug_name` 参数（`std::string_view`）。内部调用 `vkSetDebugUtilsObjectNameEXT` 为 Vulkan 对象设置可读名称。

所有已有调用点回溯补上有意义的名字（如 `"depth_buffer"`、`"global_ubo_frame0"`），RenderDoc 和 Validation Layer 报错时显示这些名称。

**引入时机**：Step 2（managed 资源引入时同步实施，新增的 managed 资源和已有资源一起补齐 debug name）。

### Bindless 纹理数组

Set 1 包含两个 binding，分别管理 2D 纹理和 Cubemap：

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[];     // 2D 纹理，上限 4096
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];   // Cubemap，上限 256
```

两个 binding 均使用 `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，固定上限。不使用 `VARIABLE_DESCRIPTOR_COUNT`——Vulkan 规定此 flag 只能设在 set 内最后一个 binding 上，两个 binding 后 binding 0 不能再用。4096 + 256 个 descriptor 的内存开销约 200-300 KB，不值得保留 `VARIABLE_DESCRIPTOR_COUNT` 的复杂性。

Cubemap 不能放入 `sampler2D[]` 数组（类型不兼容），需要独立 binding。一步到位做 bindless cubemap 数组，M1 阶段六 Reflection Probes 直接复用。

候选方案：

| 方案 | 说明 |
|------|------|
| A. Set 0 新增 binding 放 cubemap samplers | 独立 binding 最直接 |
| B. Set 1 新增 binding（bindless cubemap 数组） | 一步到位，Reflection Probes 复用 |
| C. 新的 Set 2（IBL 专用 descriptor set） | IBL 资源独立管理 |

**选择 B。** 阶段三只有 2-3 个 cubemap（irradiance + prefiltered），但 Set 1 bindless 数组为 M1 阶段六 Reflection Probes 提供零改动的扩展路径。C 增加额外 descriptor set 管理复杂度，IBL 不需要独立于其他纹理的生命周期。

> **限制**：2D 纹理 slot 上限 4096，Cubemap slot 上限 256。如果未来需要更多（不太可能在光栅化管线中发生），需要重新创建更大的 descriptor set。

**Slot 回收**：纹理销毁后，其 bindless slot 通过 free list 回收，新纹理注册时优先复用空闲 slot。回收配合 deferred deletion 确保 GPU 不再引用旧 descriptor。这支持场景切换时的纹理卸载/重载。2D 纹理和 Cubemap 的 slot 管理独立（各自的 free list + 各自的 slot 空间），`BindlessIndex` 的 `index` 值分别对应各自数组的下标。

### Set 0 Layout — 纯 Buffer

Set 0 只包含 buffer 类型的 binding，不包含任何 image/sampler 类型：

| Binding | 类型 | 内容 | 引入阶段 |
|---------|------|------|----------|
| 0 | UBO | GlobalUBO | 阶段二（已有） |
| 1 | SSBO | LightBuffer | 阶段二（已有） |
| 2 | SSBO | MaterialBuffer | 阶段二（已有） |

阶段二的 Set 0 layout 保持不变——阶段三不需要新增 Set 0 binding。Shadow map（`sampler2DArrayShadow`）归入 Set 2（render target，见下文），不占用 Set 0 binding。

### Set 1 Layout — 持久纹理资产

Set 1 bindless 数组只存放**持久纹理资产**——场景加载或初始化时创建，整个运行期间不变：

| 纹理 | Set 1 binding | 阶段 |
|------|--------------|------|
| 材质纹理（base color、normal、metallic-roughness 等） | `sampler2D[]` (binding 0) | 阶段二 |

后续阶段按需注册持久纹理资产到 Set 1（IBL cubemap、BRDF LUT 等），帧内 render target 不进入 Set 1，走 Set 2。

---

## 顶点格式

**选择：统一顶点格式**

所有 mesh 使用同一种顶点格式：position (vec3) + normal (vec3) + uv0 (vec2) + tangent (vec4) + uv1 (vec2)。glTF 加载时缺失的属性填默认值（normal 填 (0,0,1)，tangent 填 (1,0,0,1)，uv 填 0）。

**为什么统一：** 所有 mesh 共享同一种 pipeline vertex input 配置，简化 pipeline 管理。M1 的标准 PBR 需要全部属性（position + normal + uv0 + tangent 用于法线贴图 TBN 矩阵，uv1 为阶段六 lightmap 预留）。

**内存代价：** 每顶点约 24 字节的额外开销（tangent + uv1），一个几万顶点的 mesh 多几百 KB，可接受。

> **潜在优化**：当引入 skinned mesh（额外需要 joint indices + joint weights 每顶点 32 字节）或大规模场景显存紧张时，可拆分为多种顶点格式 + 对应 pipeline variant。统一格式继续给标准 PBR mesh 用，新格式给特定需求。

---

## 材质系统

**选择：代码定义 + 固定数据结构**

M1 只有一种材质（标准 PBR），材质参数用固定的 `GPUMaterialData` 结构体定义（CPU 端 struct + shader 端 struct 一一对应）。不引入运行时参数描述系统（`MaterialParamType` / `MaterialParamDesc`）——shader 端必须在编译时知道 struct 布局，运行时描述只增加 CPU 端复杂度而无法带来 GPU 端灵活性。

**GPUMaterialData 定义位置**：`material_system.h`（而非 `scene_data.h`）。`scene_data.h` 中的 GPU 结构体（`GlobalUniformData`、`GPUDirectionalLight`、`PushConstantData`）是每帧/每次绘制流经渲染管线的数据——renderer-app contract。`GPUMaterialData` 则是材质系统的内部 GPU 布局：由 `MaterialSystem` 创建、写入 Material SSBO、管理生命周期，其他模块仅通过 push constant 的 `material_index` 间接访问。放在 `material_system.h` 遵循"谁拥有谁定义"原则，MaterialSystem 是唯一生产者。

**为什么不做完整数据驱动：** M1 只有标准 PBR 一种材质，复杂的数据驱动系统（材质定义文件、自动生成 shader 变体）投入产出比不高。

**升级路径（固定 stride + 多 shader 解读）：** 引入第二种着色模型（卡通、SSS 等）时，采用固定 stride 方案：所有材质 struct 填充到相同大小（当前 80 字节），每个 shader variant 在同一 binding 上定义自己的 typed struct（如 `PBRMaterial`、`ToonMaterial`），通过 `materials[material_index]` 统一寻址。descriptor、pipeline layout、寻址方式均不变。如果新模型超出当前 stride，整体提升（如 80→128 字节）。内存浪费可忽略（几百个材质 × 几十字节 = 几 KB）。

**排除的方案：** 每种模型各自 SSBO（破坏全局 Set 0 layout 一致性，descriptor 管理变复杂）；变长 buffer + byte offset 寻址（shader 失去 typed struct，需手动解包，易错）。

### 材质数据流

从阶段二开始建立完整的材质数据流：全局 MaterialBuffer SSBO（Set 0, Binding 2）存储所有材质实例数据，shader 通过 push constant 中的 `material_index` 索引。

**为什么阶段二就完整实现：** push constant 保证的最小大小是 128 字节，mat4 model 已占 64 字节，剩余空间不足以内联材质参数。全局 SSBO 是必经之路，早建立能验证 bindless 纹理采样的完整链路（shader 中 `materials[material_index].base_color_tex` 索引到 bindless 数组），后续每个 pass 直接复用。

### Per-draw 数据演进：Push Constant → Per-instance SSBO

阶段二的 `PushConstantData` 为 `mat4 model` + `uint material_index` = 68 字节，在 128 字节最低保证内。但后续阶段会引入更多 per-instance 数据：

| 阶段 | 新增数据 | 累计大小 |
|------|---------|---------|
| 阶段二 | model(64) + material_index(4) | 68 字节 |
| 阶段六 | + lightmap bindless index + UV scale/offset | ~84+ 字节 |
| M2 | + prev_model(64, motion vectors) | ~148 字节 ← 超出 128 |

超出 128 字节保证后需迁移至 per-instance SSBO（已在阶段四 Instancing 中提前完成）。

---

## Shader 编译与热重载

**选择：运行时编译 + 热重载（仅开发模式）**

Shader 以 GLSL 源码形式存在，运行时根据需要的变体组合用 shaderc 编译成 SPIR-V，编译结果缓存。修改 shader 不需要重新构建项目。

**为什么不做构建时预编译：** 开发效率优先——M1 阶段会频繁修改 shader，热重载的价值极大。

**M1 的可行性：** 变体组合很少（标准 PBR 有无法线贴图、有无阴影接收，大概几个变体），运行时编译开销可忽略。

**升级路径：** 发布时的预编译路径可以后加，开发期和发布期使用不同的 shader 加载策略。

---

## 场景数据接口

**选择：简单渲染列表**

渲染器的输入就是几个数组：mesh 实例数组（mesh 引用 + 材质引用 + 变换矩阵）、光源数组、探针数组、相机结构体。场景加载后填充这些数组，渲染器每帧消费。

**为什么不做 ECS 或 Scene Graph：** M1 是渲染器演示而非引擎，不需要游戏对象管理系统。

**长远兼容性：** 这个接口就是渲染器的"合同"——不管上层以后用什么方式管理场景（ECS、Scene Graph、自定义），最终喂给渲染器的都是这些数组。接口设计不需要以后改。

---

## 配置与调参系统

**选择：ImGui 面板 + 配置结构体**

各个参数统一放在几个结构体里（ShadowConfig、SSAOConfig、PostProcessConfig 等），ImGui 直接操作这些结构体的字段。不做正式的参数注册系统。

**M1 原则：** 能调就行，不需要优雅。

**升级路径：** 需要序列化（保存/加载配置）或参数热重载时再做正式系统。

---

## 深度缓冲与精度

### 格式

**D32Sfloat**（32-bit 浮点深度，无 stencil）。M1 不需要 stencil 操作，纯深度格式能获得更好的深度精度。对后续 CSM 和屏幕空间效果（SSAO、contact shadows）的深度采样更有利。MSAA depth buffer 沿用 D32Sfloat 不变（Reverse-Z 需要浮点深度才能发挥精度分布优势；D24UnormS8 是定点格式且浪费 8-bit stencil，带宽与 D32Sfloat 相同；D16Unorm 精度不足）。

### Reverse-Z

从阶段二起启用 Reverse-Z，避免事后修改所有 depth 相关代码：

| 参数 | 值 |
|------|-----|
| Near plane | 1.0 |
| Far plane | 0.0 |
| 投影矩阵 | 自定义 reverse-Z perspective |
| Depth clear value | 0.0f |
| Depth compare op | `VK_COMPARE_OP_GREATER` |

**长期影响**：所有 depth 相关 pass（CSM、SSAO、contact shadows、depth prepass）统一使用 reverse-Z 约定。远处深度精度显著提升（浮点在 0 附近精度最高）。

---

## Sampler 管理

**选择：Per-texture sampler**

ResourceManager 扩展 sampler 管理，使用已定义的 `SamplerHandle`（generation-based）：

- 新增 `SamplerDesc` 结构体（filter、wrap mode、mip mode）
- `create_sampler()` / `destroy_sampler()` / `get_sampler()` 方法
- `DescriptorManager::register_texture()` 同时接收 `ImageHandle` + `SamplerHandle`
- scene_loader 从 glTF sampler 定义创建对应 sampler
- 缺失 sampler 的 glTF texture 使用 default sampler（linear filter, repeat wrap, mip-linear）

**为什么 per-texture：** glTF 材质中不同纹理可能有不同的采样需求（如 normal map 用 linear + repeat，lightmap 用 linear + clamp），per-texture sampler 忠实还原原始资产意图。

---

## Default 纹理

Texture 模块初始化时创建三个 1×1 default 纹理，注册到 bindless array 获得固定 `BindlessIndex`：

| 名称 | 颜色值 (RGBA) | 用途 |
|------|--------------|------|
| White | (1, 1, 1, 1) | base color / metallic-roughness / occlusion / lightmap 的 neutral 值 |
| Flat Normal | (0.5, 0.5, 1.0, 1.0) | normal map 无扰动（切线空间 Z 朝上） |
| Black | (0, 0, 0, 1) | emissive 无自发光 |

`GPUMaterialData` 中缺失纹理字段填入对应 default 纹理的 `BindlessIndex`，shader 无需特判——统一采样即可获得正确的 neutral 效果。

---

## 纹理格式选择

根据 glTF texture 角色选择 `VkFormat`：

| 角色 | Format | 理由 |
|------|--------|------|
| base color, emissive | `R8G8B8A8_SRGB` | 颜色数据，GPU 自动 gamma 解码 |
| normal, metallic-roughness, occlusion | `R8G8B8A8_UNORM` | 线性数据，原样读取 |

scene_loader 在加载 glTF texture 时根据其引用关系（哪个材质字段引用了这张纹理）决定 format。

---

## GPU 上传策略

**选择：批量上传（immediate command scope）**

Context 提供 immediate command scope：

- `begin_immediate()` → 纯状态切换（reset/begin 内部 command buffer，不返回 `CommandBuffer`）
- `end_immediate()` → submit + `vkQueueWaitIdle`

场景加载时一次 `begin_immediate()`，录制所有 buffer copy + image copy + mip 生成命令，一次 `end_immediate()` 提交。减少 submit 次数和 GPU idle 等待。

`upload_buffer()` 改为录制模式：在活跃的 begin/end_immediate scope 内调用时，只录制 copy 命令到当前 command buffer，不自行 submit。staging buffer 由 Context 收集，`end_immediate()` submit + wait 完成后统一销毁。scope 外调用 `upload_buffer()` 会 assert 失败。

### Image 上传与 Mip 生成

ResourceManager 新增 `upload_image()` 和 `generate_mips()` 方法，与 `upload_buffer()` 同级——通用资源操作，不绑定在特定模块（Texture、IBL、Lightmap 等均复用）。

- `upload_image(ImageHandle, data, size, dst_stage)`：创建 staging buffer + 录制 `vkCmdCopyBufferToImage`（含 UNDEFINED → TRANSFER_DST layout transition）。`dst_stage` 指定最终 barrier 的 `dstStageMask`，调用方按消费者阶段显式指定（fragment 传 `FRAGMENT_SHADER_BIT`，compute 传 `COMPUTE_SHADER_BIT`）
- `generate_mips(ImageHandle)`：逐级 `vkCmdBlitImage` + 每级 layout transition（TRANSFER_DST → TRANSFER_SRC），最终转为 SHADER_READ_ONLY_OPTIMAL

两者均在 begin/end_immediate scope 内调用，遵循录制模式。

### Staging Buffer 收集策略

Context 内部维护 `std::vector<std::pair<VkBuffer, VmaAllocation>> pending_staging_buffers_`。`upload_buffer()` 和 `upload_image()` 每次创建 staging buffer 后 push 进此列表。`end_immediate()` submit + `vkQueueWaitIdle` 完成后，遍历销毁所有 pending staging buffer 并清空列表。

**长期定位**：这是初始加载的长期方案。未来运行时流式加载使用 Transfer Queue 异步上传，是独立系统，不影响此方案。

---

## 场景加载与错误处理

**场景路径**：命令行参数 `--scene`，默认 `assets/Sponza/Sponza.gltf`。长期方向是 GUI 文件选择器。

**参数传递方式**：`main.cpp` 使用 CLI11 解析命令行参数（`--scene`、`--env` 等），`Application::init()` 接收已解析的参数结构体。Application 不感知命令行解析细节。CLI11 依赖详见「IBL 管线 — HDR 环境贴图加载路径」章节。

**错误处理策略**：加载失败（glTF 解析失败、纹理文件缺失、shader 编译失败）一律 log error + abort。开发期使用已知资产，加载失败 = 代码或路径有 bug，应立刻暴露。不做 fallback 到 default 资源——那会掩盖问题。
