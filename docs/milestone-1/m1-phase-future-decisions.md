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

## Set 2 — 未来阶段预留 binding（来自阶段三规划）

| Binding | 类型 | 名称 | 目标阶段 |
|---------|------|------|---------|
| 6 | `sampler2D` | bloom_texture | 阶段八 |
| 7 | `sampler2D` | refraction_source | 阶段七 |

阶段三设计 Set 2 layout 时预留的 binding slot。具体产生者/消费者待各阶段规划时确定。
