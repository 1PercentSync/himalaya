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
| Specular Occlusion | Bent Normal + GTSO（Jimenez 2016）：GTAO 计算 bent normal，forward.frag 解析公式评估 cone 交集 | XeGTAO 参考实现，精度高于 Lagarde 近似 |
| AO Temporal | 三层 rejection（UV + prev depth + AO 邻域 clamp），bent normal 仅 blend 不 clamp | GTAO 噪声低，temporal 即可干净 |
| Prev depth | Resolved depth 标记 temporal → `get_history_image()` 返回上一帧深度 | 零 copy 开销 |
| AO 参数传递 | Compute pass push constants | 不膨胀 GlobalUBO |
| Roughness buffer | DepthPrePass 新增 R8 输出 | M2 SSR（M1 阶段无消费方，GTSO 在 forward.frag 使用 per-fragment material roughness） |
| Contact Shadows | Screen-space ray march + 深度自适应 thickness + 距离衰减 + 无 temporal | 确定性输出，push constant 传光方向 |
| Contact Shadows 多光源 | M1 单方向光单 dispatch 单 R8 | shader 不假设光源类型，M2 方案待定 |
| HDR Sun 方向光 | 新增 `LightSourceMode::HdrSun`，equirectangular 像素坐标 → HDR 空间方向 → 逆 `rotate_y`（HDR→世界） | 方向光与天空盒太阳对齐，IBL 旋转时同步 |
| 色温模型 | Kelvin → 线性 RGB（2000K~12000K），Fallback + HDR Sun 均适用 | 统一的物理色温控制，替代固定白色 |
| HDR Sun 持久化 | `config.json` 存 HDR 路径 → 像素坐标映射 | 切换 HDR 时自动恢复已标记的太阳位置 |

---

## 基础设施决策

### Temporal 数据管理

**选择：RG Managed Temporal（阶段五引入）**

> 阶段三决策结果：Managed 资源 `use_managed_image(handle)`，initial layout 统一 UNDEFINED，无 final transition。

`create_managed_image()` 新增 `temporal=true` 参数，RG 内部分配第二张 backing image，`clear()` 时自动 swap current/history，resize 时重建两张并标记 history 无效。

- `get_history_image()` 始终返回 valid RGResourceId（两张 backing image 总存在），`is_history_valid()` 查询 history 内容是否有效（首帧/resize 后无效，调用方据此设 blend_factor=0）
- `use_managed_image(handle, final_layout)` 扩展阶段三的 `use_managed_image(handle)` API，新增必选 `final_layout` 参数。Temporal current 传 `SHADER_READ_ONLY_OPTIMAL`（帧末 transition，确保 swap 后 history layout 正确）；非 temporal 传 `UNDEFINED`（不插入帧末 barrier，等效于阶段三原始行为）

**为什么始终返回 valid 而非首帧返回 invalid：** Compute pass 的 Set 3 是 push descriptor，不支持 `PARTIALLY_BOUND`，所有 binding 在 dispatch 时必须有效。始终返回 valid 使 pass 代码无分叉，所有 binding 无条件 push。首帧 history 内容是垃圾但 blend_factor=0 意味着 shader 忽略它。

**为什么 temporal current 需要 final_layout：** 非 temporal managed image 不做 final transition（每帧 UNDEFINED 覆写，不关心帧间 layout）。Temporal current 帧末处于 compute pass 留下的 GENERAL layout，下帧 swap 后变为 history，需要以 SHADER_READ_ONLY_OPTIMAL 导入。帧末强制 transition 确保 layout 匹配，不依赖"上帧最后一个 pass 是什么类型"的假设。`final_layout` 作为 `use_managed_image` 的显式参数而非 RG 内部推导，保持 RG 不区分 temporal/非 temporal 的统一性，与 `import_image` 接受 `final_layout` 参数的 API 风格一致。

**M2 复用：** SSR、SSGI 的 temporal pass 使用同一机制——`temporal=true` + `get_history_image()` + `is_history_valid()`，零额外基础设施。

### Per-frame Set 2

> 阶段二决策结果：三层 Descriptor Set 架构（Set 0 全局 Buffer、Set 1 持久纹理、Set 2 帧内 Render Target），Set 2 分配 1 份。

Set 2 从阶段二的 1 份扩展为 **per-frame 2 份**（对应 2 frames in flight），与 Set 0 统一模式。

