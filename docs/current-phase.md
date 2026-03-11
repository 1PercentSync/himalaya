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

## 阶段三文件清单（随 Step 推进更新）

```
app/
├── include/himalaya/app/
│   ├── renderer.h               # [Step 1 新增] 渲染子系统
│   └── ...                      # application.h, scene_loader.h 等（已有，Step 1 修改）
└── src/
    ├── renderer.cpp             # [Step 1 新增]
    └── ...                      # application.cpp（Step 1 修改）
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
