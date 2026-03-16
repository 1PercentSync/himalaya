# 渲染技术栈决策结果

> 本文档仅记录最终选定的技术方案及其渐进演进路线。
> 决策推理过程见 `decision-process.md`，需求与设计理念见 `requirements-and-philosophy.md`。

---

## 0. Vulkan API 基线

**Vulkan 1.4**

| 核心特性 | 用途 |
|---------|------|
| Dynamic Rendering | 替代 VkRenderPass / VkFramebuffer，与 Render Graph 天然契合 |
| Synchronization2 | 更清晰的 barrier API，Render Graph barrier 自动插入受益 |
| Extended Dynamic State | 减少 pipeline 数量，viewport/scissor/cull mode/depth 等动态设置 |
| Descriptor Indexing | Bindless 纹理支持 |

---

## 1. 渲染框架

**Forward+**

| 演进 | 光源 Culling 方案 |
|------|-------------------|
| Pass 1 | 暴力 Forward（光源数组直接传入 shader） |
| Pass 2 | Tiled Forward（2D 屏幕 Tile 光源裁剪，Compute Pass） |
| Pass 3 | Clustered Forward（3D 深度方向扩展） |

---

## 2. PBR / BRDF

**参数化：Metallic-Roughness 工作流**（glTF 标准）

### 漫反射

| 演进 | 方案 |
|------|------|
| Pass 1 | Lambert |
| Pass 2 | Burley Diffuse |

### 镜面反射（Cook-Torrance）

| 组件 | 方案 | 演进 |
|------|------|------|
| D（NDF） | GGX | Pass 2：Multiscatter GGX 能量补偿（预计算 LUT） |
| G（几何遮蔽） | Smith Height-Correlated GGX | 锁定 |
| F（菲涅尔） | Schlick 近似 | 锁定 |

---

## 3. 阴影

### 方向光

| 演进 | 方案 |
|------|------|
| Pass 1 | 基础 Shadow Map |
| Pass 2 | CSM（cascade blend、texel snapping） |
| Pass 3 | CSM + SDF 远距离混合（**条件性**：仅当存在 SDF 基础设施） |

无 SDF 时到 CSM 即为终点。

### 点光源 / 聚光灯

| 演进 | 方案 |
|------|------|
| Pass 2 | 独立 Cubemap SM / 单张 SM + 缓存策略（Pass 1 不实现） |
| Pass 3 | Shadow Atlas 统一管理（按需） |

### 软阴影

| 演进 | 方案 |
|------|------|
| Pass 1 | PCF |
| Pass 2 | PCF + Contact Shadows（独立屏幕空间模块） |
| Pass 3 | PCSS（替换 PCF，Contact Shadows 保留） |

---

## 4. 全局光照

### 漫反射 GI

**核心：多套 Lightmap Blend + SSGI + Light Probes**

- 时间变化：多时间段 Lightmap 插值
- 门状态：开/关各一套 Lightmap（设计约束：同时仅一扇可见门可交互）
- 过渡态：SSGI 补充
- 动态物体：Light Probes 采样 Lightmap 插值

| 演进 | 内容 |
|------|------|
| Pass 1 | 基础环境光（常量 ambient / 半球光） |
| Pass 2 | 烘焙 Lightmap（单套）+ Light Probes |
| Pass 3 | SSGI + 多套 Lightmap blend |

### 镜面反射 GI

| 演进 | 方案 |
|------|------|
| Pass 1 | 天空盒 Cubemap（IBL，Split-Sum 近似） |
| Pass 2 | Reflection Probes（视差校正） |
| Pass 3 | SSR + Reflection Probes 混合 |

### 远期可选

Irradiance Probes + SDF GI（条件性，依赖 SDF 基础设施）

---

## 5. 环境光遮蔽

| 演进 | 方案 |
|------|------|
| Pass 1 | SSAO + per-effect temporal filtering（同时搭建 per-effect temporal 基础设施，SSGI/SSR 复用） |
| Pass 2 | GTAO（替换遮挡计算公式） |

跳过 HBAO。烘焙 AO 随 Lightmap 和材质管线自然覆盖。

> **Per-effect temporal vs 全画面 temporal**：per-effect temporal 作用于独立的中间效果纹理（AO、SSR 等），不能平滑嵌在最终颜色中的噪声（如 specular aliasing）。全画面 temporal（FSR/DLSS SDK 自带）对最终颜色做时域积累，覆盖所有信号噪声，依赖 Motion Vectors。两者是独立系统，不共享基础设施。

---

## 6. 抗锯齿与上采样

**不自研 TAA，使用 SDK**

| 配置模式 | 说明 |
|----------|------|
| 纯 MSAA | Forward+ 原生支持 |
| 纯 Upscaler | FSR native AA 或 DLAA/DLSS |
| MSAA + Upscaler | 质量最高 |

