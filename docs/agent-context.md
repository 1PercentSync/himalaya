# Agent 会话上下文

> 每次会话开始时阅读本文档，快速定位当前状态和所需上下文。

---

## 当前位置

- **项目**：Himalaya — 基于 Vulkan 1.4 的实时渲染器，光栅化起步
- **Milestone**：M1 — 静态场景演示（场景和光源静态、镜头自由移动，画面写实度说得过去）
- **Phase**：阶段七 — PT 烘焙器
- **进度**：Step 12 Scope 1 + OIDN 完成，Scope 2 待实现

### 下一个任务

Step 12 第十一小项：`probe_bake_finalize()` Scope 2——upload → prefilter → BC6H compress → readback。

设计详见 `docs/current-phase.md` Step 12 章节，任务清单见 `tasks/m1-phase7.md` Step 12 部分。无设计决策空间，按文档直接实现。

#### 已确认的实现要点

- `resources.cpp` 中 `array_layers == 6` 自动加 `CUBE_COMPATIBLE_BIT`，默认 view 为 `VK_IMAGE_VIEW_TYPE_CUBE`。probe cubemap 创建只需设 `array_layers = 6`，无需额外 flags
- `render()` 在调 `render_baking()` 前已调 `fill_common_gpu_data(input)`，baker 所需的 Set 0/1 数据已就绪
- `prefilter_cubemap()` 采样 `src_img.view`（CUBE view），与 cubemap 默认 view 兼容
- `compress_bc6h()` 从 `desc.array_layers` 读 face_count，逐 face × 逐 mip dispatch，原生支持 cubemap
- `probe_bake_finalize()` 的 prefilter + compress 在同一个 `begin_immediate()/end_immediate()` scope 内录制
- BC6H cubemap readback 需 per-mip 发 `vkCmdCopyImageToBuffer`（每次 `layerCount=6`），逐 mip 构造 `Ktx2WriteLevel`
- `generate_probe_grid()` 自管 immediate scope，不可在外部 scope 内调用；设计文档用 `placement_pending` 标志延迟到 `render_baking()` 首帧在 scope 外处理

---

## 必读文档

CLAUDE.md 已自动加载，以下为额外必读：

| 文档 | 说明 |
|------|------|
| `docs/milestone-1/milestone-1.md` | M1 范围、预期效果、已知局限性 |
| `docs/current-phase.md` | 当前阶段实现步骤（阶段七） |
| `docs/milestone-1/m1-rt-decisions.md` | M1 RT 架构决策（阶段六~八） |
| `docs/milestone-1/m1-interfaces.md` | M1 接口与目标结构：文件结构、关键接口与数据结构 |
| `docs/milestone-1/m1-development-order.md` | M1 开发阶段顺序（含阶段六~十） |
| `tasks/m1-phase7.md` | 阶段七任务清单（复选框进度跟踪） |

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

---

## 维护规则

- **每完成一小项后**：更新"当前位置"的进度描述和"下一个任务"
- **Phase 切换时**：更新 Phase 行、进度、下一个任务、必读文档列表中的 phase 文档路径
- **Milestone 切换时**：全面更新本文档