**为什么需要：** Temporal 资源（resolved depth、ao_filtered）每帧 swap backing image。2 frames in flight 下，帧 N 的 GPU 可能仍在读取 Set 2 中的 binding，帧 N+1 的 CPU 更新同一份 Set 2 会违反 Vulkan spec。Per-frame 双缓冲使每帧只写自己的 copy，不修改另一帧在 GPU 上使用的 copy。

- 非 temporal binding 在 init/resize/MSAA 切换时写入两份 copy
- Temporal binding 每帧更新当前帧 copy（swap 后的新 backing image）

Set 2 Pool 从 maxSets=1 扩展为 maxSets=2，容纳 16 COMBINED_IMAGE_SAMPLER（8 binding × 2 frames in flight）。Set 2 不需要 `UPDATE_AFTER_BIND`——per-frame 双缓冲已解决帧间竞争。

### READ_WRITE 与 Temporal 的区分

> 阶段二决策结果：`READ_WRITE` 表示同一张 image 在同一帧内同时读写（如 depth attachment）。

Temporal 场景不使用 `READ_WRITE`——历史帧数据通过 `get_history_image()` 获取独立的 `RGResourceId`，当前帧和历史帧是**两个不同的资源**，各自声明 `READ` 或 `WRITE`。

### Compute 绑定机制

> 阶段二决策结果：graphics pipeline layout `{Set 0, Set 1, Set 2}`。阶段三为 IBL 引入了 `push_descriptor_set()` 轻量封装（一次性 init scope）。

**Pipeline Layout**：Per-frame compute pipeline 使用 `{Set 0, Set 1, Set 2, Set 3(push)}`，Set 0-2 与 graphics 共享，Set 3 per-pass 自定义（push descriptor flag）。

**Compute Helpers**：

- `bind_compute_descriptor_sets(layout, first_set, sets, count)` — 绑定 Set 0-2 预分配 descriptor sets 到 compute pipeline（`VK_PIPELINE_BIND_POINT_COMPUTE`，与 `bind_descriptor_sets` 的 graphics 版对称）
- `push_storage_image(ResourceManager&, layout, set, binding, ImageHandle)` — compute 输出
- `push_sampled_image(ResourceManager&, layout, set, binding, ImageHandle, SamplerHandle)` — compute 输入
- `get_compute_set_layouts(set3_push_layout)` → `{set0, set1, set2, set3}`

`bind_compute_descriptor_sets` 在 Step 7 实现 GTAOPass 时引入——现有 `bind_descriptor_sets` 硬编码 `VK_PIPELINE_BIND_POINT_GRAPHICS`，compute pass 需要 `COMPUTE` bind point 绑定全局 Set 0-2。显式传 `ResourceManager&` 用于 `ImageHandle → VkImageView` 解析，保持 CommandBuffer 作为纯 `VkCommandBuffer` wrapper。`get_compute_set_layouts` 封装 Set 0-2 全局 layout + Set 3 push descriptor layout，避免 10+ compute pass 手动拼 layout，架构演进时单点修改。

### GlobalUBO — 阶段五扩展

> 阶段四 GlobalUBO 720 bytes。

阶段五新增 `inv_projection`(64 bytes) + `prev_view_projection`(64 bytes) → ~848 bytes，仍远小于 16KB。

- `inv_projection`：GTAO 从 depth 重建 view-space position（多 pass 共享，放 GlobalUBO 而非 push constants）
- `prev_view_projection`：AO temporal reprojection（当前帧世界坐标 → 上一帧 UV）。Renderer 帧末缓存当前帧 VP，首帧 prev = current

AO 特有参数（radius、directions、steps、bias、intensity 等）通过 compute pass push constants 传递，仅 GTAO compute pass 消费，不膨胀 GlobalUBO。

---

## 前序架构扩展

### Set 2 — 阶段五扩展

> 阶段三决策结果：Set 2 专用 Descriptor Set（PARTIALLY_BOUND，~8 binding 预留），binding 0 = hdr_color。阶段四新增 binding 5/6（shadow_map）。

阶段五新增：

