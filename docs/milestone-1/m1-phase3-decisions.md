# M1 阶段三开发前决策清单

> 阶段三进入开发前需要讨论和拍板的所有决策点。每项附带多个方案和倾向建议，最终由用户决定。
> 标记 **[已决定]** 的条目已经确认，**[待定]** 的条目尚待讨论。

---

## 九、长远规划反思

### 9.1 GlobalUBO 膨胀趋势 [已决定]

当前 GlobalUBO 304 bytes。阶段三需要新增：

| 字段 | 用途 |
|------|------|
| IBL cubemap bindless indices（3× uint） | irradiance/prefiltered/brdf_lut 索引 |
| IBL prefiltered mip count（uint） | roughness → mip level 映射 |
| 可能的 IBL intensity（float） | IBL 强度调节 |

后续阶段（阴影、后处理等）还会继续增长。

**建议**：暂不拆分。UBO 最小保证大小是 16KB（Vulkan spec），304 bytes 加上阶段三的增长仍然远在限制之下。即使 M1 全部完成也不太可能超过 1KB。拆分 UBO 徒增复杂度。

#### 决定：暂不拆分

M1 全部完成后 GlobalUBO 预估仍远小于 1KB，不需要拆分。

---

### 9.2 Descriptor Set Layout 稳定性 [已决定]

阶段三 IBL cubemap 已决定走 Set 1 bindless（见 6.3），Set 0 layout 无需为 IBL 变更。但后续阶段可能需要新增 Set 0 binding。

**问题**：是否现在就在 Set 0 layout 中预留后续阶段需要的 binding slot？

| 方案 | 说明 |
|------|------|
| A. 按需添加 | 每个阶段需要新 binding 时修改 layout |
| B. 一次性预留 M1 全部 binding | 现在就把 shadow map sampler 等全部预分配 |
| C. 部分预留 | 只预留阶段三到四需要的 |

**建议倾向**：A。Vulkan spec 明确允许 pipeline layout 中有 shader 未使用的 binding，但预留空 binding 增加认知负担且容易出错。按需添加更清晰，pipeline 重建只在阶段切换时发生一次（不是运行时开销）。

#### 决定：方案 B — 一次性预留 M1 全部 Set 0 binding

6.3 决定 cubemap 走 Set 1 bindless，大部分 per-pass 纹理（AO result、contact shadow mask、lightmap 等）是普通 2D 纹理走 Set 1 `sampler2D[]` bindless。唯一无法走 bindless 的是 shadow map（使用硬件比较采样器 `sampler2DArrayShadow`，类型与 `sampler2D` 不同）。

##### M1 完整 Set 0 Layout

| Binding | 类型 | 内容 | 引入阶段 |
|---------|------|------|----------|
| 0 | UBO | GlobalUBO | 阶段二（已有） |
| 1 | SSBO | LightBuffer | 阶段二（已有） |
| 2 | SSBO | MaterialBuffer | 阶段二（已有） |
| 3 | `sampler2DArrayShadow` | CSM Shadow Map Atlas | 阶段四 |

其他所有纹理走 Set 1 bindless：

| 纹理 | Set 1 binding | 阶段 |
|------|--------------|------|
| IBL irradiance / prefiltered | `samplerCube[]` (binding 1) | 阶段三 |
| BRDF LUT | `sampler2D[]` (binding 0) | 阶段三 |
| AO texture | `sampler2D[]` (binding 0) | 阶段五 |
| Contact shadow mask | `sampler2D[]` (binding 0) | 阶段五 |
| Lightmap | `sampler2D[]` (binding 0) | 阶段六 |
| Reflection Probes | `samplerCube[]` (binding 1) | 阶段六 |

##### Set 0 Pool 调整

从 "2 UBO + 4 SSBO"（maxSets=2）调整为 "2 UBO + 4 SSBO + 2 COMBINED_IMAGE_SAMPLER"（binding 3 per-frame × 2），maxSets 仍为 2。

阶段三创建 DescriptorManager 时即预留 binding 3，但不写入实际 descriptor（`PARTIALLY_BOUND` 允许未写入的 binding 存在，只要 shader 不访问即可）。阶段四实现 CSM 时写入 shadow map descriptor。

> 如果后续发现 M1 需要额外的非 bindless Set 0 binding（当前分析未覆盖的情况），仍需修改 layout。但基于当前 M1 功能范围的完整分析，binding 0-3 应覆盖全部需求。

---

### 9.3 帧流程与阶段三的 Pass 子集 [已决定]

`m1-frame-flow.md` 定义了完整 M1 帧流程（PrePass → SSAO → Shadow → Forward → Transparent → PostProcess），阶段三只实现其中子集：

```
阶段三实际帧流程：
PrePass (depth + normal) → Forward (PBR + IBL) → Tonemapping (ACES) → ImGui → Present
```

如果选择了 1.2(e) 的方案 B（不引入虚基类），那么 Renderer 的 `render()` 方法就是显式调用存在的 pass。不存在的 pass 就是还没有对应的代码，没有优雅性问题。引入虚基类后，disabled pass 通过 `enabled() = false` 跳过。

