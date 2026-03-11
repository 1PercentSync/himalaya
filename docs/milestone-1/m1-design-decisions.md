# Milestone 1：设计决策

> M1 各架构组件的设计选择与理由。接口定义见 `m1-interfaces.md`，长远架构目标见 `../project/architecture.md`，M1 功能范围见 `milestone-1.md`。

---

## 设计原则

既不过度设计（避免在项目初期花太多时间在框架上），也不欠缺考虑（避免以后吃亏需要推翻重来）。每个组件选择的实现级别都预留了向长远目标演进的通道。

---

## Vulkan 1.4 核心特性

M1 使用 Vulkan 1.4，以下 1.3+ 提升为核心的特性从一开始采用：

### Dynamic Rendering

使用 `vkCmdBeginRendering` / `vkCmdEndRendering` 替代传统的 VkRenderPass + VkFramebuffer。

**为什么采用：** 不需要创建和管理 VkRenderPass 和 VkFramebuffer 对象，attachment 在录制时内联指定，与 Render Graph 天然契合。代码量显著减少。

**移动端影响：** TBDR 架构的 tiling 优化可能受影响，但这是 RHI 内部实现细节，移动端适配时可在 RHI 层内替换，不影响上层代码。

### Synchronization2

使用 `vkCmdPipelineBarrier2` 和 `VkImageMemoryBarrier2` 替代旧的 barrier API。

**为什么采用：** 新 API 将 stage 和 access 放在同一结构体中，语义更清晰，更不容易写错组合。Render Graph 的 barrier 自动插入直接受益。

### Extended Dynamic State

Viewport、scissor、cull mode、depth test/write/compare 等状态设为动态，不烘焙进 pipeline。

**为什么采用：** 减少 pipeline 数量（阴影 pass 和主 pass 可共享 pipeline），pipeline 创建更简单。

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
| Managed 资源管理 | 阶段三 | 首批 RG 管理的资源（Depth/Normal/HDR Color Buffer）出现于阶段三 |
| Temporal 资源管理 | 阶段五 | 首个 temporal pass（SSAO temporal filter）出现于阶段五 |

阶段二的 RG 只管 barrier 和状态追踪，资源由外部创建后导入。这不影响后续扩展——pass 声明输入输出的接口从阶段二就固定下来，managed/temporal 是对 RG 内部的增量添加，已有 pass 不需要修改。

#### Managed 资源管理

阶段三引入 RG managed 资源——由 RG 根据声明创建和管理的 image，跨帧缓存，resize 时自动重建。与 imported 资源的区别：imported 由外部创建、外部管理生命周期；managed 由 RG 创建、RG 管理缓存和重建。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 引入 managed 资源管理 | RG 新增 `create_managed_image()` 等 API，根据描述创建资源，跨帧缓存，resize 自动重建 |
| B. 延迟到阶段四或五 | 阶段三仍然外部创建 + import，和阶段二一样 |
| C. 引入简化版 | RG 只做"声明式创建"，不做内存别名 |

**选择 A。** 阶段三新增大量分辨率相关资源（MSAA buffers、HDR color、normal buffer），手动管理创建/销毁/resize 重建代码量大且重复。RG managed 资源集中管理这些逻辑，降低 Renderer 的复杂度。

**演进级别：**

| 级别 | 内容 | 时机 |
|------|------|------|
| L1：RG 管理创建/缓存 | RG 根据声明创建资源，跨帧缓存（desc 不变则复用），resize 时自动重建 | **阶段三** |
| L2：L1 + 内存别名 | 生命周期不重叠的资源共享 VkDeviceMemory（VMA `CAN_ALIAS_BIT`） | **阶段八**（Bloom 链是最自然的引入点） |
| L3：L2 + 自动生命周期推导 | RG 分析 pass 依赖图推导首次/末次使用，自动决定别名 | **M2 考虑** |

**两步注册模式：** `create_managed_image()` 在初始化时注册持久 handle；`use_managed_image()` 每帧调用，返回当前帧的 `RGResourceId`。初始化时注册、每帧使用、销毁时注销——handle 跨帧稳定，RGResourceId 每帧重建。

**Initial/final layout 推导：** Managed 资源的 initial/final layout 由 RG 内部推导（与 imported 不同，不需要调用方指定），因为 RG 完全掌控这些资源的生命周期和使用模式。

