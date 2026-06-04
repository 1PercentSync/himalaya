# Phase 3 决策：Gaussian Splatting 渲染

> 渲染管线总体路线、跨阶段约定和子阶段规划。
> 当前 Phase 3.0 的具体实现决策见 `docs/current-phase.md`。
> 任务清单见 `tasks/reflector-phase3.md`。

---

## 总体路线

Phase 3 采用分阶段演进：

1. **硬件光栅路径**：Compute Cull / Project / Sort → Indirect Draw instanced quads → 硬件 alpha blend。
2. **Tile-Based Compute 路径**：复用上游 cull / project / sort → Tile Binning → Per-tile compute blend。

先用硬件光栅路径建立 correctness baseline，再在正确性稳定后替换末端为 tile-based compute renderer。Phase 3.5 之前重点是数据正确性、排序正确性和硬件光栅下的可观察基线；Phase 3.5 起重点转向密集重叠场景性能。

Phase 编号约定：

- Phase 3.0：硬件光栅基础路径。
- Phase 3.1：运行时优化。
- Phase 3.2：加载时和上传后数据效率优化。
- Phase 3.3 / 3.4：预留。
- Phase 3.5：末端切换为 tile-based compute renderer。
- Phase 3.6 / 3.7：高级性能优化、LOD 和抗锯齿。

## 跨阶段约定

### 渲染模式互斥

GS 渲染与 PT 渲染是独立 `RenderMode`，每帧只走一条路径。Phase 3.x 不要求 GS 与 mesh / skybox / PT 结果同屏合成。GS 专用 near plane、projection 稳定性策略和 output mode 只在 GS path 中生效。

### 上游管线复用

Cull、projection、visible list、sort key 和排序结果是 Phase 3.x 的稳定上游管线。Phase 3.5 切换到 tile-based compute 时，只替换 instanced quad draw 之后的渲染末端，上游数据流继续复用。

### 排序方向与 key

所有 Phase 3.x 路径统一使用 front-to-back 排序。排序依据采用 KHR `sortingMethod = cameraDistance` 对应的 camera distance squared，避免每 splat `sqrt`。

Sort entry 物理存储为 2×32-bit：`distance_key + global_splat_index`。`distance_key` 是 32-bit float key 的 bit encoding，`global_splat_index` 作为 payload 搬运。Radix sort 只处理 32-bit distance key，而不是 64-bit packed key，以减少 radix pass 数。相同 key 的顺序必须 deterministic，避免半透明累积闪烁。

### Descriptor 与资源模型

GS 使用自己的持久 Set 3 descriptor set 绑定 static baked buffers 和 GPU work buffers。该 Set 3 与现有 PT / compute pass 的 Set 3 push descriptor layout 只共享 set index，不共享 layout 或语义。

GS scene GPU resources 必须有集中 owner：由 Renderer 持有的 GS scene resource owner 负责 static/work buffers 的创建、上传、销毁以及持久 Set 3 descriptor 写入/重写。GS pass 类只负责 pipeline、RenderGraph resource declaration 和命令录制，不拥有 scene static/work buffers，也不直接管理持久 Set 3 生命周期。

容量由当前 GS scene 派生并随场景重建，不设置固定 splat 上限，也不在容量不足时静默截断。

### Subgroup 使用

Compute shader 中涉及 append、归约、scan 的操作优先采用 subgroup 模式，以降低 atomic 开销。Phase 3.0 不强制要求 `VK_KHR_shader_subgroup_uniform_control_flow`，但 subgroup intrinsic 必须写在 uniform control flow 中，避免把 subgroup 操作放入 divergent branch。

### 投影空间与 OBB

Projected center、2D covariance、OBB axes/extents 在 Phase 3.x 中统一使用 screen pixel space。硬件光栅路径使用 oriented quad；tile-based compute 路径使用 OBB-tile 相交测试。该约定保证 Phase 3.0 与 Phase 3.5 的投影数据可以复用。

### 色彩与输出

GS composition 遵循 KHR primitive colorSpace。GS 管线负责在进入 TonemappingPass 前输出 linear input；TonemappingPass 不做 GS sRGB decode。GS path 使用 display-referred 输出语义，不套 PT/HDR 的 exposure + ACES 曲线。

Phase 3.0 使用 per-channel hard clamp 作为 KHR 允许的 clamped output。未来如需要更好保 hue，可增加 max-channel range compression 作为 GS display-referred tonemapping 选项。

## 性能目标

| 场景规模 | 目标 |
|---------|------|
| 1M gaussians | Phase 3.0：正确性基线，性能未必实时；Phase 3.1 后目标基本实时（30+ FPS @ 1080p） |
| 10M gaussians | Phase 3.0-3.2：可正确渲染，允许低帧率；Phase 3.5-3.7 优化后目标可交互（20+ FPS） |

---

## Phase 3.0：基础渲染

