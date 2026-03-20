# Milestone 1：开发顺序

> 本文档记录 M1 的分阶段开发计划，每个阶段结束都有可见的、可验证的产出。
> 帧流程详见 `m1-frame-flow.md`，设计决策详见 `m1-design-decisions.md`，接口定义详见 `m1-interfaces.md`。

---

## 设计逻辑

1. **每个阶段有可见产出** — 不会在看不到结果的基础设施上连续工作太久
2. **尊重依赖关系** — Render Graph 先于所有 pass，深度 PrePass 先于屏幕空间效果
3. **基础设施复用** — temporal filtering 在阶段五搭建后，M2 的 SSR/SSGI 可直接复用

---

## 阶段一：最小可见三角形

**Layer 0 核心**

- Vulkan 初始化（Instance、Device、Queue、Swapchain）
- VMA 内存分配集成
- Shader 运行时编译（GLSL → SPIR-V，shaderc 集成）
- Command Buffer 录制辅助
- 基础资源创建（Buffer、Image）
- 基础 Pipeline 创建
- 一个硬编码的三角形能显示在屏幕上

**产出：** 验证整个 Vulkan 基础设施能跑通。这是后续所有工作的地基。

---

## 阶段二：基础渲染管线

**Layer 0 补全 + Layer 1 框架 + Layer 3 重构与加载**

- Bindless descriptor 管理（DescriptorManager：set layout、descriptor set、bindless 纹理注册/注销）
- Mesh 加载（fastgltf 解析 glTF、统一顶点格式）
- 纹理加载（stb_image 解码 JPEG/PNG、GPU 端 mip 生成）
- 材质系统基础（全局 Material SSBO + push constant 数据流、标准 PBR 参数布局）
- 相机（framework 层 Camera 数据结构 + app 层自由漫游控制器）
- 场景渲染接口（完整 SceneRenderData 结构，阶段二只填充需要的字段）
- glTF 场景加载 → 填充渲染列表
- 视锥剔除（CPU 端 AABB-frustum）
- Render Graph 骨架（barrier 自动插入 + 资源导入，typed handle 引用）
- App 层拆分（main.cpp → application / scene_loader / camera_controller / debug_ui）

**产出：** 能加载一个 glTF 场景，用基础 Lit shader（Lambert + 从 LightBuffer 读取方向光）渲染出来，相机能移动，ImGui 能显示。画面上就是一个有纹理和基础光照的静态场景。

**暂不实现：** RG transient 资源管理（阶段三引入）、RG temporal 资源管理（阶段五引入）、Pass 抽象基类（阶段三引入）。BC 纹理压缩在阶段四准备工作引入（运行时压缩 + KTX2 缓存）。

> 详细步骤见 `../current-phase.md`

---

## 阶段三：PBR 光照基础

**Layer 2 核心 Pass**

- Forward Lighting Pass（Lambert 漫反射 + Cook-Torrance 镜面反射：GGX / Smith / Schlick）
- 方向光直接光照
- IBL 环境光（Split-Sum 近似、BRDF Integration LUT 预计算、HDR Cubemap 加载）
- Depth + Normal PrePass
- MSAA 开启 + Depth Resolve + Normal Resolve + Color Resolve
- Tonemapping Pass（ACES 拟合，fullscreen fragment shader，直接写 swapchain）

**产出：** 场景有了基本正确的 PBR 光照——直接光 + 环境光。金属表面反射天空 Cubemap。HDR 渲染结果经过 Tonemapping 映射到 LDR。但没有阴影、没有 AO，画面会偏平。

---

## 阶段四：阴影