| Binding | 类型 | 名称 | 产生者 | 消费者 |
|---------|------|------|--------|--------|
| 1 | `sampler2D` | depth_resolved | PrePass | GTAO, ContactShadows |
| 2 | `sampler2D` | normal_resolved | PrePass | GTAO |
| 3 | `sampler2D` | ao_texture | AOTemporalPass | ForwardPass |
| 4 | `sampler2D` | contact_shadow_mask | ContactShadowsPass | ForwardPass |

Binding 1-2 使用 nearest sampler，binding 3-4 使用 linear sampler。Binding 1（depth_resolved）和 binding 3（ao_filtered）为 temporal binding，每帧更新当前帧 Set 2 copy。

### RenderFeatures — 阶段五扩展

> 阶段四决策结果：RenderFeatures struct + feature_flags bitmask + shader 动态分支机制。阶段四包含 `skybox` + `shadows` 字段和 `FEATURE_SHADOWS` flag。

阶段五扩展：

```cpp
struct RenderFeatures {
    // ... 阶段四已有字段 ...
    bool ao              = true;   // 阶段五新增
    bool contact_shadows = true;   // 阶段五新增
};
```

```glsl
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
```

消费端条件资源声明扩展：

```cpp
if (ctx.ao_texture.valid())
    resources.push_back({ctx.ao_texture, Read, Fragment});
```

Shader 端动态分支扩展：

```glsl
float ao = 1.0;
if ((global.feature_flags & FEATURE_AO) != 0u) {
    ao = texture(rt_ao_texture, uv).r;
}
```

### Debug 渲染模式 — 阶段五扩展

> 阶段三决策结果：HDR / passthrough 二分系统。阶段四新增 DEBUG_MODE_SHADOW_CASCADES(8)。

阶段五变更：

- `DEBUG_MODE_AO`(7) 改为显示 `ssao × material_ao` 复合结果（原为仅 material AO）
- 新增 `DEBUG_MODE_AO_SSAO`(9)，显示 GTAO A 通道原始输出，追加到 SHADOW_CASCADES(8) 之后

### RG 渐进式能力扩展

> 阶段二建立了 RG 渐进式能力建设路线（Barrier 自动插入 → 资源导入）。阶段三引入 Managed 资源管理。

阶段五新增：Temporal 资源管理——首个 temporal pass（AO temporal filter）出现于阶段五，详见上方「Temporal 数据管理」。

---

## AO 决策

### AO 算法选择

**选择：GTAO（直接实现，跳过 Crytek SSAO），全分辨率**

直接实现 GTAO（ground-truth ambient occlusion），跳过经典 Crytek SSAO 中间步骤。GTAO 使用结构化 horizon search + 解析积分（cosine-weighted），原始噪声低于随机采样方案。与 Crytek SSAO 的 shader 差距仅 ~40 行（horizon search 替代随机 kernel sampling），避免 M2 时替换丢弃品。

全分辨率运行。GTAO 采样效率高（4 方向 × 4 步 = 16 fetch），M1 无 TAA 半分辨率瑕疵更明显。

### AO 命名约定

Feature 层用 `ao`（RenderFeatures、feature_flags、DebugUI、config 结构体），Implementation 层用 `gtao`（shader 文件名、pass 类名）。特性与算法解耦——未来替换算法时 feature 层接口不变。

### AO 光照集成

**Diffuse indirect**：`ssao × material_ao` + Jimenez 2016 multi-bounce 色彩补偿。Multi-bounce 公式 `max(vec3(ao), ((ao * a + b) * ao + c) * ao)` 其中 a/b/c 由 albedo 决定，浅色表面（高 albedo）AO 压暗减轻，避免白墙角落过度黑暗。

**Specular indirect**：仅由 SO 控制，material AO 不参与。AO 是方向无关标量，specular 是方向相关的，标量 AO 乘 specular 会错误压暗反射方向朝向开阔区域的表面。

仅调制间接光（IBL diffuse + IBL specular），直接光已有 shadow map + contact shadows 覆盖。

### Specular Occlusion

**选择：Bent Normal + GTSO（Jimenez 2016）**

GTAO 在 horizon search 中额外计算 bent normal（Algorithm 2，XeGTAO v1.30 参考实现），输出 RGBA8（RGB = bent normal × 0.5 + 0.5，A = diffuse AO）。消费端（forward.frag）用 bent normal 构建 visibility cone，与 specular cone（roughness → aperture，reflection direction）做解析交集计算 SO。

