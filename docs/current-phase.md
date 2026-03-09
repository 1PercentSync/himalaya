# 当前阶段：M1 阶段二 — 基础渲染管线

> 目标：搭建完整的渲染管线骨架 — 加载 glTF 场景、用基础 Lit shader 渲染、相机自由漫游。
> 涉及 Layer 0 补全（Descriptor 管理）、Layer 1 框架建设、Layer 3 重构与场景加载。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

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
  - 创建两个独立的 Descriptor Pool：普通 pool（Set 0, 2 UBO + 4 SSBO, maxSets=2）+ UPDATE_AFTER_BIND pool（Set 1, 4096 COMBINED_IMAGE_SAMPLER, maxSets=1）
  - 分配 Set 0 × 2（per-frame）+ Set 1 × 1
  - `get_global_set_layouts()` 返回 {set0_layout, set1_layout}
  - Bindless 纹理注册（`register_texture(ImageHandle, SamplerHandle)` → `BindlessIndex`）、注销（`unregister_texture()`，free list 回收）
  - 显式 `destroy()` 方法
- ResourceManager 扩展 sampler 管理：`SamplerDesc` 结构体、`create_sampler()` / `destroy_sampler()` / `get_sampler()` 方法
- CommandBuffer 新增 `bind_descriptor_sets()` 方法
- **验证**：DescriptorManager 初始化/销毁无 validation 报错

### Step 3：Render Graph 骨架

- 创建 `framework/include/himalaya/framework/render_graph.h` + `framework/src/render_graph.cpp`
- `RGResourceId` 类型定义
- `import_image(debug_name, handle, initial_layout, final_layout)` / `import_buffer()`：导入外部资源，返回 `RGResourceId`（import_image 带 `initial_layout` 和 `final_layout` 参数，`final_layout` 必填，RG execute 结束后自动插入最终 layout transition）
- `add_pass()`：注册 pass 名称、`RGResourceUsage` 列表（读写语义由 `RGAccessType` 区分）、execute lambda
- `compile()`：根据 pass 声明的输入输出，计算每个 pass 之间需要的 barrier（仅处理 image layout transition）
- `execute(CommandBuffer& cmd)`：接收外部 command buffer，按注册顺序依次执行 pass（插入 barrier → 调用 execute lambda）
- `clear()`：每帧重建前清除所有 pass 和资源引用（RG 每帧重建是长期方案）
- `get_image()` / `get_buffer()`：在 execute 回调内通过 `RGResourceId` 获取底层句柄
- 帧循环重构：acquire → RG compile/execute → submit → present
- 三角形渲染迁移到 RG 的一个 pass 中
- ImGui 迁移到 RG 的最后一个 pass 中
- Shader 编译增加 shaderc includer 支持：
  - `ShaderCompiler::set_include_path(path)` 配置 shader 根目录（初始化时设一次）
  - 实现 `shaderc::CompileOptions::IncluderInterface`，作为 `shader.cpp` 匿名命名空间内的私有类
  - 严格路径解析：relative include（`#include "..."`）相对于当前文件所在目录解析，standard include（`#include <...>`）相对于 shader 根目录解析，不做回退
  - includer 内部读取文件内容交给 shaderc 处理嵌套 include
- CommandBuffer 新增 `begin_debug_label(name, color)` / `end_debug_label()` 方法：
  - 封装 `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT`
  - 扩展函数通过 `vkGetInstanceProcAddr` 动态加载（`init_debug_functions(VkInstance)` 静态方法，初始化时调用一次）
  - `#ifndef NDEBUG` 守卫，release 下为空操作（`VK_EXT_debug_utils` 仅 debug 构建启用）
  - `color` 为必填参数（无默认值），调用方决定颜色
- RG `execute()` 自动为每个 pass 插入 debug label，按 pass 索引生成不同颜色（黄金角 hue 分布）
- `compile()` 的 barrier 计算：从 `(RGAccessType, RGStage)` 到 `(VkImageLayout, VkPipelineStageFlags2, VkAccessFlags2)` 的映射按需实现，未实现的组合 assert 拦截。RG 不管 loadOp/storeOp
- **验证**：三角形 + ImGui 通过 RG 渲染，效果与之前一致，无 validation 报错

### Step 4：Camera + 场景数据接口

- 创建 `framework/include/himalaya/framework/camera.h` + `framework/src/camera.cpp`
  - Camera 结构体：view / projection / view_projection / inv_view_projection 矩阵、position、yaw/pitch、fov、near/far、aspect
  - `update_view()`：从 position + yaw/pitch 计算 view 矩阵
  - `update_projection()`：自定义 reverse-Z 透视投影（不用 `glm::perspective`，near 映射到 1、far 映射到 0）
  - `update_view_projection()`：view × projection + 逆矩阵
