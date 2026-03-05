# Milestone 1 阶段二：基础渲染管线

> 阶段二的目标是搭建完整的渲染管线骨架：加载 glTF 场景、用基础 Lit shader 渲染、相机自由漫游。
> 涉及 Layer 0 补全（Descriptor 管理）、Layer 1 框架建设、Layer 3 重构与场景加载。
> 高层级阶段划分见 `m1-development-order.md`，架构决策见 `m1-architecture-choices.md`。

---

## 阶段二技术决策摘要

以下决策在阶段二设计讨论中确定，详细理由见 `m1-architecture-choices.md`：

| 决策项 | 选择 | 要点 |
|--------|------|------|
| Render Graph 资源管理 | 阶段二只管 barrier + 资源导入 | Transient 资源阶段三加，temporal 阶段五加 |
| RG 资源引用 | Typed handle（`RGResourceId`） | 编译期类型安全，不用字符串 |
| RG 接管范围 | acquire 和 present 之间的所有 GPU 工作 | Swapchain image 作为 imported resource |
| ImGui 在 RG 中 | 独立 pass（最后一个） | 统一由 RG 管理 |
| Pass 抽象基类 | 推迟到阶段三 | 阶段二用 lambda 回调 |
| Descriptor 管理 | DescriptorManager 集中管理，提供 `get_global_set_layouts()` | 一致性风险集中在一处 |
| Bindless 纹理上限 | 固定 4096 | `VARIABLE_DESCRIPTOR_COUNT` + `PARTIALLY_BOUND` |
| Bindless slot 回收 | Free list + deferred deletion | 支持场景切换 |
| glTF 加载库 | fastgltf | 现代 C++17 风格，解析后立即转入自有数据结构 |
| 图像解码 | stb_image | JPEG/PNG 解码 |
| 纹理格式 | 阶段二直接加载 JPEG/PNG | BC 压缩推迟，是后续优化项 |
| 顶点格式 | 统一（position + normal + uv0 + tangent + uv1） | 缺失属性填默认值 |
| 阶段二 shader | 基础 Lit（Lambert + 硬编码方向光） | 验证 normal/tangent 数据链路 |
| 材质数据流 | 全局 Material SSBO + push constant（完整实现） | 验证 bindless 完整链路 |
| 相机 | Camera 数据在 framework，CameraController 在 app | 渲染器不知道输入如何产生 |
| Y-flip | 负 viewport height（延续阶段一） | 心智负担最小，Vulkan 核心保证 |
| 深度范围 | `GLM_FORCE_DEPTH_ZERO_TO_ONE` | Vulkan 深度 [0,1] |
| SceneRenderData | 完整结构一次定义，阶段二只填需要的字段 | 空 span 零成本 |
| 视锥剔除 | 阶段二实现 | AABB-frustum，验证 world_bounds 正确性 |
| Mip 生成 | GPU 端 vkCmdBlitImage | 标准做法，写一次复用 |
| App 层 | Application 持有全部（注释分组）+ 模块按需接口 | Composition Root，阶段三提取 Renderer |

---

## 实现步骤

### Step 1：App 层重构

- 从 `main.cpp` 拆分出 `app/application.h/cpp`（窗口管理、主循环骨架、初始化/销毁序列）
- 从 `main.cpp` 拆分出 `app/debug_ui.h/cpp`（ImGui 面板内容构建）
- `main.cpp` 只保留 `main()` 入口，创建 Application 并运行
- 现有功能（三角形渲染、debug 面板）在重构后保持不变
- **验证**：编译通过，运行效果与重构前一致

#### Application 类设计

Application 持有所有子系统，通过注释分组表达层次：

```cpp
class Application {
    // --- 窗口 ---
    GLFWwindow* window_;
    bool framebuffer_resized_;

    // --- RHI 基础设施 ---
    rhi::Context context_;
    rhi::Swapchain swapchain_;
    rhi::ResourceManager resource_manager_;

    // --- Framework ---
    framework::ImGuiBackend imgui_backend_;

    // --- App 模块 ---
    DebugUI debug_ui_;

    // --- 阶段一临时资源（Step 7 移除） ---
    // triangle pipeline, vertex buffer 等
};
```

后续 Step 加入的成员按相同分组规则添加（如 Step 2 在 RHI 区加 `DescriptorManager`，Step 3 在 Framework 区加 `RenderGraph`，Step 4 在 App 模块区加 `CameraController`）。

#### 主循环结构

`Application::run()` 内含完整循环，通过私有方法分解为四步：

```
run() → while loop:
  glfwPollEvents()
  minimize 暂停处理
  begin_frame()   — fence wait → deletion flush → acquire → imgui begin
  update()        — debug panel（后续加 camera update、culling）
  render()        — 录制 command buffer（后续改为 RG compile/execute）
  end_frame()     — submit → present → swapchain 重建检查 → advance frame
```

