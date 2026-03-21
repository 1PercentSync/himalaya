# M1 阶段五设计决策：屏幕空间效果

> 阶段五：RG temporal 机制、per-frame Set 2、compute pass helpers、GTAO（AO + SO）、AO temporal filter、Contact Shadows。
> 核心决策摘要见 `m1-design-decisions-core.md`。
> 完整实现步骤见 `../current-phase.md`，任务清单见 `../../tasks/m1-phase5.md`。

---

## 决策总览

| 组件 | 实现级别 | 核心理由 |
|------|----------|----------|
| Temporal 数据 | RG managed temporal（`temporal=true` + `get_history_image()`（始终 valid）+ `is_history_valid()`，内部 double buffer + swap） | 一次建设，AO/SSR/SSGI 复用 |
| Temporal 基础设施 | per-frame Set 2（2 份对应 2 frames in flight），`use_managed_image(handle, final_layout)` 调用方显式传 final_layout | RG 不区分 temporal/非 temporal，per-frame Set 2 解决帧间竞争 |
| Compute pass 绑定 | Set 3 push descriptors（helpers 显式传 ResourceManager&），`get_compute_set_layouts(set3_push)` | 避免 10+ compute pass 手动拼 layout |
| AO 算法 | GTAO（直接实现，跳过 Crytek SSAO），全分辨率 | 差距仅 ~40 行 shader，避免 M2 替换丢弃品 |
| AO 命名 | Feature 层 `ao`，Implementation 层 `gtao` | 特性与算法解耦 |
| AO 集成 | Diffuse: `ssao × material_ao` + multi-bounce。Specular: 仅由 SO 控制 | material AO 不参与 specular |
| Specular Occlusion | 方案 B1：GTAO 读 roughness + 重建 R，per-direction 评估 cone 与 horizon 重叠 | 精度最高，AO 纹理仅 RG8 |
| AO Temporal | 三层 rejection（UV + prev depth + 邻域 clamp），无 blur pass | GTAO 噪声低，temporal 即可干净 |
| Prev depth | Resolved depth 标记 temporal → `get_history_image()` 返回上一帧深度 | 零 copy 开销 |
| AO 参数传递 | Compute pass push constants | 不膨胀 GlobalUBO |
| Roughness buffer | DepthPrePass 新增 R8 输出 | GTAO SO + M2 SSR |
| Contact Shadows | Screen-space ray march + 深度自适应 thickness + 距离衰减 + 无 temporal | 确定性输出，push constant 传光方向 |
| Contact Shadows 多光源 | M1 单方向光单 dispatch 单 R8 | shader 不假设光源类型，M2 方案待定 |

---

## Render Graph — 阶段五扩展

#### Temporal 资源管理（阶段五引入）

```cpp
// RGResourceDesc 中添加 is_temporal 标记
// 获取 temporal 资源的历史帧版本
ImageHandle get_history_image(RGResourceId id);
```

---

## Temporal 数据管理

**选择：RG Managed Temporal（阶段五引入）**

`create_managed_image()` 新增 `temporal=true` 参数，RG 内部分配第二张 backing image，`clear()` 时自动 swap current/history，resize 时重建两张并标记 history 无效。

- `get_history_image()` 始终返回 valid RGResourceId（两张 backing image 总存在），`is_history_valid()` 查询 history 内容是否有效（首帧/resize 后无效，调用方据此设 blend_factor=0）
- `use_managed_image(handle, final_layout)` 由调用方显式指定 `final_layout`。Temporal current 传 `SHADER_READ_ONLY_OPTIMAL`（帧末 transition，确保 swap 后 history layout 正确）；非 temporal 传 `UNDEFINED`（不插入帧末 barrier）

**为什么始终返回 valid 而非首帧返回 invalid：** Compute pass 的 Set 3 是 push descriptor，不支持 `PARTIALLY_BOUND`，所有 binding 在 dispatch 时必须有效。始终返回 valid 使 pass 代码无分叉，所有 binding 无条件 push。首帧 history 内容是垃圾但 blend_factor=0 意味着 shader 忽略它。

**为什么 temporal current 需要 final_layout：** 非 temporal managed image 不做 final transition（每帧 UNDEFINED 覆写，不关心帧间 layout）。Temporal current 帧末处于 compute pass 留下的 GENERAL layout，下帧 swap 后变为 history，需要以 SHADER_READ_ONLY_OPTIMAL 导入。帧末强制 transition 确保 layout 匹配，不依赖"上帧最后一个 pass 是什么类型"的假设。`final_layout` 作为 `use_managed_image` 的显式参数而非 RG 内部推导，保持 RG 不区分 temporal/非 temporal 的统一性，与 `import_image` 接受 `final_layout` 参数的 API 风格一致。

**M2 复用：** SSR、SSGI 的 temporal pass 使用同一机制——`temporal=true` + `get_history_image()` + `is_history_valid()`，零额外基础设施。
