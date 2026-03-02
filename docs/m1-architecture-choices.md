# Milestone 1：架构实现选择

> 本文档记录 M1 阶段各架构组件的具体实现级别。
> 长远架构目标见 `architecture.md`，M1 功能范围见 `milestone-1.md`。

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

Set 0（全局数据：GlobalUBO + LightBuffer + MaterialBuffer）和 Set 1（Bindless 纹理数组）均使用传统 Descriptor Set 分配。Set 0 需要 per-frame 分配（2 帧 in-flight 各一个），Set 1 分配一次长期持有。

**Push Descriptors 评估后不采用：** Vulkan 1.4 核心的 Push Descriptors 可以省去 Set 0 的 descriptor pool 分配（约 30 行初始化代码），但每帧需要构造 VkWriteDescriptorSet（约 15 行），总代码量几乎打平。且每个 pipeline layout 只能有一个 push descriptor set，引入了第二种描述符管理模式，增加认知复杂度。收益不足以抵消一致性的损失。

---

## 材质系统

**选择：代码定义 + 运行时参数布局**

M1 只有一种材质（标准 PBR），材质类型在代码中定义。但材质参数的布局是运行时描述（"这个材质有哪些纹理、哪些标量参数"），而不是编译时硬编码结构体。M1 阶段这个描述在代码里硬编码，但接口是通用的。

**为什么不做完整数据驱动：** M1 只有标准 PBR 一种材质，复杂的数据驱动系统（材质定义文件、自动生成 shader 变体）投入产出比不高。

**升级路径：** 以后加 Burley、SSS、卡通渲染等新着色模型时，只需添加新的参数布局描述和对应 shader，不需要改材质系统核心代码。后续可将硬编码描述改为从文件加载。

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

### Framework 层使用 Vulkan 类型

架构文档约束"上层不接触 Vulkan 类型"，但 ImGui 的 Vulkan backend 天然需要 `VkDevice`、`VkInstance`、`VkCommandBuffer` 等。当前实现直接传递这些类型，这是**暂时性的实现**，不是对架构约束的例外。阶段二 RHI 抽象完善后，framework 层应通过 RHI 接口获取所需信息，不再直接接触 Vulkan 类型。

### Dynamic Rendering

ImGui Vulkan backend 配置为 Dynamic Rendering 模式，不使用 `VkRenderPass`。ImGui 在与场景相同的 dynamic rendering pass 中渲染（场景绘制之后、`end_rendering` 之前），避免额外的 pass 和 barrier 开销。

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

## 配置与调参系统

**选择：ImGui 面板 + 配置结构体**

各个参数统一放在几个结构体里（ShadowConfig、SSAOConfig、PostProcessConfig 等），ImGui 直接操作这些结构体的字段。不做正式的参数注册系统。

**M1 原则：** 能调就行，不需要优雅。

**升级路径：** 需要序列化（保存/加载配置）或参数热重载时再做正式系统。

---

## 资源句柄设计

**选择：Generation-based 句柄**

所有资源句柄（Image、Buffer、Pipeline、Sampler）包含 index + generation 两个字段。资源池每个 slot 维护 generation 计数器，资源销毁时 generation 递增，使用句柄时比对 generation。

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
| Render Graph | 手动编排 + barrier 辅助（Sync2 API） | 减痛点但不过度投资 |
| 资源 / 描述符 | Bindless 纹理 + 传统 Descriptor Set + Push Constant | Bindless 早期投资值得，不用 Push Descriptors |
| 资源句柄 | Generation-based（index + generation） | 捕获 use-after-free |
| 对象生命周期 | 显式 `destroy()` | 销毁顺序可控 |
| 帧同步 | 2 Frames in Flight | 低延迟，够用 |
| 材质系统 | 代码定义 + 运行时参数布局 | M1 简单但预留扩展 |
| Shader 变体 | 运行时编译 + 热重载 | 开发效率优先 |
| 场景数据 | 渲染列表 | 简单解耦够用 |
| Temporal 数据 | 手动 Double Buffer | M1 只有 SSAO 需要 |
| 调参 | ImGui + 配置结构体 | 能调就行 |
| ImGui 集成 | Framework 层，Dynamic Rendering，专用 Descriptor Pool | 同 pass 渲染，零额外 barrier |
| 错误处理 | VK_CHECK + Validation Layer + Debug Utils | 开发期全面检测 |
