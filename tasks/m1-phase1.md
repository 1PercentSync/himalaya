# Milestone 1 阶段一：任务清单

> 详细设计见 `docs/m1-phase1-plan.md`

---

## Step 1：项目骨架

- [x] 创建顶层 `CMakeLists.txt`（cmake_minimum_required、project、vcpkg 工具链、C++20、add_subdirectory）
- [x] 创建 `vcpkg.json`（glfw3、glm、spdlog、vulkan-memory-allocator、shaderc、imgui）
- [x] 创建 `rhi/CMakeLists.txt`（himalaya_rhi static lib、include 路径、依赖链接）
- [x] 创建 `app/CMakeLists.txt`（himalaya_app exe、链接 himalaya_rhi）
- [x] 创建 `app/main.cpp`（GLFW 窗口创建 + 空主循环 + 事件轮询）
- [x] 验证：CLion 构建通过，运行后出现可关闭的黑窗口

## Step 2：Vulkan 初始化

- [x] 创建 `rhi/include/himalaya/rhi/context.h`
- [x] 创建 `rhi/src/context.cpp`
- [x] Instance 创建（启用 Validation Layer + debug_utils 扩展）
- [x] Debug Messenger 回调
- [x] Physical Device 选择
- [x] Logical Device + Queue 创建（启用 1.4 核心特性）
- [x] VMA Allocator 初始化
- [x] `destroy()` 方法（按反序销毁所有对象）
- [x] 集成 spdlog 日志
- [x] `app/main.cpp` 中调用 Context 初始化和销毁
- [x] 验证：启动后控制台输出 GPU 名称，无 Validation 报错

## Step 3：Swapchain + 帧同步

- [x] 创建 `rhi/include/himalaya/rhi/swapchain.h`
- [x] 创建 `rhi/src/swapchain.cpp`
- [x] Swapchain 创建（format、present mode、extent 选择）
- [x] Image View 获取
- [x] Context 中添加 FrameData（command pool、command buffer、fence、semaphore × 2 帧）
- [x] 删除队列（DeletionQueue）
- [x] 帧索引轮换逻辑
- [x] 主循环：wait fence → acquire image → begin cmd → clear → end cmd → submit → present
- [x] 验证：窗口显示纯色 clear color

## Step 4：Pipeline + Shader 编译

- [ ] 创建 `rhi/include/himalaya/rhi/types.h`（句柄、枚举、格式）
- [ ] 创建 `rhi/include/himalaya/rhi/shader.h` + `rhi/src/shader.cpp`
- [ ] shaderc 集成（编译 GLSL 源码为 SPIR-V）
- [ ] 编译结果内存缓存
- [ ] 创建 `rhi/include/himalaya/rhi/pipeline.h` + `rhi/src/pipeline.cpp`
- [ ] Graphics Pipeline 创建（适配 Dynamic Rendering、Extended Dynamic State）
- [ ] Pipeline Layout 创建
- [ ] 创建 `shaders/triangle.vert`（硬编码三角形顶点 + 颜色）
- [ ] 创建 `shaders/triangle.frag`（输出插值颜色）
- [ ] CMakeLists.txt 添加 shader 拷贝到 build 目录的命令
- [ ] 验证：Pipeline 创建成功，无报错

## Step 5：绘制三角形

- [ ] 创建 `rhi/include/himalaya/rhi/commands.h` + `rhi/src/commands.cpp`
- [ ] Command Buffer wrapper：begin/end、begin_rendering/end_rendering
- [ ] bind_pipeline、draw、set_viewport、set_scissor
- [ ] pipeline_barrier（Synchronization2 API）
- [ ] Swapchain image layout transition 集成到帧循环
- [ ] 渲染三角形到 Swapchain image
- [ ] 验证：屏幕上出现彩色三角形

## Step 6：基础资源管理

- [ ] 创建 `rhi/include/himalaya/rhi/resources.h` + `rhi/src/resources.cpp`
- [ ] 资源池实现（generation-based slot 分配/回收）
- [ ] Buffer 创建接口（BufferDesc → BufferHandle）
- [ ] Image 创建接口（ImageDesc → ImageHandle）
- [ ] Staging buffer 上传流程
- [ ] 创建 Vertex Buffer，上传三角形顶点数据
- [ ] Pipeline 更新：添加顶点输入属性描述
- [ ] Shader 更新：从顶点属性读取位置和颜色
- [ ] 窗口 resize 处理（Swapchain 重建）
- [ ] 窗口最小化处理（extent 为 0 时暂停渲染）
- [ ] 验证：三角形正确显示，resize 和最小化不崩溃

## Step 7：ImGui 集成

- [ ] ImGui Vulkan backend 初始化（适配 Dynamic Rendering）
- [ ] 创建 ImGui 专用 Descriptor Pool
- [ ] ImGui GLFW backend 初始化
- [ ] 每帧 ImGui 渲染集成（NewFrame → Render → draw data 录制到 command buffer）
- [ ] 基础调试面板（FPS、GPU 名称、窗口分辨率、VRAM 占用）
- [ ] 验证：ImGui 面板正常显示，三角形渲染不受影响
