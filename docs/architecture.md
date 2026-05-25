# 渲染器架构与设计理念

> 渲染器在长远视角下的架构特征、层次结构、边界约束和贯穿技术选型的设计理念。
> 技术选型结果见 `technical-decisions.md`。

---

## 项目定位

Himalaya 是基于 Vulkan 1.4 的渲染器，`reflector` 分支以 Path Tracing 和 Gaussian Splatting 为核心渲染方式。

- **性质**：个人长期学习和练习项目
- **渲染方式**：Path Tracing（GLTF mesh）+ Gaussian Splatting（GLTF GS 扩展）
- **开发方式**：AI 辅助开发，瓶颈在于审查、理解和架构决策

---

## 硬件目标

- **目标平台**：支持 Vulkan Ray Tracing 的桌面 GPU
- **性能理念**：追求技术和画面的最佳性价比，不为低端过度优化，不追求只有高端才能运行的方案

---

## 设计原则

### 排除过于复杂而收益不高的技术

复杂度和收益必须成正比。实现成本远高于视觉或性能收益的技术，排除。

### 渐进式实现

先能用，再好用，再优秀。每个模块可分阶段实现，阶段间的演进应尽量自然（在已有基础上加东西而非推翻重来）。

### 业界已验证的技术

采用有成熟实现和资料的技术，不做实验性方案。资料丰富度直接影响 AI 辅助开发的可靠性。

### 性能性价比

同等画面质量选性能更优的方案；同等性能选画面更好的方案。

### 不可有明显 glitch

画面可以不精确但不能有明显视觉瑕疵。在锐利+闪烁和模糊+不闪烁之间，选择后者。

### 插件化与延后实现

不是所有东西都需要立刻做。后处理等效果天然是独立的全屏 pass，可独立启用/禁用。

---

## 四层架构

```
Layer 3（应用层）
  ↓ 填充渲染列表，调用渲染
Layer 2（渲染 Pass 层）
  ↓ 通过 Render Graph 注册和执行
Layer 1（渲染框架层）
  ↓ 使用资源和命令接口
Layer 0（Vulkan 抽象层 / RHI）
```

**严格单向依赖** — 上层依赖下层，下层不知道上层的存在。Layer 2 的各个 Pass 之间也没有直接依赖，只通过 Layer 1 的 Render Graph 间接关联。

### Layer 0：Vulkan 抽象层（RHI）

封装 Vulkan 底层 API，向上层提供简洁的资源创建和操作接口。

包含：Device / Instance / Queue 管理、资源创建（Buffer、Image、Sampler）、Bindless descriptor 管理、Pipeline 创建与缓存（含 RT Pipeline 和 SBT）、Shader 编译（运行时 GLSL→SPIR-V + 热重载）、Command Buffer 录制辅助、Swapchain 管理、内存分配（VMA）、加速结构（BLAS/TLAS 创建、构建、销毁）。

**设计原则**：不包含任何渲染逻辑。薄封装为主，对特别繁琐的部分（descriptor 管理、pipeline 创建、barrier 插入）做适度便利封装。

**对外暴露类型**：句柄或轻量包装类型（ImageHandle、BufferHandle 等），Command Buffer 通过轻量 wrapper 暴露给 Pass 层。

### Layer 1：渲染框架层（Render Framework）

提供渲染相关的通用框架和基础设施，不涉及具体的渲染效果。

包含：Render Graph（编排 + barrier 辅助 + temporal 资源管理）、材质系统（材质模板、材质实例、参数布局）、Mesh / Geometry 管理、纹理加载与格式处理（BC 压缩、mip 生成）、相机（投影矩阵、视图矩阵）、场景渲染接口（渲染列表）、Scene AS Builder（BLAS/TLAS 构建）、环境光与 emissive 采样数据构建、ImGui 集成。

**关键设计**：定义了"渲染一帧"的骨架——接收渲染列表，经 Render Graph 调度各 pass，输出最终图像。具体有哪些 pass、每个 pass 做什么，由上面一层定义。

### Layer 2：渲染 Pass 层（Render Passes）

实现每一个具体的渲染 Pass。每个 Pass 独立模块，声明输入输出，注册到 Render Graph。

