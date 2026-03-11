# M1 阶段三开发前决策清单

> 阶段三进入开发前需要讨论和拍板的所有决策点。每项附带多个方案和倾向建议，最终由用户决定。
> 标记 **[已决定]** 的条目已经确认，**[待定]** 的条目尚待讨论。

---

## 七、Depth + Normal PrePass 设计

### 7.1 PrePass 的范围 [已决定]

| 方案 | 说明 |
|------|------|
| A. 仅 Depth PrePass | 只输出深度，不输出法线 |
| B. Depth + Normal PrePass | 同时输出深度和法线 |
| C. 阶段三做 A，阶段五（需要 SSAO）时升级为 B | 延迟法线输出 |

**建议倾向**：C。阶段三本身不消费 normal buffer（没有 SSAO/SSR），输出 normal 是为阶段五准备。

- 选 B 的好处：阶段三 PrePass 的 pipeline 和 shader 一步到位，阶段五直接复用。
- 选 C 的好处：阶段三更聚焦，不引入未被消费的资源。

#### 决定：方案 B — Depth + Normal PrePass 一步到位

阶段五（SSAO / Contact Shadows）和 M2（SSR / SSGI）都依赖 resolved normal buffer。阶段三直接输出法线，pipeline 和 shader 无需后续修改。Normal buffer 格式见 4.2 决定（R10G10B10A2_UNORM, world-space）。

---

### 7.2 Alpha Mask 物体在 PrePass 中的处理 [已决定]

glTF 有 Alpha Mask 材质（`alphaMode: "MASK"`），需要在 fragment shader 中做 alpha test（discard）。

| 方案 | 说明 |
|------|------|
| A. PrePass 跳过 Alpha Mask 物体 | Mask 物体只在 forward pass 中画 |
| B. PrePass 两批绘制 | 同一 RG pass 内先画 Opaque（FS 无 discard），再画 Mask（FS 有 discard），两个 pipeline |
| C. PrePass 统一用带 discard 的 shader | 所有物体都走同一个含 discard 的 fragment shader |

**建议倾向**：B。

#### 决定：方案 B — 两批绘制（Opaque pipeline + Mask pipeline）

##### 核心理由：`discard` 对 GPU Early-Z 的影响

GPU Early-Z 在 fragment shader 执行**之前**做深度测试，不通过的 fragment 直接跳过 FS。`discard` 关键字破坏此优化——GPU 无法在 FS 执行前确定 fragment 是否会被丢弃，必须先执行 FS 再决定是否写入深度。

即使 Opaque 物体永远不走 discard 分支，只要 shader 二进制中包含 `discard` 指令，GPU/driver 就可能对该 pipeline 的所有调用禁用 Early-Z（编译期决定，非运行时分支）。因此 Opaque 和 Mask 必须使用不同 pipeline + 不同 shader。

##### 排除方案 A 的理由

PrePass 跳过 Mask 物体导致：
- Mask 物体的深度不在 prepass buffer 中，Forward Pass 无法对其使用 EQUAL depth test
- 阶段五 SSAO / Contact Shadows 在 Mask 物体区域读到错误深度（空洞）
- Normal buffer 缺少 Mask 物体法线，SSAO 质量降低

##### 两个 Pipeline 对比

由于 7.1 选择了 Depth + Normal PrePass，Opaque 也需要 fragment shader 写法线。区别在于"FS 无 discard"vs"FS 有 discard"：

| | Opaque Pipeline | Mask Pipeline |
|---|---|---|
| VS 输出 | position, normal, tangent, uv0 | 同左 |
| FS 采样 | normal_tex（无条件，default = flat normal） | normal_tex + base_color_tex（alpha test） |
| FS 操作 | TBN → encode world normal → 写 color attachment | alpha test → discard → TBN → encode → 写 color |
| `discard` | **无** | **有** |
| Early-Z | **保证** | **可能被禁用** |
| Depth write | ON, Compare GREATER | ON, Compare GREATER |

##### 绘制顺序

同一个 RG pass 内：

1. **先画 Opaque**（大多数物体）— 填充 depth buffer
2. **再画 Mask** — Opaque 已写入的深度帮助 depth test 拒绝被遮挡的 Mask fragment

##### Shader 文件

