# M1 阶段三：开发前待决策问题

> 阶段三进入开发前的查漏补缺。每个问题列出背景、候选方案和建议，最终由人决策。
> 阶段三实现步骤见 `../current-phase.md`，设计决策见 `m1-design-decisions.md`，接口定义见 `m1-interfaces.md`。

---

## 一、关键设计缺口

> 直接影响实现，必须在开发前决策。

### ~~1. 后处理 Pass 如何采样中间渲染目标~~ — 已决策

> 选择方案 C（Set 2 专用 Render Target Descriptor Set），扩展为三层 Descriptor Set 架构。
> 决策详见 `m1-design-decisions.md`「Descriptor Set 三层架构」+「Set 2 — Render Target Descriptor Set」。

---

### 2. MSAA 切换时 Pass pipeline 重建机制

**背景**：Pass 接口为 `setup()` / `on_resize()` / `record()` / `destroy()`。MSAA 切换需要重建 pipeline（`rasterizationSamples` 烘焙在 pipeline 中），但没有对应的方法。`setup()` 是一次性初始化，`on_resize()` 语义上是分辨率变化。

ForwardPass 和 DepthPrePass 的 pipeline 依赖 sample count。TonemappingPass 的 pipeline 不依赖（它只处理 1x resolved 产物）。

**方案 A — 新增 `rebuild_pipelines(uint32_t sample_count)` 方法**

每个 pass 新增此方法，`Renderer::handle_msaa_change()` 调用每个受影响 pass 的 `rebuild_pipelines()`。

- 优点：语义清晰，只有受影响的 pass 需要实现
- 缺点：增加一个接口方法

**方案 B — 扩展 `on_resize()` 为 `on_config_change(RenderConfig)`**

分辨率、sample count 等配置变化统一走此方法。

- 优点：统一处理多种配置变化
- 缺点：语义模糊，不是所有 pass 都关心所有配置项

**方案 C — 销毁 pass 并重新 setup()**

MSAA 变化时 `pass.destroy()` → `pass.setup(...)` + `pass.on_resize(...)`。

- 优点：不增加新方法
- 缺点：销毁/重建比只重建 pipeline 代价大（虽然不显著），代码模式不直观

**建议**：方案 A。MSAA 切换是明确的低频事件，专用方法最清晰。阶段四 shadow_pass 不受 MSAA 影响不需要实现此方法，避免不相关 pass 处理不属于自己的配置变化。

**附带问题**：`setup()` 是否需要接收初始 sample count？如果 `setup()` 创建 pipeline，它需要知道 sample count。建议 `setup()` 增加参数：

```cpp
void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
           rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
           uint32_t sample_count);
```

---

### 3. 透明物体在 Step 5 之后的处理

**背景**：Step 5 将 Forward Pass 改为 `depth compare EQUAL + depth write OFF`。Alpha masked 物体由 PrePass mask pipeline 处理，Forward EQUAL 正确着色。但 `AlphaMode::Blend` 的真正透明物体不在 PrePass 中渲染（它们不写 depth），因此在 Forward EQUAL test 中会被全部拒绝。

Phase 7 才引入 Transparent Pass。Phase 3 Step 5 到 Phase 7 之间，透明物体完全不渲染。

Sponza 场景主体是 opaque + alpha masked，但如果有任何透明物体（如部分窗户），它们会消失。

**方案 A — 接受回归，文档明确记录**

Phase 3 到 Phase 7 期间不渲染透明物体。Sponza 场景中的透明物体（如果有）暂时不显示。

- 优点：最简单，不增加代码复杂度
- 缺点：可能有视觉回归

**方案 B — ForwardPass 内保留透明子通道**

先用 EQUAL 画 opaque/masked，再用 GREATER（或 GREATER_EQUAL）+ depth write OFF 画透明物体。需要两套 pipeline（或动态 depth compare op）。

- 优点：透明物体继续渲染
- 缺点：Forward Pass 内部逻辑更复杂，需要在 EQUAL 和 GREATER 间切换 pipeline

**方案 C — 注册独立的 "Simple Transparent" RG pass**

在 Forward Pass 之后、Tonemapping 之前，注册一个临时的透明 pass，使用 GREATER depth test + alpha blending + depth write OFF。Phase 7 时替换为正式的 Transparent Pass。

- 优点：代码分离清晰，Forward Pass 保持纯 opaque/masked
- 缺点：多一个临时 pass，Phase 7 需要替换

**建议**：先确认 Sponza 和计划使用的测试场景中是否有 `AlphaMode::Blend` 的物体。如果没有，方案 A 最简单。如果有，方案 B 更务实（Phase 7 正式 Transparent Pass 引入后移除 Forward 中的透明子通道）。

---

## 二、需要明确的设计细节

> 有明确方向但需确认。

### ~~4. Managed 资源 Descriptor 更新生命周期~~ — 已决策

> RG 新增 `get_managed_backing_image()` API，resize handler 中即时获取新 ImageHandle 更新 Set 2 descriptor。
> 决策详见 `m1-design-decisions.md`「Backing Image 即时查询」。

---

### 5. GlobalUBO IBL 字段精确布局

