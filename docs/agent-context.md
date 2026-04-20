# Agent 会话上下文

> 每次会话开始时阅读本文档，快速定位当前状态和所需上下文。

---

## 当前位置

- **项目**：Himalaya — 基于 Vulkan 1.4 的实时渲染器，光栅化起步
- **Milestone**：M1 — 静态场景演示（场景和光源静态、镜头自由移动，画面写实度说得过去）
- **Phase**：阶段八 — 间接光照集成（实现中）
- **进度**：Step 8.1a + 8.1d 完成

### 下一个任务

Step 8.1：Bake 数据生命周期修复（场景/HDR/缓存变化时卸载回退、加载 bake 前 ensure UV、key 含 UV hash、清理死数据）。

---

## 必读文档

CLAUDE.md 已自动加载，以下为额外必读：

| 文档 | 说明 |
|------|------|
| `docs/milestone-1/milestone-1.md` | M1 范围、预期效果、已知局限性 |
| `docs/current-phase.md` | 当前阶段实现步骤（阶段八） |
| `docs/milestone-1/m1-rt-decisions.md` | M1 RT 架构决策（阶段六~八） |
| `docs/milestone-1/m1-interfaces.md` | M1 接口与目标结构：反映当前 Phase 结束时的目标状态，Phase 开始前更新至设计目标，实现完成后应与代码一致 |
| `docs/milestone-1/m1-development-order.md` | M1 开发阶段顺序（含阶段六~十） |
| `tasks/m1-phase8.md` | 阶段八任务清单（复选框进度跟踪） |

## 按需文档

遇到需要理解"为什么这么设计"时查阅：

| 文档 | 说明 |
|------|------|
| `docs/project/technical-decisions.md` | 所有技术模块的最终选型结果与演进路线 |
| `docs/project/decision-process.md` | 每项选型的推理过程、候选方案、排除理由 |
| `docs/project/requirements-and-philosophy.md` | 项目定位、技术选型原则、画面质量目标 |
| `docs/project/architecture.md` | 渲染器长远架构、四层结构、架构约束 |
| `docs/milestone-1/m1-design-decisions-core.md` | M1 核心设计决策参考（决策结果摘要，日后开发仍需参考） |
| `docs/milestone-1/m1-rt-decisions.md` | M1 RT 架构决策（阶段六~八：PT 参考视图 + 烘焙器 + 间接光照集成） |
| `docs/milestone-1/m1-phase-future-decisions.md` | M1 未来阶段设计构想（阶段九~十非正式规划） |
| `docs/milestone-1/m1-frame-flow.md` | M1 完整帧流程（pass 执行顺序、资源生命周期） |
| `docs/milestone-1/m1-development-order.md` | M1 的 10 个开发阶段及依赖关系 |
| `docs/roadmap/milestone-2.md` | M2 规划（画质全面提升） |
| `docs/roadmap/milestone-3.md` | M3 规划（动态物体 + 性能优化） |
| `docs/roadmap/milestone-future.md` | 远期可选目标 |

## 归档文档

| 文档 | 说明 |
|------|------|
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

---

## 维护规则

- **每完成一小项后**：更新"当前位置"的进度描述和"下一个任务"
- **Phase 切换时**：更新 Phase 行、进度、下一个任务、必读文档列表中的 phase 文档路径
- **Milestone 切换时**：全面更新本文档
