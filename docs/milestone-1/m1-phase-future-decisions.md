# M1 未来阶段设计构想

> **非正式决定**：以下内容是早期规划阶段的初步构想，实际实现时可能调整。
> 各条目标注了原始讨论所在的 phase，以便追溯。

---

## Set 1 — 持久纹理资产预留（阶段八构想，来自阶段二规划）

| 纹理 | Set 1 binding | 目标阶段 |
|------|--------------|---------|
| Lightmap | `sampler2D[]` (binding 0) | 阶段八 |
| Reflection Probes | `samplerCube[]` (binding 1) | 阶段八 |

阶段二设计 Set 1 bindless 架构时预留的容量规划。

---

## Set 0 — RT 资源扩展（阶段六构想）

| 资源 | Set 0 binding | 目标阶段 |
|------|--------------|---------|
| TLAS (acceleration structure) | binding 4 | 阶段六 |
| Geometry Info SSBO | binding 5 | 阶段六 |

阶段六引入 RT 基础设施时扩展 Set 0，供 RT Pipeline 和 Ray Query shader 访问场景加速结构和几何数据。光栅化 shader 不引用这些 binding。

---

## MSAA Resolve 改造（阶段九构想，来自阶段三规划）

阶段九透明 Pass 也写入 MSAA color buffer，color resolve 必须在透明 pass 之后才能做。届时 Forward Pass 的 Dynamic Rendering color resolve 移除，改为在透明 pass 结束时配置 resolve。改造成本低——只是把 `resolveImageView` 配置从 Forward Pass 的 attachment info 移到透明 Pass 的 attachment info。

---

## 后处理链 Compute 化（阶段十构想，来自阶段三规划）

阶段十引入完整后处理链（Tonemapping → Vignette → Color Grading）后，所有后处理改为 compute shader 输出到中间 LDR buffer，末端新增 Final Output fullscreen fragment pass 拷贝到 SRGB swapchain。届时 `tonemapping.frag` 改为 `tonemapping.comp`，`fullscreen.vert` 保留给 Final Output pass 复用。

阶段十后处理链改为 compute shader 后，compute pass 的输出通过 storage image（`imageStore`）写入，不经过 Set 2。输入仍可通过 Set 2 采样（支持硬件双线性过滤）。Auto Exposure 输出走 GlobalUBO 的 exposure 字段（GPU 写回 buffer），不占 Set 2 binding。

---

## 材质变体演进（阶段八/M2 构想，来自阶段三规划）

| 阶段 | 策略 | 场景 |
|------|------|------|
| 阶段八 | 动态分支 | Lightmap 按 per-object 属性条件执行（warp 不发散，GPU 开销为零） |
| M2+ | `#define` 编译变体 | POM ray march 循环显著增加 shader 体积和寄存器压力 |
| M2+ 可选 | Specialization Constants | 从 `#define` 迁移以减少编译次数 |

---

## RenderFeatures 后处理扩展（阶段十构想，来自阶段四规划）

阶段四设计 RenderFeatures 机制时预留了后处理 pass 的运行时开关扩展空间（阶段十 Bloom、Vignette 等后处理 pass 各自的 bool flag）。

---

## GTAO 深度 MIP 层级（M1 末尾评估，来自阶段五 Step 10 审查）

当前 GTAO 直接采样全分辨率深度。大半径时远处步进的纹理缓存命中率低。XeGTAO 构建 5 级深度 MIP 层级（偏向远端的加权滤波器），远处步进读粗 MIP，缓存效率显著提升。

方案概要：新增 depth prefilter compute pass，输入硬件深度 → 输出线性 view-space depth 的 5 级 MIP 链（R16F 或 R32F）。GTAO shader 按步进距离选择 MIP（`mipLevel = clamp(log2(offsetLength) − offset, 0, 4)`）。

M1 分辨率和默认半径（0.5m）下缓存压力不显著。在 M1 所有 Step 完成后进行性能评估，若 GTAO pass 耗时超预期再实施。

---

## Visibility Bitmask AO / VBAO（潜在升级路径，来自阶段五 Step 10 审查）

GTAO 的 heightfield 假设将每个可见表面视为无限厚。Thickness heuristic（Step 10a）能缓解但不能根治薄物体（围栏、密集植被）的光晕问题。

Visibility Bitmask（Therrien et al., 2023）将 GTAO 的两个 horizon 角替换为 32-bit 遮挡/可见扇区掩码，允许光线穿过恒定厚度的表面背后。Bevy 引擎在 v0.15 从 GTAO 迁移到此方案。

这是核心算法替换（horizon search → bitmask 积累），shader 需大幅改写。仅在实测发现 thickness heuristic 不足以应对场景中的薄物体时实施，优先级在 M2 或更后。
