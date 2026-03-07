# Himalaya 渲染器 — 设计文档索引

---

## 会话入口

| 文件 | 说明 |
|------|------|
| `agent-context.md` | Agent 会话启动入口：当前进度、下一步任务、必读/按需文档列表 |

---

## 项目基础

| 文件 | 说明 |
|------|------|
| `requirements-and-philosophy.md` | 项目定位、硬件目标、9 条技术选型原则、画面质量目标、场景假设 |
| `technical-decisions.md` | 所有技术模块的最终选型结果与渐进演进路线（Pass 1/2/3） |
| `decision-process.md` | 每项技术选型的推理过程：候选方案、排除理由、关键权衡 |
| `architecture.md` | 渲染器长远架构：核心特征、四层结构、架构约束、Shader 数据分层 |

---

## Milestone 1（当前开发目标）

### M1 整体设计（milestone-1/）

| 文件 | 说明 |
|------|------|
| `milestone-1/milestone-1.md` | M1 范围定义：预期效果、已知局限性、包含 / 不包含的功能清单 |
| `milestone-1/m1-implementation-ref.md` | M1 实现参考：目标文件结构、架构实现选择、关键接口与数据结构 |
| `milestone-1/m1-frame-flow.md` | M1 一帧的完整 Pass 执行顺序、输入输出、MSAA 处理策略、资源生命周期 |
| `milestone-1/m1-development-order.md` | M1 的 8 个开发阶段、每阶段的产出、阶段间依赖关系 |

### 当前阶段计划

| 文件 | 说明 |
|------|------|
| `current-phase.md` | 当前阶段（M1 阶段二）实现步骤、文件清单、技术笔记 |

---

## 后续里程碑（roadmap/）

开发 M1 期间无需关注，记录的是后续阶段的规划。

| 文件 | 说明 |
|------|------|
| `roadmap/milestone-2.md` | M2：画质全面提升（SSR、SSGI、PCSS、Bruneton、POM、FSR/DLSS 等） |
| `roadmap/milestone-3.md` | M3：动态物体支持 + 画面补全 + 性能优化（点光源阴影、Froxel、Clustered、LOD 等） |
| `roadmap/milestone-future.md` | 远期 / 可选：体积云、水体、地形、粒子、Streaming、混合管线 RT 方向等 |

---

## 归档（archive/）

| 文件 | 说明 |
|------|------|
| `archive/conversation-initial-design.md` | 初始设计的完整对话记录，所有决策的原始讨论上下文 |
| `archive/m1-phase1-plan.md` | M1 阶段一（最小可见三角形）的 7 个实现步骤（已完成） |
