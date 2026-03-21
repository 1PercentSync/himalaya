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