**现有 depth buffer 迁移：** 阶段三将阶段二手动管理的 depth buffer 迁移为 managed 资源，删除手动创建/销毁代码。

接口定义详见 `m1-interfaces.md`「Managed 资源 API」。

**闲置资源处理（M1 不实现，备忘）：** Pass 长期 disable 时，其 managed 资源的 backing VkImage 持续占用显存。L2 内存别名解决帧内生命周期不重叠时的复用；跨帧长期闲置可通过显式 sleep/wake 释放。阶段八实现 L2 时同步评估是否需要 sleep/wake。

#### MSAA Resolve 策略

MSAA resolve 需要在 RG 中表达。

| 方案 | 说明 |
|------|------|
| A. Resolve 作为独立 RG pass | 每个 resolve 注册为独立 pass |
| B. Resolve 内建到 RG | RG 标记 sample count，compile 时自动插入 resolve |
| C. Resolve 集成到 Dynamic Rendering | 使用 `VkRenderingAttachmentInfo::resolveImageView` 在渲染结束时自动 resolve |

**选择纯 C。** Vulkan Dynamic Rendering 原生支持 color resolve（`pResolveAttachments`），Vulkan 1.2 核心支持 depth resolve mode（MIN/MAX/SAMPLE_ZERO），zero 额外 pass 开销。Normal resolve 用 AVERAGE + 消费方 shader `normalize()` 是标准做法。

**RG 零改动：** Forward pass 声明写入 MSAA target 和 resolved target（共 4 个资源），RG 只看到"谁读谁写"，自动生成正确 barrier。Resolve destination 需要的 layout 与现有 `Write + ColorAttachment` / `Write + DepthAttachment` 生成的 barrier 完全一致。Resolve 配置完全在 pass callback 内部通过 `VkRenderingAttachmentInfo` 设置。

自定义 resolve（tone-mapped resolve、bilateral normal resolve 等）是高级视觉优化，M1/M2 不涉及。如需，可在对应 pass 内部替换为 custom shader，不影响 RG 和整体架构。

#### Depth Resolve 模式

MSAA depth resolve 用于生成屏幕空间效果所需的 resolved depth buffer。

| 方案 | 说明 |
|------|------|
| A. `VK_RESOLVE_MODE_MIN_BIT` | Reverse-Z 下 MIN = 最远深度（保守遮蔽） |
| B. `VK_RESOLVE_MODE_MAX_BIT` | Reverse-Z 下 MAX = 最近深度（前景优先） |
| C. `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` | 取 sample 0 |
| D. Custom resolve shader | 完全自定义 |

**选择 B（MAX_BIT — 前景深度）。** Reverse-Z 下 MAX = 最大值 = 最近表面（前景）。分析 M1 和 M2 所有 resolved depth 消费者：

| 效果 | 阶段 | 偏好 | 原因 |
|------|------|------|------|
| SSAO | M1 五 | MAX | 重建位置对应可见前景表面，range check 自然处理深度不连续 |
| Contact Shadows | M1 五 | MAX | ray march 起点需准确前景表面位置 |
| DOF | M1 八 | MAX | 防止背景模糊渗入前景轮廓 |
| SSR / SSGI | M2 | MAX | ray march 起点需准确前景表面位置 |
| Camera Motion Blur | M2 | MAX | 防止背景速度渗入前景 |
| God Rays | M2 | MIN | 遮挡检测用最远深度更保守 |

7 个消费者中 6 个偏好 MAX。唯一偏好 MIN 的 God Rays 用 MAX 的瑕疵不可感知（低频柔和效果，radial blur 大量采样稀释边缘像素的深度偏差）。

#### Temporal 资源管理（阶段五引入）

```cpp
// RGResourceDesc 中添加 is_temporal 标记
// 获取 temporal 资源的历史帧版本
ImageHandle get_history_image(RGResourceId id);
```

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

**与 temporal 的区分：** 阶段五引入 temporal 资源后，历史帧数据通过 `get_history_image()` 获取独立的 `RGResourceId`，当前帧和历史帧是**两个不同的资源**，各自声明 `READ` 或 `WRITE`。`READ_WRITE` 不用于 temporal 场景。

### Render Graph 接管范围

Render Graph 接管 acquire image 和 present 之间的**所有** GPU 工作。帧循环变为：acquire image → CPU 准备 → RG compile → RG execute → submit → present。Swapchain image 作为 imported resource 导入 RG，RG 管理其渲染期间的 layout transition。