当前 GlobalUBO 304 bytes（结尾 3 个 float pad 从 292 到 304）。IBL 新增 4 个 uint32（irradiance_cubemap_index、prefiltered_cubemap_index、brdf_lut_index、prefiltered_mip_count）= 16 bytes。Step 7 还需要 debug_render_mode（uint32）。

**建议布局**（std140，偏移均满足 4-byte 对齐）：

```
offset   0: mat4 view
offset  64: mat4 projection
offset 128: mat4 view_projection
offset 192: mat4 inv_view_projection
offset 256: vec4 camera_position_and_exposure
offset 272: vec2 screen_size
offset 280: float time
offset 284: uint directional_light_count
offset 288: float ambient_intensity
offset 292: uint irradiance_cubemap_index      ← Step 6 新增
offset 296: uint prefiltered_cubemap_index     ← Step 6 新增
offset 300: uint brdf_lut_index                ← Step 6 新增
offset 304: uint prefiltered_mip_count         ← Step 6 新增
offset 308: uint debug_render_mode             ← Step 7 新增
offset 312: float _pad[2]                      ← padding to 320
```

总大小 320 bytes（20 × 16，满足 std140 struct 对齐要求）。IBL 未初始化时字段值为 0（index 0 对应 default textures，行为正确）。

---

### 6. IBL 输入 Image 格式与中间产物

**equirect 输入**：stbi_loadf 返回 RGB float32。GPU 端需要选择格式：

| 格式 | 大小 | 精度 | 兼容性 |
|------|------|------|--------|
| R32G32B32A32_SFLOAT | 16 B/px | 完整 float32 | 保证支持 SAMPLED + STORAGE |
| R16G16B16A16_SFLOAT | 8 B/px | half float (max ~65504) | 保证支持，HDR 环境贴图极少超出范围 |

两者都需要 CPU 侧 RGB→RGBA 扩展（加 alpha=1.0）。equirect 输入是一次性使用（转 cubemap 后可销毁），带宽不敏感。

**中间 cubemap**（equirect→cubemap 转换结果）：文档未指定分辨率。此 cubemap 是 irradiance/prefiltered 的源数据。建议 **1024×1024 per face**（R16G16B16A16_SFLOAT）。

**需要确认**：

- equirect 输入格式选 R32 还是 R16？（R16 够用且节省一半内存/带宽）
- 中间 cubemap 分辨率选多大？（1024 是常见选择，512 也可接受）
- 中间 cubemap 预计算后是否保留？如果保留可用于天空渲染（Phase 3 forward 不渲染天空，但阶段八或 M2 Bruneton 之前可能需要静态天空背景）。如果销毁则节省约 72MB 显存（1024×1024×6 face × 8 B/px）

---

### 7. Pass setup() 的 Swapchain Format 依赖

TonemappingPass 的 pipeline 创建时需要知道 color attachment format（swapchain 为 `VK_FORMAT_B8G8R8A8_SRGB`）。ForwardPass 需要知道 HDR color format（`VK_FORMAT_R16G16B16A16_SFLOAT`）+ depth format（`VK_FORMAT_D32_SFLOAT`）。

当前 `setup()` 签名不包含这些信息。

**方案 A — 扩展 setup() 参数**

```cpp
struct PassSetupInfo {
    rhi::Context& ctx;
    rhi::ResourceManager& rm;
    rhi::DescriptorManager& dm;
    rhi::ShaderCompiler& sc;
    uint32_t sample_count;
    VkFormat swapchain_format;     // TonemappingPass 用
    VkFormat hdr_color_format;     // ForwardPass 用
    VkFormat depth_format;         // ForwardPass/DepthPrePass 用
    VkFormat normal_format;        // DepthPrePass 用
};
```

- 优点：一次性传入所有配置
- 缺点：struct 字段多，不是每个 pass 都用全部字段

**方案 B — 每个 pass 的 setup() 接受不同参数**

```cpp
// ForwardPass
void setup(Context&, ResourceManager&, DescriptorManager&, ShaderCompiler&,
           uint32_t sample_count, VkFormat color_format, VkFormat depth_format);
// TonemappingPass
void setup(Context&, ResourceManager&, DescriptorManager&, ShaderCompiler&,
           VkFormat output_format);
```

- 优点：每个 pass 只接收自己需要的参数
- 缺点：签名不统一，如果后续引入虚基类会受限

**建议**：方案 B，与"具体类，不用虚函数"的决策一致。每个 pass 只接收自己关心的参数。如果阶段四引入虚基类，届时再统一为 config struct。

---

## 三、规划层面的考量

### 8. Step 4 的粒度

Step 4（MSAA + HDR + Tonemapping + Pass 类基础设施）是阶段三最大的 step，同时引入：

- FrameContext 概念
- Pass 类基础设施（ForwardPass + TonemappingPass 提取）
- MSAA managed 资源（多个）
- HDR color buffer
- 全屏三角形 + Tonemapping shader
- MSAA 运行时切换 + DebugUI
- Dynamic Rendering resolve 配置

相比阶段二的每个 step 只做一件事的粒度，Step 4 同时做了很多件事。