**每个 Pass 的标准接口**：声明输入/输出资源、Setup（创建 pipeline 等）、Execute（每帧录制 command buffer）、可选配置结构体、enabled()、on_resize()。

**设计原则**：Pass 之间不直接互相调用或引用，数据传递完全通过 Render Graph 的资源声明。

### Layer 3：应用层（Application）

场景加载、资产管理、相机控制、用户输入。填充渲染列表（mesh 实例、相机），然后调用 Layer 1 的"渲染一帧"接口。

---

## 核心架构特征

### Render Graph

声明式的帧渲染管理系统。每个 pass 声明自己读什么资源、写什么资源，系统负责执行顺序、资源生命周期、同步屏障、资源复用。

**演进方向**：初期手动编排 pass 顺序 + barrier 自动插入，后期升级为自动拓扑排序 + 资源别名分析。

#### 资源管理准则

| 准则 | 管理方式 | 典型资源 |
|------|---------|---------|
| 需要 resize（屏幕尺寸相关） | **RG Managed**：RG 创建、缓存、resize 自动重建 | PT accumulation buffer、HDR color buffer |
| 不需要 resize（固定尺寸） | **Pass 自管理 + 每帧 `import_image()`** | 特定 pass 的自有资源 |

### 材质系统

- **材质模板 / 着色模型** — 定义一种着色方式，对应一组 shader 变体
- **材质实例** — 基于模板设置具体参数（albedo 贴图、roughness 值等）
- **Shader 变体管理** — 编译时开关的编译和缓存系统

每种着色模型定义自己的 GPU 材质数据结构体（CPU 端 struct + shader 端 struct 一一对应）。

**Shader 编译**：开发期运行时编译（GLSL→SPIR-V）+ 热重载。

### 场景表示与数据流

渲染器接收一个"渲染视图"：一组要渲染的物体（mesh + 材质 + 变换）和相机参数。环境照明来自 IBL，面光源来自 emissive 材质三角形。

### Pass 可插拔性

每个渲染 Pass 是一个自包含的模块，声明自己的输入输出，注册到 Render Graph。添加或移除一个 pass 不需要修改其他 pass 的代码。

### 配置与调参系统

ImGui 面板 + 配置结构体，运行时热调整。

---

## 架构约束

| 约束 | 保护的架构属性 |
|------|---------------|
| 上层不接触 Vulkan 类型 | Layer 0 内部实现自由度 |
| Pass 间只通过资源声明通信 | Pass 可插拔性 |
| 配置参数单向传递 | 数据流清晰可追踪 |
| Shader 不硬编码绑定 | 材质系统灵活性（通过 bindless index 访问）|
| Validation Layer 常开 | 开发期 bug 可见性 |

### Framework 层 Vulkan 类型例外

以下 Framework 层组件允许直接使用 Vulkan 类型：

| 组件 | 理由 |
|------|------|
| Render Graph | 本质是 Vulkan barrier 管理器 |
| ImGui Backend | 第三方库的 Vulkan backend |

其他 Framework 模块的公开接口仍然不使用 Vulkan 类型。

### Shader 数据分层

| 层级 | 内容 | 更新频率 | 绑定方式 |
|------|------|----------|----------|
| 全局数据 | 相机矩阵、曝光值、IBL 索引与旋转 | 每帧一次 | 全局 uniform buffer |
| 材质数据 | PBR 参数、纹理 index | 加载时一次 | 全局 SSBO，通过 material index 读取 |
| Per-draw 数据 | 模型矩阵、材质 index | 每次绘制 | push constant |

---

## Gaussian Splatting 数据管线

### 概述

GS 数据管线独立于 PT 管线，负责将 3D Gaussian Splatting 数据从文件加载到 CPU 端 SoA 数据结构。GPU buffer 创建和渲染在 Phase 3 实现。

支持两种输入：glTF（`KHR_gaussian_splatting` 扩展）和 PLY（通过内部转换为 glTF）。

### 模块结构

```
Application 打开文件
    │
    ├─ .ply ──→ PLY 转换器 (framework) ──→ 缓存 .gltf
    │                                          │
    └─ .gltf/.glb ─────────────────────────────┤
                                                ▼
                                    gltf_utils::parse_gltf()
                                    gltf_utils::has_gaussian_splatting()
                                                │
                                    ┌───────────┴───────────┐
                                    ▼                       ▼
                            GaussianSplatLoader       SceneLoader
                            (GS primitive)            (Mesh primitive)
                                    │
                                    ▼
                            GaussianSplatData (CPU, SoA)
```