- 创建 `framework/include/himalaya/framework/scene_data.h`（纯头文件）
  - 场景数据：SceneRenderData、MeshInstance、DirectionalLight、CullResult、AABB（PointLight、ReflectionProbe、LightmapInfo 延迟到实际需要时再加）
  - GPU 数据结构：GlobalUniformData、GPUDirectionalLight、PushConstantData（与 shader 端 `bindings.glsl` 一一对应）
- 创建 `app/camera_controller.h/cpp`（WASD 移动 + 鼠标旋转的自由漫游控制器）
  - 检查 `ImGui::GetIO().WantCaptureMouse` / `WantCaptureKeyboard`，为 true 时跳过相机输入处理
- `GLM_FORCE_DEPTH_ZERO_TO_ONE` 编译定义：在 `rhi/CMakeLists.txt` 中 `target_compile_definitions(himalaya_rhi PUBLIC ...)` 传播到所有上层
- 三角形接入 Camera（验证所需的隐含工作）：
  - 三角形顶点改为 3D（vec3 position，z=0 平面）
  - `triangle.vert` 从 GlobalUBO 读取 `view_projection` 矩阵做变换
  - 每帧创建 GlobalUBO × 2（per-frame, CpuToGpu），填充 camera 矩阵，绑定 Set 0
- **验证**：相机能自由移动，三角形随视角变化正确透视

### Step 5：Mesh + 纹理加载

- 创建 `framework/include/himalaya/framework/mesh.h` + `framework/src/mesh.cpp`
  - 统一顶点格式定义（`Vertex`：position vec3 + normal vec3 + uv0 vec2 + tangent vec4 + uv1 vec2）
  - Mesh 数据结构（vertex buffer handle、index buffer handle、顶点/索引数量）
  - MikkTSpace 集成（vcpkg 引入 mikktspace）：mesh 缺失 tangent 属性时从 position + normal + uv0 自动生成
- 创建 `framework/include/himalaya/framework/texture.h` + `framework/src/texture.cpp`
  - stb_image 加载 JPEG/PNG → RGBA8 像素数据（强制 4 通道，`stbi_load` 第四参数传 4，统一 R8G8B8A8 format）
  - 上传到 GPU image（staging buffer）
  - GPU 端 mip 生成（`vkCmdBlitImage` 逐级降采样 + layout transition 链）
  - 纹理加载接口接受 `TextureRole` 枚举（`Color` / `Linear`），根据角色选择 format（Color → SRGB, Linear → UNORM）。scene_loader 根据 glTF 材质字段引用关系确定角色后传入
  - 注册到 DescriptorManager 获取 `BindlessIndex`
- 批量上传：Context 新增 `begin_immediate()` / `end_immediate()` scope
  - `upload_buffer()` API 变更：改为录制模式（必须在 scope 内调用）。旧的三角形顶点上传代码在此步骤同步适配
- Default 纹理创建（1×1 white、flat normal、black），Texture 模块初始化时注册到 bindless array 获得固定 BindlessIndex
- Sampler 创建：从 glTF sampler 定义创建对应 VkSampler（缺失时使用 default: linear repeat mip-linear）
- **验证**：改造三角形 shader 添加 hardcoded bindless index 采样，纹理正确显示在三角形上

### Step 6：材质系统 + glTF 场景加载 + Unlit 渲染

- 创建 `framework/include/himalaya/framework/material_system.h` + `framework/src/material_system.cpp`
  - `GPUMaterialData` 结构体（std430 layout, 64 字节, alignas(16)）：base_color_factor(vec4) + emissive_factor(vec4, w unused) + metallic/roughness(float×2) + 5 个纹理 BindlessIndex(uint×5) + padding
  - 全局 Material SSBO 管理（场景加载后知道材质总数，一次性创建恰好大小的 buffer）
  - `MaterialInstance` 管理（template_id + buffer offset）
  - 缺失纹理字段填入 default 纹理的 BindlessIndex（包括 occlusion_tex 和 emissive_tex 占位）