**建议考虑是否拆分**：

| 子 Step | 内容 | 验证标准 |
|---------|------|----------|
| 4a | FrameContext + Pass 类基础设施 + ForwardPass 提取 | 纯重构，渲染输出与 Step 3 一致 |
| 4b | HDR color buffer + Tonemapping | Forward 渲染到 HDR → tonemapping → swapchain，高光不截断 |
| 4c | MSAA 资源 + resolve + 运行时切换 | MSAA 渲染正确，DebugUI 可切换采样数 |

好处：每个子 step 有独立验证点，与阶段二的粒度一致。代价：增加了 step 数量和审查轮次。

---

### 9. 虚基类的引入时机

阶段三使用具体类，文档说"阶段四 pass 数量增长后评估是否引入虚基类"。到阶段四结束时 pass 总数 4 个，阶段五结束 7 个。

如果阶段四引入虚基类，阶段三的 3 个 pass 都需要改造（加 `: public RenderPass`，方法变 `override`）。改造很轻量，但如果在阶段三就用虚基类，阶段四就完全不用改。

**权衡**：

- 阶段三就用虚基类 → 稍微多一点前期工作，后续零改动
- 阶段三用具体类（当前设计）→ 阶段三更简单，阶段四有一小波重构

根据"既不过度设计也不欠缺考虑"原则，倾向于维持当前决策（阶段三具体类），阶段四评估时有更多样本。

---

### 10. Debug 渲染模式与 Tonemapping 的交互

Step 7 通过 GlobalUBO 的 debug_render_mode 字段控制 forward.frag 输出不同分量（Diffuse Only / Normal / Metallic 等）。但 debug 输出通常是线性值（如 normal 的 [0,1] 编码），经过 ACES tonemapping 会失真。

**方案 A — Debug 模式下跳过 tonemapping**

TonemappingPass 读 debug flag，flag != 0 时直接 passthrough（只保留硬件 linear→sRGB 转换，不做 ACES 曲线）。

- 优点：debug 输出值精确
- 缺点：TonemappingPass 需要感知 debug 模式

**方案 B — 接受 tonemapping 失真**

Debug 模式主要用于定性观察，不需要精确值。

- 优点：零额外代码
- 缺点：Normal / AO 等 [0,1] 值经 ACES 后不直观

**方案 C — Debug 模式使用 linear→sRGB 而非 ACES**

保留 gamma correction 但不做色调映射。

- 优点：debug 输出可读且有正确的感知亮度
- 缺点：与方案 A 实现成本相同

**建议**：方案 A（或 C，实现相同）。TonemappingPass 检查 debug flag 成本为零（一行 `if`），debug 输出的可读性值得这一行代码。

---

## 四、远期规划反思

### 11. 阶段八 Tonemapping 输出目标的灵活性

文档已注明阶段八将 Tonemapping 从 fragment 改为 compute，新增 Final Output pass。当前 TonemappingPass 设计为直接输出到 swapchain。

**建议**：TonemappingPass::record() 内部的输出目标不要硬编码为 swapchain——通过 FrameContext 传入输出目标的 RGResourceId。阶段三这个 ID 是 swapchain image，阶段八改为中间 LDR buffer。这样 TonemappingPass 本身不需要修改，只是 Renderer 填 FrameContext 时传不同的 target。

具体地说，FrameContext 可以有一个 `tonemapping_output` 字段（阶段三 = swapchain，阶段八 = LDR buffer），TonemappingPass 从 FrameContext 取输出目标。

---

### 12. Renderer 持有 Pass 的方式

当前设计 Renderer 持有具体类型成员。后续每阶段增加 pass：

| 阶段 | 新增 pass | 累计 |
|------|----------|------|
| 阶段三 | ForwardPass, TonemappingPass, DepthPrePass | 3 |
| 阶段四 | ShadowPass | 4 |
| 阶段五 | SSAOPass, SSAOTemporalPass, ContactShadowsPass | 7 |
| 阶段七 | TransparentPass | 8 |
| 阶段八 | BloomPass, AutoExposurePass, VignettePass, ColorGradingPass, HeightFogPass | 13 |

13 个具体类型成员完全可管理。如果引入虚基类，可改为 `vector<unique_ptr<RenderPass>>` + 类型查找，但牺牲编译期类型安全。

**建议**：维持具体类型成员，与当前设计一致。

---

### 13. Forward Shader 阶段间演进的向前兼容

Phase 3 的 forward.frag 在 Step 7 升级为 Cook-Torrance + IBL。Phase 4 再加阴影采样，Phase 5 加 AO/Contact Shadow，Phase 6 加 lightmap。每个阶段都修改 forward.frag。

**问题**：Step 7 的 forward.frag 是否应该在结构上预留后续阶段的注入点（预留 `#include` 和函数调用位置）？

**建议**：不预留。每个阶段按需修改是自然的增量开发。`lighting.glsl` 的 `evaluate_directional_light()` 函数签名可以在阶段四自然增加 `shadow_factor` 参数。过度预留反而增加当前复杂度，且预留的结构可能与实际需求不匹配。
