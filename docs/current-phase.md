# 当前阶段：M1 阶段三 — PBR 光照基础

> 目标：在阶段二的基础渲染管线上，升级为完整 PBR 光照 — Cook-Torrance + IBL 环境光、Depth+Normal PrePass、MSAA、HDR + Tonemapping。
> 涉及 App 层重构（Renderer 提取）、RG 演进（Managed 资源）、Layer 0 补全（Compute Pipeline）、Layer 1 扩展（IBL）、shader 架构升级。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

#### 依赖关系

```
Step 1: Renderer 提取
    ↓
Step 2: RG Managed 资源
    ↓
Step 3: Descriptor Layout + Compute Infra
    ↓
Step 4a: FrameContext + Pass 类 + ForwardPass 提取
    ↓
Step 4b: HDR + Tonemapping
    ↓
Step 4c: MSAA ──────────────→ Step 5: Depth + Normal PrePass
    ↓                                      ↓
Step 6: IBL Pipeline + Skybox ──→ Step 7: PBR Shader 升级
```

#### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | Renderer 提取 | 编译通过，渲染输出与阶段二一致 |
| 2 | RG Managed 资源 | 现有渲染正常，depth buffer 改由 RG 管理 |
| 3 | Descriptor Layout + Compute Infra | 布局更新无 validation 报错，compute shader 能创建并 dispatch |
| 4a | FrameContext + Pass 类 + ForwardPass 提取 | 纯重构，渲染输出与 Step 3 一致 |
| 4b | HDR + Tonemapping | Forward 渲染到 HDR → tonemapping → swapchain，高光不截断 |
| 4c | MSAA | MSAA 渲染正确，DebugUI 可切换采样数 |
| 5 | Depth + Normal PrePass | PrePass 运行，Forward zero-overdraw，normal buffer 有输出 |
| 6 | IBL Pipeline + Skybox | IBL 预计算完成，Skybox 渲染天空背景，RenderDoc 可检查 cubemap 和 LUT 内容 |
| 7 | PBR Shader 升级 | 完整 PBR 光照画面，金属表面反射环境，各 debug 模式可用 |

### Step 1：Renderer 提取

- 从 Application 中提取 Renderer 类（纯重构，零功能变化）
- 创建 `RenderInput` 结构体和 `Renderer` 类骨架
- 将渲染资源所有权从 Application 迁移到 Renderer
- 将渲染逻辑从 Application 迁移到 Renderer
- 两阶段 resize 适配
- **验证**：编译通过，运行效果与阶段二一致，无 validation 报错

#### Renderer 类设计

所有权划分、接口定义见 `milestone-1/m1-interfaces.md`「Layer 3 — Renderer 接口」和 `milestone-1/m1-design-decisions.md`「App 层设计」。

迁移到 Renderer 的成员：`render_graph_`、`shader_compiler_`、`material_system_`、`unlit_pipeline_`（重命名为 `forward_pipeline_`）、`depth_image_`、`default_sampler_`、`default_textures_`、`global_ubo_buffers_`、`light_buffers_`、`swapchain_image_handles_`。

Application 主循环改为：

```
run() → while loop:
  glfwPollEvents()
  minimize 暂停处理
  begin_frame()   — fence wait → deletion flush → acquire → imgui begin
  update()        — camera update → light 选择 → 填充 RenderInput → culling → debug panel
  render()        — renderer_.render(cmd, input)
  end_frame()     — submit → present → swapchain 重建检查 → advance frame
```

### Step 2：RG Managed 资源

- 实现 Render Graph 的 managed 资源管理（L1 级别：创建 + 缓存 + resize 自动重建）
- 新增 `RGImageDesc`、`RGManagedHandle` 类型和 managed 资源 API
- 迁移现有 depth buffer 从手动管理到 managed 资源
- **验证**：现有渲染正常工作，depth buffer 由 RG managed 管理，resize 时自动重建

#### 设计要点

Managed 资源的设计决策和演进级别见 `milestone-1/m1-design-decisions.md`「Managed 资源管理」，接口定义见 `milestone-1/m1-interfaces.md`「Managed 资源类型」。

关键设计：
- 两步注册模式：`create_managed_image()` 返回持久 `RGManagedHandle`，`use_managed_image()` 每帧返回 `RGResourceId`
- Relative 模式通过 `set_reference_resolution()` 配合 resize 自动重建
- `update_managed_desc()` 支持 MSAA 切换时更新资源描述（Step 4 使用）
- 不适合 RG 管理的资源（IBL 预计算产物等一次性 init 资源）不经过 RG