**为什么选 GTSO 而非 B1（per-direction cone overlap）：** B1 方案（GTAO 直接在每个 slice 中评估 specular cone 与 horizon 重叠）没有公开参考实现。GTSO 有 XeGTAO 完整参考，bent normal 计算仅在 GTAO shader 中增加 ~25% ALU 开销，且 bent normal 是通用中间表示，M2 可复用于 SSGI 等场景。

**为什么用 smoothstep 近似而非 3D LUT：** 论文建议将 cone 交集预计算为 3D LUT（β, αv, αs）。但项目中无 3D 纹理管理基础设施。运行时使用 smoothstep 近似（工业标准，XeGTAO / UE / Frostbite 均采用），在 forward.frag 中仅 ~5 ALU ops/片元。精确球面 cap 面积比需额外 acos + 分支处理且数值稳定性差（`sin(αv)·sin(β)` 作除数，角度趋零时发散），视觉差异在最终画面不可察觉——SO 本身是环境光上的二次调制。

**GTSO cone 交集参数：**
- Visibility cone：方向 = bent normal，半角 `αv = acos(sqrt(1 − AO))`（从 AO 值反推 cone 面积）
- Specular cone：方向 = reflection direction R，半角 `αs` 由 roughness 推导：`cos(αs) = 0.01^(0.5 * r²)`（Jimenez 2016 最优拟合，u = 0.01）
- Cone 间夹角：`β = acos(clamp(dot(bentNormal, R), -1, 1))`
- SO = `smoothstep(0.0, 1.0, (αv − β) / αs)`（smoothstep 近似，工业标准做法）

**分阶段实施：** Step 9 先用 Lagarde 近似公式（`saturate(pow(NdotV + ao, exp2(-16 * roughness - 1)) - 1 + ao)`，Lagarde & de Rousiers 2014）验证 AO 管线正确性（不需要 bent normal），Step 12 升级到 GTSO 后可直接对比精度差异。Lagarde 函数保留在 forward.frag 中，通过 `global.ao_so_mode`（0=Lagarde, 1=GTSO）运行时切换，DebugUI 提供 Checkbox 供实时对比。

**SO mode GlobalUBO 布局：** `ao_so_mode` 位于 offset 852（`frame_index` 之后），复用 `_phase5_pad` 的一个 slot，GlobalUniformData 总大小不变（864 bytes）。AOConfig 新增 `bool use_gtso = true`，Renderer 填充 `ubo.ao_so_mode = ao_config->use_gtso ? 1 : 0`。

**输出格式变更：** GTAO 输出从 RG8 升级为 RGBA8_UNORM。Step 7 先只写 A 通道（AO），RGB 填充默认值（编码后的 view-space normal 或 (0.5, 0.5, 0.5)），Step 12 升级后写完整 bent normal + AO。`ao_noisy`、`ao_blurred`、`ao_filtered` 三张纹理同步从 RG8 改为 RGBA8（bent normal 需要经过 spatial blur 和 temporal filter 降噪）。

**Bent normal 逐 slice 计算（Algorithm 2）：** GTAO 论文 Algorithm 2 的 cosine-weighted 平均可见方向解析积分。每个 slice 从 horizon 角 h1（正方向）、h2（负方向）和投影法线角 γ 计算切线方向分量 t1 和视线方向分量 t2：

```
t1 = (6·sin(h1−γ) − sin(3h1−γ) + 6·sin(h2+γ) − sin(3h2+γ)
      + 16·sin(γ) − 3·(sin(h1+γ) + sin(h2−γ))) / 12
t2 = (−cos(3h1−γ) − cos(3h2+γ) + 8·cos(γ)
      − 3·(cos(h1+γ) + cos(h2−γ))) / 12
bent += n_proj_len × (tangent × t1 + view_dir × t2)
```

零额外 trig 调用实现：公式中所有三角函数通过已有值的代数组合展开。当前代码已持有 `cos(h1) = max_cos_pos`、`cos(h2) = max_cos_neg`、`sin(γ) = sin_n`、`cos(γ) = cos_n`。展开恒等式：

- `sin(h) = sqrt(1 − cos²(h))`（2 次 sqrt，正/负各一）
- `sin(3h) = 3·sin(h) − 4·sin³(h)`，`cos(3h) = 4·cos³(h) − 3·cos(h)`（纯乘加）
- `sin(a±b) = sin(a)·cos(b) ± cos(a)·sin(b)`（纯乘加）
- `cos(a±b) = cos(a)·cos(b) ∓ sin(a)·sin(b)`（纯乘加）