**目的**：正确渲染 GS 场景的最小可行实现。

**技术**：

- Upload-time bake：world position、world covariance、cull radius、SH happy path。
- Per-attribute SoA GPU buffers，通过 GS 持久 Set 3 绑定。
- RenderGraph buffer barrier 扩展，支持 GS compute/sort/draw 的 buffer hazard。
- Compute frustum cull + 2D projection + SH evaluation。
- Bitonic sort correctness baseline，后续接 radix capacity / visible-count-driven radix。
- Indirect instanced quad draw + front-to-back premultiplied-under hardware blend。
- GS color conversion path + TonemappingPass `LinearClamp` output mode。

**结束目标**：能正确渲染 1M gaussian 级别场景，画面正确、无明显 artifact，性能未必实时。详细 Phase 3.0 决策和完成标准见 `docs/current-phase.md`。

---

## Phase 3.1：运行时优化

**目的**：降低每帧计算量和带宽消耗，使 1M 场景达到基本实时。

**技术**：

- SH evaluation 从 cull/project shader 中拆为独立 post-cull compute pass。
- 多级剔除：sub-pixel 半径、低 opacity、异常大投影等。
- GPU buffer 热 / 冷 / 暖分离。
- 排序带宽优化：评估将适合 in-place compare-and-swap 的排序阶段改为单 primary sort entry buffer 路径，减少 primary/scratch ping-pong 读写带宽；不改变 sort entry 物理格式和最终 draw range 语义。
- 距离自适应 SH 截断：远处 splat 只计算低阶 SH。

**优先级**：SH 求值分离 > 多级剔除 > buffer 分层 > 距离自适应 SH 截断。

**结束目标**：1M 场景达到基本实时（30+ FPS @ 1080p）。

---

## Phase 3.2：加载时优化

**目的**：通过预处理改善运行时数据效率和显存占用。

**技术**：

- 空间分块 + chunk AABB，cull 从全量遍历改为可见 chunk 遍历。
- FP16 / 压缩量化，量化对象是 Phase 3.0 bake 后的 GPU 数据，例如 world covariance、opacity、SH、position 的分块局部表示。
- Morton / Z-order 空间排序，提高缓存命中率和分支一致性。
- 死 splat 剪枝：opacity / scale / position 异常值过滤。

具体量化格式、误差预算、哪些属性保留 FP32，在 Phase 3.2 开始前重新讨论决定。

**优先级**：空间分块 > bake 后 GPU 数据量化方案设计 > Morton 排序 > 死 splat 剪枝。

**结束目标**：显著降低 VRAM 占用和 cull 遍历成本，10M 场景可加载和正确渲染。

---

## Phase 3.5：Tile-Based Compute Renderer

**目的**：替换硬件光栅末端为全 compute 管线，获得 per-tile early-out 能力。

**技术**：

- Tile binning：splat → tile 多对多映射，生成 per-tile splat index list。
- Per-tile compute shader：shared memory 中按 front-to-back 顺序累积颜色和 alpha。
- Transmittance 足够低时 early-out。
- Tile shader 中颜色累积使用 FP32，仅最终输出时转换到目标格式。

**保留**：cull、projection、sort、visible list compaction、SH pre-eval 等上游管线。
**替换**：instanced quad indirect draw → tile binning + per-tile compute render。

**结束目标**：密集重叠场景性能显著提升，10M 场景中等重叠下可交互。

---

## Phase 3.6：高级优化

**目的**：压榨排序、剔除和调度环节的剩余性能空间。

**技术**：

- Budget Rendering：按 `opacity × projected_area` 重要性截断可见列表，锁定渲染上限保帧率。
- Non-Empty Tile Indirect Dispatch：只 dispatch 有覆盖的 tile。
- Temporal Sort Reuse：缓慢移动时复用上一帧排序，局部修正，必要时 fallback 全量 radix sort。
- Previous-Frame Depth Occlusion：前帧深度 reproject 遮挡剔除。
- Pipeline Double-Buffer Overlap：Frame N+1 cull/sort 与 Frame N tile render 重叠。

**决策点**：Phase 3.6 结束时评估 10M 场景 VRAM 占用，决定是否追加 VRAM Streaming。

**结束目标**：已识别的主要性能瓶颈得到缓解，1M 场景高帧率，10M 场景流畅可交互。

---

## Phase 3.7：LOD + 抗锯齿

**目的**：改善 10M 场景的空间自适应精度控制和远景画质。

**技术**：

- Chunk 级重要性子采样 LOD：加载时 per-chunk 排序，运行时按 chunk 到相机距离选择渲染比例。
- Mip-Splatting 抗锯齿：在 cull/project 中对 2D covariance 施加像素 footprint 下界约束，减少远处闪烁。

**结束目标**：10M 场景实时可交互，远景无明显 aliasing 闪烁。