### ImGui 作为 Render Graph Pass

ImGui 注册为 Render Graph 中的最后一个 pass（独立 pass），声明对最终 color attachment 的读写。`begin_frame()` 在 CPU 准备阶段调用（RG execute 之前），`render()` 在 pass 的 execute 回调中调用。

**为什么不在其他 pass 内部附加：** ImGui 作为独立 pass 更透明，RG 统一管理所有渲染工作。

### Pass 类设计

阶段二只有一两个 pass，直接在 Render Graph 的 lambda 回调中编写渲染逻辑。阶段三 pass 增多后（DepthPrePass、ForwardPass、TonemappingPass），从 lambda 提取为独立的**具体类**。

#### 具体类 vs 虚基类

| 方案 | 说明 |
|------|------|
| A. 虚基类 `RenderPass` | 标准多态，Renderer 持有 `vector<unique_ptr<RenderPass>>` |
| B. 具体类，不用虚函数 | 每个 Pass 是独立类，Renderer 持有具体类型成员 |
| C. 继续 lambda | 不提取类 |

**选择 B。** 阶段三只有 2-3 个 pass，虚函数多态收益有限。具体类让类型更明确、编译期可见。阶段四 pass 数量增长后（shadow、SSAO 等）再引入多态基类——届时有更多样本验证接口设计。

#### 共享依赖获取

| 方案 | 说明 |
|------|------|
| A. 构造函数注入 | Pass 构造时接收所需引用 |
| B. RenderContext / FrameResources 结构体 | execute / register 时传入 |
| C. Pass 持有 Renderer 引用 | Pass 通过 Renderer 间接访问 |

**选择 B。** 构造参数随 pass 增多会爆炸；持有 Renderer 引用导致 Layer 2 反向依赖 Layer 3。`FrameResources` 是一个薄 struct，持有当前帧所有 imported / managed 的 `RGResourceId`，由 Renderer 每帧填充后传给各 pass 的 `register_resources()`。因为选择了具体类（非虚基类），`FrameResources` 是约定惯例而非接口契约——Renderer 有自由度按需偏离。

#### 方法职责分离

| 方案 | 说明 |
|------|------|
| A. `setup()` 一次性 + resize 重调 | Pipeline + 资源都在 setup 中 |
| B. `setup()` + `on_resize()` 分离 | setup 管 pipeline，on_resize 管资源 |

**选择 B。** Pipeline 用 dynamic state 不因 resize 重建；render target 随分辨率重建。init 时先 `setup()` 再 `on_resize()`，resize 时只调 `on_resize()`。

#### 显式 `destroy()`

遵循项目约定（显式 `destroy()` 管理 Vulkan 对象生命周期），每个 Pass 持有 pipeline 和私有资源，需显式销毁。

方法约定详见 `m1-interfaces.md`「Pass 类约定」。

> **演进计划**：阶段四 pass 数量增长后，评估是否引入虚基类。

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

### Descriptor Set 管理方式

Set 0（全局数据：GlobalUBO + LightBuffer + MaterialBuffer）和 Set 1（Bindless 纹理数组）均使用传统 Descriptor Set 分配。Set 0 需要 per-frame 分配（2 帧 in-flight 各一个），Set 1 分配一次长期持有。共 3 个 descriptor set：Set 0 × 2 + Set 1 × 1。

### Descriptor Pool 分离

Set 0 和 Set 1 使用**独立的 Descriptor Pool**：

- **Set 0 Pool**（普通 pool）：容纳 2 UBO + 4 SSBO，maxSets = 2。分配 Set 0 × 2（per-frame）
- **Set 1 Pool**（`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`）：容纳 4096 COMBINED_IMAGE_SAMPLER，maxSets = 1。分配 Set 1 × 1

**为什么分离：** `UPDATE_AFTER_BIND_BIT` 加在 pool 上会影响从该 pool 分配的所有 set。分离后 Set 0 使用普通 pool，职责隔离更清晰。

ImGui 专用 Descriptor Pool 独立于上述两个 pool（已在阶段一实现）。

### Set 0 Per-frame Buffer 策略

- **GlobalUBO × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **LightBuffer × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **MaterialBuffer × 1**：场景加载后一次性创建