SDK：**DLSS**（N 卡首选）+ **FSR**（通用回退），统一接口层。XeSS 远期可选。

关键基础设施：**Motion Vectors**（所有 temporal 效果共享依赖）。

---

## 7. 色彩管线

| 项目 | 方案 |
|------|------|
| 工作色彩空间 | sRGB 线性 |
| 浮点格式 | R11G11B10F / R16G16B16A16F |
| 广色域 | 不实现，架构预留 |
| HDR 输出 | 远期可选，不排入当前计划 |

### Tonemapping

| 演进 | 方案 |
|------|------|
| Pass 1 | ACES 拟合 |
| Pass 2 | Khronos PBR Neutral |

---

## 8. 后处理

所有后处理设计为**插件化**，可独立启用/禁用，排在渲染主体完成之后实现。

### 确定实现

Bloom、自动曝光（EV 范围限制 + 曝光补偿 + 加权测光）、Color Grading / LUT、Vignette、Chromatic Aberration、Film Grain

### 渐进实现

| 效果 | 演进 |
|------|------|
| DOF | Gaussian → 半分辨率 gather-based Bokeh（Bokeh 标记为**可选**） |
| Motion Blur | Camera Motion Blur → Per-Object Motion Blur |
| 体积光占位 | Screen-Space God Rays → Froxel Volumetric Fog 实现后退役 |

### 低优先级

Lens Flare（基于 Bloom 降采样）

---

## 9. 大气与天空

### 天空

| 演进 | 方案 |
|------|------|
| Pass 1 | 静态 HDR Cubemap |
| Pass 2 | Bruneton 大气散射（跳过 Hosek-Wilkie） |

### 雾

| 演进 | 方案 |
|------|------|
| Pass 1 | 高度雾 |
| Pass 2 | Bruneton aerial perspective（与天空 Pass 2 同步） |

### 云

| 演进 | 方案 |
|------|------|
| Pass 1 | 无云 |
| Pass 2 | 2D 云层 |

远期可选：体积云（复杂度高，2D 云层配合 Bruneton 天空已足够）

---

## 10. 透明、半透明与体积渲染

### 透明

| 情况 | 方案 |
|------|------|
| 默认 | 简单排序后 Alpha Blending |
| 按需 | WBOIT（出现排序问题时切换） |

### 折射

屏幕空间折射

### 次表面散射

| 演进 | 方案 |
|------|------|
| Pass 1 | Pre-Integrated SSS |
| Pass 2 | Screen-Space SSS |

归入角色渲染需求，需要角色时才考虑。

### 体积渲染

| 演进 | 方案 |
|------|------|
| Pass 1 | Screen-Space God Rays（占位） |
| Pass 2 | Froxel-Based Volumetric Fog |

### 水体

静态水体渲染：反射（SSR + Probes）、折射（屏幕空间）、法线贴图、深度着色、菲涅尔（PBR 已有）、法线计算投影焦散（可预烘焙）、岸线泡沫。

---

## 11. 几何管线

### 剔除

| 演进 | 方案 |
|------|------|
| 基础 | 视锥剔除（CPU 端） |
| 进阶 | Hardware Occlusion Query（保守两趟策略，零瑕疵） |

### LOD

离散 LOD + dithering cross-fade

### 实例化

CPU 侧 mesh_id 分组 + per-instance SSBO + instanced draw（M1 阶段四准备工作引入）

### 不采用

GPU-Driven Rendering、Mesh Shader、HLOD

---

## 12. 纹理、烘焙、地形、粒子、Streaming

### 纹理格式

| 演进 | 方案 |
|------|------|
| Pass 1 | 运行时 BC 压缩 + KTX2 缓存（BC7 通用 + BC5 法线，首次 CPU 压缩后缓存） |
| Pass 2 | 离线预处理管线（构建时生成 KTX2，运行时直接加载） |

POM 纳入计划。

### Virtual Texturing

不实现，Lightmap 采样接口预留替换可能。

### 烘焙

| 演进 | 方案 |
|------|------|
| Pass 1 | 离线工具（Blender 等） |
| Pass 2 | RT 烘焙（混合管线 RT 的第一个应用场景） |

### 地形

Heightmap（Clipmap LOD、Splatmap 材质混合）+ Mesh 补全（洞穴、悬崖等）

### 粒子

| 演进 | 方案 |
|------|------|
| Pass 1 | CPU 粒子 |
| Pass 2 | GPU 粒子（Compute Shader） |

### Streaming

初期全部预加载。中期搭建异步资源加载框架（Vulkan Transfer Queue），之后按需实现纹理 Mip Streaming、Lightmap 压缩 + 区域加载。Mesh Streaming 和 World Streaming 优先级低。
