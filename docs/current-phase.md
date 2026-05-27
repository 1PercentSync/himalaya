# 当前阶段：Reflector Phase 3.0 — Gaussian Splatting 基础渲染

> 目标：正确渲染 GS 场景的最小可行实现。
> 整体路线与子阶段规划见 `docs/phase3-decisions.md`。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 背景

Phase 2 完成了 GS 数据管线（PLY → glTF 转换 + GS glTF 加载），CPU 端 SoA 数据结构已就绪。Phase 3.0 构建从 CPU 数据到屏幕的完整渲染路径——数据 bake + 上传、计算管线（剔除 + 投影 + 排序）、硬件光栅输出。

## 范围

### 新增

- **GPU 数据上传**：CPU bake（transform 烘焙、3D 协方差预计算、SH Wigner-D rotation）+ per-attribute GPU buffer 创建
- **Cull/Project Compute Pass**：视锥剔除、3D→2D 投影、OBB extent、SH 求值、可见列表构建、排序键生成
- **Sort Compute Pass**：Bitonic sort（正确性验证）→ 自实现 Radix sort（最终方案），64-bit key 前到后深度排序
- **Quad Rendering**：Instanced quad indirect draw + premultiplied-under 硬件 blend
- **管线集成**：Descriptor Set 3、push constant、RenderMode 分发、TonemappingPass 复用

### 不在范围内

- 运行时优化（SH 分离 pass、多级剔除、buffer 分层等，Phase 3.1）
- 加载时优化（空间分块、FP16 量化、Morton 排序等，Phase 3.2）
- Tile-based compute 渲染（Phase 3.5）

## 管线流程

Upload（CPU bake → GPU buffers）:

    position    → apply transform → world space
    covariance  → quat+scale → Cov_local → M·Cov_local·Mᵀ → Σ_world (6 floats)
    SH          → Wigner-D rotation → rotated coefficients
    opacity     → 直传

Cull/Project Compute（workgroup 256）:

    → frustum cull (sphere vs planes)
    → 3D Σ → 2D cov projection
    → OBB extent (3σ cutoff)
    → SH(view_dir) → RGB
    → defensive measures
    → subgroup ballot → visible list append
    → 64-bit sort key (depth|index)

Bitonic Sort Compute → sorted visible list

Indirect Draw（instanced quads）:

    → 6 verts/quad (gl_VertexIndex % 6)
    → premultiplied-under blend (front-to-back)
    → R16G16B16A16Sfloat output

sRGB → Linear → TonemappingPass（skip curve）

## 实现步骤

共 6 个 Step，详见 `tasks/reflector-phase3.md`。

| Step | 内容 | 说明 |
|------|------|------|
| 0 | GPU 数据上传 | CPU bake + GPU buffer 创建 + Descriptor Set 3 |
| 1 | Cull/Project Compute | 视锥剔除 + 2D 投影 + SH 求值 + 可见列表 + 排序键 |
| 2 | Bitonic Sort | 64-bit key 深度排序（正确性验证用） |
| 3 | Quad Rendering | Instanced quad + 硬件 blend + render target |
| 4 | 管线集成 | RenderMode 分发 + Tonemapping + 端到端渲染 |
| 5 | Radix Sort | 自实现 radix sort 替换 bitonic sort |
