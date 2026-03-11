# 当前阶段：M1 阶段三 — PBR 光照基础

> 目标：在阶段二的基础渲染管线上，升级为完整 PBR 光照 — Cook-Torrance + IBL 环境光、Depth+Normal PrePass、MSAA、HDR + Tonemapping。
> 涉及 App 层重构（Renderer 提取）、RG 演进（Managed 资源）、Layer 0 补全（Compute Pipeline）、Layer 1 扩展（IBL）、shader 架构升级。
>
> 关键接口定义见 `milestone-1/m1-interfaces.md`，设计决策见 `milestone-1/m1-design-decisions.md`。

---

## 实现步骤

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

描述符布局更新见 `milestone-1/m1-design-decisions.md`「Bindless 纹理数组」+「IBL 管线 — Descriptor Binding」。Compute 基础设施为阶段三 IBL 预计算提供前置支持。

关键设计：
- Set 1 新增 binding 1（`samplerCube[]`），与 binding 0（`sampler2D[]`）共用 `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，去掉 `VARIABLE_DESCRIPTOR_COUNT`
- Set 0 预留 binding 3（`sampler2DArrayShadow`）供阶段四 CSM 使用，当前不写入（`PARTIALLY_BOUND`）
- `DescriptorManager` 新增 `register_cubemap()` / `unregister_cubemap()` API
- `pipeline.h` 新增 `ComputePipelineDesc` + `create_compute_pipeline()`
- `commands.h` 新增 `CommandBuffer::dispatch()`

---

### Step 4：MSAA + HDR + Tonemapping

- 建立完整的 MSAA → HDR → Tonemapping 渲染管线
- 创建 MSAA 和 resolved 的 managed 资源
- 实现 ACES tonemapping pass（fullscreen fragment shader）
- 实现 MSAA 运行时切换（DebugUI 控件 + pipeline/资源重建）
- **验证**：MSAA 渲染 + HDR tonemapped 输出正确，高光不截断，DebugUI 可切换采样数

#### 阶段三帧流程

```
MSAA HDR Color (forward pass 渲染, R16G16B16A16F, 4x)
    ↓ Dynamic Rendering resolve (AVERAGE)
HDR Color (resolved, R16G16B16A16F, 1x)
    ↓ texture() 采样
Tonemapping pass (fullscreen fragment): exposure → ACES → output linear [0,1]
    ↓ 硬件自动 linear → sRGB (swapchain format)
Swapchain Image (B8G8R8A8_SRGB)
    ↓
ImGui pass
    ↓
Present
```

#### 设计要点

MSAA 配置策略（运行时可切换 1x/2x/4x/8x）和 Tonemapping 设计见 `milestone-1/m1-design-decisions.md`「MSAA 配置策略」+「Tonemapping」。Depth resolve 模式（MAX_BIT — 前景深度）见「Depth Resolve 模式」。

关键设计：
- MSAA resolve 通过 Dynamic Rendering 原生完成（`VkRenderingAttachmentInfo` 配置 color AVERAGE + depth MAX_BIT），RG 零改动
- Tonemapping 使用 fullscreen fragment shader（非 compute），因为 SRGB swapchain 不支持 `STORAGE_BIT`
- MSAA 切换触发 `update_managed_desc()` + pipeline 重建，与 resize/vsync 统一模式
- 1x MSAA 时 forward pass 不配置 resolve，只声明 2 个资源（无 MSAA target）

---

### Step 6：IBL Pipeline

- 实现 IBL 预计算全流程（equirect → cubemap → irradiance/prefiltered/BRDF LUT）
- 4 个 compute shader 共享相同模式（immediate scope + dispatch），验证点统一，不拆分 Step
- IBL 模块位于 Framework 层（`framework/ibl.h`），预计算在 `Renderer::init()` 中执行
- **验证**：IBL 预计算无 validation 报错，RenderDoc 检查 cubemap 各面和 mip 级别内容、BRDF LUT 呈现预期渐变图案

#### 设计要点

IBL 管线设计（环境贴图输入、预计算策略、产物参数、模块归属）见 `milestone-1/m1-design-decisions.md`「IBL 管线」。

关键设计：
- Equirectangular .hdr 输入，GPU compute shader 转 cubemap
- 预计算在 `begin_immediate()` / `end_immediate()` scope 内一次性完成
- Irradiance/prefiltered cubemap 注册到 Set 1 binding 1（`register_cubemap()`），BRDF LUT 注册到 Set 1 binding 0（`register_texture()`）
- `GlobalUniformData` 新增 IBL 字段传递 bindless index

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
shaders/
├── fullscreen.vert              # [Step 4 新增] fullscreen triangle（无顶点输入）
└── tonemapping.frag             # [Step 4 新增] ACES tonemapping
passes/
├── include/himalaya/passes/
│   └── tonemapping_pass.h       # [Step 4 新增] Tonemapping pass 类
└── src/
    └── tonemapping_pass.cpp     # [Step 4 新增]
framework/
├── include/himalaya/framework/
│   └── ibl.h                    # [Step 6 新增] IBL 预计算模块
└── src/
    └── ibl.cpp                  # [Step 6 新增]
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
| FrameResources | Step 1 定义基础结构（swapchain + depth），后续 Step 扩展 |
| Pass 类约定 | Step 1 暂无独立 pass 类（渲染逻辑仍在 RG lambda 中），Step 4 开始引入 |
| Managed vs imported | Managed 资源由 RG 创建和缓存（depth、MSAA、HDR 等渲染中间产物），imported 资源由外部创建（swapchain image）。Managed 的 initial/final layout 由 RG 推导 |
| MSAA resolve | 所有 resolve 通过 Dynamic Rendering 原生完成（RG 零改动），不引入独立 resolve pass |
| Depth buffer 迁移 | Step 2 将阶段二手动管理的 depth buffer 迁移为 RG managed 资源 |
| Tonemapping 策略 | ACES fullscreen fragment shader，SRGB swapchain 不支持 `STORAGE_BIT` |
| Fullscreen triangle | Tonemapping 等全屏 pass 使用 hardcoded fullscreen triangle（vertex shader 生成，无顶点输入） |
| FrameResources 扩展 | Step 4 扩展 FrameResources：msaa_color、msaa_depth、hdr_color、depth（resolved） |
| IBL 预计算 | 4 个 compute shader 共享 immediate scope + dispatch 模式，验证点统一不拆 Step |