Set 0 descriptor 初始化时通过 `vkUpdateDescriptorSets` 写一次（per-frame set 各指向自己的 buffer），之后每帧只 `memcpy` buffer 内容，不需要再 update descriptor。

**Push Descriptors 评估后不采用：** Vulkan 1.4 核心的 Push Descriptors 可以省去 Set 0 的 descriptor pool 分配（约 30 行初始化代码），但每帧需要构造 VkWriteDescriptorSet（约 15 行），总代码量几乎打平。且每个 pipeline layout 只能有一个 push descriptor set，引入了第二种描述符管理模式，增加认知复杂度。收益不足以抵消一致性的损失。

### DescriptorManager 职责

`DescriptorManager`（`rhi/descriptors.h/cpp`）集中管理所有 descriptor 相关资源：

- **持有全局 Descriptor Set Layout**（Set 0 和 Set 1），提供 `get_global_set_layouts()` 方法供 pipeline 创建时使用，确保所有 pipeline 使用一致的 layout
- **管理 Descriptor Pool** 和 **Descriptor Set 分配**
- **管理 Bindless 纹理数组**（注册、注销、slot 分配）

### Descriptor Set Layout 与 Pipeline Layout 的关系

DescriptorManager 持有全局 set layout，Pipeline 创建时通过 `GraphicsPipelineDesc::descriptor_set_layouts` 传入。每个 Pipeline 各自创建 `VkPipelineLayout`。虽然产生重复的 layout 对象（轻量元数据，开销忽略不计），但保留了灵活性——未来 compute pipeline 或特殊 pass 可能需要不同的 layout 组合。

`get_global_set_layouts()` 方法将一致性风险集中在 DescriptorManager 一处，各 pass 统一调用而非自己构造 layout 数组。

### Bindless 纹理数组

**固定上限 4096**：创建时通过 `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` + `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` 设置上限为 4096，实际写入按需增长。4096 个 combined image sampler descriptor 占用极小（一组指针大小的数据），足以覆盖 M1–M3 的所有需求。

> **限制**：bindless 纹理 slot 上限 4096。如果未来需要更多（不太可能在光栅化管线中发生），需要重新创建更大的 descriptor set。

**Slot 回收**：纹理销毁后，其 bindless slot 通过 free list 回收，新纹理注册时优先复用空闲 slot。回收配合 deferred deletion 确保 GPU 不再引用旧 descriptor。这支持场景切换时的纹理卸载/重载。

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

**迁移计划（阶段六执行）**：引入 per-instance SSBO，将 `model`、`material_index`、lightmap 数据等搬入。Push constant 缩减为 `uint instance_id`（4 字节），shader 通过 `instance_data[instance_id]` 读取所有 per-instance 数据。这同时为 M2 的 `prev_model` 预留空间，不再受 push constant 大小限制。

---

## Shader 变体管理

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

## Temporal 数据管理

**选择：手动 Double Buffer 管理**

需要历史数据的 pass 自己维护一对 buffer，每帧手动交换 current/previous 的引用。在 pass 声明里标记"这个资源需要历史帧"，系统帮管理交换。

**为什么不做更复杂的方案：** M1 只有 SSAO 一个 temporal pass（加上自动曝光的 1×1 buffer），复杂度完全可控。

**升级路径：** M2 引入 SSR、SSGI、FSR 后 temporal pass 增多，可在 Render Graph 内建 temporal 资源支持（自动管理 double buffer 和帧间切换）。

---

## ImGui 集成

**选择：Framework 层集成，直接使用 Vulkan 类型**

ImGui backend（初始化、每帧渲染循环、销毁）放在 `framework/` 层（`imgui_backend.h/cpp`），调试面板内容由 `app/` 层构建。

### Framework 层使用 Vulkan 类型（ImGui）

ImGui 的 Vulkan backend 天然需要 `VkDevice`、`VkInstance`、`VkCommandBuffer` 等。这与 Render Graph 一样属于 framework 层的**明确例外**——详见上方「Framework 层 Vulkan 类型使用」章节和 `../project/architecture.md` 的「Framework 层 Vulkan 类型例外」。

### Dynamic Rendering

ImGui Vulkan backend 配置为 Dynamic Rendering 模式，不使用 `VkRenderPass`。