因此 t1 和 t2 的全部计算仅需 2 个 sqrt + 乘加运算，无 sin/cos/acos 调用。

各 slice 累积后 normalize 编码为 `RGB = bn × 0.5 + 0.5`，AO 写入 A 通道。

**Bent normal 坐标空间：** View-space 存储（GTAO 自然输出空间）。消费端 forward.frag 通过 `transpose(mat3(view))` 转回 world-space，与 reflection direction 计算 cone 夹角。View-space 存储意味着相机旋转时 temporal history 的 bent normal 与当前帧不一致——已知风险，由 depth rejection 部分保护，见下方 AO Temporal Filter 章节「Bent normal temporal 风险」。

### GTAO 正确性修订（Step 10a）

初始实现（Step 7）在测试中发现两类问题：深度不连续处的黑色光晕、倾斜表面 AO 强度偏差。审查后确认以下正确性问题，基于 XeGTAO（Intel 开源参考实现）和原始 GTAO 论文修正：

**1. 缺失投影法线长度权重**

GTAO 论文公式：`slice_visibility = n_proj_len × (â(h₀, γ) + â(h₁, γ))`。初始实现遗漏了 `n_proj_len` 因子。当法线与 slice 平面接近垂直时，投影法线长度趋近 0，该 slice 本不应贡献 AO，但缺失权重导致所有 slice 等权参与。对倾斜表面（如地面 + 水平 slice）AO 强度偏差明显。

**2. Horizon 初始化与 falloff 目标**

初始实现将 horizon cosine 初始化为固定 `bias`（0.01），falloff 也衰减到 `bias`。正确做法（XeGTAO）：先投影法线到 slice 平面得到角度 γ，然后：
- 正方向初始化为 `cos(γ + π/2) = -sin(γ)`
- 负方向初始化为 `cos(-γ + π/2) = sin(γ)`

这使得初始 horizon 和 falloff 目标反映实际表面几何。当 γ≈0（表面朝向相机）时，初始值≈0，与原始 `bias=0.01` 近似等效，但当 γ 显著时（倾斜表面）差异很大。

**修复需重构流程**：法线投影从搜索循环后移到循环前（每 slice 先投影，再用切平面极限初始化 horizon，再搜索）。

**3. Thickness heuristic（薄物体光晕）**

Heightfield 假设将每个可见表面视为无限厚——薄物体（围栏、柱子）背后的深度回退不会降低 horizon。初始实现的 `max()` 操作无法"放松"已建立的高 horizon。

添加 thickness heuristic：当后续样本的 horizon cosine 低于当前最大值（深度回退），用可控强度衰减当前最大值。这允许 horizon 在通过薄物体后回退，减少虚假遮蔽。

**4. 帧间噪声变化**

初始 IGN 噪声是纯像素坐标确定性的，每帧相同像素产生相同噪声模式。Push constants 新增 `frame_index`，噪声计算加入帧偏移（`pixel + frame_index × constant`），使 temporal filter 能跨帧积累不同方向/步进偏移的采样，等效方向数从 N 提升到 N × 帧数。

### AO 采样优化（Step 10b）

**步进分布**：从线性 `t = j` 改为二次幂 `t = (j/N)²×N`，将更多样本集中在像素附近（细缝和角落 AO 最需要近处精度）。XeGTAO 的自动调优系统确认最优幂次约为 2.0。

**步进抖动**：新增 R1 quasi-random 序列对每个步进位置施加偏移（与方向抖动的 IGN 正交），将步进量化 banding 打散为高频噪声，由空间/temporal filter 高效去除。

**Falloff 形状**：从 `1 − d²/r²`（从 0 距离就开始衰减）改为 XeGTAO 风格——内部 ~38% 半径保持 100% 权重，外部 ~62% 线性衰减。近处细节保留完整，远处更积极截断。

### AO 空间降噪（Step 10b）

**选择：5×5 edge-aware bilateral blur**

初始实现依赖纯 temporal filter 降噪。帧间噪声变化和采样优化提高了每帧噪声方差，需要空间降噪先平滑帧内噪声，减轻 temporal filter 负担（降低所需 blend factor → 更少 ghosting）。

