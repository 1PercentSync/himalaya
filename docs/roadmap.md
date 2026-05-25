# Reflector 开发路线

> reflector 分支的整体 phase 规划和顺序。

---

## Phase 概览

| Phase | 主题 | 状态 |
|-------|------|------|
| 1 | 管线精简 | 完成 |
| 2 | Gaussian Splatting 数据管线 | 完成 |
| 3 | Gaussian Splatting 渲染 | 已规划 |
| 4 | 渲染增强与离屏输出 | 待规划 |
| 5 | MCP 协议集成 | 待规划 |

---

## Phase 1：管线精简

移除光栅化和烘焙管线，只保留 Path Tracing 渲染 GLTF 的能力。为 Phase 2 提供干净的代码基础。

详见 `current-phase.md` 和 `tasks/reflector-phase1.md`。

## Phase 2：Gaussian Splatting 数据管线

PLY → glTF 转换工具 + GS 场景加载。

- **PLY → glTF 转换**：将 3D Gaussian Splatting PLY 文件转换为 glTF + `KHR_gaussian_splatting` 扩展格式
- **glTF 加载**：扩展现有 glTF 加载器，支持 `KHR_gaussian_splatting` 扩展
- **PLY 直接加载**：通过内部调用转换流程实现，无需单独的 PLY 加载路径

## Phase 3：Gaussian Splatting 渲染

Compute Tile-Based Rendering 实时渲染管线。

- **Compute Tile-Based 管线**：Projection+Culling → per-splat coverage count / prefix sum → duplicate entries(tile_id, depth) → stable sort(depth) → stable sort(tile id) → tile range build → Tile Rendering（16×16 tile，前到后 alpha blend + early termination）；当前 per-tile atomic scatter 仅作为诊断 baseline，后续再评估 64-bit / 更快 radix sort
- **PresentPass 重构**：原 TonemappingPass 泛化，支持 PT tonemapping 和 GS display-referred 输出；swapchain 始终 SRGB view，GS sRGB 输入在 shader 内 decode
- **Mip Splatting**：投影阶段 3D 频率限制 + 2D mip filter 抗锯齿

详见 `current-phase.md` 和 `tasks/reflector-phase3.md`。

## Phase 4：渲染增强与离屏输出

一组独立改进，可并行或按需排列：

- **PT 自适应采样**：根据帧率动态调整每帧 SPP（samples per pixel），平衡交互响应与收敛速度
- **PT 离屏渲染**：CLI 模式，传入参数（场景、相机、SPP 等）输出图片文件
- **GS 离屏渲染**：CLI 模式，同上

## Phase 5：MCP 协议集成

将渲染器作为 MCP server 暴露能力。具体方案待前序 Phase 完成后确定。