阶段一中 ImGui 在与场景相同的 dynamic rendering pass 中渲染。阶段二引入 Render Graph 后，ImGui 注册为 RG 中的独立 pass（见上方「Render Graph — ImGui 作为 Render Graph Pass」章节）。

### Descriptor Pool

ImGui 使用专用 Descriptor Pool，与渲染器自身的 descriptor 管理完全隔离。

### CommandBuffer::handle()

为 `CommandBuffer` wrapper 添加 `handle()` 方法暴露底层 `VkCommandBuffer`，供 ImGui 等第三方库直接录制命令。

### 调试面板

**数据指标**

| 数据 | 来源 |
|------|------|
| FPS + 帧时间 | `FrameStats` 采样器（1 秒周期结算，取平均值） |
| 1% Low FPS + 帧时间 | `FrameStats` 采样器（周期内最慢 1% 帧的平均值） |
| VRAM 占用 | `Context::query_vram_usage()`（基于 `vmaGetHeapBudgets()`，`VK_EXT_memory_budget`），不做 fallback |
| GPU 名称 | `Context::gpu_name`（init 时缓存自 `vkGetPhysicalDeviceProperties()`） |
| 窗口分辨率 | `Swapchain::extent` |

**运行时控件**

| 控件 | 作用 |
|------|------|
| VSync | 切换 `Swapchain::vsync`，触发 swapchain 重建（FIFO / MAILBOX） |
| Log Level | 运行时切换 `spdlog` 日志级别 |

### Docking

启用 `ImGuiConfigFlags_DockingEnable`，支持 ImGui 窗口停靠布局，为后续多面板调参做准备。

---

## App 层设计

**选择：Application + Renderer 分离**

阶段二期间 `Application` 作为 Composition Root 持有所有子系统，通过注释分组表达层次。初始化和销毁顺序由 `init()` / `destroy()` 显式控制，不依赖成员声明顺序。

阶段三开始时提取 `Renderer` 类，将渲染子系统从 Application 分离。Application 管应用逻辑（窗口、输入、UI），Renderer 管渲染管线（RG、pass、资源）。

### Renderer 提取时机

| 方案 | 说明 | 优势 | 劣势 |
|------|------|------|------|
| A. 阶段三开头先重构 | 第一个 Step 专门做提取 | 后续 Step 在干净架构上工作 | 重构时还不知道阶段三具体需要什么 |
| B. 阶段三中间自然提取 | 写到明显臃肿时再提取 | 有实际经验指导 | 中间重构打断功能开发 |
| C. 阶段三结束后再提取 | 仍用 Application 持有一切 | 延迟决策 | Application 过于臃肿 |

**选择 A。** 阶段二结束后 Application 已有约 10 个成员，阶段三至少再加 5-6 个（MSAA 资源、Normal buffer、HDR buffer、IBL 资源、PrePass pipeline 等）。阶段二的完整经验已足够指导提取——已知哪些是 RHI 基础设施、哪些是框架组件、哪些是 app 逻辑。

### 所有权划分

**Renderer 持有（渲染子系统）：**

| 类别 | 成员 | 说明 |
|------|------|------|
| 渲染基础设施 | `render_graph_` | pass 编排 |
| | `shader_compiler_` | shader 编译 |
| | `material_system_` | 材质 SSBO 管理 |
| Pipeline | `forward_pipeline_`（原 `unlit_pipeline_`） | 阶段三新增 depth_prepass、tonemapping 等 |
| 渲染目标 | `depth_image_` | 阶段三新增 MSAA/HDR/Normal buffer |
| 共享渲染资源 | `default_sampler_`, `default_textures_` | 场景加载通过 accessor 获取 |
| Per-frame GPU 数据 | `global_ubo_buffers_`, `light_buffers_` | UBO/SSBO 填充是渲染关注点 |
| Swapchain 追踪 | `swapchain_image_handles_` | RG import 用 |

Renderer 持有的非 owning 引用（从 Application 接收）：`context_*`、`swapchain_*`、`resource_manager_*`、`descriptor_manager_*`、`imgui_backend_*`。

**Application 保留（平台 + App 逻辑）：**