算法：Gaussian 空间权重（σ≈1.5）× 深度 bilateral 边缘权重，路径累乘对称。

**边缘权重 — 路径累乘**：5×5 kernel 中距离 >1 的样本，沿网格路径（先水平后垂直）累乘逐步边缘权重。例如 center→(2,0) 的路径权重 = edge(0,1) × edge(1,2)，每条边权重 `depth_edge_weight(z_a, z_b)` 使用 `abs(z_a - z_b)` 天然对称（`w(A,B) == w(B,A)`）。路径累乘防止"穿隧"——即使两端深度相近，中间存在深度不连续处时权重仍为零，天然限制光晕扩散。

**深度比较**：使用线性 view-space 深度（`projection[3][2] / (d - projection[2][2])`）。原始 Reverse-Z 深度非线性，近远距离灵敏度不一致。边缘权重使用相对阈值（`|Δz| / max(z_center, ε)`），scene-scale independent，硬编码为 shader 常量。

**空间权重**：5×5 Gaussian σ≈1.5，预计算为 `const float` 数组，零运行时开销。最终权重 = Gaussian 空间权重 × 路径累乘边缘权重。

**无 push constants**：所有参数（Gaussian 权重、深度阈值）为 shader 常量。深度从 Set 2 binding 1（`rt_depth_resolved`）读取，与 GTAOPass 一致。如后续需调参，再升级为 push constant + DebugUI。

**管线变更**：GTAO → **AO Spatial Blur** → AO Temporal。新增 `ao_blurred`（RGBA8，Step 12 前为 RG8）managed image 作为 spatial blur 输出 / temporal filter 输入。AO Temporal 的输入从 `ao_noisy` 改为 `ao_blurred`。

**AOSpatialPass Set 3 push descriptor layout（2 bindings）**：

| Binding | 类型 | 资源 | Sampler |
|---------|------|------|---------|
| 0 | `image2D` (storage, rgba8) | ao_blurred (output) | — |
| 1 | `sampler2D` (sampled) | ao_noisy (input) | nearest_clamp |

### AO Temporal Filter

**选择：三层 rejection + 空间降噪后 temporal**

GTAO 经空间降噪（5×5 bilateral blur）后进入 temporal filter。空间降噪先平滑帧内噪声，temporal filter 在干净输入上做跨帧积累，blend factor 可适当降低以减少 ghosting。

三层 rejection：
1. **UV 有效性**：prev_uv 越界 → reject
2. **Prev depth 深度一致性**：当前世界坐标在上一帧的预期深度 vs 实际存储深度，**相对阈值** 5%（`abs(expected - stored) / max(expected, 1e-4) > 0.05`）。相对阈值在 Reverse-Z 下所有距离的 disocclusion 检测灵敏度一致——绝对阈值在远处（depth→0）过于宽松，会漏掉远处 disocclusion 导致 ghosting
3. **邻域 clamp**：对当前帧输入（`ao_blurred`，空间降噪后）A 通道（AO）3×3 `texelFetch` 采样取 min/max，将 history AO 值 clamp 到 `[min, max]`（`texelFetch` 避免双线性插值缩小真实范围）。空间降噪后 clamp 范围更窄，stale history 被更积极拒绝 → 减少 ghosting。Bent normal（RGB）不做邻域 clamp（方向向量的 min/max 无物理意义），使用与 AO 相同的 blend factor 直接 blend

**History 有效性传递**：Renderer 查询 `is_history_valid(managed_ao_filtered_)` 写入 FrameContext 的 `ao_history_valid` 字段，AOTemporalPass 据此设置 push constant blend factor（无效时 blend_factor = 0，纯用当前帧）。

Prev depth 来自 resolved depth 的 temporal history（`get_history_image(depth_handle)`），零额外 copy 开销。首帧 / resize 后 / history 无效时 blend_factor = 0（纯用当前帧）。

**Set 3 push descriptor layout（4 bindings）**：

| Binding | 类型 | 资源 | Sampler |
|---------|------|------|---------|
| 0 | `image2D` (storage, rgba8) | ao_filtered (output) | — |
| 1 | `sampler2D` (sampled) | ao_blurred (input) | nearest_clamp |
| 2 | `sampler2D` (sampled) | ao_history (input) | nearest_clamp |
| 3 | `sampler2D` (sampled) | depth_prev (input) | nearest_clamp |

