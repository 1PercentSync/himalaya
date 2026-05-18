# Reflector 开发路线

> reflector 分支的整体 phase 规划和顺序。

---

## Phase 概览

| Phase | 主题 | 状态 |
|-------|------|------|
| 1 | 管线精简 | 完成 |
| 2 | Gaussian Splatting | 待规划 |

---

## Phase 1：管线精简

移除光栅化和烘焙管线，只保留 Path Tracing 渲染 GLTF 的能力。为 Phase 2 提供干净的代码基础。

详见 `current-phase.md` 和 `tasks/reflector-phase1.md`。

## Phase 2：Gaussian Splatting

实现 GLTF Gaussian Splatting 扩展的读取和渲染。

具体技术方案待 Phase 1 完成后研究确定。