| 类别 | 成员 |
|------|------|
| 平台 | `window_`, `framebuffer_resized_` |
| RHI 基础设施（顶层拥有） | `context_`, `swapchain_`, `resource_manager_`, `descriptor_manager_` |
| ImGui | `imgui_backend_` |
| App 模块 | `camera_`, `camera_controller_`, `debug_ui_`, `scene_loader_`, **`renderer_`** |
| 每帧场景数据 | `scene_render_data_`, `cull_result_` |
| 灯光/调试参数 | `default_lights_`, light yaw/pitch/intensity, 拖拽状态 |
| 渲染参数 | `ambient_intensity_`, `exposure_` |
| 帧状态 | `vsync_changed_`, `image_index_` |

> `resource_manager_` 和 `descriptor_manager_` 归 Application 持有，因为 `scene_loader_` 加载时也要用。Renderer 和 SceneLoader 都是使用者，不是拥有者。

### Resize 两阶段

`swapchain_image_handles_` 引用旧 VkImage，必须在 `swapchain_.recreate()` 之前注销；新资源在 recreate 之后创建：

```cpp
void Application::handle_resize() {
    vkQueueWaitIdle(context_.graphics_queue);
    renderer_.on_swapchain_invalidated();  // unregister old + destroy depth
    swapchain_.recreate(context_, window_);
    renderer_.on_swapchain_recreated();    // register new + create depth
}
```

### UBO/SSBO 填充归属

`GlobalUniformData` 的布局是 shader 约定，属于渲染层。Application 只传语义数据（`RenderInput`），Renderer 打包为 GPU 格式。

### App 层模块接口

App 层各模块（DebugUI、CameraController、SceneLoader、Renderer）不追求统一接口，各自按实际职责设计方法。

**为什么不统一：** 四个模块职责差异大，统一接口是为统一而统一，没有实际收益。

接口定义见 `m1-interfaces.md`。

---

## 配置与调参系统

**选择：ImGui 面板 + 配置结构体**

各个参数统一放在几个结构体里（ShadowConfig、SSAOConfig、PostProcessConfig 等），ImGui 直接操作这些结构体的字段。不做正式的参数注册系统。

**M1 原则：** 能调就行，不需要优雅。

**升级路径：** 需要序列化（保存/加载配置）或参数热重载时再做正式系统。

---

## 深度缓冲与精度

### 格式

**D32Sfloat**（32-bit 浮点深度，无 stencil）。M1 不需要 stencil 操作，纯深度格式能获得更好的深度精度。对后续 CSM 和屏幕空间效果（SSAO、contact shadows）的深度采样更有利。

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

## MSAA 配置策略

**选择：运行时可配置（1x / 2x / 4x / 8x），默认 4x**

| 方案 | 说明 |
|------|------|
| A. 固定 4x | 最常见的平衡点 |
| B. 可配置（2x/4x/8x） | 运行时切换，需重建 MSAA 资源和 pipeline |
| C. 阶段三固定 4x，后续加配置 | 先简单后灵活 |

**选择 B，并扩展可选级别包含 1x。** 1x（无 MSAA）用于调试和性能基准对比。1x 时 forward pass 不配置 resolve，只声明 2 个资源而非 4 个。

不使用 `VK_EXT_extended_dynamic_state3`（动态 rasterization samples）。该扩展非 Vulkan 1.4 核心，MSAA 切换是低频操作，重建 pipeline 开销可忽略。

### 切换机制

切换 MSAA 时需要重建：

| 需重建 | 原因 |
|--------|------|
| 所有 MSAA pipeline | `rasterizationSamples` 烘焙在 pipeline 中 |
| 所有 MSAA managed 资源 | sample_count 变化 |

不需要重建：descriptor layout、resolved target、非 MSAA pipeline。

**重建触发流程**与 resize/vsync 统一模式。RG `update_managed_desc()` 更新 MSAA 资源描述，handle 保持不变（见「Managed 资源管理」章节）。

```cpp
// Application 检测 DebugUI 的 MSAA 变更
if (actions.msaa_changed) {
    renderer_.handle_msaa_change(actions.new_sample_count);
}

// Renderer 处理 MSAA 变更
void Renderer::handle_msaa_change(uint32_t new_sample_count) {
    vkQueueWaitIdle(...);
    // 1. 更新 MSAA managed 资源 desc
    render_graph_.update_managed_desc(managed_msaa_color_, new_msaa_color_desc);
    render_graph_.update_managed_desc(managed_msaa_depth_, new_msaa_depth_desc);
    // 2. 重建所有 MSAA pipeline
    rebuild_pipelines(new_sample_count);
}
```

