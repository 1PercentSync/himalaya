# Phase 3 决策：Gaussian Splatting 渲染

> 渲染管线总体路线、子阶段规划与关键技术选型。
> 每个子阶段仅记录目的、采纳技术和结束目标，实现细节在各阶段开始时细化。

---

## 总体路线

**两阶段演进**：

1. **硬件光栅方案**：Compute Cull+Sort → Indirect Draw instanced quads → 硬件 alpha blend
2. **Tile-Based Compute 方案**：Compute Cull+Sort → Tile Binning → Per-tile compute blend（early-out）

先实现硬件光栅方案验证正确性，再演进到 Tile-Based Compute 方案获取 per-tile early-out 能力。从 1 到 2 只替换渲染末端，上游 cull/sort 管线 100% 复用。

**两个阶段统一使用前到后（front-to-back）深度排序**。Phase 3.0 使用 premultiplied under 硬件混合，Phase 3.5 在 shared memory 中做前到后累积 + early-out。排序方向一致，演进时无需修改 sort 逻辑。

**Phase 编号说明**：3.0/3.1/3.2 为硬件光栅阶段的基础和优化，3.5 为演进到 tile-based compute 的转折点，3.6/3.7 为后续优化。3.3/3.4 为预留编号。

**渲染模式互斥**：GS 渲染（Phase 3）与 PT 渲染是独立的 `RenderMode`，每帧只走一条路径，不存在 GS + mesh 同屏合成的需求。GS 专用的 near plane、投影参数等仅在 GS 模式下生效。

## 贯穿实现约定

以下不是独立阶段，而是从 Phase 3.0 第一个 shader 起就必须遵循的技术约定：

### Subgroup 操作优先

所有 compute shader 中涉及 append、归约、scan 的操作，默认使用 subgroup 模式而非朴素 per-thread atomic。典型应用：

- Visible list append：`subgroupBallot` + `subgroupElect` + `subgroupBroadcastFirst`，每 subgroup 只发一次 `atomicAdd`（原子操作减少约 32x）
- Tile binning append：同上模式
- Prefix-sum / compaction：subgroup inclusive/exclusive scan

### Subgroup Uniform Control Flow

要求 `VK_KHR_shader_subgroup_uniform_control_flow` 扩展特性。上述 subgroup append 模式依赖 `subgroupBallot` 后所有活跃 invocation 重新汇合，此扩展提供该语义的正确性保证。设备初始化时作为必需扩展请求；不支持该扩展的设备无法使用 GS 渲染模式。

### 投影 Extent 收紧（Oriented Bounding Box）

投影阶段计算 2D 协方差椭圆的 OBB（贴合主轴的旋转矩形），而非 AABB。长条 splat 的 AABB 面积可达 OBB 的 2-4 倍，多余区域内 fragment 经高斯衰减后贡献近零。Phase 3.0 中用 oriented quad 替代 axis-aligned quad；Phase 3.5 中 tile binning 使用 OBB-tile 相交测试。

### Tile Shared Memory 协作预加载（Phase 3.5 起）

Per-tile compute shader 中，workgroup 内所有线程协作将该 tile 的 splat 数据批量加载进 shared memory，而非每线程独立从全局内存随机读取。分批处理：每批加载 workgroup 大小数量的 splat 进 shared memory，处理完再加载下一批。

### 颜色累积精度（Phase 3.5 起）

Tile compute shader 中前到后 alpha blend 的颜色累积使用 FP32（shared memory 中 `vec4` 全精度），仅在最终 `imageStore` 输出时转为目标格式。避免 FP16 累积在 100+ 次 blend 后产生颜色漂移和灰雾。

## 性能目标

| 场景规模 | 目标 |
|---------|------|
| 1M gaussians | 基本实时（30+ FPS @ 1080p） |
| 10M gaussians | Phase 3.0-3.2：可正确渲染，允许低帧率（10+ FPS）；Phase 3.5-3.7 优化后目标可交互（20+ FPS） |

## 防御性措施

内置于基础实现（Phase 3.0），不单独成阶段：

| 异常场景 | 防御方式 |
|---------|---------|
| 巨型屏幕投影 | 屏幕空间半径 clamp（上限为屏幕短边的 25%） |
| 针状 splat（训练 artifact） | 加载时长宽比过滤/clamp |
| 近平面投影爆炸 | 视空间 z clamp 到 near + ε |
| 视锥边缘大 splat 误剔除 | 球体 vs frustum 平面测试（包围半径膨胀） |
| 2D 协方差矩阵退化 | 对角线加 ε 保正定 |
| 高斯贴脸 | GS 专用 near plane（场景 AABB 对角线的 0.5%-1%） |

---

## Phase 3.0：基础渲染