所有 sampled image 使用 nearest sampler：邻域 clamp 需要精确逐像素值，深度不可线性插值，history 也需精确读取。Output 在 binding 0，与 GTAOPass 的 Set 3 layout 风格一致。Binding 1 的 shader 变量从 `ao_noisy_tex` 重命名为 `ao_input_tex`（实际资源从 `ao_noisy` 变为 `ao_blurred`）。

**Bent normal temporal 处理**：RGB（bent normal）与 A（AO）共享 blend factor 和 rejection 结果，但 bent normal 不做邻域 clamp（方向向量的 min/max 无物理意义）。Bent normal 在 blend 后需 `normalize()`。XeGTAO 同样将 bent normal 与 AO 一起降噪。

**Bent normal temporal 风险**：Bent normal 是 view-dependent 的（从 view-space horizon search 导出），相机旋转时 history bent normal 与当前帧不一致。Depth rejection（Layer 2）对此有一定保护（disoccluded 区域被 reject），但不完美。初始实现采用共享 blend factor，如实测发现光滑金属表面 SO ghosting，备选方案：bent normal 不做 temporal，RGB 直接写当前帧值。改动仅 `ao_temporal.comp` 几行，无格式变更。

### Roughness Buffer

DepthPrePass 新增 R8 roughness managed image 输出。DepthPrePass 已读材质数据（alpha test），roughness 从同一材质取出（`material.roughness_factor * texture(metallic_roughness_tex).g`），shader 改动很小。MSAA 时 AVERAGE resolve（与 normal 一致），roughness 是标量 [0,1]，多个子采样平均物理合理。

**M1 阶段无消费方的原因**：Roughness buffer 最初为 B1 SO 方案设计——B1 要求 GTAO 在 compute shader 中读取 per-pixel roughness 来评估 specular cone 与 horizon 的重叠。后来 SO 方案从 B1 改为 Bent Normal + GTSO（见上方「Specular Occlusion」），GTSO 的 SO 评估移到了 forward.frag，直接使用 per-fragment material roughness（从材质纹理实时采样，比 roughness buffer 更精确），GTAO 不再需要读 roughness buffer。但 roughness buffer 本身仍有价值——M2 SSR 需要屏幕空间 roughness 来确定反射 ray 的 cone 宽度，因此保留 Step 11 的实现，确保 DepthPrePass 管线和资源就绪，M2 可直接使用。

**Clear 值 1.0f**：未覆盖像素（天空等）为最大粗糙度，M2 SSR 中 roughness=1 的像素不会产生错误的镜面反射。

**管线适配**：新增 roughness color attachment 后，`GraphicsPipelineDesc::color_formats` 从 `{kNormalFormat}` 扩展为 `{kNormalFormat, kRoughnessFormat}`。`create_pipelines()` 被 `setup()` 和 `on_sample_count_changed()` 共用，只改一处。

**FrameContext**：新增 `roughness`（resolved，1x）+ `msaa_roughness`（MSAA target，sample_count > 1 时有效），与 `normal` / `msaa_normal` 模式一致。`msaa_roughness` 仅 DepthPrePass::record() 内部使用（渲染目标选择 + RG 资源声明）。

---

## Contact Shadows

**选择：Screen-space ray march + 深度自适应 thickness + 距离衰减 + 无 temporal**

- Screen-space ray march：光方向投影到屏幕空间后在 UV 上均匀步进，每步线性插值深度
- 搜索距离使用世界空间（物理意义明确），shader 内部投影到屏幕空间确定步进范围
- 深度自适应 thickness：`base_thickness × linear_depth`，远处更宽容（深度精度低 + 物体屏幕尺寸小）
- 距离衰减：首次命中 → 按 ray 上的位置 smoothstep 衰减，接触点全强度，搜索极限渐淡
- 无 temporal filter：确定性输出（无随机采样），步数不够则加步数
- Push constant 传光方向：shader 不假设光源类型

**多光源**：M1 单方向光单 dispatch 单 R8。Push constant 传光方向使 shader 不假设光源类型，M2 多光源方案待定。

---

## HDR Sun 方向光

### 光源模式扩展

**选择：新增 `LightSourceMode::HdrSun`，与 Scene / Fallback / None 并列**