---

## Tonemapping

**选择：ACES fullscreen fragment shader**

阶段三引入 HDR color buffer 后，需要 tonemapping 将 HDR 映射到 SDR 显示范围。

| 方案 | 说明 |
|------|------|
| A. 临时 blit/copy（HDR→SDR 截断） | 高光全部 clamp 为纯白，效果差 |
| B. 简易 Reinhard | `color / (color + 1.0)`，偏平 |
| C. ACES fullscreen fragment shader | 工业标准 tonemapping |

**选择 C。** PBR 光照输出亮度范围是 HDR 的（镜面高光轻松超过 1.0），直接截断到 [0,1] 丢失大量信息。阶段三直接实现 ACES，不用 Reinhard 过渡。后续阶段八可替换为 Khronos PBR Neutral（项目 Tonemapping 演进见 `../project/technical-decisions.md`）。

**为什么 fullscreen fragment 而非 compute shader：** SRGB swapchain format（`VK_FORMAT_B8G8R8A8_SRGB`）通常不支持 `VK_IMAGE_USAGE_STORAGE_BIT`，无法作为 compute shader 的 storage image 输出。Fullscreen fragment shader 通过硬件自动 linear→sRGB 转换写入 SRGB swapchain。

**Fullscreen triangle：** 使用 hardcoded fullscreen triangle（vertex shader 中根据 `gl_VertexIndex` 生成覆盖全屏的三角形，无顶点输入），比全屏 quad 少一个三角形、避免对角线像素被光栅化两次。

### ImGui 与 Tonemapping 的关系

ImGui 作为 debug overlay 直接渲染到 swapchain image（不经过 MSAA 或 tonemapping），与阶段二一致。渲染到中间 LDR buffer 无实际优势。

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

- `upload_image(ImageHandle, data, size)`：创建 staging buffer + 录制 `vkCmdCopyBufferToImage`（含 UNDEFINED → TRANSFER_DST layout transition）
- `generate_mips(ImageHandle)`：逐级 `vkCmdBlitImage` + 每级 layout transition（TRANSFER_DST → TRANSFER_SRC），最终转为 SHADER_READ_ONLY_OPTIMAL

两者均在 begin/end_immediate scope 内调用，遵循录制模式。

### Staging Buffer 收集策略

Context 内部维护 `std::vector<std::pair<VkBuffer, VmaAllocation>> pending_staging_buffers_`。`upload_buffer()` 和 `upload_image()` 每次创建 staging buffer 后 push 进此列表。`end_immediate()` submit + `vkQueueWaitIdle` 完成后，遍历销毁所有 pending staging buffer 并清空列表。

**长期定位**：这是初始加载的长期方案。未来运行时流式加载使用 Transfer Queue 异步上传，是独立系统，不影响此方案。

---

## 场景加载与错误处理

**场景路径**：命令行参数，不传参时使用写死的默认路径（`assets/Sponza/Sponza.gltf`）。长期方向是 GUI 文件选择器。

**参数传递方式**：`main.cpp` 负责解析 `argc/argv`，`Application::init()` 只接收已解析的 `const std::string& scene_path`。Application 不感知命令行解析细节。

**错误处理策略**：加载失败（glTF 解析失败、纹理文件缺失、shader 编译失败）一律 log error + abort。开发期使用已知资产，加载失败 = 代码或路径有 bug，应立刻暴露。不做 fallback 到 default 资源——那会掩盖问题。

---

## 资源句柄设计

**选择：Generation-based 句柄**

资源池管理的资源句柄（Image、Buffer、Sampler）包含 index + generation 两个字段。资源池每个 slot 维护 generation 计数器，资源销毁时 generation 递增，使用句柄时比对 generation。

Pipeline 不纳入资源池管理——Pipeline 所有权始终是单一且明确的（pass 或 MaterialTemplate 持有），不像 Image/Buffer 被多个 pass、descriptor set、render graph 交叉引用。生命周期保护由 Vulkan Validation Layer 兜底（检测已销毁 VkPipeline 的使用、设备销毁时的泄漏报告），无需 generation 机制。

**为什么加 generation：** 检测所有 use-after-free 错误——旧句柄引用已销毁并被新资源复用的 slot 时立即报错，而非静默访问错误资源。开销仅为一次 `uint32_t` 比较。

---

## 对象生命周期