**目的**：正确渲染 GS 场景的最小可行实现。

**技术**：

- Compute frustum cull + 2D 投影
- GPU Radix Sort（深度排序可见列表）
- Indirect Draw instanced quads + 硬件 alpha blend
- GPU 数据上传（SoA 布局）
- 上述防御性措施

**结束目标**：能正确渲染 1M gaussian 场景，画面正确无明显 artifact，性能未必达到实时。

---

## Phase 3.1：运行时优化

**目的**：减少每帧计算量和带宽消耗。

**技术**：

- 3D 协方差预计算（加载时一次性，省去每帧 quaternion → matrix 转换）
- SH 颜色预求值（每帧一次独立 compute pass，对可见 splat 求值 SH(view_dir) → RGB，渲染阶段只读预计算结果；与距离自适应 SH 截断配合——远处 splat 只求低阶 SH）
- 多级剔除（sub-pixel 半径 / 低 opacity / 异常大投影）
- GPU buffer 热/冷/暖分离
- 距离自适应 SH 截断（远处只算低阶 SH）

**优先级**：SH 预求值 > 多级剔除 > 热冷分离 > 3D 协方差预计算 > SH 截断

**结束目标**：1M 场景达到基本实时（30+ FPS）。

---

## Phase 3.2：加载时优化

**目的**：通过预处理改善运行时全管线的数据效率。

**技术**：

- 空间分块 + 逐块 AABB（chunk 级视锥剔除，cull 只遍历可见 chunk）
- FP16 量化（rotation / scale / opacity / SH 上传为 half precision）
- Morton/Z-order 空间排序（全管线缓存命中率 + 分支一致性）
- 死 splat 剪枝（opacity / scale / position 异常值过滤）
- 世界空间包围球半径预计算

**优先级**：空间分块 > FP16 量化 > Morton 排序 > 死 splat 剪枝 > 包围球预计算

**结束目标**：相比 FP32 全属性上传，VRAM 占用减少约 45%；cull pass 从全量遍历降为可见 chunk 遍历；10M 场景可加载和渲染。

---

## Phase 3.5：Tile-Based Compute 渲染

**目的**：替换硬件光栅末端为全 compute 管线，获得 per-tile early-out。

**技术**：

- Tile binning（splat → tile 多对多映射，per-tile splat index list）
- Per-tile compute shader（shared memory 前到后 alpha blend，transmittance 归零时 early-out）
- `imageStore` 输出到 storage image

**保留**：cull、projection、sort、visible list compaction、SH pre-eval 等上游管线不变。
**替换**：instanced quad indirect draw → tile binning + per-tile compute render。

**结束目标**：密集重叠场景性能大幅提升，10M 场景中等重叠下可交互。

---

## Phase 3.6：高级优化

**目的**：压榨排序、剔除、调度环节的剩余性能空间。

**技术**（按性价比排序）：

- Budget Rendering（按 `opacity × projected_area` 重要性截断可见列表，锁定渲染上限保帧率）
- Non-Empty Tile Indirect Dispatch（只 dispatch 有覆盖的 tile）
- Temporal Sort Reuse（帧间排序复用：验证 pass 检查相邻元素有序性，无序率低于阈值时做局部修正，超过阈值时 fallback 全量 radix sort。缓慢移动时省 50-80% sort 成本）
- Previous-Frame Depth Occlusion（前帧深度 reproject 遮挡剔除）
- Pipeline Double-Buffer Overlap（Frame N+1 cull+sort 与 Frame N tile render 重叠）

**决策点**：Phase 3.6 结束时评估 10M 场景 VRAM 占用，决定是否追加 VRAM Streaming 模块。

**结束目标**：全部已识别的性能瓶颈得到缓解，1M 场景高帧率，10M 场景流畅可交互。

---

## Phase 3.7：LOD + 抗锯齿

**目的**：10M 场景空间自适应精度控制 + 远景画质改善。

**技术**：

- Chunk 级重要性子采样 LOD：加载时 per-chunk 按 `opacity × max_scale²` 排序，运行时按 chunk 到相机距离选择渲染比例（近 100% → 远 10-25%）。利用 Phase 3.2 已有的空间分块基础设施，运行时额外开销仅为 per-chunk 距离计算和 count 选择（在 chunk 级 cull pass 中顺带完成）
- Mip-Splatting 抗锯齿：在 cull+projection compute shader 中对 2D 协方差施加像素 footprint 下界约束，防止 splat 小于一个像素导致闪烁。放在 Phase 3.7 而非更早，是因为优先级低于性能优化——它改善画质但不影响正确性或性能

**结束目标**：10M 场景实时可交互（LOD 大幅压缩远景可见集），远景无 aliasing 闪烁。