#### 使用流程参照

```cpp
// Renderer::init() — 一次性注册
render_graph_.set_reference_resolution(swapchain.extent);
managed_depth_ = render_graph_.create_managed_image("Depth", depth_desc);
managed_hdr_   = render_graph_.create_managed_image("HDR Color", hdr_desc);

// Renderer::render() — 每帧使用
render_graph_.clear();
fr.depth     = render_graph_.use_managed_image(managed_depth_);
fr.hdr_color = render_graph_.use_managed_image(managed_hdr_);

// Renderer::on_swapchain_recreated() — resize
render_graph_.set_reference_resolution(new_extent);
// 内部自动重建所有尺寸变化的 relative managed images

// Renderer::destroy()
render_graph_.destroy_managed_image(managed_depth_);
render_graph_.destroy_managed_image(managed_hdr_);
```

---

### Step 3：Descriptor Layout + Compute Infra

- 更新描述符布局（为 M1 全部阶段预留）
- 新增 compute shader 基础设施（pipeline 创建 + dispatch）
- **验证**：所有布局更新无 validation 报错，现有渲染正常；能创建并 dispatch 一个空 compute shader

#### 设计要点

描述符三层架构见 `milestone-1/m1-design-decisions.md`「Descriptor Set 三层架构」+「Set 2 — Render Target Descriptor Set」。Compute 基础设施为阶段三 IBL 预计算提供前置支持。

关键设计：
- Set 1 新增 binding 1（`samplerCube[]`），与 binding 0（`sampler2D[]`）共用 `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，去掉 `VARIABLE_DESCRIPTOR_COUNT`
- 新增 Set 2 layout + pool + descriptor set（render target 专用，M1 全部 8 个 binding 一次性预留，`PARTIALLY_BOUND`）
- `DescriptorManager` 新增 `register_cubemap()` / `unregister_cubemap()` API + Set 2 管理（layout、pool、set 分配、render target binding 更新）
- `pipeline.h` 新增 `ComputePipelineDesc` + `create_compute_pipeline()`，`ComputePipelineDesc` 支持自定义 `descriptor_set_layouts`（IBL push descriptors 需要）
- `commands.h` 新增 `CommandBuffer::dispatch()` + `CommandBuffer::push_descriptor_set()`
- 所有 pipeline layout 统一为 `{Set 0, Set 1, Set 2}`，`get_global_set_layouts()` 返回三个 layout

---

### Step 4a：FrameContext + Pass 类 + ForwardPass 提取

- 引入 FrameContext 概念（`framework/frame_context.h`），携带 RG 资源 ID + 场景数据引用 + 帧参数
- 引入 Pass 类基础设施：ForwardPass + TonemappingPass 提取为独立类（`passes/`）
- ForwardPass 从 RG lambda 提取为独立类，渲染逻辑不变
- TonemappingPass 骨架创建（此阶段仅 passthrough，4b 实现 ACES）
- **验证**：纯重构，渲染输出与 Step 3 完全一致，无 validation 报错

#### 设计要点

Pass 类设计（具体类、方法签名、FrameContext）见 `milestone-1/m1-design-decisions.md`「Pass 类设计」。

关键设计：
- 具体类（非虚基类），Renderer 持有具体类型成员
- 方法：`setup()` / `on_resize()` / `on_sample_count_changed()` / `record()` / `destroy()`
- MSAA 相关 pass 的 `setup()` 接收 `sample_count`，非 MSAA pass 不接收
- Attachment format 在 pass 内部硬编码，`setup()` 不传入
- FrameContext 是纯每帧数据，服务引用由 pass 在 `setup()` 时存储指针

---

### Step 4b：HDR + Tonemapping

- 创建 HDR color buffer（R16G16B16A16_SFLOAT，managed 资源）
- 实现 ACES tonemapping pass（fullscreen fragment shader）
- Forward Pass 渲染到 HDR buffer，Tonemapping 映射到 swapchain
- **验证**：高光不截断，画面正确 tonemapped

#### 阶段三帧流程

```
HDR Color (forward pass 渲染, R16G16B16A16F)
    ↓ texture() 采样