- Instancing：per-instance SSBO + mesh_id 分组 + instanced draw，push constant 72→4 bytes
- 运行时场景/HDR 加载：ImGui + Windows 原生对话框 + JSON 配置持久化，CLI11 退役
- 缓存基础设施 + BC 纹理压缩 + IBL 缓存：BC7 + BC5 + KTX2 缓存 + IBL readback 缓存
- RenderFeatures 运行时开关机制（为后续可选效果奠基）
- 直接实现 CSM（cascade=1 起步，不经单张中间步骤，4 cascade 2048²，PSSM 分割、texel snapping）
- PCF 软阴影 + cascade blend + dual bias（slope + normal offset，constant 对 D32Sfloat 无效已移除）
- Alpha mask 阴影（双 pipeline：opaque + mask）
- Shadow 采样集成到 Forward Lighting Pass（feature_flags 条件分支）
- Culling 模块泛化（通用 frustum 剔除，shadow + camera 共用）
- Cascade 数量 / 分辨率运行时可调

**产出：** 场景有了阴影，立体感大幅提升。但物体接地感还差一些（没有 AO 和 Contact Shadows）。

---

## 阶段五：屏幕空间效果

- SSAO Pass
- Temporal filtering 基础设施（历史 buffer 管理、相机 reprojection）
- SSAO Temporal Filter Pass
- Contact Shadows Pass

**产出：** 物体之间、角落里有了自然的暗部过渡（AO），物体贴地处有了精细的接触阴影。画面层次感明显提升。

**战略价值：** temporal filtering 基础设施（per-effect 历史 buffer + reprojection + blend）在此搭建，M2 的 SSR、SSGI 可直接复用同一模式。注意 DLSS/FSR 的全画面 temporal accumulation 是 SDK 自带实现，依赖 Motion Vectors 而非此基础设施。

---

## 阶段六：间接光照

- Lightmap 加载与 UV2 处理
- Lightmap 采样集成到 Forward Lighting Pass
- Reflection Probes 数据加载
- Reflection Probe 视差校正采样集成到 Forward Lighting Pass

**产出：** 室内场景有了间接光照（Lightmap），不再是纯黑的角落。光滑表面反射周围环境（Reflection Probes），不再只反射天空。这是画面写实度的一个重要跳跃。

---

## 阶段七：透明

- 透明物体排序
- HDR Color Buffer 拷贝（折射源）
- Transparent Pass（Alpha Blending + 屏幕空间折射）

**产出：** 玻璃窗、半透明物体正确显示，有折射效果。

**开发前检查：** transparent.frag 需要复用 forward.frag 的大量光照代码（PBR 直射光、IBL、阴影、AO 等），开发前评估是否需要将 forward.frag 光照逻辑重构为可复用的 `common/lighting.glsl`。

---

## 阶段八：后处理链 + 大气

- 自动曝光（亮度降采样到 1×1 + 时域平滑）
- Bloom（降采样链 + 升采样链）
- 高度雾 Pass
- Tonemapping 演进（阶段三已实现基础 ACES 拟合，此处升级）：
  - 从 fullscreen fragment shader 迁移到 compute shader（LDR buffer 非 SRGB swapchain，可以用 STORAGE_BIT）
  - 从直接写 swapchain 改为写中间 LDR buffer（后处理链串联：HDR → Bloom → Tonemapping → LDR → Vignette → Color Grading → Final Output → Swapchain）
- Vignette Pass
- Color Grading Pass

**产出：** M1 完成。画面有完整的 HDR → LDR 管线，高光有光晕，曝光自动适应，远景融入雾中，画面有风格感。

---

## 阶段间依赖总结

```
阶段一 ──→ 阶段二 ──→ 阶段三 ──→ 阶段四
              │                      │
              │                      ↓
              │            阶段五（依赖阶段三的 Depth/Normal）
              │                      │
              │                      ↓
              │            阶段六（集成到阶段三的 Forward Pass）
              │                      │
              │                      ↓
              │            阶段七（依赖阶段三的 HDR Color、Depth）
              │                      │
              │                      ↓
              └──────────→ 阶段八（依赖阶段二的 Render Graph）
```

阶段四、五、六在代码依赖上都需要阶段三完成，但它们之间没有硬依赖——理论上可以调换顺序。选择当前顺序的理由是：阶段四的阴影对画面立体感的提升最直接，阶段五的 temporal filtering 是重要基础设施，阶段六的间接光照在有了阴影和 AO 之后观感对比更明显。