现有方向光模式中，Scene 依赖 glTF 灯光数据，Fallback 由用户手动调整 yaw/pitch。HDR Sun 从 HDR 环境贴图中太阳的像素坐标推导方向，与 IBL 旋转耦合。

三种模式解决不同场景：
- **Scene**：glTF 文件自带方向光，直接使用
- **Fallback**：无 glTF 灯光时手动调整方向，灵活但与天空盒不关联
- **HDR Sun**：方向光方向由 HDR 太阳位置决定，旋转 IBL 时方向光同步跟随，确保光影与天空一致

### Equirectangular 坐标转换

HDR 环境贴图使用 equirectangular 投影。像素坐标 (x, y) 到方向向量的转换：

```
// 与 shader sample_equirect() 的 atan(dir.z, dir.x) 约定一致
φ = (x / width - 0.5) * 2π     // 方位角（经度），[-π, π]
θ = (0.5 - y / height) * π     // 仰角（纬度），[-π/2, π/2]

sun_dir = (cos(θ)·cos(φ), sin(θ), cos(θ)·sin(φ))
```

`sun_dir` 是 HDR 空间中指向太阳的方向（IBL 旋转前）。Shader 中 `rotate_y(world_dir, sin(yaw), cos(yaw))` 将世界空间→HDR 空间，因此 HDR→世界需要逆旋转（取反 sin 分量）：`rotated_sun = (c·x - s·z, y, s·x + c·z)`。取反后作为 `DirectionalLight.direction`（光线传播方向）。

三角函数符号约定对照 `equirect_to_cubemap.comp` 中的 `sample_equirect()` 确认：`phi = atan(dir.z, dir.x)`，`uv.x = phi / 2π + 0.5`，`theta = asin(dir.y)`，`uv.y = 0.5 - theta / π`。上述公式是其精确逆映射。

**宽高信息**：`init()` 开头用 `stbi_info()` 只读 HDR 文件头获取宽高（不加载像素数据），存入成员变量，通过 `equirect_width()` / `equirect_height()` getter 暴露。这样缓存路径和计算路径均无需额外处理——尺寸获取独立于 `load_equirect()` 和 KTX2 缓存逻辑。

**自动选择**：场景加载时，有 glTF 灯光→Scene，无灯光但有 HDR→HdrSun，都没有→Fallback。

**无坐标时行为**：保持 HdrSun 模式，坐标默认 (0, 0)，对应 HDR 左上角方向。用户通过 UI 输入正确坐标后自动保存。

### 色温模型

**选择：Kelvin → 线性 RGB 转换函数，Fallback 和 HDR Sun 模式共用**

- 范围：2000K（烛光暖橙）~ 12000K（蓝天冷白），默认 6500K（D65 标准光源）
- 输出：线性空间 RGB，归一化到 (1,1,1) 附近（6500K ≈ 白色）
- 算法：基于 CIE 色彩匹配函数的分段多项式近似（Tanner Helland 或类似方法）
- 位置：`framework/color_utils.h`（渲染相关的颜色科学工具）

Fallback 模式原来固定 `color = vec3(1.0f)`，改为 `color = color_temperature_to_rgb(fallback_light_color_temp_)`。HDR Sun 模式同理。两种模式各自独立的色温参数。

**为什么不采样 HDR 像素颜色**：IBL 预计算后原始 equirect 像素数据已释放（仅保留 cubemap），恢复需要额外保留内存或重新读取文件。色温滑条简单且可控，用户可直接调到视觉上匹配天空的色调。M2 可考虑加载时采样太阳区域自动推导色温。

### 持久化策略

**选择：扩展 `AppConfig`，config.json 新增 `hdr_sun_coords` 字段**

```json
{
  "scene_path": "...",
  "env_path": "...",
  "hdr_sun_coords": {
    "D:/HDR/meadow.hdr": [4865, 939],
    "D:/HDR/studio.hdr": [2048, 512]
  }
}
```

- **Per-HDR**：仅像素坐标 (x, y)，因为太阳位置是 HDR 图片的固有属性
- **全局**：色温、强度、投影设置属于用户偏好，不随 HDR 切换
- 切换 HDR 时自动查找已存坐标；修改坐标时自动 `save_config()`
- key 使用 HDR 绝对路径字符串（与 `env_path` 一致）
