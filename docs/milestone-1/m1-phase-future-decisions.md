# M1 未来阶段设计构想

> **非正式决定**：以下内容是早期规划阶段的初步构想，实际实现时可能调整。
> 各条目标注了原始讨论所在的 phase，以便追溯。

---

## Set 1 — 持久纹理资产预留（阶段六构想，来自阶段二规划）

| 纹理 | Set 1 binding | 目标阶段 |
|------|--------------|---------|
| Lightmap | `sampler2D[]` (binding 0) | 阶段六 |
| Reflection Probes | `samplerCube[]` (binding 1) | 阶段六 |

阶段二设计 Set 1 bindless 架构时预留的容量规划。

---

## MSAA Resolve 改造（阶段七构想，来自阶段三规划）

阶段七透明 Pass 也写入 MSAA color buffer，color resolve 必须在透明 pass 之后才能做。届时 Forward Pass 的 Dynamic Rendering color resolve 移除，改为在透明 pass 结束时配置 resolve。改造成本低——只是把 `resolveImageView` 配置从 Forward Pass 的 attachment info 移到透明 Pass 的 attachment info。

---

## 后处理链 Compute 化（阶段八构想，来自阶段三规划）

阶段八引入完整后处理链（Tonemapping → Vignette → Color Grading）后，所有后处理改为 compute shader 输出到中间 LDR buffer，末端新增 Final Output fullscreen fragment pass 拷贝到 SRGB swapchain。届时 `tonemapping.frag` 改为 `tonemapping.comp`，`fullscreen.vert` 保留给 Final Output pass 复用。

阶段八后处理链改为 compute shader 后，compute pass 的输出通过 storage image（`imageStore`）写入，不经过 Set 2。输入仍可通过 Set 2 采样（支持硬件双线性过滤）。Auto Exposure 输出走 GlobalUBO 的 exposure 字段（GPU 写回 buffer），不占 Set 2 binding。

---

## 材质变体演进（阶段六/M2 构想，来自阶段三规划）

| 阶段 | 策略 | 场景 |
|------|------|------|
| 阶段六 | 动态分支 | Lightmap 按 per-object 属性条件执行（warp 不发散，GPU 开销为零） |
| M2+ | `#define` 编译变体 | POM ray march 循环显著增加 shader 体积和寄存器压力 |
| M2+ 可选 | Specialization Constants | 从 `#define` 迁移以减少编译次数 |

---

## RenderFeatures 后处理扩展（阶段八构想，来自阶段四规划）

阶段四设计 RenderFeatures 机制时预留了后处理 pass 的运行时开关扩展空间（阶段八 Bloom、Vignette 等后处理 pass 各自的 bool flag）。