Tonemapping pass (fullscreen fragment): exposure → ACES → output linear [0,1]
    ↓ 硬件自动 linear → sRGB (swapchain format)
Swapchain Image (B8G8R8A8_SRGB)
    ↓
ImGui pass
    ↓
Present
```

关键设计：
- Tonemapping 使用 fullscreen fragment shader（非 compute），因为 SRGB swapchain 不支持 `STORAGE_BIT`
- Step 4b 不引入 MSAA（Forward 直接渲染到 1x hdr_color）

---

### Step 4c：MSAA

- 创建 MSAA managed 资源（msaa_color、msaa_depth）
- 配置 Dynamic Rendering color resolve（AVERAGE）
- 实现 MSAA 运行时切换（DebugUI 控件 + `on_sample_count_changed()` + 资源重建）
- 1x MSAA 时不创建 MSAA 资源，Forward Pass 直接渲染到 hdr_color
- **验证**：MSAA 渲染正确，DebugUI 可切换 1x/2x/4x/8x 采样数

#### 设计要点

MSAA 配置策略（运行时可切换 1x/2x/4x/8x）和 Tonemapping 设计见 `milestone-1/m1-design-decisions.md`「MSAA 配置策略」+「Tonemapping」。Depth resolve 模式（MAX_BIT — 前景深度）见「Depth Resolve 模式」。

关键设计：
- Step 4c 不创建 resolved depth（Tonemapping 不需要 depth），Step 5 PrePass 引入时创建
- MSAA resolve 通过 Dynamic Rendering 原生完成（`VkRenderingAttachmentInfo` 配置 color AVERAGE），RG 零改动
- 1x MSAA 时不创建 MSAA 资源，Forward Pass 直接渲染到 hdr_color（1x）。MSAA 复杂度不向 Tonemapping 泄漏
- MSAA 切换触发 `update_managed_desc()` + `on_sample_count_changed()`，与 resize/vsync 统一模式

---

### Step 5：Depth + Normal PrePass

- 在 forward pass 之前添加深度+法线预渲染 pass，实现 zero-overdraw
- 创建 PrePass shader（共享 VS + Opaque FS + Mask FS）和 normal 公共库
- 创建 MSAA + resolved normal buffer（managed 资源）
- 修改 Forward pass 为 EQUAL depth test（利用 PrePass 深度消除 overdraw）
- **验证**：PrePass 正确填充 depth 和 normal buffer（RenderDoc 检查），Forward pass zero-overdraw 无视觉瑕疵

#### 设计要点

Depth + Normal PrePass 设计、Alpha Mask 处理、Forward Pass 深度配合见 `milestone-1/m1-design-decisions.md`「Depth + Normal PrePass」+「Forward Pass 升级」。

关键设计：
- Opaque 和 Mask 使用独立 pipeline + shader（`discard` 影响 Early-Z）
- 绘制顺序：先 Opaque（填充 depth）再 Mask（利用已有深度）
- PrePass 通过 Dynamic Rendering 同时 resolve depth（MAX_BIT）和 normal（AVERAGE），成为 resolved depth/normal 的唯一产生者
- Forward pass 改为 depth compare EQUAL + depth write OFF + 不配 depth resolve，资源声明减为 3 个（msaa_color Write、msaa_depth Read、hdr_color Write）
- 两个 VS 都加 `invariant gl_Position` 保证 bit-identical 深度
- Normal buffer（R10G10B10A2_UNORM, world-space）
- FrameContext 扩展：msaa_normal、normal（resolved）；1x 时无 msaa_normal

---

### Step 6：IBL Pipeline + Skybox

- 实现 IBL 预计算全流程（equirect → cubemap → irradiance/prefiltered/BRDF LUT）
- 4 个 compute shader 共享相同模式（immediate scope + dispatch），验证点统一，不拆分 Step
- IBL 模块位于 Framework 层（`framework/ibl.h`），预计算在 `Renderer::init()` 中执行
- 实现 Skybox Pass（独立 RG pass，采样中间 cubemap 渲染天空背景）
- **验证**：IBL 预计算无 validation 报错，RenderDoc 检查 cubemap 各面和 mip 级别内容、BRDF LUT 呈现预期渐变图案；天空背景正确显示

#### 设计要点

IBL 管线设计（环境贴图输入、预计算策略、产物参数、模块归属）见 `milestone-1/m1-design-decisions.md`「IBL 管线」。

关键设计：
- Equirectangular .hdr 输入（R16G16B16A16_SFLOAT），GPU compute shader 转 cubemap（1024×1024 per face）
- 中间 cubemap 保留用于 Skybox Pass 天空渲染（M2 Bruneton 替换后可销毁）
- 预计算在 `begin_immediate()` / `end_immediate()` scope 内一次性完成
- IBL compute shader 使用 Push Descriptors 绑定输入/输出 image（项目例外，仅限一次性 init compute dispatch）
- Irradiance/prefiltered cubemap 注册到 Set 1 binding 1（`register_cubemap()`），BRDF LUT 注册到 Set 1 binding 0（`register_texture()`）
- `GlobalUniformData` 新增 IBL 字段 + 更新 `bindings.glsl` 布局（Step 6 完成，不等 Step 7）
- Skybox Pass 为独立 RG pass，Forward Pass 之后、Tonemapping 之前，渲染到 resolved 1x hdr_color，读 resolved depth（GREATER_OR_EQUAL + depth write OFF）

---

### Step 7：PBR Shader 升级

- 升级 forward shader 为完整 PBR（Cook-Torrance + IBL），消费全部材质纹理
- 创建 shader 公共库（constants.glsl、brdf.glsl、lighting.glsl）
- 更新 bindings.glsl（cubemap 声明 + GlobalUBO IBL 字段）
- 添加 DebugUI 渲染模式可视化
- **验证**：glTF 场景正确 PBR 渲染，金属表面反射环境，粗糙表面漫反射，Debug 各模式可用

#### 设计要点

Forward Pass 升级策略见 `milestone-1/m1-design-decisions.md`「Forward Pass 升级」，Shader 公共文件组织见「Shader 系统 — 公共文件组织」，材质变体策略见「Shader 系统 — 材质变体策略」。

关键设计：
- forward.frag 原地升级 Lambert → Cook-Torrance + IBL，不保留 Lambert 切换
- 无条件消费全部 5 个材质纹理（bindless default 纹理消除特判）
- `bindings.glsl` 新增 `samplerCube cubemaps[]` 声明和 GlobalUBO IBL 字段
- DebugUI 渲染模式通过 GlobalUBO 传递 debug mode 标志

---

## 阶段三文件清单（随 Step 推进更新）

```
app/
├── include/himalaya/app/
│   ├── renderer.h               # [Step 1 新增] 渲染子系统
│   └── ...                      # application.h, scene_loader.h 等（已有，Step 1 修改）
└── src/
    ├── renderer.cpp             # [Step 1 新增]
    └── ...                      # application.cpp（Step 1 修改）