#### 决定：无问题

1.2(e) 选择了方案 B（具体类，无虚基类），Renderer::render() 显式调用每个存在的 pass。尚未实现的 pass 不在代码中，无需任何"缺席"处理机制。

---

### 9.4 阶段二 forward.frag 的光照模型保留 [已决定]

阶段二的 Lambert 光照在阶段三被 Cook-Torrance 取代。

**建议**：不保留 Lambert 切换。Lambert 只是过渡用的简化模型，PBR 上线后没有调试价值。如果需要诊断 PBR 问题，debug UI 可以显示各分量（diffuse only、specular only、IBL only 等），比回退到 Lambert 更有用。

#### 决定：不保留

见 8.1 决定。

---

### 9.5 阶段三引入的 Compute Shader 基础设施 [已确认]

IBL 预计算需要 compute shader（cubemap 卷积、LUT 生成）。阶段二没有用过 compute shader。

**需要确认**：

- `Pipeline` 类是否已支持 compute pipeline 创建？（阶段一只实现了 graphics pipeline）
- `CommandBuffer` 是否已有 `dispatch()` 方法？
- 如果没有，这是阶段三的前置基础设施工作

#### 确认结果：均不存在，需要作为前置工作实现

经代码检查确认：

| 项目 | 状态 |
|------|------|
| `ComputePipelineDesc` / `create_compute_pipeline()` | **不存在**，`pipeline.h` 仅有 `GraphicsPipelineDesc` + `create_graphics_pipeline()` |
| `CommandBuffer::dispatch()` | **不存在**，`commands.h` 仅有 graphics 命令 |
| `.comp` shader 文件 | **不存在** |

阶段三需要新增：

- `pipeline.h`：`ComputePipelineDesc` 结构体 + `create_compute_pipeline()` 函数
- `commands.h`：`dispatch(uint32_t group_count_x, group_count_y, group_count_z)` 方法
- `ShaderCompiler`：确认 `shaderc_compute_shader` stage 支持（shaderc 的 stage 枚举是通用的，大概率已支持，实现时验证）

---

## 十、Step 划分 [已决定]

### 依赖关系

```
Step 1: Renderer 提取
    ↓
Step 2: RG Managed 资源
    ↓
Step 3: Descriptor Layout + Compute Infra
    ↓
Step 4: MSAA + HDR + Tonemapping ──→ Step 5: Depth + Normal PrePass
    ↓                                          ↓
Step 6: IBL Pipeline ─────────────→ Step 7: PBR Shader 升级
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | Renderer 提取 | 编译通过，渲染输出与阶段二一致 |
| 2 | RG Managed 资源 | 现有渲染正常，depth buffer 改由 RG 管理 |
| 3 | Descriptor Layout + Compute Infra | 布局更新无 validation 报错，compute shader 能创建并 dispatch |
| 4 | MSAA + HDR + Tonemapping | MSAA 渲染 + HDR tonemapped 输出正确，DebugUI 可切换采样数 |
| 5 | Depth + Normal PrePass | PrePass 运行，Forward zero-overdraw，normal buffer 有输出 |
| 6 | IBL Pipeline | IBL 预计算完成，RenderDoc 可检查 cubemap 和 LUT 内容 |
| 7 | PBR Shader 升级 | 完整 PBR 光照画面，金属表面反射环境，各 debug 模式可用 |

---

### Step 1：Renderer 提取

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「App 层设计」+「Pass 类设计」。

---

### Step 2：RG Managed 资源

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「Managed 资源管理」+「MSAA Resolve 策略」。

---

### Step 3：Descriptor Layout + Compute Infra

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「Shader 系统」+「Bindless 纹理数组」+「IBL 管线 — Descriptor Binding」。

---

### Step 4：MSAA + HDR + Tonemapping

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「MSAA 配置策略」+「Tonemapping」+「Depth Resolve 模式」。

---

### Step 5：Depth + Normal PrePass

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「Depth + Normal PrePass」+「Forward Pass 升级」。

---

### Step 6：IBL Pipeline

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「IBL 管线」。

---

### Step 7：PBR Shader 升级

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「Shader 系统」+「Forward Pass 升级」。

---

### 设计备注

| 主题 | 说明 |
|------|------|
| DebugUI 跟随 Step | 每个 Step 涉及的 DebugUI 变更跟随该 Step：MSAA 切换在 Step 4，渲染模式在 Step 7 |
| PrePass 在 MSAA 之后 | PrePass 直接写入 MSAA depth/normal，不需要经历 1x→4x 的中间迁移状态 |
| FrameResources | Step 1 定义，随 Step 推进扩展（Step 4 加 msaa_color/hdr_color/depth，Step 5 加 msaa_normal/normal） |
| Pass 类约定 | 所有 pass 类统一方法签名：setup / on_resize / register_resources(RG&, FrameResources&) / destroy，但无虚基类 |