```
shaders/
├── depth_prepass.vert          # 共享（输出 position + normal/tangent + uv0）
├── depth_prepass.frag          # Opaque: 采样 normal_tex → TBN → encode world normal
└── depth_prepass_masked.frag   # Mask: alpha test + discard → 采样 normal_tex → TBN → encode
```

VS 共享——Opaque FS 也需要 uv0（用于 normal_tex 采样）。两个 FS 的差异仅为 alpha test 几行代码。

---

## 八、Forward Pass 升级

### 8.1 从阶段二 Lambert 到阶段三 Cook-Torrance [已决定]

阶段二已有 `forward.vert` + `forward.frag`（Lambert 光照）。阶段三升级为完整 PBR。

**建议**：原地升级。shader 结构不变（输入相同），只是光照计算从 Lambert 扩展为 Cook-Torrance + IBL。`#include "common/brdf.glsl"` + `#include "common/lighting.glsl"` 引入新函数。

#### 决定：原地升级

forward.vert 保持不变（输出 world position / normal / uv0 / tangent）。forward.frag 光照计算从 Lambert 替换为 Cook-Torrance（GGX / Smith / Schlick）+ IBL，通过 `#include` 引入 `common/brdf.glsl` + `common/lighting.glsl`。同时消费 GPUMaterialData 全部 5 个纹理字段（见 6.2 决定）。

不保留 Lambert 切换——如需诊断 PBR 问题，debug UI 可显示各光照分量（diffuse only、specular only、IBL only 等），比回退 Lambert 更有用。

---

### 8.2 Forward Pass 与 PrePass 的深度配合 [已决定]

PrePass 写入深度后，Forward Pass 如何利用此深度避免 overdraw（被遮挡 fragment 执行昂贵 PBR 着色后被丢弃）。

| 方案 | 说明 |
|------|------|
| A. EQUAL + depth write OFF + `invariant gl_Position` | 经典 Z-PrePass 配合，确定性 zero overdraw |
| B. GREATER_OR_EQUAL + depth write OFF | 容忍微小浮点差异，近零 overdraw |

注：原文中的"GREATER + write ON"在有 PrePass 时是错误的——Reverse-Z 下 GREATER 要求 fragment depth 严格大于 buffer depth，PrePass 已写入的同一表面深度 D 无法通过 D > D 测试，导致全部 fragment 被拒绝。

**建议倾向**：A。EQUAL + write OFF 是 Z-PrePass 的业界标准做法，`invariant gl_Position` 是 GLSL 规范为此场景专门设计的保证。

#### 决定：方案 A — EQUAL + depth write OFF + `invariant gl_Position`

##### 工作原理

```
PrePass:  depth compare GREATER, write ON  → 写入最近表面深度 D
Forward:  depth compare EQUAL,   write OFF → fragment depth == D ? 着色 : 拒绝
```

确定性 zero overdraw：只有恰好位于最近表面上的 fragment 通过深度测试并执行 PBR 着色。不写深度（depth buffer 已正确），省带宽。

##### `invariant gl_Position` 保证 bit-identical 深度

EQUAL 测试要求 PrePass 和 Forward Pass 对同一顶点产生完全相同的深度值。GLSL `invariant` 限定符保证：不同 shader program 中，相同表达式 + 相同输入 → bit-identical 输出。

```glsl
// depth_prepass.vert
invariant gl_Position;
gl_Position = global.view_projection * push.model * vec4(in_position, 1.0);

// forward.vert
invariant gl_Position;
gl_Position = global.view_projection * push.model * vec4(in_position, 1.0);
```

即使两个 shader 的其他输出不同（PrePass 输出 normal，Forward 输出 position/normal/uv/tangent），`gl_Position` 的计算结果完全相同。这是 Vulkan/GLSL 规范明确定义的行为。

##### MSAA 兼容性

MSAA 深度测试是 per-sample 的。`invariant` 保证两个 pass 产生相同的 sample coverage 和 per-sample depth。前提是两个 pipeline 的光栅化配置相同（相同 sample count、相同 cull mode），在本项目中自然满足。

##### 排除方案 B 的理由

GREATER_OR_EQUAL 看似更"安全"，实际只在一个方向容忍误差（Forward 深度略大 = 略近 → 通过），另一方向仍有孔洞风险（Forward 深度略小 = 略远 → 被拒绝）。`invariant` 从根本上消除浮点差异，EQUAL 是正确且完整的方案。