#### DebugUI 接口

DebugUI 每帧通过参数 struct 接收所需数据，返回用户操作结果：

```cpp
struct DebugUIContext {
    float delta_time;           // FrameStats 采样用
    rhi::Context& context;      // GPU 名称、VRAM
    rhi::Swapchain& swapchain;  // 分辨率、VSync 状态
    // 后续扩展：scene stats, camera info...
};

struct DebugUIActions {
    bool vsync_toggled = false;
    // 后续扩展：shader reload, render mode change...
};

class DebugUI {
public:
    DebugUIActions draw(const DebugUIContext& ctx);
private:
    FrameStats frame_stats_;    // 内部持有，外部不关心
};
```

`framebuffer_resized` 由 GLFW callback 直接写 Application 成员变量，不经过 DebugUI。

### Step 2：Descriptor 管理

- 创建 `rhi/include/himalaya/rhi/descriptors.h` + `rhi/src/descriptors.cpp`
- `DescriptorManager` 实现：
  - 创建 Set 0 layout（GlobalUBO binding 0 + LightBuffer binding 1 + MaterialBuffer binding 2）
  - 创建 Set 1 layout（bindless sampler2D array，上限 4096，`VARIABLE_DESCRIPTOR_COUNT` + `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`）
  - 创建 Descriptor Pool
  - 分配 Set 0 × 2（per-frame）+ Set 1 × 1
  - `get_global_set_layouts()` 返回 {set0_layout, set1_layout}
  - Bindless 纹理注册（`register_texture()` → `BindlessIndex`）、注销（`unregister_texture()`，free list 回收）
  - 显式 `destroy()` 方法
- **验证**：DescriptorManager 初始化/销毁无 validation 报错

### Step 3：Render Graph 骨架

- 创建 `framework/include/himalaya/framework/render_graph.h` + `framework/src/render_graph.cpp`
- `RGResourceId` 类型定义
- `import_image()` / `import_buffer()`：导入外部资源，返回 `RGResourceId`
- `add_pass()`：注册 pass 名称、`RGResourceUsage` 列表（读写语义由 `RGAccessType` 区分）、execute lambda
- `compile()`：根据 pass 声明的输入输出，计算每个 pass 之间需要的 barrier
- `execute()`：按注册顺序依次执行 pass（插入 barrier → 调用 execute lambda）
- `get_image()` / `get_buffer()`：在 execute 回调内通过 `RGResourceId` 获取底层句柄
- 帧循环重构：acquire → RG compile/execute → submit → present
- 三角形渲染迁移到 RG 的一个 pass 中
- ImGui 迁移到 RG 的最后一个 pass 中
- **验证**：三角形 + ImGui 通过 RG 渲染，效果与之前一致，无 validation 报错

### Step 4：Camera + 场景数据接口

- 创建 `framework/include/himalaya/framework/camera.h`（Camera 结构体：view/projection/view_projection 矩阵、position、fov、near/far、aspect）+ 矩阵计算方法
- 创建 `framework/include/himalaya/framework/scene_data.h`（完整 SceneRenderData、MeshInstance、DirectionalLight、PointLight、ReflectionProbe、LightmapInfo、CullResult、AABB）
- 创建 `app/camera_controller.h/cpp`（WASD 移动 + 鼠标旋转的自由漫游控制器）
- 负 viewport height 处理 Y-flip + `GLM_FORCE_DEPTH_ZERO_TO_ONE`
- **验证**：相机能自由移动，三角形随视角变化正确透视

### Step 5：Mesh + 纹理加载

- 创建 `framework/include/himalaya/framework/mesh.h` + `framework/src/mesh.cpp`
  - 统一顶点格式定义（`Vertex`：position vec3 + normal vec3 + uv0 vec2 + tangent vec4 + uv1 vec2）
  - Mesh 数据结构（vertex buffer handle、index buffer handle、顶点/索引数量）
- 创建 `framework/include/himalaya/framework/texture.h` + `framework/src/texture.cpp`
  - stb_image 加载 JPEG/PNG → RGBA8 像素数据
  - 上传到 GPU image（staging buffer）
  - GPU 端 mip 生成（`vkCmdBlitImage` 逐级降采样 + layout transition 链）
  - 注册到 DescriptorManager 获取 `BindlessIndex`
- **验证**：能加载单张纹理并在 shader 中通过 bindless index 采样显示

### Step 6：材质系统 + glTF 场景加载

- 创建 `framework/include/himalaya/framework/material_system.h` + `framework/src/material_system.cpp`
  - `GPUMaterialData` 结构体（base_color_factor、metallic_factor、roughness_factor、各纹理 BindlessIndex）
  - 全局 Material SSBO 管理（创建 buffer、写入材质数据）
  - `MaterialInstance` 管理（template_id + buffer offset）