- 创建 `app/scene_loader.h/cpp`
  - 场景路径通过命令行参数传入（`argc/argv`），不传参时使用写死的默认路径
  - 加载失败（glTF 解析、纹理缺失等）log error + abort，不做 fallback
  - fastgltf 解析 glTF 文件
  - 遍历 mesh：转换为统一顶点格式，创建 vertex/index buffer
  - 遍历 material：提取 PBR 参数和纹理引用，创建 MaterialInstance
  - 遍历 node：计算世界变换矩阵，填充 MeshInstance（含 world_bounds 计算）
  - glTF 每个 primitive 展开为独立 MeshInstance（长期方案）
  - 使用 fastgltf `LoadExternalBuffers` + `LoadExternalImages` options 统一处理嵌入式与外部资源
  - Sampler 不做 ResourceManager 层去重，glTF 同一 sampler index 只创建一个 VkSampler（自然去重），后续按需升级为 hash 去重
  - SceneLoader 持有所有加载产生的资源句柄列表（mesh buffer、texture、sampler、material），提供 `destroy()` 方法一次性清理，支持场景切换
  - 填充 SceneRenderData
- CommandBuffer 新增 `bind_index_buffer()`、`draw_indexed()`、`push_constants()` 方法
- 创建 `shaders/common/bindings.glsl`（全局绑定布局：Set 0 + Set 1 + push constant）
- 创建 unlit shader（forward.frag 的雏形）：采样 base_color_tex × base_color_factor，无光照计算
- 创建 `shaders/forward.vert`（MVP 变换，输出 world position / normal / uv0 / tangent）
- Depth buffer 创建（D32Sfloat，imported resource 导入 RG，resize 时 Application 重建）
- Reverse-Z 配置：depth clear 0.0f，compare op `VK_COMPARE_OP_GREATER`（投影矩阵已在 Step 4 Camera 中实现）
- Resize 资源重建：提取 `handle_resize()` 私有方法，`vkQueueWaitIdle` 后立即销毁旧资源（不走 deferred deletion），acquire 失败和帧末 resize 两处共用
- 创建 GlobalUBO × 2 + LightBuffer × 2（`CpuToGpu` memory，descriptor 初始化写一次，每帧 memcpy 更新内容）
- Unlit pass 注册到 Render Graph（使用 depth attachment）
- Pipeline 创建（使用 `DescriptorManager::get_global_set_layouts()`）
- Draw loop：遍历物体，push constant 传 model matrix + material_index，draw call
- 移除旧的三角形渲染代码和 triangle shader（被 unlit pass 取代）
- **验证**：glTF 场景渲染出有纹理的画面（无光照，验证数据管线完整性：顶点、变换、材质、bindless 全链路）

### Step 7：Forward 光照 + 视锥剔除

- 升级 `shaders/forward.frag`：在 unlit 基础上加入 Lambert 光照（从 LightBuffer SSBO 读取方向光参数，不硬编码）+ 法线贴图采样（TBN 矩阵）
- Application 提供保底方向光（检测 SceneRenderData 无方向光时填入默认值），scene_loader 忠实还原 glTF（无光源则输出空数组）
- 创建 `framework/include/himalaya/framework/culling.h` + `framework/src/culling.cpp`
  - AABB vs 6 frustum planes 测试
  - 输入 SceneRenderData + Camera，输出 CullResult
- Draw loop 改为遍历 CullResult 的 visible indices
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
│   ├── scene_data.h           # SceneRenderData、MeshInstance、光源、探针、GPU 数据结构（纯头文件）
│   └── culling.h              # 视锥剔除
└── src/
    ├── render_graph.cpp
    ├── mesh.cpp
    ├── texture.cpp
    ├── material_system.cpp
    ├── camera.cpp
    ├── culling.cpp
    └── stb_impl.cpp           # stb_image 单头文件库实现（#define STB_IMAGE_IMPLEMENTATION）

app/
├── include/himalaya/app/
│   ├── application.h          # 主循环、窗口管理、初始化/销毁序列
│   ├── scene_loader.h         # fastgltf 加载 → 填充渲染列表
│   ├── camera_controller.h    # WASD + 鼠标自由漫游
│   └── debug_ui.h             # ImGui 面板
└── src/
    ├── main.cpp               # 入口（创建 Application 并运行）
    ├── application.cpp
    ├── scene_loader.cpp
    ├── camera_controller.cpp
    └── debug_ui.cpp