##### 性能代价

`invariant` 可能轻微限制编译器优化（不能重排或改变精度），实践中性能影响不可测量。

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

在 forward pass 之前添加深度+法线预渲染 pass，实现 zero-overdraw。

- [ ] 创建 `shaders/common/normal.glsl`（TBN 构造、normal map 采样解码、R10G10B10A2 world-space 编码 `n*0.5+0.5`）
- [ ] 创建 `shaders/depth_prepass.vert`（输出 position/normal/tangent/uv0，`invariant gl_Position`）
- [ ] 创建 `shaders/depth_prepass.frag`（Opaque：采样 normal_tex → TBN → encode world normal，无 discard）
- [ ] 创建 `shaders/depth_prepass_masked.frag`（Mask：采样 base_color_tex alpha test → discard → 采样 normal_tex → TBN → encode）
- [ ] 创建 MSAA normal buffer（R10G10B10A2_UNORM，managed 资源）+ resolved normal buffer（R10G10B10A2_UNORM，managed 资源）
- [ ] DepthPrePass 类（setup 创建 Opaque pipeline + Mask pipeline、on_resize、register_resources、destroy）
- [ ] DepthPrePass 在 RG 中注册（写 msaa_depth + msaa_normal，配置 Dynamic Rendering normal resolve AVERAGE + depth resolve MAX_BIT）
- [ ] PrePass 绘制：先 Opaque 批次（Early-Z 保证），再 Mask 批次（含 discard）
- [ ] Forward pass 改为 depth compare EQUAL + depth write OFF，forward.vert 添加 `invariant gl_Position`
- [ ] 验证：PrePass 正确填充 depth 和 normal buffer（RenderDoc 检查），Forward pass zero-overdraw 无视觉瑕疵

---

### Step 6：IBL Pipeline

> 已迁移到 `docs/current-phase.md` 和 `tasks/m1-phase3.md`，设计决策见 `m1-design-decisions.md`「IBL 管线」。

---

### Step 7：PBR Shader 升级

升级 forward shader 为完整 PBR（Cook-Torrance + IBL），消费全部材质纹理。

- [ ] 创建 `shaders/common/constants.glsl`（PI、EPSILON 等数学常量）
- [ ] 创建 `shaders/common/brdf.glsl`（D_GGX、G_SmithGGX、F_Schlick、Lambert_diffuse，纯函数无场景数据依赖）
- [ ] 创建 `shaders/common/lighting.glsl`（evaluate_directional_light、evaluate_ibl，内部 include constants + brdf，依赖 bindings.glsl 的光源/IBL 结构体）
- [ ] 升级 `forward.frag`：替换 Lambert 为 Cook-Torrance（GGX / Smith Height-Correlated / Schlick）+ IBL 环境光（irradiance + prefiltered + BRDF LUT Split-Sum）
- [ ] `forward.frag` 消费全部 5 个材质纹理：occlusion_tex 调制 IBL/ambient，emissive_tex × emissive_factor 加到最终颜色
- [ ] `bindings.glsl` 更新：新增 `samplerCube cubemaps[]` 声明（Set 1 binding 1）+ GlobalUBO IBL 字段
- [ ] DebugUI 渲染模式：增加可视化选项（Diffuse Only / Specular Only / IBL Only / Normal / Metallic / Roughness / AO）通过 GlobalUBO 传递 debug mode 标志，forward.frag 根据标志输出对应分量
- [ ] 验证：glTF 场景正确 PBR 渲染，金属表面反射环境，粗糙表面漫反射，Debug 各模式可用

---

### 设计备注

| 主题 | 说明 |
|------|------|
| DebugUI 跟随 Step | 每个 Step 涉及的 DebugUI 变更跟随该 Step：MSAA 切换在 Step 4，渲染模式在 Step 7 |
| PrePass 在 MSAA 之后 | PrePass 直接写入 MSAA depth/normal，不需要经历 1x→4x 的中间迁移状态 |
| FrameResources | Step 1 定义，随 Step 推进扩展（Step 4 加 msaa_color/hdr_color/depth，Step 5 加 msaa_normal/normal） |
| Pass 类约定 | 所有 pass 类统一方法签名：setup / on_resize / register_resources(RG&, FrameResources&) / destroy，但无虚基类 |
