# Reflector Phase 3.0：Gaussian Splatting 基础渲染

> 目标：正确渲染 GS 场景的最小可行实现。
> 整体路线见 `docs/phase3-decisions.md`，实现计划见 `docs/current-phase.md`。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | Transform 处理 | Baked：upload 时烘焙到 position/covariance/SH，低频变更时重上传 |
| 2 | GPU buffer 布局 | Per-attribute 独立 storage buffer（SoA） |
| 3 | SH 上传范围 | 全部 degree（完整画质） |
| 4 | CPU 内存布局 | 保持现有 SoA 结构，upload 函数中转置高阶 SH |
| 5 | 排序键值 | View-space Z |
| 6 | 排序算法 | Bitonic sort 先行（正确性验证），后自实现 Radix sort |
| 7 | 排序键格式 | 64-bit packed（high 32-bit float depth + low 32-bit uint index） |
| 8 | Cull 与 Projection | 合并在一个 compute shader |
| 9 | Workgroup size | 256 |
| 10 | Draw 方式 | Non-indexed instanced draw（gl_VertexIndex % 6，两三角形） |
| 11 | 输出颜色格式 | R16G16B16A16Sfloat |
| 12 | Tonemapping | 复用 TonemappingPass，GS 模式跳过 tonemap 曲线；管线内 sRGB→linear 后输出线性 |
| 13 | Descriptor set | Set 3（GS 专用，与 PT 的 Set 2 互斥使用） |
| 14 | Per-frame 参数 | Push constant（GS 专用 uniform） |
| 15 | 模式分发 | RenderInput 中 RenderMode，Renderer::render() 分支 |
| 16 | 资源创建策略 | PT + GS 两套资源均在初始化时创建 |
| 17 | 3D Covariance 计算 | Upload 时预计算 full world-space Σ（6 floats，symmetric 3×3） |
| 18 | SH 求值位置 | Cull/Project compute shader 中 per-splat 求值 |
| 19 | SH rotation | Upload bake 时 CPU 执行完整 Wigner-D 矩阵旋转（degree 1-3） |
| 20 | OBB 截断半径 | 3σ（KHR 规范要求） |

---

## Step 0：GPU 数据上传

- [ ] 定义 GS GPU 数据结构（buffer 句柄、splat 总数、per-primitive range 映射）
- [ ] 实现 position baking（apply transform → world space）
- [ ] 实现 3D 协方差预计算（quaternion + scale → Cov_local symmetric 3×3 → apply transform → Σ_world 6 floats）
- [ ] 实现 Wigner-D 矩阵 SH rotation（从 transform 提取旋转分量，旋转 degree 1-3 SH 系数）
- [ ] 实现 GPU buffer 创建与数据上传（per-attribute storage buffers）
- [ ] 定义 Descriptor Set 3 layout 和 GS push constant 结构体
- [ ] 创建 Descriptor Set 3 并写入 buffer 绑定
- [ ] 编译验证

## Step 1：Cull/Project Compute Pass

- [ ] 创建 intermediate buffers（visible count atomic、projected data buffers、sort key buffer）
- [ ] 创建 cull/project compute shader（buffer 声明、workgroup 256、main 框架）
- [ ] 实现视锥剔除（world-space 球体 vs frustum 平面，包围半径膨胀）+ 防御性措施（巨型投影 clamp、近平面 z clamp、协方差正定化 ε、长宽比 clamp）
- [ ] 实现 3D→2D 协方差投影 + OBB 计算（2D 协方差椭圆 eigendecomposition → oriented extent，3σ 截断）
- [ ] 实现 per-splat SH 求值（splat center → camera view direction → evaluate all degrees → RGB）
- [ ] 实现 subgroup ballot 可见列表 append + 64-bit 排序键生成（view-space Z | splat index）
- [ ] 创建 compute pipeline + dispatch 逻辑（C++ 端）
- [ ] 编译验证

## Step 2：Bitonic Sort

- [ ] 创建 bitonic sort compute shader（64-bit key compare-and-swap）
- [ ] 实现多 pass dispatch 逻辑（log²(N) stages × log(N) steps）
- [ ] 编译验证

## Step 3：Quad Rendering

- [ ] 创建 vertex shader（从 sorted visible list 读取投影数据，展开 6-vertex instanced quad）
- [ ] 创建 fragment shader（高斯 alpha 衰减 + 颜色输出）
- [ ] 实现 indirect draw command buffer 设置（visible count → VkDrawIndirectCommand::instanceCount）
- [ ] 创建 R16G16B16A16Sfloat GS 渲染目标
- [ ] 配置 graphics pipeline（blend state: premultiplied-under front-to-back，depth test off）
- [ ] 编译验证

## Step 4：管线集成

- [ ] 创建 render_gaussian_splatting() 方法（orchestrate cull → sort → draw）
- [ ] 实现 RenderMode 分发（render() 根据 RenderMode 调用对应渲染路径）
- [ ] 集成 TonemappingPass（GS 模式：sRGB→linear 转换 + 跳过 tonemap 曲线）
- [ ] 实现 GS 专用 near plane 计算（scene AABB diagonal 的 0.5%-1%）
- [ ] 端到端渲染验证（bitonic sort 正确性基线）

## Step 5：Radix Sort

- [ ] 创建 radix sort compute shader（per-digit histogram + prefix sum + scatter，64-bit key）
- [ ] 实现多 pass dispatch 逻辑（每 pass 处理若干 bit，从 LSB 到 MSB）
- [ ] 替换 bitonic sort dispatch 为 radix sort dispatch
- [ ] 正确性验证（与 bitonic sort 基线对比渲染结果）
- [ ] 编译验证
