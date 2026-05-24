# 当前阶段：Reflector Phase 3 — Gaussian Splatting 渲染

> 目标：实现 Gaussian Splatting 的实时渲染管线（Compute Tile-Based Rendering）。
> 技术决策见 `technical-decisions.md` 第 22-23 节。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 背景

Phase 2 完成了 GS 数据管线（PLY → glTF 转换 + GS glTF 加载），CPU 端已有完整的 `GaussianSplatScene` 数据。Phase 3 构建从 CPU 数据到屏幕像素的完整渲染路径：GPU buffer 上传、tile-entry based compute 管线（投影 → entry 生成 → 两次 stable sort → range build → 混合）、输出到 swapchain。

## 范围

### 新增

- **GaussianSplatCore**（Framework 层）：打包核心属性 struct，替代原 SoA 中的独立 vector
- **GPU Buffer 管理**（Framework 层）：GS 数据的 GPU buffer 创建、上传、销毁
- **GPU Radix Sort**（Framework 层）：通用 compute stable radix sort（32-bit key + 32-bit value）
- **GS Projection Pass**（Passes 层）：投影 + 视锥剔除 + SH 求值 + Mip Splatting
- **GS Tile Entry / Range Pass**（Passes 层）：per-tile entry 生成、两次 stable sort、tile range build
- **GS Tile Rendering Pass**（Passes 层）：前到后 alpha blend + early termination
- **GS Compute Shaders**（Shaders）：投影、排序、分 tile、渲染的 compute shader

### 重构

- **GaussianSplatPrimitive**：核心属性从 4 个独立 vector 改为 `vector<GaussianSplatCore>`
- **GaussianSplatLoader**：适配新数据结构
- **TonemappingPass → PresentPass**：改名 + 加 mode/color-space 分支，swapchain 始终 SRGB view，GS sRGB 输入 shader decode
- **Renderer**：添加 `render_gaussian_splatting()` 路径

### 不在范围内

- GS 离屏渲染 / 截图输出（Phase 4）
- PT 自适应采样（Phase 4）
- MCP 集成（Phase 5）

## 渲染管线概览

```
GaussianSplatScene (CPU)
    │
    ▼ upload (场景加载时一次)
GPU Buffers: Core/Covariance + SH[degree]
    │
    ▼ 每帧
┌─────────────────────────────┐
│ 1. Projection + Culling     │  per-splat compute
│    视锥剔除 + 2D 投影       │
│    SH 求值 + Mip Splatting  │
│    → 2D 属性 + RGB + depth  │
├─────────────────────────────┤
│ 2. Tile Entry Generation    │  per visible splat × covered tile
│    → depth key + tile id    │
├─────────────────────────────┤
│ 3. Stable Radix Sort × 2    │  depth sort → stable tile-id sort
│    → tile 内前到后顺序      │
├─────────────────────────────┤
│ 4. Tile Range Build         │  sorted entries → offsets/counts
├─────────────────────────────┤
│ 5. Tile Rendering           │  16×16 workgroup
│    前到后 alpha blend       │
│    → imageStore GS color buf│
└─────────────────────────────┘
    │
    ▼
PresentPass → swapchain
(根据 RenderMode 读 PT accumulation 或 GS color buffer)
```

## 实现步骤

基础 Step 0-5 已完成；审查后新增 Step 5.5-5.9 修正设计与集成边界，详见 `tasks/reflector-phase3.md`。

| Step | 内容 | 说明 |
|------|------|------|
| 0 | 数据结构重构 | GaussianSplatCore 打包 struct，修改 loader 适配 |
| 1 | PresentPass 重构 | TonemappingPass 改名，加 mode 分支；Step 5.5 回退 UNORM view 方案，改为 shader 内颜色空间处理 |
| 2 | GPU Buffer 上传 | GsGpuData 类与 SSBO 上传；Renderer 接入拆到 Step 5.7 |
| 3 | 投影与剔除 | Projection compute pass + SH 求值 + Mip Splatting；group barrier 与 stats 在 Step 5.5/5.6 修正 |
| 4 | GPU Radix Sort | 32-bit stable key+value radix sort；Step 5.5 复用两次实现 tile entry `(tile_id, depth)` 排序 |
| 5 | Tile Binning | 已实现的 count/scan/scatter 方案将由 Step 5.5 重构为 tile entry generation + range build |
| 5.5 | Correctness Fixes + Tile Entry Pipeline | PresentPass 色彩修订、projection barrier、entry capacity/stats、两次 stable sort、tile range build |
| 5.6 | GS covariance / transform 修正 | GPU core 改为 world covariance，修复 node transform 只作用 position 的问题 |
| 5.7 | GsGpuData 接入 Renderer | Renderer 持有并上传 GS GPU 数据，Application 加载 GS 后触发上传 |
| 5.8 | RHI compute utility refactor | 抽取重复 compute Vulkan helper，允许修改 `rhi/CMakeLists.txt` |
| 5.9 | RenderMode flow cleanup | 保留 `Path Tracing` checkbox，内部通过 RenderMode 流转；GS 完成前 checkbox disabled |
| 6 | Tile Rendering + 集成 | 前到后混合 + Renderer GS 路径 + DebugUI stats |
| 6.5 | GS performance review | 检查两次 RadixSort、stable scatter local-rank 与 entry capacity 是否需要优化 |
