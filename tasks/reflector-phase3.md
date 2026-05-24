# Reflector Phase 3：Gaussian Splatting 渲染

> 目标：实现 GS 实时渲染管线（Compute Tile-Based Rendering）。
> 技术决策见 `docs/technical-decisions.md` 第 22-23 节。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | 渲染管线架构 | Compute Tile-Based Rendering（纯 compute 软光栅） |
| 2 | GPU 排序算法 | 自行实现 GPU Radix Sort |
| 3 | 排序 key 位宽 | 32-bit key（depth）+ 32-bit value（index）分离，4 pass |
| 4 | SH 求值时机 | 投影阶段 per-splat 一次 |
| 5 | GPU 数据布局 | 混合方案：核心属性打包 struct + SH 独立 buffer，CPU 端同步改结构 |
| 6 | Tile 大小 | 16×16（256 threads/workgroup） |
| 7 | GS 输出目标 | 写入与 PT 共享的 color buffer（R16G16B16A16F） |
| 8 | 输出 Pass | TonemappingPass → PresentPass，push constant mode 分支 + SRGB/UNORM view 切换 |
| 9 | Descriptor 绑定 | Push Descriptor（Set 3），与现有 pass 一致 |
| 10 | Compute Pass 拆分 | 四阶段：Projection+Culling → Radix Sort → Tile Binning → Tile Rendering |
| 11 | 颜色空间处理 | GS 侧原样输出 SH 值，gamma 由 PresentPass view 选择控制 |
| 12 | 抗锯齿 | 直接实现 Mip Splatting |
| 13 | 多 Primitive 处理 | 核心属性合并 + SH 按 degree 分组 dispatch |
| 14 | 背景色 | 纯黑（vec4(0)） |

---

## Step 0：数据结构重构

- [ ] 定义 `GaussianSplatCore` 打包 struct（position + rotation + scale + opacity）
- [ ] 将 `GaussianSplatPrimitive` 中 4 个独立 vector 替换为 `vector<GaussianSplatCore>`
- [ ] 修改 `GaussianSplatLoader` 适配新数据结构
- [ ] 修改 `ply_converter` 输出适配（如需要）
- [ ] 更新 AABB 计算逻辑（从 `cores[i].position` 读取）
- [ ] 编译验证

## Step 1：PresentPass 重构

- [ ] 重命名 `TonemappingPass` → `PresentPass`（头文件、源文件、类名、所有引用）
- [ ] 重命名 shader 文件 `tonemapping.frag` → `present.frag`
- [ ] 添加 push constant struct（`uint mode`）
- [ ] 更新 shader：根据 mode 分支（PT 走 exposure + ACES，GS 走 passthrough）
- [ ] 为 swapchain image 创建额外的 UNORM VkImageView
- [ ] `record()` 根据 RenderMode 和 color_space 选择 SRGB/UNORM view
- [ ] 更新 Renderer 中的 PresentPass 调用
- [ ] 编译验证

## Step 2：GPU Buffer 上传

- [ ] 创建 GS GPU buffer 管理模块（核心属性 SSBO + SH SSBO）
- [ ] 实现场景加载时的 GPU 上传（staging → device local）
- [ ] 实现多 primitive 合并上传（核心属性应用 transform 后拼接，SH 按 degree 分组）
- [ ] 实现场景卸载时的 buffer 销毁
- [ ] 编译验证

## Step 3：投影与剔除 Pass

- [ ] 创建 GS 投影 compute shader（视锥剔除 + 3D→2D covariance 投影）
- [ ] 实现 SH 求值（支持 degree 0-3，push constant 传 degree）
- [ ] 实现 Mip Splatting（3D 频率限制 + 2D mip filter）
- [ ] 创建投影输出中间 buffer（2D 属性 + RGB + depth key）
- [ ] 创建 GS projection pass 类
- [ ] 编译验证

## Step 4：GPU Radix Sort

- [ ] 实现 prefix sum（scan）compute shader
- [ ] 实现 radix sort scatter compute shader
- [ ] 实现 4-pass radix sort 编排（每 pass 8 bit）
- [ ] 创建 sort 临时 buffer 管理
- [ ] 创建 RadixSort 工具类
- [ ] 编译验证

## Step 5：Tile Binning

- [ ] 实现 per-tile splat 计数 compute shader（atomicAdd）
- [ ] 实现 prefix sum 计算 per-tile 偏移
- [ ] 实现 scatter compute shader（将 splat index 写入 per-tile 区域）
- [ ] 创建 tile binning pass 类
- [ ] 编译验证

## Step 6：Tile Rendering + 集成

- [ ] 创建 tile rendering compute shader（16×16 workgroup，前到后 alpha blend）
- [ ] 实现 2D Gaussian 求值 + transmittance early termination
- [ ] 实现 imageStore 写入 color buffer（背景黑色）
- [ ] 创建 tile rendering pass 类
- [ ] 在 Renderer 中实现 `render_gaussian_splatting()` 路径
- [ ] 将四阶段 pass 注册到 Render Graph
- [ ] 实现 RenderMode 切换（GS 路径 ↔ PT 路径）
- [ ] 添加 DebugUI GS 控制面板
- [ ] 编译验证
