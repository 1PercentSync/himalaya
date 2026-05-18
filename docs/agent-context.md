# Agent 会话上下文

> 每次会话开始时阅读本文档，快速定位当前状态和所需上下文。

---

## 当前位置

- **项目**：Himalaya — 基于 Vulkan 1.4 的渲染器
- **分支**：`reflector` — Path Tracing + Gaussian Splatting
- **Phase**：Phase 1 — 管线精简（移除光栅化和烘焙管线）
- **进度**：Step 0 文档归档与重建进行中

### 下一个任务

Step 0：创建 `docs/technical-decisions.md` + 归档源文档

---

## 必读文档

CLAUDE.md 已自动加载，以下为额外必读：

| 文档 | 说明 |
|------|------|
| `docs/roadmap.md` | Reflector 分支 phase 概览与顺序 |
| `docs/current-phase.md` | Phase 1 精简范围与步骤概述 |
| `tasks/reflector-phase1.md` | Phase 1 任务清单（复选框进度跟踪） |

---

## 按需文档

### M1 参考（待归档）

以下文档属于 main 分支的 M1 路线，待 Step 0 后续归档步骤完成后移入归档区。精简过程中可能需要参考（了解当前代码的设计背景）。

| 文档 | 说明 |
|------|------|
| `docs/milestone-1/milestone-1.md` | M1 范围、预期效果、已知局限性 |
| `docs/milestone-1/m1-design-decisions-core.md` | M1 核心设计决策参考 |
| `docs/milestone-1/m1-rt-decisions.md` | M1 RT 架构决策（阶段六~八点五） |
| `docs/milestone-1/m1-interfaces.md` | M1 接口与目标结构 |
| `docs/milestone-1/m1-development-order.md` | M1 开发阶段顺序 |
| `docs/milestone-1/m1-frame-flow.md` | M1 完整帧流程 |
| `docs/milestone-1/m1-phase-future-decisions.md` | M1 未来阶段设计构想 |

### 项目级文档（待归档重写）

| 文档 | 说明 |
|------|------|
| `docs/project/architecture.md` | 渲染器长远架构、四层结构、架构约束 |
| `docs/project/technical-decisions.md` | 所有技术模块的最终选型结果与演进路线 |
| `docs/project/decision-process.md` | 每项选型的推理过程、候选方案、排除理由 |
| `docs/project/requirements-and-philosophy.md` | 项目定位、技术选型原则、画面质量目标 |

### Roadmap（待归档）

| 文档 | 说明 |
|------|------|
| `docs/roadmap/milestone-2.md` | M2 规划 |
| `docs/roadmap/milestone-3.md` | M3 规划 |
| `docs/roadmap/milestone-future.md` | 远期可选目标 |

---

## 归档文档

| 文档 | 说明 |
|------|------|
| `docs/archive/m1-phase8.5-plan.md` | M1 阶段八点五实现步骤（原 current-phase.md） |
| `tasks/archive/m1-phase8.5.md` | M1 阶段八点五任务清单 |
| `docs/archive/conversation-initial-design.md` | 初始设计的完整对话记录 |
| `docs/archive/m1-phase1-plan.md` | M1 阶段一实现步骤（已完成） |
| `docs/archive/m1-phase2-plan.md` | M1 阶段二实现步骤（已完成） |
| `docs/archive/m1-phase2-tasks.md` | M1 阶段二任务清单（已完成） |
| `docs/archive/m1-phase3-plan.md` | M1 阶段三实现步骤（已完成） |
| `tasks/archive/m1-phase3.md` | M1 阶段三任务清单（已完成） |
| `docs/archive/pcss-reference.md` | PCSS 方向光实现参考资料 |
| `docs/archive/gtao-reference.md` | GTAO 算法实现参考资料 |
| `docs/archive/m1-phase4-plan.md` | M1 阶段四实现步骤（已完成） |
| `tasks/archive/m1-phase4.md` | M1 阶段四任务清单（已完成） |
| `docs/archive/m1-phase1-decisions.md` | M1 阶段一设计决策（已完成） |
| `docs/archive/m1-phase2-decisions.md` | M1 阶段二设计决策（已完成） |
| `docs/archive/m1-phase3-decisions.md` | M1 阶段三设计决策（已完成） |
| `docs/archive/m1-phase4-decisions.md` | M1 阶段四设计决策（已完成） |
| `docs/archive/contact-shadows-reference.md` | Contact Shadows 实现参考资料 |
| `docs/archive/m1-phase5-plan.md` | M1 阶段五实现步骤（已完成） |
| `tasks/archive/m1-phase5.md` | M1 阶段五任务清单（已完成） |
| `docs/archive/m1-phase5-decisions.md` | M1 阶段五设计决策（已完成） |
| `docs/archive/m1-phase6-plan.md` | M1 阶段六实现步骤（已完成） |
| `tasks/archive/m1-phase6.md` | M1 阶段六任务清单（已完成） |
| `docs/archive/path-tracing-reference.md` | 实时路径追踪技术综述参考资料 |
| `docs/archive/m1-phase7-plan.md` | M1 阶段七实现步骤（已完成） |
| `tasks/archive/m1-phase7.md` | M1 阶段七任务清单（已完成） |
| `docs/archive/m1-phase8-plan.md` | M1 阶段八实现步骤（已完成） |
| `tasks/archive/m1-phase8.md` | M1 阶段八任务清单（已完成） |

---

## 维护规则

- **每完成一小项后**：更新"当前位置"的进度描述和"下一个任务"
- **Phase 切换时**：更新 Phase 行、进度、下一个任务、必读文档列表
- **文档归档时**：将文档从"按需"移入"归档"，更新路径
