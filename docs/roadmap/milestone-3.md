# Milestone 3：动态物体 + 画面补全 + 性能优化

> **目标**：支持动态物体（非角色），补全画面明显短板，具备应对更大场景规模的性能手段。

---

## 效果 / 画质优化

| 项目 | 说明 |
|------|------|
| 点光源/聚光灯阴影 + 缓存策略 | 补全室内灯具投影阴影的画面缺口。独立 Cubemap/单张 SM，静态光源缓存不重渲染 |
| Froxel Volumetric Fog | 替换 Screen-Space God Rays。体积光柱、晨雾阳光等效果质变，与点光源阴影配合提升室内氛围 |
| Multiscatter GGX | 高粗糙度金属表面亮度修正。一张 LUT + 十几行 shader，IBL LUT 基础设施已有 |
| Khronos PBR Neutral | 替换 ACES tonemapping，PBR 材质所见即所得，最小化 tonemapping 对材质外观的干扰 |
| 门状态 Lightmap | 门完全开/完全关各烘焙一套 Lightmap，过渡期 SSGI 补充（设计约束：同时仅一扇可见门可交互）|
| 卡通渲染着色模型（场景） | 引入第二种着色模型（色阶化光照、风格化阴影等），Forward+ 下不同物体绑不同 pipeline 天然适配。首次需要材质系统支持多 pipeline 变体 |

---

## 动态物体支持

| 项目 | 说明 |
|------|------|
| Light Probes | 动态物体接收间接光的基础，采样 Lightmap 插值结果 |
| Per-object motion vectors | 动态物体运动后所有 temporal 效果（DLSS/FSR、Motion Blur、temporal filtering）正常工作的基础设施。M2 已接入的 DLSS/FSR 效果完整化 |
| Per-Object Motion Blur | 替换 Camera Motion Blur，物体移动有独立的运动模糊 |
| MSAA + TAA/upscaler 组合模式 | 开放质量档位配置选项（纯 MSAA / 纯 Upscaler / MSAA + Upscaler） |

---

## 性能 / 规模优化

| 项目 | 说明 |
|------|------|
| Clustered Forward | 从 Tiled 升级，沿深度方向切分为 3D Cluster，光源数量增长后的性能保障 |
| Hardware Occlusion Query | 保守两趟策略，零瑕疵的遮挡剔除。复用深度 PrePass，室内多房间场景收益大 |
| ~~Instancing~~ | ~~已提前到 M1 阶段四准备工作实现（CPU 侧 mesh_id 分组 + per-instance SSBO）~~ |
| 离散 LOD + dithering cross-fade | 远处物体减面，大场景性能保障 |
| Shadow Atlas | 多光源阴影的统一管理和调度，按重要性分配分辨率、按需更新 |

---

## 优先级说明

M3 内部三个方向可根据 Demo 需求灵活安排优先级：

- 展示角色/动态物体 → 动态物体支持优先
- 展示更大场景 → 性能优化优先
- 展示室内灯光氛围 → 效果优化优先