### gltf_utils（App 层共享）

从 `SceneLoader` 提取的公用函数：

- `parse_gltf()` — fastgltf 文件解析（.gltf / .glb），返回 `Asset` + `base_dir`
- `has_gaussian_splatting()` — 检查 `extensionsUsed` 是否包含 `KHR_gaussian_splatting`
- `transform_aabb()` — 将 local AABB 通过变换矩阵转换到 world space

### PLY 转换器（Framework 层）

将 INRIA 3DGS PLY 文件转换为符合 `KHR_gaussian_splatting` 规范的 .gltf + .bin。处理激活函数（sigmoid / exp）、坐标系转换（COLMAP → glTF）、四元数重排（wxyz → xyzw）。转换结果缓存到 `%TEMP%\himalaya\gaussians\`。

### GaussianSplatLoader（App 层）

从 glTF 加载 GS 数据。使用 fastgltf 读取 attribute accessor，nlohmann/json 二次解析提取 extension 元数据。输出 CPU 端 `GaussianSplatScene`（包含一个或多个 `GaussianSplatPrimitive`，核心属性为打包 struct `GaussianSplatCore`，SH 系数按实际 degree 独立分配）。

---

## Gaussian Splatting 渲染管线

### 概述

GS 渲染采用 Compute Tile-Based Rendering（纯 compute 软光栅）。当前已跑通的原型管线为 Projection+Culling → Tile Entry Generation → 两次 stable Radix Sort（depth 后 tile-id）→ Tile Range Build → Tile Rendering。大场景验证后，后续主线改为 Step 6.3 per-tile binning / local ordering，以替代两次全局 RadixSort 主路径。输出到独立 GS color buffer，通过 PresentPass 输出到 swapchain。

### 数据流

```
GaussianSplatScene (CPU)
    │ upload (场景加载时)
    ▼
GPU: Core/Covariance SSBO + SH SSBO[degree]
    │
    ├─ Projection+Culling (compute) ──→ 2D attrs + RGB + depth key
    │
    ├─ Per-Tile Count / Offset ───────→ per-tile ranges
    │
    ├─ Per-Tile Scatter ──────────────→ per-tile entries (depth, splat_id)
    │
    ├─ Per-Tile Local Ordering ───────→ front-to-back entries per tile
    │
    └─ Tile Rendering (compute, 16×16) ──→ GS color buffer
                                                   │
                                             PresentPass → swapchain
```

### 模块分层

| 模块 | 层级 | 职责 |
|------|------|------|
| GS GPU Buffer 管理 | Framework | GPU SSBO 创建、上传、销毁 |
| RadixSort | Framework | 通用 32-bit key+value GPU radix sort；保留为工具 / 原型路径，不作为最终 GS 主路径优化目标 |
| GS Projection Pass | Passes | 投影 + 剔除 + SH 求值 + Mip Splatting |
| GS Tile Binning Pass | Passes | Step 6.3 后负责 per-tile count / offset / scatter / local ordering |
| GS Tile Rendering Pass | Passes | 前到后 alpha blend → imageStore |
| PresentPass | Passes | color buffer → swapchain（原 TonemappingPass） |

### 多 Primitive 合并

多个 `GaussianSplatPrimitive` 的核心属性合并为一个 GPU buffer。上传时 position 变换到 world space，rotation/scale 转为 world covariance（node linear transform 合入 covariance）。SH 系数按 degree 分组，投影 pass 按组 dispatch（push constant 传入 offset/count/degree）。后续 entry generation、排序和渲染阶段统一处理，不区分来源 primitive。

### PresentPass（原 TonemappingPass）

统一的 color buffer → swapchain 输出 pass。根据 RenderMode 选择处理路径：PT 走 exposure + tonemapping；GS 根据 `GsColorSpace` 处理 display-referred 颜色。Swapchain 与 ImGui 始终使用 SRGB view；`srgb_rec709_display` 输入在 Present shader 内先做 sRGB→linear，再由 SRGB attachment 编码输出。
