# Agent 会话上下文

> 每次会话开始时阅读本文档，快速定位当前状态和所需上下文。

---

## 当前位置

- **项目**：Himalaya — 基于 Vulkan 1.4 的实时渲染器，光栅化起步
- **Milestone**：M1 — 静态场景演示（场景和光源静态、镜头自由移动，画面写实度说得过去）
- **Phase**：阶段三 — PBR 光照基础（Cook-Torrance + IBL + MSAA + HDR）
- **进度**：Step 6.5 进行中，第 1 项完成（forward.frag 共享 normal.glsl + metallic 工作流分离），下一步第 2 项

### 下一个任务

Step 6.5 第 2 项：`forward.frag` IBL 漫反射 + 镜面反射（irradiance + prefiltered + BRDF LUT Split-Sum，含 `rotate_y`），用 `ibl_intensity` 调制，移除固定 ambient 项。

### 代码审查修复记录

Phase 3 全面代码审查（Step 1 ~ Step 6）发现并修复了两处 bug：

1. **`generate_mips()` cubemap 支持**（影响 Step 6 IBL）：barrier/blit 硬编码 `layerCount=1`，cubemap 面 1-5 的 mip 链未生成。prefilter shader 对非 +X 面高 roughness 采样到 UNDEFINED 数据。已修复为使用 `img.desc.array_layers`。
2. **`forward.frag` 双重曝光**（影响 Step 4b Exposure）：exposure 在 forward.frag 和 tonemapping.frag 各乘一次，导致 exposure²。EV=0 时不可见（1²=1），EV≠0 时画面过亮/过暗。已移除 forward.frag 中的 exposure。

---

## 必读文档

CLAUDE.md 已自动加载，以下为额外必读：

| 文档 | 说明 |
|------|------|
| `docs/milestone-1/milestone-1.md` | M1 范围、预期效果、已知局限性 |
| `docs/current-phase.md` | 当前阶段实现步骤（阶段三） |
| `docs/milestone-1/m1-interfaces.md` | M1 接口与目标结构：文件结构、关键接口与数据结构 |
| `tasks/m1-phase3.md` | 阶段三任务清单（复选框进度跟踪） |

## 按需文档

遇到需要理解"为什么这么设计"时查阅：

| 文档 | 说明 |
|------|------|
| `docs/project/technical-decisions.md` | 所有技术模块的最终选型结果与演进路线 |
| `docs/project/decision-process.md` | 每项选型的推理过程、候选方案、排除理由 |
| `docs/project/requirements-and-philosophy.md` | 项目定位、技术选型原则、画面质量目标 |
| `docs/project/architecture.md` | 渲染器长远架构、四层结构、架构约束 |
| `docs/milestone-1/m1-design-decisions.md` | M1 各组件的设计选择与决策理由 |
| `docs/milestone-1/m1-frame-flow.md` | M1 完整帧流程（pass 执行顺序、资源生命周期） |
| `docs/milestone-1/m1-development-order.md` | M1 的 8 个开发阶段及依赖关系 |
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

---

## 维护规则

- **每完成一小项后**：更新"当前位置"的进度描述和"下一个任务"
- **Phase 切换时**：更新 Phase 行、进度、下一个任务、必读文档列表中的 phase 文档路径
- **Milestone 切换时**：全面更新本文档