- 创建 `app/scene_loader.h/cpp`
  - fastgltf 解析 glTF 文件
  - 遍历 mesh：转换为统一顶点格式，创建 vertex/index buffer
  - 遍历 material：提取 PBR 参数和纹理引用，创建 MaterialInstance
  - 遍历 node：计算世界变换矩阵，填充 MeshInstance（含 world_bounds 计算）
  - 填充 SceneRenderData
- **验证**：glTF 场景加载后数据结构正确（可通过 ImGui 显示统计信息验证：mesh 数、材质数、纹理数）

### Step 7：Forward Pass + 视锥剔除

- 创建基础 Lit shader：`shaders/forward.vert` + `shaders/forward.frag`
  - Vertex shader：MVP 变换，输出 world position / normal / uv0 / tangent
  - Fragment shader：采样 base color 纹理 × base_color_factor + Lambert 光照（硬编码方向光）+ 法线贴图采样（TBN 矩阵）
  - 使用 `shaders/common/bindings.glsl` 全局绑定布局
- 创建 GlobalUBO 管理（每帧更新相机矩阵、屏幕尺寸等）
- 创建 LightBuffer 管理（填充方向光数据）
- Forward pass 注册到 Render Graph
- Pipeline 创建（使用 `DescriptorManager::get_global_set_layouts()`）
- Draw loop：遍历可见物体，push constant 传 model matrix + material_index，draw call
- 创建 `framework/include/himalaya/framework/culling.h` + `framework/src/culling.cpp`
  - AABB vs 6 frustum planes 测试
  - 输入 SceneRenderData + Camera，输出 CullResult
- 移除旧的三角形渲染代码和 triangle shader（被 forward pass 取代）
- **验证**：glTF 场景正确渲染，有纹理和基础光照，相机移动时视锥剔除生效

---

## 阶段二文件清单

```
rhi/
├── include/himalaya/rhi/
│   └── descriptors.h          # DescriptorManager: set layout, descriptor set, bindless 管理
└── src/
    └── descriptors.cpp

framework/
├── include/himalaya/framework/
│   ├── render_graph.h         # Render Graph（barrier 自动插入 + 资源导入）
│   ├── mesh.h                 # 统一顶点格式、Mesh 数据结构
│   ├── texture.h              # 纹理加载（stb_image）、GPU mip 生成、bindless 注册
│   ├── material_system.h      # 材质模板、实例、全局 Material SSBO
│   ├── camera.h               # Camera 数据结构 + 矩阵计算
│   ├── scene_data.h           # SceneRenderData、MeshInstance、光源、探针（纯头文件）
│   └── culling.h              # 视锥剔除
└── src/
    ├── render_graph.cpp
    ├── mesh.cpp
    ├── texture.cpp
    ├── material_system.cpp
    ├── camera.cpp
    └── culling.cpp

app/
├── main.cpp                   # 入口（创建 Application 并运行）
├── application.h/cpp          # 主循环、窗口管理、初始化/销毁序列
├── scene_loader.h/cpp         # fastgltf 加载 → 填充渲染列表
├── camera_controller.h/cpp    # WASD + 鼠标自由漫游
└── debug_ui.h/cpp             # ImGui 面板

shaders/
├── common/
│   └── bindings.glsl          # 全局绑定布局（Set 0 + Set 1 + push constant）
├── forward.vert               # 基础 Lit vertex shader
└── forward.frag               # 基础 Lit fragment shader（Lambert + 方向光 + 法线贴图）
```

---

## 阶段二技术笔记

| 主题 | 说明 |
|------|------|
| 无 BC 纹理压缩 | 阶段二直接加载 JPEG/PNG 解码为 RGBA8，BC 压缩是后续优化项 |
| 无 Pass 抽象基类 | 渲染逻辑直接写在 RG 的 lambda 回调中，阶段三再提取 `RenderPass` 基类 |
| 无 RG transient 资源 | 所有资源由外部创建后通过 `import_image/buffer()` 导入 RG |
| 无 RG temporal 资源 | 阶段五 SSAO temporal filter 时引入 |
| 顶点格式固定 | 统一 position + normal + uv0 + tangent + uv1，缺失属性填默认值 |
| Y-flip | 负 viewport height，Vulkan 核心保证。深度 [0,1] 用 `GLM_FORCE_DEPTH_ZERO_TO_ONE` |
| triangle shader 退役 | Step 7 完成后 `shaders/triangle.vert/frag` 被 forward shader 取代，可删除 |
| fastgltf + stb_image | fastgltf 不带图像解码，stb_image 负责 JPEG/PNG 解码 |
| glTF 纹理归属 | framework 层的 texture 模块管理纹理生命周期，scene_loader 调用它 |
| Descriptor Pool 规格 | 需要容纳 Set 0 × 2（per-frame）+ Set 1 × 1 + ImGui 专用池（已有） |
