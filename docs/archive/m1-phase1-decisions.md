# M1 阶段一设计决策：最小可见三角形

> 阶段一：Vulkan 初始化、VMA、Shader 编译、Command Buffer、基础资源、Pipeline、硬编码三角形。
> 核心决策摘要见 `../milestone-1/m1-design-decisions-core.md`。

---

## 决策总览

| 组件 | 实现级别 | 核心理由 |
|------|----------|----------|
| Vulkan 特性 | Dynamic Rendering + Sync2 + Extended Dynamic State | 1.4 核心，简化代码 |
| 资源句柄 | Generation-based（index + generation） | 捕获 use-after-free |
| 对象生命周期 | 显式 `destroy()` | 销毁顺序可控 |
| 帧同步 | 2 Frames in Flight | 低延迟，够用 |
| 错误处理 | VK_CHECK + Validation Layer + Debug Utils | 开发期全面检测 |
| ImGui 集成 | Framework 层，Dynamic Rendering，专用 Descriptor Pool | 与场景同 pass 渲染，后续迁移为独立 RG pass |

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

## 资源句柄设计

**选择：Generation-based 句柄**

资源池管理的资源句柄（Image、Buffer、Sampler）包含 index + generation 两个字段。资源池每个 slot 维护 generation 计数器，资源销毁时 generation 递增，使用句柄时比对 generation。

Pipeline 不纳入资源池管理——Pipeline 所有权始终是单一且明确的（pass 持有），不像 Image/Buffer 被多个 pass、descriptor set、render graph 交叉引用。生命周期保护由 Vulkan Validation Layer 兜底（检测已销毁 VkPipeline 的使用、设备销毁时的泄漏报告），无需 generation 机制。

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

## ImGui 集成

**选择：Framework 层集成，直接使用 Vulkan 类型**

ImGui backend（初始化、每帧渲染循环、销毁）放在 `framework/` 层（`imgui_backend.h/cpp`），调试面板内容由 `app/` 层构建。

### Framework 层使用 Vulkan 类型（ImGui）

ImGui 的 Vulkan backend 天然需要 `VkDevice`、`VkInstance`、`VkCommandBuffer` 等。这与 Render Graph 一样属于 framework 层的**明确例外**——详见 `../project/architecture.md` 的「Framework 层 Vulkan 类型例外」。

### Dynamic Rendering

ImGui Vulkan backend 配置为 Dynamic Rendering 模式，不使用 `VkRenderPass`。

阶段一中 ImGui 在与场景相同的 dynamic rendering pass 中渲染。

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