framework/
├── include/himalaya/framework/
│   ├── frame_context.h          # [Step 4a 新增] FrameContext（纯头文件）
│   └── ibl.h                    # [Step 6 新增] IBL 预计算模块
└── src/
    └── ibl.cpp                  # [Step 6 新增]
passes/
├── include/himalaya/passes/
│   ├── tonemapping_pass.h       # [Step 4a 新增] Tonemapping pass 类
│   ├── forward_pass.h           # [Step 4a 新增] Forward Lighting pass 类
│   ├── depth_prepass.h          # [Step 5 新增] Depth + Normal PrePass 类
│   └── skybox_pass.h            # [Step 6 新增] Skybox pass 类
└── src/
    ├── tonemapping_pass.cpp     # [Step 4a 新增]
    ├── forward_pass.cpp         # [Step 4a 新增]
    ├── depth_prepass.cpp        # [Step 5 新增]
    └── skybox_pass.cpp          # [Step 6 新增]
shaders/
├── fullscreen.vert              # [Step 4b 新增] fullscreen triangle（无顶点输入）
├── tonemapping.frag             # [Step 4b 新增] ACES tonemapping
├── skybox.frag                  # [Step 6 新增] Cubemap 天空采样
├── depth_prepass.vert           # [Step 5 新增] PrePass 共享 VS（invariant gl_Position）
├── depth_prepass.frag           # [Step 5 新增] Opaque: normal_tex → TBN → encode
├── depth_prepass_masked.frag    # [Step 5 新增] Mask: alpha test + discard → encode
├── common/constants.glsl        # [Step 7 新增] 数学常量
├── common/brdf.glsl             # [Step 7 新增] BRDF 函数
├── common/lighting.glsl         # [Step 7 新增] 光照计算
└── common/normal.glsl           # [Step 5 新增] TBN 构造、normal map 解码
shaders/ibl/
├── equirect_to_cubemap.comp     # [Step 6 新增] Equirectangular → Cubemap
├── irradiance.comp              # [Step 6 新增] Irradiance 余弦卷积
├── prefilter.comp               # [Step 6 新增] Prefiltered environment map
└── brdf_lut.comp                # [Step 6 新增] BRDF Integration LUT
```

---

## 技术笔记

| 主题 | 说明 |
|------|------|
| Renderer 命名空间 | `himalaya::app`，与 Application 同层 |
| Resize 两阶段 | `on_swapchain_invalidated()` 在 `swapchain_.recreate()` 之前，`on_swapchain_recreated()` 在之后。`swapchain_image_handles_` 引用旧 VkImage，必须先注销 |
| UBO/SSBO 填充 | 归 Renderer。`GlobalUniformData` 布局是 shader 约定，属于渲染层。Application 只传语义数据（RenderInput） |
| FrameContext | Step 4a 引入 `FrameContext`（`framework/frame_context.h`），携带 RG 资源 ID + 场景数据引用 + 帧参数。替代原 `FrameResources` 设计 |
| Pass 类约定 | Step 1 暂无独立 pass 类（渲染逻辑仍在 RG lambda 中），Step 4a 引入 Pass 类（TonemappingPass + ForwardPass），所有 pass 统一放 Layer 2（`passes/`） |
| Pass 方法 | `setup()` / `on_resize()` / `on_sample_count_changed()` / `record()` / `destroy()`。MSAA 相关 pass 的 setup 接收 `sample_count`，attachment format 在 pass 内部硬编码 |
| Managed vs imported | Managed 资源由 RG 创建和缓存（depth、MSAA、HDR 等渲染中间产物），imported 资源由外部创建（swapchain image）。Managed 每帧以 UNDEFINED 为 initial layout，不追踪帧间状态 |
| MSAA resolve | 所有 resolve 通过 Dynamic Rendering 原生完成（RG 零改动），不引入独立 resolve pass |
| Resolve 产生者 | Step 5+ resolved depth/normal 由 PrePass 产生，resolved hdr_color 由 Forward Pass 产生。Forward Pass 不配 depth resolve（depth write OFF，内容不变） |
| 1x MSAA | 不创建 MSAA 资源，Forward/PrePass 直接渲染到 1x target。FrameContext 中 msaa_* 字段 invalid。Tonemapping 等下游 pass 不感知 MSAA 模式 |
| Depth buffer 迁移 | Step 2 将阶段二手动管理的 depth buffer 迁移为 RG managed 资源 |
| Tonemapping 策略 | ACES fullscreen fragment shader，SRGB swapchain 不支持 `STORAGE_BIT` |
| Fullscreen triangle | Tonemapping 等全屏 pass 使用 hardcoded fullscreen triangle（vertex shader 生成，无顶点输入） |
| Step 4c 资源拓扑 | 4x 时创建 msaa_color + msaa_depth + hdr_color + depth；1x 时只创建 hdr_color + depth。Step 4c 不创建 resolved depth（Step 5 PrePass 引入时创建） |
| IBL 预计算 | 4 个 compute shader 共享 immediate scope + dispatch 模式，验证点统一不拆 Step。Equirect 输入 R16G16B16A16F，中间 cubemap 1024×1024 保留用于 Skybox |
| Skybox Pass | Step 6 引入。独立 RG pass，Forward 之后 Tonemapping 之前，渲染到 resolved 1x hdr_color，GREATER_OR_EQUAL depth test，不需要 MSAA |
| DebugUI 跟随 Step | 每个 Step 涉及的 DebugUI 变更跟随该 Step：MSAA 切换在 Step 4c，渲染模式在 Step 7 |
| Debug 渲染模式 | debug_render_mode != 0 时 TonemappingPass 跳过 ACES（passthrough），保留硬件 linear→sRGB |
| 透明物体回归 | Step 5 EQUAL depth test 后，AlphaMode::Blend 物体不渲染。Phase 7 Transparent Pass 修复 |
| PrePass 在 MSAA 之后 | PrePass 直接写入 MSAA depth/normal，不需要经历 1x→4x 的中间迁移状态 |
