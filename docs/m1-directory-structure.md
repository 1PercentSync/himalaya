# Milestone 1：目录结构

> 本文档定义 M1 的文件组织，直接反映四层架构。
> 四层架构定义见 `architecture.md`。

---

## 项目顶层结构

```
himalaya/
├── CMakeLists.txt               # 顶层 CMake（project、vcpkg 工具链、add_subdirectory）
├── vcpkg.json                   # vcpkg manifest（依赖声明）
├── CLAUDE.md                    # Agent 持续性指令
│
├── rhi/                         # himalaya_rhi (static lib)
│   ├── CMakeLists.txt
│   ├── include/himalaya/rhi/    # 公开头文件
│   └── src/                     # 实现文件
│
├── framework/                   # himalaya_framework (static lib) → himalaya_rhi
│   ├── CMakeLists.txt
│   ├── include/himalaya/framework/
│   └── src/
│
├── passes/                      # himalaya_passes (static lib) → himalaya_framework
│   ├── CMakeLists.txt
│   ├── include/himalaya/passes/
│   └── src/
│
├── app/                         # himalaya_app (exe) → all above
│   ├── CMakeLists.txt
│   └── main.cpp
│
├── shaders/                     # GLSL 源码
│   ├── common/                  # 共享头文件 / 函数
│   └── *.vert / *.frag / *.comp
│
├── docs/                        # 设计文档
└── tasks/                       # 任务规划
```

每层为独立 CMake target，通过 `target_link_libraries` 表达依赖关系。编译期强制单向依赖——如果 `rhi/` 的代码试图引用 `framework/` 的头文件，编译直接报错。

第三方库全部通过 vcpkg 管理，不使用 `third_party/` 目录。

---

## Layer 0 — RHI（rhi/）

```
rhi/
├── include/himalaya/rhi/
│   ├── types.h                  # 句柄类型（generation-based）、枚举、格式定义
│   ├── context.h                # Instance, Device, Queue, VMA, 帧同步, 删除队列
│   ├── swapchain.h              # Swapchain 管理
│   ├── resources.h              # Buffer, Image 创建与管理, 资源池
│   ├── descriptors.h            # Bindless descriptor 管理, Descriptor Set 分配
│   ├── pipeline.h               # Pipeline 创建与缓存
│   ├── commands.h               # Command Buffer 录制 wrapper
│   └── shader.h                 # 运行时编译（GLSL → SPIR-V）、缓存、热重载
└── src/
    ├── context.cpp
    ├── swapchain.cpp
    ├── resources.cpp
    ├── descriptors.cpp
    ├── pipeline.cpp
    ├── commands.cpp
    ├── shader.cpp
    └── vma_impl.cpp            # VMA 单头文件库实现
```

**与旧版差异说明：**
- `device.h` + `sync.h` 合并为 `context.h`——帧同步（fence/semaphore）是 Context 核心职责之一，与 Device/Queue 管理内聚
- 文件按需创建，不提前建空骨架

---

## Layer 1 — Framework（framework/）

```
framework/
├── include/himalaya/framework/
│   ├── render_graph.h           # Render Graph（编排 + barrier + temporal）
│   ├── material_system.h        # 材质模板、材质实例、参数布局
│   ├── mesh.h                   # 顶点格式、Mesh 数据管理
│   ├── texture.h                # 纹理加载、格式处理、mip 生成
│   ├── camera.h                 # 相机、投影、jitter
│   ├── scene_data.h             # 渲染列表、光源、探针数据结构定义（纯头文件）
│   ├── culling.h                # 视锥剔除
│   └── imgui_backend.h          # ImGui 集成
└── src/
    └── ...
```

`scene_data.h` 是纯头文件（只有数据结构定义），没有 .cpp——它定义的是渲染器和应用层之间的"合同"。AABB 等共享类型定义在此层。

---

## Layer 2 — Passes（passes/）

```
passes/
├── include/himalaya/passes/
│   ├── pass_interface.h         # Pass 基础接口定义（纯接口）
│   ├── depth_prepass.h          # Depth + Normal PrePass
│   ├── shadow_pass.h            # CSM 阴影
│   ├── forward_pass.h           # 主光照（Forward Lighting）
│   ├── transparent_pass.h       # 透明物体
│   ├── ssao_pass.h              # SSAO
│   ├── ssao_temporal.h          # SSAO Temporal Filter
│   ├── contact_shadows.h        # Contact Shadows
│   ├── bloom_pass.h             # Bloom 降采样 + 升采样链
│   ├── auto_exposure.h          # 自动曝光
│   ├── tonemapping.h            # Tonemapping（ACES）
│   ├── vignette.h               # Vignette
│   ├── color_grading.h          # Color Grading
│   ├── height_fog.h             # 高度雾
│   └── msaa_resolve.h           # MSAA Resolve（Depth、Normal、Color）
└── src/
    └── ...
```

---

## Layer 3 — App（app/）

```
app/
├── CMakeLists.txt
├── main.cpp                     # 入口
├── application.h/cpp            # 主循环、窗口管理
├── scene_loader.h/cpp           # glTF 加载 → 渲染列表
├── camera_controller.h/cpp      # 自由漫游相机控制
└── debug_ui.h/cpp               # ImGui 面板、各 Pass 参数调整
```

App 层拥有 GLFW 窗口，传 `GLFWwindow*` 给 RHI 创建 Surface。

---

## Shaders

```
shaders/
├── common/                      # 共享头文件 / 函数
│   ├── brdf.glsl                # BRDF 函数（Lambert、GGX、Smith、Schlick）
│   ├── lighting.glsl            # 光照计算（方向光、IBL）
│   ├── shadow.glsl              # 阴影采样（CSM、PCF）
│   └── bindings.glsl            # 全局绑定布局定义
├── depth_prepass.vert/frag
├── forward.vert/frag
├── shadow.vert/frag
├── ssao.comp
├── ssao_temporal.comp
├── contact_shadows.comp
├── bloom_downsample.comp
├── bloom_upsample.comp
├── auto_exposure.comp
├── tonemapping.comp
├── transparent.vert/frag
├── vignette.comp
├── color_grading.comp
└── height_fog.comp
```

- `shaders/common/bindings.glsl` 定义全局绑定布局，所有 shader 通过 `#include` 引用，确保绑定一致性
- 后处理 shader 全部用 Compute Shader（`.comp`），不使用全屏三角形
- CMake 构建后拷贝到 build 目录，开发期通过编辑 build 副本触发热重载

---

## 层间依赖规则

| 目录 | 架构层 | 允许的依赖 |
|------|--------|-----------|
| `rhi/` | Layer 0 | Vulkan SDK、vcpkg 库（VMA、shaderc、GLFW） |
| `framework/` | Layer 1 | `himalaya_rhi`、vcpkg 库（GLM、ImGui、fastgltf、stb_image） |
| `passes/` | Layer 2 | `himalaya_framework`（通过 Render Graph 和资源接口） |
| `shaders/` | — | `common/` 被所有 shader 引用 |
| `app/` | Layer 3 | `himalaya_rhi`、`himalaya_framework`、`himalaya_passes`、vcpkg 库（GLFW、spdlog） |

**禁止的依赖方向：**

- `rhi/` 不依赖 `framework/`、`passes/`、`app/`
- `framework/` 不依赖 `passes/`、`app/`
- `passes/` 中的各 pass 文件之间不互相依赖（通过 Render Graph 间接关联）
