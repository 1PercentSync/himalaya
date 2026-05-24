# 当前阶段：Reflector Phase 3 — Gaussian Splatting 渲染

> 目标：实现 Gaussian Splatting 的实时渲染管线（Compute Tile-Based Rendering）。
> 技术决策见 `technical-decisions.md` 第 22-23 节。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 背景

Phase 2 完成了 GS 数据管线（PLY → glTF 转换 + GS glTF 加载），CPU 端已有完整的 `GaussianSplatScene` 数据。Phase 3 构建从 CPU 数据到屏幕像素的完整渲染路径：GPU buffer 上传、四阶段 compute 管线（投影 → 排序 → 分 tile → 混合）、输出到 swapchain。

## 范围

### 新增

- **GaussianSplatCore**（Framework 层）：打包核心属性 struct，替代原 SoA 中的独立 vector
- **GPU Buffer 管理**（Framework 层）：GS 数据的 GPU buffer 创建、上传、销毁
- **GPU Radix Sort**（Framework 层）：通用 compute radix sort（32-bit key + 32-bit value）
- **GS Projection Pass**（Passes 层）：投影 + 视锥剔除 + SH 求值 + Mip Splatting
- **GS Tile Binning Pass**（Passes 层）：per-tile splat 计数 + prefix sum + scatter
- **GS Tile Rendering Pass**（Passes 层）：前到后 alpha blend + early termination
- **GS Compute Shaders**（Shaders）：投影、排序、分 tile、渲染的 compute shader

### 重构

- **GaussianSplatPrimitive**：核心属性从 4 个独立 vector 改为 `vector<GaussianSplatCore>`
- **GaussianSplatLoader**：适配新数据结构
- **TonemappingPass → PresentPass**：改名 + 加 mode 分支 + UNORM view 支持
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
GPU Buffers: CoreAttributes + SH[degree]
    │
    ▼ 每帧
┌─────────────────────────────┐
│ 1. Projection + Culling     │  per-splat compute
│    视锥剔除 + 2D 投影       │
│    SH 求值 + Mip Splatting  │
│    → 2D 属性 + RGB + depth  │
├─────────────────────────────┤
│ 2. Radix Sort               │  4 pass × (scan + scatter)
│    32-bit depth key 排序    │
├─────────────────────────────┤
│ 3. Tile Binning             │  count + prefix sum + scatter
│    per-tile (offset, count) │
├─────────────────────────────┤
│ 4. Tile Rendering           │  16×16 workgroup
│    前到后 alpha blend       │
│    → imageStore GS color buf│
└─────────────────────────────┘
    │
    ▼
PresentPass → swapchain
(根据 RenderMode 读 PT accumulation 或 GS color buffer)
```

## 实现步骤

共 7 个 Step，详见 `tasks/reflector-phase3.md`。

| Step | 内容 | 说明 |
|------|------|------|
| 0 | 数据结构重构 | GaussianSplatCore 打包 struct，修改 loader 适配 |
| 1 | PresentPass 重构 | TonemappingPass 改名，加 mode 分支 + UNORM view |
| 2 | GPU Buffer 上传 | GsGpuData 类（Renderer 持有），core SSBO（position 应用 transform 后合并上传）+ SH SSBO（按 max_sh_degree 分组，累计系数） |
| 3 | 投影与剔除 | Projection compute pass + SH 求值 + Mip Splatting |
| 4 | GPU Radix Sort | 先补 Step 3 排序输入写入遗漏，再实现 32-bit key+value radix sort compute |
| 5 | Tile Binning | per-tile 计数 + 独立 tile scan + scatter（使用 `tile_cursors[]` 写游标与保守容量 `tile_splat_ids[]`） |
| 6 | Tile Rendering + 集成 | 前到后混合 + Renderer 集成 + DebugUI |