shaders/
├── common/
│   └── bindings.glsl          # 全局绑定布局（Set 0 + Set 1 + push constant）
├── forward.vert               # 基础 Lit vertex shader
└── forward.frag               # 基础 Lit fragment shader（Lambert + 方向光 + 法线贴图）
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| 无 BC 纹理压缩 | 阶段二直接加载 JPEG/PNG 解码为 RGBA8，BC 压缩是后续优化项 |
| 无 Pass 抽象基类 | 渲染逻辑直接写在 RG 的 lambda 回调中，阶段三再提取 `RenderPass` 基类 |
| 无 RG transient 资源 | 所有资源由外部创建后通过 `import_image/buffer()` 导入 RG |
| 无 RG temporal 资源 | 阶段五 SSAO temporal filter 时引入 |
| 顶点格式固定 | 统一 position + normal + uv0 + tangent + uv1，缺失属性填默认值 |
| Y-flip | 负 viewport height，Vulkan 核心保证。深度 [0,1] 用 `GLM_FORCE_DEPTH_ZERO_TO_ONE`（`rhi/CMakeLists.txt` PUBLIC 传播） |
| Reverse-Z 投影 | Step 4 Camera 即实现 reverse-Z 投影矩阵（near→1, far→0），避免 Step 6 再改投影。depth buffer/clear/compare 配置仍在 Step 6 |
| Step 4 三角形接入 | 验证需要三角形响应 camera：顶点改 3D + shader 读 GlobalUBO VP + 每帧填充绑定 Set 0。作为验证的隐含工作 |
| triangle shader 退役 | Step 6 完成后 `shaders/triangle.vert/frag` 被 forward shader 取代，可删除 |
| fastgltf + stb_image | fastgltf 不带图像解码，stb_image 负责 JPEG/PNG 解码 |
| glTF 纹理归属 | framework 层的 texture 模块管理纹理生命周期，scene_loader 调用它 |
| Descriptor Pool 规格 | 需要容纳 Set 0 × 2（per-frame）+ Set 1 × 1 + ImGui 专用池（已有） |
| stb_image 通道 | 强制 4 通道（RGBA），stbi_load 第四参数传 4。避免 R8G8B8 三通道格式的 GPU 兼容性问题 |
| 纹理角色 | texture 模块通过 `TextureRole` 枚举（`Color`/`Linear`）选择 format。同一张图被不同角色引用时创建两个 GPU image（极少发生） |
| MikkTSpace | vcpkg 引入 mikktspace。仅在 mesh 缺失 tangent 属性时调用生成。填充顺序：position → normal → uv0 → MikkTSpace → uv1 |
| Sampler 去重 | ResourceManager 层不做 hash 去重。glTF 同一 sampler index 只创建一个 VkSampler（自然去重）。后续按需升级 |
| upload_buffer 行为 | 必须在 begin/end_immediate scope 内调用。只录制 copy 命令，不自行 submit。scope 外调用 assert 失败。API 变更在 Step 5 执行，旧三角形上传代码同步适配 |
| Resize 销毁策略 | vkQueueWaitIdle 后立即销毁（不走 deferred deletion），因 idle 已保证 GPU 不再引用 |
| CameraController 输入 | 检查 ImGui WantCaptureMouse/WantCaptureKeyboard 避免输入冲突 |
| 默认方向光 | Application 检测 SceneRenderData 无方向光时填入保底光源。scene_loader 不捏造不存在的数据 |
| Descriptor Pool 分离 | 两个独立 pool：普通 pool (Set 0, 2 UBO + 4 SSBO) + UPDATE_AFTER_BIND pool (Set 1, 4096 COMBINED_IMAGE_SAMPLER) |
| GPUMaterialData 对齐 | std430 布局 64 字节。emissive_factor 用 vec4 避免 vec3 对齐问题。C++ 侧 alignas(16) |
| 场景路径 | `argc/argv` + 写死默认路径。CLion Run Configuration 切换场景。长期方向 GUI 文件选择器 |
| 加载错误处理 | 加载失败（glTF / 纹理 / shader）一律 log error + abort。开发期不做 fallback |
| Step 5→6 视觉验证 | Step 5 用三角形 shader + bindless 验证纹理链路。Step 6 用 unlit shader 验证完整数据管线（顶点、变换、材质）。Step 7 加光照和剔除 |
| PushConstant 演进 | 阶段二 68 bytes（model + material_index）。阶段六迁移到 per-instance SSBO，为 lightmap 和 M2 motion vectors 预留空间 |
| shaderc includer | `set_include_path()` 配置根目录，严格路径解析（relative 相对当前文件，standard 相对根目录，不回退）。私有类不暴露到头文件 |
| Debug label | `begin_debug_label(name, color)` + `end_debug_label()`。扩展函数动态加载（`init_debug_functions`），`#ifndef NDEBUG` 守卫。color 必填，RG 按 pass 索引黄金角分配 hue |
