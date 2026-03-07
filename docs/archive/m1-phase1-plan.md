# Milestone 1 阶段一：最小可见三角形

> 阶段一的目标是搭建 Vulkan 基础设施并在屏幕上显示一个硬编码的彩色三角形。
> 全部工作集中在 Layer 0（RHI）和最小的 Layer 3（App）壳。
> 高层级阶段划分见 `m1-development-order.md`。

---

## 实现步骤

### Step 1：项目骨架

- 创建顶层 `CMakeLists.txt`（C++20、vcpkg 工具链集成）
- 创建 `vcpkg.json`（声明 GLFW、GLM、spdlog、VMA、shaderc、imgui）
- 创建 `rhi/CMakeLists.txt`（himalaya_rhi static lib）
- 创建 `app/CMakeLists.txt` + `main.cpp`（himalaya_app exe）
- GLFW 窗口创建 + 主循环（空循环 + 事件轮询）
- **验证**：CLion 能构建，运行后出现可关闭的黑窗口

### Step 2：Vulkan 初始化

- `rhi/context.h/cpp`：
  - Instance 创建（启用 Validation Layer）
  - Debug Messenger 回调（`VK_EXT_debug_utils`）
  - Physical Device 选择（优先独立显卡）
  - Logical Device 创建 + Graphics Queue 获取
  - 启用 Vulkan 1.4 核心特性（Dynamic Rendering、Synchronization2、Extended Dynamic State、Descriptor Indexing）
  - VMA Allocator 初始化
  - 显式 `destroy()` 方法
- spdlog 集成（基础日志输出）
- **验证**：启动后控制台输出 GPU 名称，无 Validation Layer 报错

### Step 3：Swapchain + 帧同步

- `rhi/swapchain.h/cpp`：
  - Swapchain 创建（优先 `MAILBOX`，回退 `FIFO`，格式 `B8G8R8A8_SRGB`）
  - Image View 获取
  - 显式 `destroy()` + 重建接口
- `rhi/context` 中添加 FrameData：
  - 2 帧 in-flight 的 fence + semaphore
  - 帧索引轮换
  - 删除队列（deferred deletion）
- 主循环集成：acquire image → submit → present
- **验证**：窗口内容从黑变为 clear color（如蓝色）

### Step 4：Pipeline + Shader 编译

- `rhi/types.h`：基础句柄类型（generation-based）、枚举、格式定义
- `rhi/shader.h/cpp`：
  - shaderc 集成
  - GLSL 源码 → SPIR-V 编译
  - 编译结果缓存
- `rhi/pipeline.h/cpp`：
  - Graphics Pipeline 创建（Dynamic Rendering 适配）
  - Extended Dynamic State 配置
  - Pipeline Layout 创建
- 三角形 shader：`shaders/triangle.vert` + `shaders/triangle.frag`
- CMake 添加构建后 shader 拷贝命令
- **验证**：Pipeline 创建不报错，shader 编译成功

### Step 5：绘制三角形

- `rhi/commands.h/cpp`：
  - Command Buffer wrapper
  - `begin()` / `end()`
  - `begin_rendering()` / `end_rendering()`（Dynamic Rendering）
  - `bind_pipeline()` / `draw()` / `set_viewport()` / `set_scissor()`
  - `pipeline_barrier()`（Synchronization2 API）
- 硬编码三角形顶点（直接写在 vertex shader 中，使用 `gl_VertexIndex`）
- Swapchain image layout transition（通过 barrier）
- **验证**：屏幕上出现彩色三角形 ← 阶段一核心里程碑

### Step 6：基础资源管理

- `rhi/resources.h/cpp`：
  - 资源池（generation-based slot 管理）
  - Buffer 创建（GPU_ONLY、CPU_TO_GPU 等）
  - Image 创建
  - Staging buffer 上传流程（一次性：创建 → 拷贝 → 销毁）
- 三角形改为从 Vertex Buffer 读取顶点
- Pipeline 更新：添加顶点输入描述
- 窗口 resize 处理（Swapchain 重建）
- **验证**：三角形仍正确显示，窗口 resize 不崩溃

### Step 7：ImGui 集成

- ImGui 初始化（适配 Dynamic Rendering 的 Vulkan backend）
- ImGui 需要的 Descriptor Pool 创建
- 每帧 ImGui 渲染集成到主循环
- 基础调试面板（FPS、GPU 信息、VRAM 占用显示）
- **验证**：ImGui 面板正常显示且不影响三角形渲染

---

## 阶段一文件清单

```
rhi/
├── include/himalaya/rhi/
│   ├── types.h            # 句柄（generation-based）、枚举、格式
│   ├── context.h          # Instance, Device, Queue, VMA, 帧同步, 删除队列
│   ├── swapchain.h        # Swapchain 管理
│   ├── shader.h           # GLSL → SPIR-V 编译, 缓存
│   ├── pipeline.h         # Pipeline 创建
│   ├── commands.h         # Command Buffer wrapper
│   └── resources.h        # Buffer, Image 创建, 资源池
└── src/
    ├── context.cpp
    ├── swapchain.cpp
    ├── shader.cpp
    ├── pipeline.cpp
    ├── commands.cpp
    ├── resources.cpp
    └── vma_impl.cpp        # VMA 单头文件库实现

framework/
├── include/himalaya/framework/
│   └── imgui_backend.h    # ImGui 生命周期管理（init, destroy, begin_frame, render）
└── src/
    └── imgui_backend.cpp

app/
├── CMakeLists.txt
└── main.cpp               # 窗口创建, 主循环, 最小 Layer 3 壳

shaders/
├── triangle.vert
└── triangle.frag
```

---

## 阶段一技术笔记

| 主题 | 说明 |
|------|------|
| 无 Render Graph | 阶段一手动管理 barrier 和渲染顺序，Render Graph 在阶段二引入 |
| 无材质系统 | 三角形使用硬编码颜色，材质系统在阶段二引入 |
| 无 Bindless | 阶段一不加载纹理，`descriptors.h/cpp` 延迟到阶段二 |
| 一次性 Staging | 资源上传用 create → upload → destroy 的一次性 staging buffer |
| 单 Queue | 阶段一只使用 Graphics Queue，Transfer Queue 在后续按需引入 |
| 三角形顶点 | Step 5 硬编码在 shader 中（gl_VertexIndex），Step 6 改为 Vertex Buffer |
| 窗口归属 | App 层拥有 GLFW 窗口，传 `GLFWwindow*` 给 RHI 创建 Surface |
| Framework 提前引入 | 原计划阶段二创建 framework 层，因 ImGui backend 需要独立于 app 的封装而提前至阶段一 Step 7 引入，仅含 `imgui_backend.h/cpp` |
| GLFW 回调链 | ImGui GLFW backend 以 `install_callbacks = true` 初始化，链式调用先前注册的回调；因此 ImGui 初始化必须在 app 的 GLFW 回调注册之后 |
| ImGui API 版本 | vcpkg baseline 安装的版本中 `MSAASamples` 和 `PipelineRenderingCreateInfo` 为 `ImGui_ImplVulkan_InitInfo` 顶层字段，无 `ColorAttachmentFormat` 简写字段 |
| Descriptor Pool 规格 | 专用池，4 个 `COMBINED_IMAGE_SAMPLER`，`FREE_DESCRIPTOR_SET_BIT` |
| ImGui 架构决策 | 详见 `../milestone-1/m1-design-decisions.md`「ImGui 集成」章节 |