**选择：显式 `destroy()` 方法**

不使用 RAII 析构函数管理 Vulkan 对象。所有持有 Vulkan 资源的类提供 `destroy()` 方法，调用方负责在正确时机调用。

**为什么不用 RAII：** Vulkan 对象销毁顺序重要且复杂（Device 必须最后销毁、依赖资源必须先于被依赖资源销毁），析构函数的隐式调用顺序可能踩坑。显式方法让销毁时机完全可控。

---

## 帧同步

**选择：2 Frames in Flight**

每帧的 CPU 侧资源（command pool、command buffer、fence、semaphore）分为 2 套，通过帧索引轮换。每帧开始时等前 N 帧的 fence，执行该帧的延迟删除队列，然后录制新命令。

**为什么不是 3 帧：** CPU 端无复杂游戏逻辑，帧时间不会剧烈波动。2 帧的更低延迟对交互式场景漫游更好。后续如需改为 3 帧只需修改常量。

---

## 错误处理与调试

**Vulkan 错误处理：** `VK_CHECK` 宏 + abort。Vulkan API 错误在开发期几乎都是编程错误，不需要运行时恢复。

**Validation Layer：** 开发期常开，所有 Vulkan 调用经过验证。

**Debug Utils：** 集成 `VK_EXT_debug_utils`：
- 为资源添加 debug name（RenderDoc 和 Validation Layer 报错时显示可读名称）
- 为 command buffer 区域添加标记（GPU profiler 中显示 pass 名称）

---

## 总结

| 组件 | M1 实现级别 | 核心理由 |
|------|------------|----------|
| Vulkan 特性 | Dynamic Rendering + Sync2 + Extended Dynamic State | 1.4 核心，简化代码 |
| Render Graph | 手动编排 + barrier 辅助（Sync2 API），渐进增加 transient/temporal | 减痛点但不过度投资 |
| 资源 / 描述符 | Bindless 纹理（4096 上限）+ 传统 Descriptor Set + Push Constant + DescriptorManager 集中管理 | Bindless 早期投资值得，不用 Push Descriptors |
| 资源句柄 | Generation-based（index + generation） | 捕获 use-after-free |
| 对象生命周期 | 显式 `destroy()` | 销毁顺序可控 |
| 帧同步 | 2 Frames in Flight | 低延迟，够用 |
| 顶点格式 | 统一格式（position + normal + uv0 + tangent + uv1） | 简化 pipeline 管理，M1 够用 |
| 材质系统 | 代码定义 + 固定数据结构 + 全局 SSBO（阶段二起） | 简单直接，GPU 端无法运行时灵活 |
| Shader 变体 | 运行时编译 + 热重载 | 开发效率优先 |
| 场景数据 | 渲染列表（完整结构一次定义，按需填充） | 简单解耦够用 |
| Temporal 数据 | 手动 Double Buffer | M1 只有 SSAO 需要 |
| 调参 | ImGui + 配置结构体 | 能调就行 |
| ImGui 集成 | Framework 层，Dynamic Rendering，专用 Descriptor Pool，RG 独立 pass | 统一由 RG 管理 |
| Pass 抽象 | 阶段三引入（阶段二用 lambda 回调） | 有实际经验后再提取 |
| App 层 | Application 持有全部（注释分组）+ 模块按需设计接口 | Composition Root，阶段三提取 Renderer |
| 错误处理 | VK_CHECK + Validation Layer + Debug Utils | 开发期全面检测 |
| 深度缓冲 | D32Sfloat + Reverse-Z（阶段二起） | 远处精度提升，所有 depth pass 统一约定 |
| MSAA | 运行时可配置 1x/2x/4x/8x，默认 4x | 调试灵活，切换重建 pipeline + managed 资源 |
| Tonemapping | ACES fullscreen fragment shader | SRGB swapchain 不支持 compute，后续可替换 PBR Neutral |
| Sampler | Per-texture sampler，ResourceManager 管理 | glTF sampler 忠实还原，default: linear repeat mip-linear |
| Default 纹理 | 3 个 1×1（white / flat normal / black） | 材质缺失纹理填 default index，shader 无需特判 |
| 纹理格式 | 按角色选（SRGB / UNORM） | 颜色数据 gamma 解码，线性数据原样读取 |
| GPU 上传 | 批量 immediate command scope | 一次 submit，流式加载是独立系统 |
