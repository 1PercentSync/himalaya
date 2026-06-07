# Agent 会话上下文

> 每次会话开始时阅读本文档，快速定位当前状态和所需上下文。

---

## 当前位置

- **项目**：Himalaya — 基于 Vulkan 1.4 的渲染器
- **分支**：`reflector` — Path Tracing + Gaussian Splatting
- **Phase**：Phase 3.0 — Gaussian Splatting 基础渲染
- **进度**：Phase 3.0 Step 8 完成：已完整排查 GS happy path 从 PLY 转换、glTF/GS 加载、upload bake、GlobalUBO 相机矩阵、cull/project shader 到 quad draw 的坐标链路；GS projection 已与 PT/reference view 的 Y-up camera NDC 和 y-down framebuffer pixel space 对齐，NDC→pixel 与 2D covariance Jacobian 中执行一次 Y flip。用户已验证两个 GS 场景，暂未发现可见问题；小场景贴近视角约 60-70 FPS，大场景约 13.5 FPS。Step 8 剩余 multi-primitive、colorSpace、核心渲染细项、生命周期/异常路径因当前无专用测试资产而标记跳过，后续有资产时再补测。离群 splat 导致大场景自动 framing 与 scene-derived near 失真后，GS near 已改为独立参数（默认 0.25 world units，由 Rendering 面板 GS Near 滑块控制，不改变 PT/camera near 默认值），并新增 R 键将相机移动到世界原点。

### 下一个任务

Phase 3.0 Step 9 第一小项：继续确认 radix sort pass layout。已选 key/equal-key 策略：32-bit distance_key stable radix + global_splat_index payload；radix 前用 per-splat visibility flag / prefix compact 按 global splat index 顺序生成 deterministic visible list。workgroup-local compact + group prefix 作为后续运行时优化候选。

---

## 必读文档

CLAUDE.md 已自动加载，以下为额外必读：

| 文档 | 说明 |
|------|------|
| `docs/roadmap.md` | Reflector 分支 phase 概览与顺序 |
| `docs/current-phase.md` | Phase 3.0 目标、范围、按任务顺序组织的实现指南与阶段特有决策 |
| `tasks/reflector-phase3.md` | Phase 3.0 可执行任务清单 |
| `docs/phase3-decisions.md` | Phase 3.x 渲染管线总体路线、跨阶段约定与子阶段规划 |
| `docs/khr-gaussian-splatting-spec.md` | KHR_gaussian_splatting glTF 扩展规范（Khronos Release Candidate） |
| `docs/architecture.md` | 渲染器架构与设计理念（含 GS 数据管线） |
| `docs/technical-decisions.md` | 技术选型与决策（GS 部分仅保留管线级重大决策） |

---

## 归档文档

| 文档 | 说明 |
|------|------|
| `docs/archive/reflector-phase2-plan.md` | Reflector Phase 2 实现步骤（原 current-phase.md） |
| `tasks/archive/reflector-phase2.md` | Reflector Phase 2 任务清单（原 tasks/reflector-phase2.md） |
| `docs/archive/reflector-phase1-plan.md` | Reflector Phase 1 精简范围与步骤（原 current-phase.md） |
| `tasks/archive/reflector-phase1.md` | Reflector Phase 1 任务清单（原 tasks/reflector-phase1.md） |
| `docs/archive/m1-phase8.5-plan.md` | M1 阶段八点五实现步骤（原 current-phase.md） |
| `tasks/archive/m1-phase8.5.md` | M1 阶段八点五任务清单 |
| `docs/archive/m1-milestone-1.md` | M1 范围、预期效果、已知局限性 |
| `docs/archive/m1-interfaces.md` | M1 接口与目标结构 |
| `docs/archive/m1-frame-flow.md` | M1 完整帧流程 |
| `docs/archive/m1-development-order.md` | M1 开发阶段顺序 |
| `docs/archive/m1-phase-future-decisions.md` | M1 未来阶段设计构想 |
| `docs/archive/m1-design-decisions-core.md` | M1 核心设计决策参考 |
| `docs/archive/m1-rt-decisions.md` | M1 RT 架构决策（阶段六~八点五） |
| `docs/archive/project-architecture.md` | 渲染器长远架构（原 project/architecture.md） |
| `docs/archive/project-requirements-and-philosophy.md` | 项目定位与设计理念（原 project/requirements-and-philosophy.md） |
| `docs/archive/project-technical-decisions.md` | 全技术栈选型结果（原 project/technical-decisions.md） |
| `docs/archive/project-decision-process.md` | 全技术栈选型推理过程（原 project/decision-process.md） |
| `docs/archive/milestone-2.md` | M2 规划 |
| `docs/archive/milestone-3.md` | M3 规划 |
| `docs/archive/milestone-future.md` | 远期可选目标 |
| `docs/archive/gaussian-splatting-reference.md` | ChatGPT 的 GS 实时渲染研究参考（Phase 3 讨论输入） |
| `docs/archive/conversation-initial-design.md` | 初始设计的完整对话记录 |
| `docs/archive/m1-phase1-plan.md` | M1 阶段一实现步骤 |
| `docs/archive/m1-phase2-plan.md` | M1 阶段二实现步骤 |
| `docs/archive/m1-phase2-tasks.md` | M1 阶段二任务清单 |
| `docs/archive/m1-phase3-plan.md` | M1 阶段三实现步骤 |
| `tasks/archive/m1-phase3.md` | M1 阶段三任务清单 |
| `docs/archive/pcss-reference.md` | PCSS 方向光实现参考资料 |
| `docs/archive/gtao-reference.md` | GTAO 算法实现参考资料 |
| `docs/archive/m1-phase4-plan.md` | M1 阶段四实现步骤 |
| `tasks/archive/m1-phase4.md` | M1 阶段四任务清单 |
| `docs/archive/m1-phase1-decisions.md` | M1 阶段一设计决策 |
| `docs/archive/m1-phase2-decisions.md` | M1 阶段二设计决策 |
| `docs/archive/m1-phase3-decisions.md` | M1 阶段三设计决策 |
| `docs/archive/m1-phase4-decisions.md` | M1 阶段四设计决策 |
| `docs/archive/contact-shadows-reference.md` | Contact Shadows 实现参考资料 |
| `docs/archive/m1-phase5-plan.md` | M1 阶段五实现步骤 |
| `tasks/archive/m1-phase5.md` | M1 阶段五任务清单 |
| `docs/archive/m1-phase5-decisions.md` | M1 阶段五设计决策 |
| `docs/archive/m1-phase6-plan.md` | M1 阶段六实现步骤 |
| `tasks/archive/m1-phase6.md` | M1 阶段六任务清单 |
| `docs/archive/path-tracing-reference.md` | 实时路径追踪技术综述参考资料 |
| `docs/archive/m1-phase7-plan.md` | M1 阶段七实现步骤 |
| `tasks/archive/m1-phase7.md` | M1 阶段七任务清单 |
| `docs/archive/m1-phase8-plan.md` | M1 阶段八实现步骤 |
| `tasks/archive/m1-phase8.md` | M1 阶段八任务清单 |

---

## 维护规则

- **每完成一小项后**：更新"当前位置"的进度描述和"下一个任务"
- **Phase 切换时**：更新 Phase 行、进度、下一个任务、必读文档列表
- **文档归档时**：移入"归档"，更新路径
