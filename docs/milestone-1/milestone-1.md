# Milestone 1：静态场景演示

> **目标**：实现一个场景和光源静态、镜头可自由移动的 Demo，画面写实度说得过去。
> 场景类型：室内 + 室外。
> 包含 GPU 路径追踪烘焙器（自烘焙 Lightmap / Reflection Probes）和 PT 参考视图。

---

## 预期效果

室内部分有烘焙的间接光照（Lightmap），光线从窗户照进来反弹照亮房间内部，效果柔和自然。室外部分有方向光阴影覆盖近中远距离（CSM），阴影边缘有合理的软化（PCF），物体贴地处有细致的接触阴影（Contact Shadows）。所有表面都是 PBR 材质，金属表面反射周围环境（Reflection Probes 视差校正）。画面有 AO 给出的空间层次感（SSAO），物体接缝、角落自然变暗。天空是一张静态 HDR 图。室外远景通过高度雾融入天空。透明物体有基本的透明混合和屏幕空间折射。后处理给画面加上 Bloom 光晕、合理曝光和 Color Grading 调色。

整体观感是一个**光照质量不错的静态场景漫游**。

---

## 已知局限性

| 局限 | 原因 | 解决时间 |
|------|------|----------|
| 反射不精确 | Reflection Probes 是近似的，光滑地面无精确镜面反射 | M2（SSR） |
| 阴影软硬一致 | PCF 软阴影宽度固定，不随遮挡距离变化 | M2（PCSS） |
| 无动态间接光 | 靠近红墙时白色物体不会被染红（仅 Lightmap 静态 color bleeding） | M2（SSGI） |
| 天空静止 | 无昼夜变化 | M2（Bruneton） |
| 无体积光效果 | 无光柱、无雾气中光散射 | M2（God Rays）/ M3（Froxel） |
| 仅 MSAA 抗锯齿 | 几何边缘干净但高光闪烁和着色锯齿未处理 | M2（FSR SDK） |
| 透明物体暂缺 | Phase 3 Step 5（EQUAL depth test）到 Phase 7 期间，`AlphaMode::Blend` 物体不渲染 | M1 Phase 7（Transparent Pass） |
| 单方向光限制 | 仅支持 1 盏方向光（含 CSM 阴影），多方向光 CSM 需多张 shadow map + per-light cascade 数据 | M2 |
| 无动态光源阴影 | 只有方向光有阴影，室内点光源/聚光灯无投影阴影 | M3 |

---

## 实现内容

### 渲染框架

- Forward+（暴力 Forward，无 Light Culling）
- 视锥剔除（CPU 端）
- Instancing（CPU 侧 mesh_id 分组 + per-instance SSBO + instanced draw）

### 材质

- PBR：Lambert 漫反射 + Cook-Torrance 镜面反射（GGX / Smith Height-Correlated / Schlick）
- 参数化：Metallic-Roughness 工作流
- 法线贴图

### 光照

- 方向光直接光照（限 1 盏）
- IBL 环境光（Split-Sum 近似，BRDF Integration LUT 预计算）
- 单套烘焙 Lightmap（GPU PT 烘焙器自烘焙）
- Reflection Probes（视差校正 Parallax Corrected Cubemap，GPU PT 烘焙器自烘焙）

### 阴影

- CSM（多级 cascade、分割策略、texel snapping、cascade blend）
- PCF 软阴影
- Contact Shadows（独立屏幕空间 Pass）

### 环境光遮蔽

- AO（GTAO 算法）+ temporal filtering（同时搭建 temporal filtering 基础设施）

### 抗锯齿

- MSAA（Forward+ 原生支持）

### 透明 / 半透明

- 简单排序后 Alpha Blending
- 屏幕空间折射

### 天空 / 大气

- 静态 HDR Cubemap 天空
- 高度雾

### 后处理

- Bloom（多级降采样 + 升采样链）
- 自动曝光（亮度降采样到 1×1 + 时域平滑，EV 范围限制 + 曝光补偿 + 加权测光）
- ACES Tonemapping（拟合曲线）
- Vignette
- Color Grading

### 路径追踪

- RT 基础设施（Vulkan RT 扩展、BLAS/TLAS 加速结构、RT Pipeline 抽象）
- GPU PT 烘焙器（Lightmap + Reflection Probes，NEE + MIS + 低差异序列 + Blue noise + OIDN GPU 降噪）
- PT 参考视图（accumulation debug 渲染 + OIDN viewport denoising，相机静止时持续累积采样）

### 不包含

- 多套 Lightmap blend / 门状态 Lightmap
- SSGI / SSR
- 动态光源阴影
- Motion Vectors / Motion Blur / DOF
- 体积雾 / God Rays
- LOD / 遮挡剔除
- 粒子系统
- 地形 / 水体
- SSS
- 纹理 Streaming
- 离线纹理预处理（M1 使用运行时 BC 压缩 + KTX2 缓存）
