# M1 阶段三开发前决策清单

> 阶段三进入开发前需要讨论和拍板的所有决策点。每项附带多个方案和倾向建议，最终由用户决定。
> 标记 **[已决定]** 的条目已经确认，**[待定]** 的条目尚待讨论。

---

## 一、架构重构决策

### 1.1 Renderer 类提取时机 [已决定]

`m1-design-decisions.md` 提到"阶段三 pass 实例增多时，自然提取 Renderer 类"。阶段三至少新增 Depth PrePass、IBL 预计算、MSAA Resolve 三个 pass 相关逻辑，Application 成员将显著膨胀。

**问题**：何时提取 Renderer？

| 方案 | 说明 | 优势 | 劣势 |
|------|------|------|------|
| A. 阶段三开头先重构 | 第一个 Step 专门做提取，再加新功能 | 后续 Step 在干净架构上工作 | 重构时还不知道阶段三具体需要什么，可能过早抽象 |
| B. 阶段三中间自然提取 | 写到 Application 明显臃肿时再提取 | 有实际经验指导提取边界 | 中间重构打断功能开发节奏 |
| C. 阶段三结束后再提取 | 阶段三仍用 Application 持有一切 | 延迟决策，阶段三经验最完整 | Application 可能在阶段三中变得非常臃肿 |

**建议倾向**：A。阶段二结束后 Application 已有约 10 个成员，阶段三至少再加 5-6 个（MSAA 资源、Normal buffer、HDR buffer、IBL 资源、PrePass pipeline 等）。提前提取可以建立清晰的 Renderer 边界。阶段二的完整经验已经足够指导提取——我们知道哪些是 RHI 基础设施、哪些是框架组件、哪些是 app 逻辑。

**如果选 A，需要进一步决定**：

- Renderer 持有什么？（RG、DescriptorManager、所有 render resource、所有 pass 实例？）
- Application 保留什么？（Window、Camera、CameraController、SceneLoader、DebugUI？）
- Renderer 的接口长什么样？（`init()`, `render(SceneRenderData, Camera)`, `on_resize()`, `destroy()`？）

#### 决定：方案 A + 以下细化

**所有权划分**：

Renderer 持有（渲染子系统）：

| 类别 | 成员 | 说明 |
|------|------|------|
| 渲染基础设施 | `render_graph_` | pass 编排 |
| | `shader_compiler_` | shader 编译 |
| | `material_system_` | 材质 SSBO 管理 |
| Pipeline | `forward_pipeline_`（原 `unlit_pipeline_`） | 阶段三新增 depth_prepass、tonemapping 等 |
| 渲染目标 | `depth_image_` | 阶段三新增 MSAA/HDR/Normal buffer |
| 共享渲染资源 | `default_sampler_`, `default_textures_` | 场景加载通过 accessor 获取 |
| Per-frame GPU 数据 | `global_ubo_buffers_`, `light_buffers_` | UBO/SSBO 填充是渲染关注点 |
| Swapchain 追踪 | `swapchain_image_handles_` | RG import 用 |

Renderer 持有的非 owning 引用（从 Application 接收）：`context_*`、`swapchain_*`、`resource_manager_*`、`descriptor_manager_*`、`imgui_backend_*`。

Application 保留（平台 + App 逻辑）：

| 类别 | 成员 |
|------|------|
| 平台 | `window_`, `framebuffer_resized_` |
| RHI 基础设施（顶层拥有） | `context_`, `swapchain_`, `resource_manager_`, `descriptor_manager_` |
| ImGui | `imgui_backend_` |
| App 模块 | `camera_`, `camera_controller_`, `debug_ui_`, `scene_loader_`, **`renderer_`** |
| 每帧场景数据 | `scene_render_data_`, `cull_result_` |
| 灯光/调试参数 | `default_lights_`, light yaw/pitch/intensity, 拖拽状态 |
| 渲染参数 | `ambient_intensity_`, `exposure_` |
| 帧状态 | `vsync_changed_`, `image_index_` |

> `resource_manager_` 和 `descriptor_manager_` 归 Application 持有，因为 `scene_loader_` 加载时也要用。Renderer 和 SceneLoader 都是使用者，不是拥有者。

**接口设计**：

```cpp
/// Data passed from Application to Renderer each frame (semantic-level, no GPU layout knowledge).
struct RenderInput {
    uint32_t image_index;
    uint32_t frame_index;
    const framework::Camera& camera;
    std::span<const framework::DirectionalLight> lights;
    const framework::CullResult& cull_result;
    std::span<const framework::MeshData> meshes;
    std::span<const framework::MaterialInstance> materials;
    float ambient_intensity;
    float exposure;
};

class Renderer {
public:
    void init(rhi::Context& ctx, rhi::Swapchain& swapchain,
              rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
              framework::ImGuiBackend& imgui);

    /// Fills GPU buffers (UBO/SSBO), builds RG, executes all passes.
    void render(const rhi::CommandBuffer& cmd, const RenderInput& input);

    /// Two-phase resize: call before swapchain_.recreate().
    void on_swapchain_invalidated();
    /// Two-phase resize: call after swapchain_.recreate().
    void on_swapchain_recreated();

    void destroy();

    // --- Accessors for scene loading ---
    rhi::SamplerHandle default_sampler() const;
    const framework::DefaultTextures& default_textures() const;
    framework::MaterialSystem& material_system();
};
```

**Resize 两阶段**：`swapchain_image_handles_` 引用旧 VkImage，必须在 `swapchain_.recreate()` 之前注销；新资源在 recreate 之后创建。

```cpp
void Application::handle_resize() {
    vkQueueWaitIdle(context_.graphics_queue);
    renderer_.on_swapchain_invalidated();  // unregister old + destroy depth
    swapchain_.recreate(context_, window_);
    renderer_.on_swapchain_recreated();    // register new + create depth
}
```

**UBO/SSBO 填充归 Renderer**：`GlobalUniformData` 的布局是 shader 约定，属于渲染层。Application 只传语义数据，Renderer 打包为 GPU 格式。

**命名空间/文件位置**：`himalaya::app`，`app/include/himalaya/app/renderer.h` + `app/src/renderer.cpp`。

---

### 1.2 RenderPass 设计 [已决定]

`m1-interfaces.md` 定义了草案接口，但阶段二用 lambda 回调的实际经验可能暴露需要调整的地方。

**问题**：RenderPass 基类的具体设计？

草案接口回顾：

```cpp
class RenderPass {
    virtual std::string name() const = 0;
    virtual void setup(Context& context) = 0;
    virtual void register_resources(RenderGraph& graph) = 0;
    virtual void execute(CommandBuffer& cmd, RenderGraph& graph) = 0;
    virtual bool enabled() const { return true; }
    virtual void on_resize(uint32_t width, uint32_t height) {}
};
```

#### (a) Pass 如何获取共享依赖（ResourceManager、DescriptorManager、场景数据等）？

| 方案 | 说明 |
|------|------|
| A. 构造函数注入 | 每个 Pass 构造时接收所需的引用 |
| B. RenderContext 结构体 | 定义一个包含所有共享资源引用的 struct，`execute()` 签名变为 `execute(CommandBuffer&, RenderGraph&, const RenderContext&)` |
| C. Pass 持有 Renderer 引用 | Pass 通过 Renderer 间接访问一切 |

**建议倾向**：B。构造函数注入在 Pass 多了之后构造参数爆炸；持有 Renderer 引用依赖方向不好（Layer 2 反向依赖 Layer 3 的 Renderer）。RenderContext 是一个薄 struct，只传递 pass execute 时需要的运行时数据（camera、scene data、共享资源句柄），保持 Pass 的自包含性。

#### (b) `setup()` 的调用时机和语义？

| 方案 | 说明 |
|------|------|
| A. 仅初始化调用一次 | Pipeline 创建等一次性工作 |
| B. 初始化 + resize 时重新调用 | 依赖分辨率的 pipeline/resource 重建 |
| C. setup 只管 pipeline，on_resize 管资源 | 职责分离 |

**建议倾向**：C。Pipeline 很少因 resize 重建（viewport/scissor 是 dynamic state），但 render target 每次 resize 都要重建。职责分明更清晰。

#### (c) 是否需要 `destroy()` 虚方法？

草案没有 `destroy()`，但项目约定是显式 destroy。

**建议倾向**：需要加。每个 Pass 持有 Pipeline 和可能的私有资源，需要显式销毁。

#### (d) `register_resources()` 的参数是否足够？

当前签名只接收 RenderGraph。但 pass 注册资源时可能需要知道当前帧的 imported resource ID（如 swapchain image、depth buffer），这些 ID 每帧重建。

| 方案 | 说明 |
|------|------|
| A. 通过参数传入所有 imported resource ID | `register_resources(RenderGraph&, const FrameResources&)` |
| B. Pass 在构造时持有 Renderer 引用，从 Renderer 获取 | Pass 自己去问 |
| C. Renderer 注册所有 imported 资源后，以 map/struct 形式传入 | 类似 A 但更结构化 |

**建议倾向**：A（用一个 `FrameResources` struct 持有所有 imported RGResourceId）。这让 pass 不需要反向引用 Renderer。

#### (e) 阶段三实际引入基类，还是继续用组织良好的类（非虚函数）？

| 方案 | 说明 |
|------|------|
| A. 引入虚基类 `RenderPass` | 标准多态，Renderer 持有 `vector<unique_ptr<RenderPass>>` |
| B. 具体类，不用虚函数 | 每个 Pass 是独立类但不继承基类，Renderer 持有具体类型成员 |
| C. 阶段三暂不引入 | 继续 lambda + 类封装，阶段四或更多 pass 时再引入 |

**建议倾向**：B。阶段三只有 2-3 个 pass（DepthPrePass、ForwardPass、可能的 ResolvePass），虚函数多态的收益有限。具体类让类型更明确，编译期可见。阶段四 pass 数量增长后（shadow pass、SSAO 等）再引入多态基类更合时宜——那时有更多样本验证接口设计。

#### 决定

| 子问题 | 决定 | 要点 |
|--------|------|------|
| (a) 共享依赖获取 | B — RenderContext 结构体 | 薄 struct 传递运行时数据，避免构造参数爆炸和反向依赖 |
| (b) setup() 语义 | C — setup 管 pipeline，on_resize 管资源 | Pipeline 用 dynamic state 不因 resize 重建；render target 随分辨率重建。init 时先 `setup()` 再 `on_resize()`，resize 时只调 `on_resize()` |
| (c) destroy() | 加 | 项目约定显式 destroy，每个 Pass 持有 pipeline 和私有资源 |
| (d) register_resources 参数 | A — FrameResources struct | Renderer import 后填充 struct，各 pass 按需取用。因 (e)=B，FrameResources 是约定惯例而非接口契约——所有 pass 统一使用同一签名 `(RenderGraph&, const FrameResources&)`，但 Renderer 有自由度按需偏离 |
| (e) 基类形式 | B — 具体类，不用虚函数 | Renderer 持有具体类型成员。阶段四 pass 数量增长后再考虑虚基类 |

每个 Pass 的方法约定（非虚函数，统一命名）：

```cpp
class ForwardPass {  // 具体类，无基类
public:
    void setup(Context& ctx);                                          // 一次性：创建 pipeline
    void on_resize(uint32_t w, uint32_t h);                            // 创建/重建 resolution-dependent 资源
    void register_resources(RenderGraph& rg, const FrameResources& fr); // 每帧：注册 RG 资源使用
    void destroy();                                                    // 销毁 pipeline + 私有资源
};
```

---

## 二、Render Graph 演进

### 2.1 Transient 资源管理 [已决定]

`m1-design-decisions.md` 规划阶段三引入 transient 资源。阶段三的首批 transient 资源候选：MSAA Depth Buffer、MSAA Normal Buffer、MSAA HDR Color Buffer、Resolved Normal Buffer。

**问题**：阶段三是否引入 RG transient 资源管理？

| 方案 | 说明 | 优势 | 劣势 |
|------|------|------|------|
| A. 按计划引入 | RG 新增 `declare_resource(RGResourceDesc)`，RG 在 compile 时创建资源 | 向长远 RG 演进迈一步 | 实现工作量不小，阶段三已有很多新功能 |
| B. 延迟到阶段四或五 | 阶段三仍然外部创建 + import，和阶段二一样 | 阶段三专注渲染功能 | MSAA 资源/HDR buffer 都由 Application/Renderer 手动管理 |
| C. 引入简化版 | RG 只做"声明式创建"（根据描述创建资源，不做内存别名），延后真正的 transient 优化 | 简化创建代码，降低工作量 | 不是完整 transient 管理 |

**建议倾向**：B 或 C。阶段三的核心价值是 PBR 光照和 MSAA，RG transient 是基础设施增强，可以与核心功能解耦。外部创建 + import 已经验证可行（阶段二 depth buffer 就是这么做的）。阶段三新增的资源（MSAA buffers、HDR color、normal buffer）都是 resize 时重建、帧间持久的，不是真正的"帧内 transient"。真正受益于 transient 的场景是阶段五屏幕空间效果的中间 buffer。

如果倾向 A，需要进一步讨论 `RGResourceDesc` 的设计和 RG 的资源创建/销毁流程。

#### 决定：方案 A（引入 managed 资源管理）

##### 演进级别

| 级别 | 内容 | 时机 |
|------|------|------|
| L1：RG 管理创建/缓存 | RG 根据声明创建资源，跨帧缓存（desc 不变则复用），resize 时自动重建 | **阶段三实现** |
| L2：L1 + 内存别名 | 生命周期不重叠的资源共享 VkDeviceMemory（VMA `CAN_ALIAS_BIT`） | **阶段八实现**（Bloom 链是最自然的引入点） |
| L3：L2 + 自动生命周期推导 | RG 分析 pass 依赖图推导首次/末次使用，自动决定别名 | **M2 考虑**（非正式决定，标记为届时需评估） |

##### 资源描述符

采用混合尺寸模式，以相对尺寸为主。只有 `sample_count` 和 `mip_levels` 有默认值（1 始终合法），其余字段必须显式指定：

```cpp
enum class RGSizeMode : uint8_t { Relative, Absolute };

struct RGImageDesc {
    RGSizeMode size_mode;
    // Relative mode
    float width_scale;
    float height_scale;
    // Absolute mode
    uint32_t width;
    uint32_t height;
    // Common
    rhi::Format format;
    rhi::ImageUsage usage;
    uint32_t sample_count = 1;
    uint32_t mip_levels = 1;
};
```

- Relative：render target 等屏幕尺寸相关资源（阶段三全部、M2 半分辨率 SSAO/SSR）
- Absolute：Shadow Map（阶段四，固定 2048/4096）等固定尺寸资源
- 不适合 RG 管理的资源（IBL 预计算产物等一次性 init 资源）不经过 RG

##### 缓存机制：持久 Handle（两步注册）

```cpp
// --- RG 新增 API ---
struct RGManagedHandle { uint32_t index; };

RGManagedHandle create_managed_image(const char* debug_name, const RGImageDesc& desc);
RGResourceId    use_managed_image(RGManagedHandle handle);
void            destroy_managed_image(RGManagedHandle handle);
void            set_reference_resolution(VkExtent2D extent);
```

使用流程：

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

imported vs managed 的区别：

| | imported | managed |
|---|---|---|
| 创建者 | 外部 | RG |
| 生命周期 | 外部管理 | RG 缓存，resize 自动重建 |
| initial/final layout | 调用者指定 | RG 推导 |
| 用途 | swapchain image 等外部资源 | depth、MSAA、HDR 等渲染中间产物 |

##### 现有 depth buffer 迁移

阶段三将现有手动管理的 depth buffer 迁移为 managed 资源，删除 `create_depth_buffer()` / `destroy_depth_buffer()`。

##### 闲置资源处理（M1 不实现，记录备忘）

当 pass 长期 disable 时，其 managed 资源的 backing VkImage 持续占用显存。两个相关但解决不同问题的方案：

- **L2 内存别名**：解决的是**帧内**资源生命周期不重叠时的显存复用。闲置资源若有别名伙伴，物理显存自动被活跃资源复用，额外开销为零。但无法处理无别名伙伴的闲置资源。
- **显式休眠/唤醒（sleep/wake）**：解决的是**跨帧**长期闲置资源的显存释放。`sleep_managed_image(handle)` 销毁 backing VkImage，`wake_managed_image(handle)` 重建。使用者完全控制资源生命周期，不做隐式回收。

规划时机：**阶段八**实现 L2 时，同步评估 M1 是否存在无别名伙伴的闲置资源场景，据此决定是否需要实现 sleep/wake。

---

### 2.2 RG 对 MSAA 的感知 [已决定]

MSAA resolve 需要在 RG 中表达。问题是 RG 如何知道一个 image 是多采样的，以及如何表达 resolve 操作。

| 方案 | 说明 |
|------|------|
| A. Resolve 作为独立 RG pass | 每个 resolve（depth/normal/color）注册为独立 pass，声明 MSAA image 为 READ、resolved image 为 WRITE |
| B. Resolve 内建到 RG | RG 在 import 或 declare 时标记 sample count，compile 时自动插入 resolve |
| C. Resolve 集成到 Dynamic Rendering | 使用 `VkRenderingAttachmentInfo::resolveImageView` 在渲染结束时自动 resolve |

**建议倾向**：C（color resolve）+ A（depth/normal resolve）。

Vulkan Dynamic Rendering 原生支持 color resolve attachment（`pResolveAttachments`），零额外 pass 开销。但 depth resolve 需要特殊模式（min/max 而非 average），normal resolve 可能需要重新归一化——这两个用独立 pass（compute shader 或 fragment shader）更可控。

#### 决定：纯 C — 所有 resolve 通过 Dynamic Rendering 原生完成

Vulkan 1.2 核心支持 depth resolve mode（MIN/MAX/SAMPLE_ZERO），不需要 custom shader。Normal resolve（阶段五）用 AVERAGE + 消费方 shader `normalize()` 是标准做法。三种 resolve 均可在 Dynamic Rendering 的 `VkRenderingAttachmentInfo` 中配置，零额外 pass。

**RG 零改动**：Forward pass 声明写入 MSAA target 和 resolved target（共 4 个资源），RG 只看到"谁读谁写"，自动生成正确 barrier。Resolve destination 需要的 layout（`COLOR_ATTACHMENT_OPTIMAL` / `DEPTH_ATTACHMENT_OPTIMAL`）与现有 `Write + ColorAttachment` / `Write + DepthAttachment` 生成的 barrier 完全一致。

```cpp
// Forward pass 资源声明示例
forward_pass_.register_resources(rg, {
    {fr.msaa_color, Write, ColorAttachment},      // MSAA 渲染目标
    {fr.msaa_depth, ReadWrite, DepthAttachment},   // MSAA 深度
    {fr.hdr_color,  Write, ColorAttachment},       // color resolve 目标
    {fr.depth,      Write, DepthAttachment},       // depth resolve 目标
});
// resolve 配置完全在 pass callback 内部通过 VkRenderingAttachmentInfo 设置
```

自定义 resolve（tone-mapped resolve、bilateral normal resolve 等）是高级视觉优化，M1/M2 不涉及。如果未来需要，可在对应 pass 内部替换为 custom shader，不影响 RG 和整体架构。

---

## 三、MSAA 决策

### 3.1 采样数配置 [已决定]

| 方案 | 说明 |
|------|------|
| A. 固定 4x | 最常见的平衡点 |
| B. 可配置（2x/4x/8x） | 运行时切换，需重建所有 MSAA 资源和 pipeline |
| C. 阶段三固定 4x，后续加配置 | 先简单后灵活 |

**建议倾向**：C。4x 是甜蜜点。运行时切换 MSAA 需要重建 pipeline（sample count 烘焙在 pipeline 中，除非 Vulkan 1.4 有相关 dynamic state——需要确认）和所有 MSAA render target，可以但阶段三不必做。

#### 决定：方案 B — 运行时可配置（1x / 2x / 4x / 8x），默认 4x

可选级别包含 1x（无 MSAA），用于调试和性能基准对比。1x 时 forward pass 不配置 resolve，只声明 2 个资源而非 4 个。

切换 MSAA 时需要重建：

| 需重建 | 原因 |
|--------|------|
| 所有 MSAA pipeline | `rasterizationSamples` 烘焙在 pipeline 中 |
| 所有 MSAA managed 资源 | msaa_color、msaa_depth 的 sample_count 变了 |

不需要重建：descriptor layout、resolved target、非 MSAA pipeline。

不使用 `VK_EXT_extended_dynamic_state3`（动态 rasterization samples）。该扩展非 Vulkan 1.4 核心，MSAA 切换是低频操作，重建 pipeline 开销可忽略。

**Managed 资源 desc 更新**：RG 新增 `update_managed_desc(handle, new_desc)` API。RG 内部比较 desc，变化时销毁旧 backing image 并创建新的。Handle 保持不变，Renderer 成员变量无需更新。

**重建触发流程**：与 resize/vsync 统一模式。

```cpp
// Application 检测 DebugUI 的 MSAA 变更
if (actions.msaa_changed) {
    renderer_.handle_msaa_change(actions.new_sample_count);
}

// Renderer 处理 MSAA 变更
void Renderer::handle_msaa_change(uint32_t new_sample_count) {
    vkQueueWaitIdle(...);
    // 1. 更新 MSAA managed 资源 desc
    render_graph_.update_managed_desc(managed_msaa_color_, new_msaa_color_desc);
    render_graph_.update_managed_desc(managed_msaa_depth_, new_msaa_depth_desc);
    // 2. 重建所有 MSAA pipeline
    rebuild_pipelines(new_sample_count);
}
```

### 3.2 MSAA 与 ImGui / Tonemapping [已决定]

阶段二 ImGui 直接渲染到 swapchain image。阶段三引入 MSAA 后：

| 方案 | 说明 |
|------|------|
| A. ImGui 渲染到 resolved 后的 LDR buffer | 正常路径：MSAA forward → resolve → 后处理 → ImGui 叠加到最终 buffer |
| B. ImGui 直接渲染到 swapchain image | 与阶段二一致，RG 最后一个 pass |

**建议倾向**：B，与阶段二保持一致。ImGui 作为 debug overlay 不需要 MSAA，直接画到 swapchain image 上。

**引出的重要子问题**：阶段三引入 HDR color buffer 后，渲染链变为 MSAA HDR → resolve → (future: post-processing) → copy/blit to swapchain → ImGui。阶段三还没有后处理链（阶段八才有），那 HDR color buffer resolve 之后如何到 swapchain？

| 方案 | 说明 |
|------|------|
| A. 临时 blit/copy pass | resolve 后直接 blit 到 swapchain（可能涉及格式转换 HDR→SDR） |
| B. 简易 tonemapping pass | 即使阶段三不做完整后处理，也加一个最简 tonemapping（如 Reinhard 一行代码）把 HDR 映射到 SDR |
| C. 先不用 HDR buffer | 阶段三 forward pass 直接输出到 LDR（swapchain format） |

**建议倾向**：B。PBR 光照输出的亮度范围是 HDR 的（镜面高光轻松超过 1.0），直接截断到 [0,1] 会丢失大量信息，高光全部 clamp 为纯白，效果很差。一个 `color / (color + 1.0)` 的 Reinhard tonemapping 只需几行 compute shader，但能让画面可看。阶段八替换为 ACES。

#### 决定

**ImGui**：方案 B — 渲染到 swapchain image，与阶段二一致。渲染到中间 LDR buffer 无实际优势。

**Tonemapping**：阶段三实现 ACES tonemapping pass（非临时 Reinhard，直接上 ACES），作为 fullscreen fragment shader。不使用 compute shader 是因为 SRGB swapchain format 通常不支持 `VK_IMAGE_USAGE_STORAGE_BIT`。

阶段三完整帧流程：

```
MSAA HDR Color (forward pass 渲染)
    ↓ Dynamic Rendering resolve (AVERAGE)
HDR Color (resolved, R16G16B16A16F)
    ↓ texture() 采样
Tonemapping pass (fullscreen fragment): exposure → ACES → output linear [0,1]
    ↓ 硬件自动 linear → sRGB (swapchain format)
Swapchain Image (B8G8R8A8_SRGB)
    ↓
ImGui pass
    ↓
Present
```

### 3.3 Depth Resolve 模式 [已决定]

SSAO 和 Contact Shadows（阶段五）需要 resolved depth。标准 resolve 对 depth 做平均是错误的。

| 方案 | 说明 |
|------|------|
| A. `VK_RESOLVE_MODE_MIN_BIT` | Vulkan 1.2+ 核心支持 depth resolve mode。Reverse-Z 下 MIN = 最远深度（保守） |
| B. `VK_RESOLVE_MODE_MAX_BIT` | Reverse-Z 下 MAX = 最近深度 |
| C. `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` | 取 sample 0，最简单 |
| D. Custom resolve shader | 完全自定义 |

**建议倾向**：A（MIN）。Reverse-Z 下，near=1 far=0，MIN 取最小值即最远的深度。对 SSAO 来说使用最远深度是保守的（宁可少遮蔽不要错误遮蔽）。这是 Vulkan 1.2 核心功能，无需扩展。

但这个选择需要确认是否在所有 screen space effect 场景下都合适。用 MAX（最近深度）也有道理——Contact Shadows ray march 从最近表面出发更准确。可以阶段五再根据实际效果决定。

#### 决定：方案 B — `VK_RESOLVE_MODE_MAX_BIT`（前景深度）

Reverse-Z 下 MAX = 最大值 = 最近表面（前景）。经分析 M1 和 M2 所有 resolved depth 消费者：

| 效果 | 阶段 | 偏好 | 原因 |
|------|------|------|------|
| SSAO | M1 五 | MAX 更好 | 重建位置对应可见前景表面，range check 自然处理深度不连续。MIN 反而重建"幽灵"背景位置 |
| Contact Shadows | M1 五 | MAX | ray march 起点需要准确的前景表面位置 |
| DOF | M1 八 | MAX | 防止背景模糊渗入前景轮廓 |
| SSR / SSGI | M2 | MAX | ray march 起点需要准确前景表面位置 |
| Camera Motion Blur | M2 | MAX | 防止背景速度渗入前景 |
| God Rays | M2 | MIN | 遮挡检测用最远深度更保守 |

7 个消费者中 6 个偏好 MAX。唯一偏好 MIN 的 God Rays 用 MAX 的瑕疵不可感知（God Rays 是低频柔和效果，radial blur 大量采样稀释边缘像素的深度偏差）。

---

## 四、资源格式决策

### 4.1 HDR Color Buffer 格式 [已决定]

| 格式 | 大小 | Alpha | 精度 | 负值 | 适用 |
|------|------|-------|------|------|------|
| R11G11B10F | 4B/px | 无 | 6-bit mantissa (R/G), 5-bit (B) | 不支持 | 带宽敏感、不需要 alpha 的场景 |
| R16G16B16A16F | 8B/px | 有 | 10-bit mantissa | 支持 | 最通用，后处理链友好 |

**需要考虑的因素**：

- MSAA 下带宽加倍（4x MSAA + R16G16B16A16F = 32B/px）
- 透明 pass（阶段七）是否需要 alpha？→ 透明 pass 使用 blending，alpha 在 blend equation 中，render target alpha 不是必须的
- 后处理链（阶段八）是否需要负值？→ 正常 HDR 渲染不产生负值
- Bloom 降采样链是否对精度敏感？→ 6-bit mantissa 对 Bloom 够用

**建议倾向**：两个可选策略——

- **策略 1（混合格式）**：R11G11B10F 作为 MSAA HDR render target，R16G16B16A16F 作为 resolve 后的后处理链输入。MSAA 下省一半带宽；resolve 后只有一份，精度更好，后处理链更灵活。
- **策略 2（统一格式）**：全程 R16G16B16A16F，不做混合格式。减少复杂度。

#### 决定：全程 R16G16B16A16F

带宽分析（1920×1080, 4x MSAA）：R16G16B16A16F 的 color buffer 带宽约为 R11G11B10F 的 2 倍（~269 MB/帧 vs ~135 MB/帧），但额外的 ~134 MB 在 60fps 下仅占桌面 GPU 显存带宽的 1-3%，不构成瓶颈。

精度分析：R11G11B10F 的 6-bit 尾数在暗部渐变区域可能产生可见 banding（色带），R16G16B16A16F 无此风险。M2 后处理链（SSR、SSGI、DOF 等）多次读写 HDR buffer 会累积量化误差，高精度更可控。

混合格式（MSAA 用 R11G11B10F，resolved 用 R16G16B16A16F）不可行——Dynamic Rendering 的 resolve 要求格式兼容，两者不兼容。

### 4.2 Normal Buffer 格式与编码 [已决定]

| 方案 | 格式 | 大小 | 精度 | 编解码复杂度 |
|------|------|------|------|-------------|
| A. World-space xyz + padding | R16G16B16A16F | 8B/px | 极高 | 无 |
| B. World-space xyz | R10G10B10A2_UNORM | 4B/px | 中等（10-bit per channel） | 需 [-1,1]→[0,1] 映射 |
| C. View-space octahedron | R16G16_SFLOAT | 4B/px | 高 | 需编解码函数 |
| D. World-space octahedron | R16G16_SFLOAT | 4B/px | 高 | 需编解码函数 |
| E. World-space xy + reconstruct z | R16G16_SFLOAT | 4B/px | 高 | z = sqrt(1-x²-y²)，Z 符号丢失 |

**长远考虑**：

- SSAO（阶段五）需要 normal，view-space 更直接
- SSR（M2）需要 normal，world-space 更通用
- SSGI（M2）需要 normal
- 阶段三只是 PrePass 写入 + 未来 pass 消费，格式选择影响后续所有 pass

**建议倾向**：B（R10G10B10A2_UNORM, world-space）或 A（R16G16B16A16F, world-space）。

- 倾向 B 的理由：4 字节/像素在 MSAA 下省一半带宽，10-bit 精度对 normal 足够。World-space 是更通用的选择，view-space 效果可以在 shader 中用 view matrix 转换（一次矩阵乘法）。A2 通道可以存额外信息（如 material ID 的 2 bit 标记）。
- 倾向 A 的理由：最简单，不需要任何编码解码，精度绝对够。如果 MSAA 带宽不是瓶颈就没必要省。

#### 决定：方案 B — R10G10B10A2_UNORM, world-space

精度：10-bit/通道 → 角度分辨率 ~0.1°，对 SSAO/SSR 等所有消费者足够。编解码：1 MAD 指令（`n*0.5+0.5` / `n*2.0-1.0`），开销可忽略。MSAA resolve：编码是线性映射，AVERAGE resolve 正确（平均编码值 = 平均法线分量），解码后 `normalize()` 即可。

排除 C/D（octahedron）的关键原因：八面体映射是非线性的，AVERAGE resolve 产生错误法线，需要 custom resolve pass，与 2.2 决定矛盾。排除 E：world-space Z 符号丢失。

2-bit A 通道可在未来存储 material flag（区分皮肤/金属等），用于屏幕空间效果的特殊处理。

### 4.3 MSAA Depth Buffer 格式 [已决定]

沿用阶段二的 D32Sfloat 不变。需确认：**MSAA depth buffer 是否仍使用 D32Sfloat？**

#### 决定：沿用 D32Sfloat

Reverse-Z 需要浮点深度才能发挥精度分布优势。D24UnormS8 是定点格式且浪费 8-bit stencil（项目无 stencil 需求），带宽与 D32Sfloat 相同（均 4B/px）。D16Unorm 精度不足。

---

## 五、IBL 管线决策

### 5.1 HDR 环境贴图输入格式 [已决定]

| 方案 | 说明 | 优势 | 劣势 |
|------|------|------|------|
| A. Equirectangular .hdr 文件 | stb_image 加载，GPU 转 cubemap | 资源最容易获取（HDRI Haven 等） | 需要实现 equirect→cubemap 转换 |
| B. 预处理好的 cubemap 6 面 | 离线工具处理好，直接加载 6 张图 | 加载最简单 | 需要离线工具，资源不灵活 |
| C. .ktx2 / .dds cubemap | 预压缩格式加载 | 性能最好 | 需要额外库或离线转换 |

**建议倾向**：A。.hdr equirectangular 是最常见的 HDR 环境贴图格式，网上免费资源丰富。Equirect→cubemap 转换是一个 compute shader（or fragment shader rendering to cubemap faces），约 20-30 行 shader 代码，一次性工作。stb_image 已集成，支持 .hdr 格式（`stbi_loadf` 返回 float RGB 数据）。

#### 决定：方案 A

### 5.2 IBL 预计算管线 [已决定]

IBL Split-Sum 需要三个预计算产物：

| 产物 | 说明 | 典型参数 |
|------|------|----------|
| Irradiance Map | 余弦加权半球卷积 | Cubemap, 32×32 per face |
| Prefiltered Env Map | 不同 roughness 级别的模糊环境 | Cubemap, 128-256 per face, mip chain |
| BRDF Integration LUT | 基于 roughness + NdotV 的积分查表 | 2D texture, 256×256 or 512×512, R16G16 |

**问题**：预计算在哪里做？

| 方案 | 说明 |
|------|------|
| A. GPU Compute Shader（加载时计算） | 每次环境贴图变化时 GPU 重新计算 |
| B. 离线预计算 + 文件缓存 | 首次计算后保存到磁盘，后续直接加载 |
| C. GPU 计算 + 运行时缓存（不持久化） | 每次启动重新计算 |

**建议倾向**：C（简单起步）→ B（后续优化）。三个预计算总共几十毫秒到几百毫秒（一次性），不影响运行时性能。GPU compute shader 实现最直接，与现有的 immediate command scope 配合良好。阶段三先做 C，阶段八或 M2 做昼夜循环时如果需要频繁重算再考虑缓存。

**需要进一步讨论**：

- Irradiance/Prefiltered map 的 cubemap 分辨率？
- BRDF LUT 的分辨率和格式？（R16G16_SFLOAT 还是 R16G16_UNORM？值域在 [0,1] 内所以 UNORM 够用）
- 这些预计算资源由谁持有？（独立的 IBL 模块？Renderer？）

#### 决定：方案 C（GPU 计算 + 运行时缓存）

B（磁盘缓存）不做正式规划，标记为"需要时再加"。C→B 是纯优化（加一层磁盘读写），不影响架构。A（GPU 从头计算）是 C 的核心实现，也是 B 的 fallback——永远保留。

M2 的 Bruneton 大气散射使 IBL 变为动态计算（太阳角度变化时重算），磁盘缓存对动态输入无意义。B 真正有价值的时机是多个静态环境快速切换或 Reflection Probes 数量增多时。

预计算在 `Renderer::init()` 中执行，使用现有的 `begin_immediate()` / `end_immediate()`。

##### 预计算产物参数

| 产物 | 分辨率 | 格式 | 内存 | 说明 |
|------|--------|------|------|------|
| Irradiance Map | 32×32 per face | R11G11B10F | ~48 KB | 极其平滑的低频信号，R11G11B10F 远超需求 |
| Prefiltered Env Map | 256×256 per face (含 mip chain) | R16G16B16A16F | ~24 MB | 高精度避免镜面反射 banding（banding 难排查，分辨率不足易排查）。分辨率提取为常量供后续配置 |
| BRDF Integration LUT | 256×256 | R16G16_UNORM | ~256 KB | 值域 [0,1]，UNORM 足够。与环境无关，所有环境共享 |

格式选择理由：Prefiltered map 用 R16G16B16A16F 而非 R11G11B10F 是因为 mip0（低 roughness）近似镜面反射，直接暴露 cubemap 内容，6-bit 尾数在平滑渐变中可能产生 banding。Irradiance map 是余弦积分后的极低频信号，不存在此风险。

### 5.3 IBL 模块归属 [已决定]

IBL 预计算涉及 compute shader、cubemap 处理、LUT 生成——放在哪一层？

| 方案 | 说明 |
|------|------|
| A. Framework 层新模块 `framework/ibl.h` | 与 texture/mesh 同级的通用基础设施 |
| B. App 层 | 与 scene_loader 一起处理环境贴图 |
| C. Passes 层 | 作为独立的"预计算 pass" |

**建议倾向**：A。IBL 预计算是渲染框架级别的基础设施（M2 的 Reflection Probes 复用相同的 cubemap 处理），不是 app 逻辑，也不是每帧运行的 pass。

#### 决定：方案 A — Framework 层 `framework/ibl.h`

---

## 六、Shader 架构决策

### 6.1 Shader 公共文件组织 [已决定]

`m1-interfaces.md` 规划了 `shaders/common/` 下的文件。阶段三需要新增：

| 文件 | 内容 |
|------|------|
| `common/brdf.glsl` | BRDF 函数（D_GGX, G_SmithGGX, F_Schlick, Lambert） |
| `common/lighting.glsl` | 光照计算（evaluate_directional_light, evaluate_ibl） |
| `common/bindings.glsl` | 已有，可能需要扩展 |
| `common/constants.glsl`? | PI, EPSILON 等数学常量 |
| `common/normal.glsl`? | Normal map 解码、TBN 构造 |

**问题**：文件拆分粒度？

| 方案 | 说明 |
|------|------|
| A. 细粒度（如上表） | 每个功能独立文件 |
| B. 粗粒度 | brdf.glsl 包含所有 BRDF + 光照 + 常量 |
| C. 按依赖拆分 | constants/math.glsl → brdf.glsl → lighting.glsl（依赖链） |

**建议倾向**：C。清晰的依赖链让 include 顺序自明。`constants.glsl`（数学常量）→ `brdf.glsl`（纯 BRDF 函数，不依赖场景数据）→ `lighting.glsl`（组合 BRDF 函数 + 光源数据计算最终光照，依赖 bindings.glsl 的光源结构体）。

#### 决定：方案 C — 按依赖链拆分 + 独立 `normal.glsl`

```
common/constants.glsl   ← PI, EPSILON 等纯数学常量（无依赖）
    ↓
common/brdf.glsl        ← D_GGX, G_SmithGGX, F_Schlick, Lambert（纯函数，不依赖场景数据）
    ↓
common/lighting.glsl    ← evaluate_directional_light, evaluate_ibl（组合 BRDF + 光源结构体）
                           内部 #include constants.glsl + brdf.glsl

common/bindings.glsl    ← Set 0/Set 1/push constant 布局定义（已有，独立于上面的链）
common/normal.glsl      ← TBN 构造、normal map 解码（独立文件，depth_prepass 和 forward 共用）
```

消费方式：

```glsl
// forward.frag
#include "common/bindings.glsl"
#include "common/normal.glsl"
#include "common/lighting.glsl"  // 内部已 include constants + brdf
```

`normal.glsl` 独立于依赖链——depth_prepass.frag（阶段五输出法线时）和 forward.frag 都需要 TBN 构造，但 depth_prepass 不需要 brdf/lighting。

---

### 6.2 Shader 变体策略 [已决定]

阶段三目前只有标准 PBR 一种材质，但已经存在变体需求：

- 有/无法线贴图
- Alpha Mask vs Opaque（Depth PrePass 中 Mask 物体需要 discard）
- 未来：有/无 AO 贴图、有/无 emissive

| 方案 | 说明 |
|------|------|
| A. `#ifdef` 编译宏 | `#ifdef HAS_NORMAL_MAP` 等，ShaderCompiler 传入 defines |
| B. 动态分支（if/else） | `if (material.normal_tex != INVALID_INDEX)` 运行时判断 |
| C. Specialization Constants | Vulkan 的 specialization constant 在 pipeline 创建时特化 |

**建议倾向**：B（阶段三），A 或 C（后续 pass 增多后）。阶段三变体少，动态分支在现代 GPU 上几乎零额外开销（所有 fragment 走同一分支时 warp 不发散）。而且 bindless 架构下 default 纹理已经处理了"缺失纹理"的情况——normal map 缺失时 `normal_tex` 指向 flat normal，采样结果是 (0.5, 0.5, 1.0)，TBN 变换后就是原始法线，数学上正确。

这意味着阶段三的 forward shader 可以**无条件执行所有纹理采样和计算**，不需要变体。这是 bindless + default 纹理的设计红利。

#### 决定：方案 B — 无条件采样 + 全阶段变体演进路线

##### 阶段三：无变体，无条件采样一切

forward.frag 消费 `GPUMaterialData` 的**全部 5 个纹理字段**：

| 纹理 | default 值 | 无条件采样的数学效果 |
|------|-----------|-------------------|
| `base_color_tex` | white (1,1,1,1) | factor × 1.0 = factor 本身 |
| `normal_tex` | flat normal (0.5,0.5,1.0) | TBN 变换后 = 原始顶点法线 |
| `metallic_roughness_tex` | white (1,1,1,1) | factor × 1.0 = factor 本身 |
| `occlusion_tex` | white (1,1,1,1) | strength × 1.0 = 无遮蔽 |
| `emissive_tex` | black (0,0,0,1) | factor × 0.0 = 无自发光 |

- `occlusion_tex × occlusion_strength` 调制 IBL/ambient 光照（阶段三引入 IBL 时同步消费）
- `emissive_tex × emissive_factor` 加到最终颜色（一行代码）

Depth PrePass 的 opaque vs mask 不是 shader 变体，是两个独立 pipeline + 独立 shader（见 7.2）。

##### 阶段六：动态分支

Lightmap 引入语义上不能用 default texture 解决的差异：

```glsl
if (instance.lightmap_index != INVALID_INDEX) {
    indirect = texture(textures[instance.lightmap_index], uv1).rgb;
} else {
    indirect = evaluate_ibl(...);
}
```

Lightmap 是 per-object 属性，同一 draw call 内所有 fragment 走同一分支（warp 不发散），GPU 开销为零。

##### M2+：可考虑编译变体 `#define`

POM（Parallax Occlusion Mapping）是第一个值得编译变体的场景——内部 ray march 循环（8-32 次迭代）显著增加 shader 体积和寄存器压力，没有 POM 的物体不应为此付出代价。

| 方案 | 说明 | 适用 |
|------|------|------|
| `#define` + ShaderCompiler | 编译时传入 defines，按 (path + stage + defines hash) 缓存 SPIR-V | 大结构差异（POM 循环、着色模型） |
| Specialization Constants | 一份 SPIR-V，pipeline 创建时特化 | 小开关（后续如需从 `#define` 迁移以减少编译次数） |

ShaderCompiler 已原生支持 `shaderc::CompileOptions::AddMacroDefinition`，无需额外基础设施。具体策略在 M2 规划时正式拍板。

---

### 6.3 IBL Cubemap 的 Descriptor Binding [已决定]

当前 Set 0 layout：

- Binding 0: GlobalUBO
- Binding 1: LightBuffer (SSBO)
- Binding 2: MaterialBuffer (SSBO)

IBL 的 irradiance map、prefiltered env map、BRDF LUT 放在哪里？

| 方案 | 说明 |
|------|------|
| A. Set 0 新增 binding（3, 4, 5） | 与全局数据放一起 |
| B. 通过 Bindless Set 1 | 注册到 bindless 数组，GlobalUBO 存 bindless index |
| C. 新的 Set 2 | IBL 专用 descriptor set |

**关于方案 B 的问题**：bindless 数组目前是 `sampler2D[]`（2D 纹理）。Cubemap 是 `samplerCube`，不能放进 `sampler2D[]` 数组。

因此 cubemap 需要独立 binding。BRDF LUT 是 2D 纹理，可以走 bindless。

**Cubemap binding 的子方案**：

| 方案 | 说明 |
|------|------|
| A. Set 0 新增 binding 放 cubemap samplers | `layout(set=0, binding=3) uniform samplerCube irradiance_map;` etc. |
| B. 单独的 bindless cubemap 数组 | Set 1 加一个 `samplerCube[] cubemaps;` binding |
| C. Set 1 第二个 binding | `binding=0` 是 sampler2D[]，`binding=1` 是 samplerCube[] |

**建议倾向**：A（阶段三简单做法）。阶段三只有 2-3 个 cubemap（irradiance + prefiltered），用独立 binding 最直接。M2 阶段六 Reflection Probes 需要多个 cubemap 时再考虑 bindless cubemap 数组。

这需要修改 DescriptorManager 的 Set 0 layout，增加 cubemap sampler binding。

#### 决定：子方案 B/C — Set 1 新增 binding 1 bindless cubemap 数组

一步到位做 bindless cubemap 数组，M1 阶段六 Reflection Probes 直接复用。

##### Set 1 新布局

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[];     // 2D 纹理，上限 4096
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];   // Cubemap，上限 256
```

##### VARIABLE_DESCRIPTOR_COUNT 处理

Vulkan 规定 `VARIABLE_DESCRIPTOR_COUNT` 只能设在 set 内最后一个 binding 上。两个 binding 后，binding 0 不能再用此 flag。

| 方案 | 说明 |
|------|------|
| A. 两个 binding 都用固定上限 + PARTIALLY_BOUND | 去掉 VARIABLE_DESCRIPTOR_COUNT，driver 按上限分配 |
| B. binding 0 固定上限，binding 1 用 VARIABLE_DESCRIPTOR_COUNT | 只在最后一个 binding 保留 |

**决定**：方案 A。4096 + 256 个 descriptor 的内存开销 ~200-300 KB，不值得为此保留 VARIABLE_DESCRIPTOR_COUNT 的复杂性。两个 binding 统一使用 `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`。

##### Descriptor Pool 调整

Set 1 的 UPDATE_AFTER_BIND pool 容量从 4096 扩展到 4096 + 256 = 4352 个 COMBINED_IMAGE_SAMPLER，maxSets 仍为 1。

##### DescriptorManager API

```cpp
// 新增（与现有 register_texture / unregister_texture 对称）
BindlessIndex register_cubemap(ImageHandle cubemap, SamplerHandle sampler);
void unregister_cubemap(BindlessIndex index);
```

Cubemap 的 slot 管理与 2D texture 独立（各自的 free list + 各自的 slot 空间）。`BindlessIndex` 的 `index` 值分别对应各自数组的下标。

##### BRDF LUT

2D 纹理，注册到 `textures[]`（binding 0），GlobalUBO 存 `uint brdf_lut_index`。不走 cubemap 数组。

##### GlobalUBO IBL 字段

```cpp
// GlobalUniformData 新增
uint32_t irradiance_cubemap_index;    // cubemaps[] 下标
uint32_t prefiltered_cubemap_index;   // cubemaps[] 下标
uint32_t brdf_lut_index;             // textures[] 下标
uint32_t prefiltered_mip_count;      // roughness → mip level 映射
```

Shader 端通过 `cubemaps[global.irradiance_cubemap_index]` 和 `textures[global.brdf_lut_index]` 访问。

---

## 七、Depth + Normal PrePass 设计

### 7.1 PrePass 的范围

| 方案 | 说明 |
|------|------|
| A. 仅 Depth PrePass | 只输出深度，不输出法线 |
| B. Depth + Normal PrePass | 同时输出深度和法线 |
| C. 阶段三做 A，阶段五（需要 SSAO）时升级为 B | 延迟法线输出 |

**建议倾向**：C。阶段三本身不消费 normal buffer（没有 SSAO/SSR），输出 normal 是为阶段五准备。

- 选 B 的好处：阶段三 PrePass 的 pipeline 和 shader 一步到位，阶段五直接复用。
- 选 C 的好处：阶段三更聚焦，不引入未被消费的资源。

### 7.2 Alpha Mask 物体在 PrePass 中的处理

glTF 有 Alpha Mask 材质（`alphaMode: "MASK"`），需要在 fragment shader 中做 alpha test（discard）。

| 方案 | 说明 |
|------|------|
| A. PrePass 跳过 Alpha Mask 物体 | Mask 物体只在 forward pass 中画 |
| B. PrePass 两个 sub-pass | 先画 Opaque（无 fragment shader），再画 Mask（有 fragment shader 做 discard） |
| C. PrePass 统一用带 discard 的 shader | 所有物体都走 fragment shader |

**建议倾向**：B。Opaque 物体的 PrePass 可以用 null fragment shader（只写深度，fragment shader 空或不绑定），性能最好。Mask 物体需要采样 base color texture 判断 alpha，必须有 fragment shader。两者用不同 pipeline，按 alpha mode 分批绘制。

---

## 八、Forward Pass 升级

### 8.1 从阶段二 Lambert 到阶段三 Cook-Torrance

阶段二已有 `forward.vert` + `forward.frag`（Lambert 光照）。阶段三升级为完整 PBR。

**建议**：原地升级。shader 结构不变（输入相同），只是光照计算从 Lambert 扩展为 Cook-Torrance + IBL。`#include "common/brdf.glsl"` + `#include "common/lighting.glsl"` 引入新函数。

### 8.2 Forward Pass 是否读 PrePass 深度做 Early-Z 优化？

标准做法：PrePass 写入深度 → Forward Pass 设置 depth compare EQUAL + depth write OFF → 只着色通过深度测试的 fragment，避免 overdraw。

| 方案 | 说明 |
|------|------|
| A. Forward Pass depth compare EQUAL, write OFF | 经典 Z-PrePass 配合 |
| B. Forward Pass depth compare GREATER (reverse-Z), write ON | 与阶段二一致，PrePass 深度仍提供 Early-Z 优化但不改 compare mode |

**建议倾向**：A。GREATER + write ON 意味着 forward pass 仍在写深度，虽然 GPU 的 Early-Z 硬件能在大部分情况下跳过被遮挡 fragment，但无法保证。EQUAL + write OFF 是确定性的 zero-overdraw。

**注意事项**：MSAA 下 EQUAL 测试可能因浮点精度问题导致闪烁——可能需要 GREATER_OR_EQUAL。PrePass 和 Forward Pass 用相同的顶点 shader、相同的变换矩阵，产生的 depth 值应该是 bit-identical 的（只要 invariant 属性正确声明）。需要在 vertex shader 中用 `invariant gl_Position;` 保证这一点。

---

## 九、长远规划反思

### 9.1 GlobalUBO 膨胀趋势

当前 GlobalUBO 304 bytes。阶段三需要新增：

| 字段 | 用途 |
|------|------|
| IBL cubemap bindless indices（3× uint） | irradiance/prefiltered/brdf_lut 索引 |
| IBL prefiltered mip count（uint） | roughness → mip level 映射 |
| 可能的 IBL intensity（float） | IBL 强度调节 |

后续阶段（阴影、后处理等）还会继续增长。

**建议**：暂不拆分。UBO 最小保证大小是 16KB（Vulkan spec），304 bytes 加上阶段三的增长仍然远在限制之下。即使 M1 全部完成也不太可能超过 1KB。拆分 UBO 徒增复杂度。

### 9.2 Descriptor Set Layout 稳定性

阶段三如果在 Set 0 新增 cubemap binding（问题 6.3），会改变 Set 0 layout。这意味着所有 pipeline 都需要用新 layout 重建。

**问题**：是否现在就在 Set 0 layout 中预留后续阶段需要的 binding slot？

| 方案 | 说明 |
|------|------|
| A. 按需添加 | 每个阶段需要新 binding 时修改 layout |
| B. 一次性预留 M1 全部 binding | 现在就把 shadow map sampler、AO texture sampler、IBL cubemap 等全部预分配 |
| C. 部分预留 | 只预留阶段三到四需要的 |

**建议倾向**：A。Vulkan spec 明确允许 pipeline layout 中有 shader 未使用的 binding，但预留空 binding 增加认知负担且容易出错。按需添加更清晰，pipeline 重建只在阶段切换时发生一次（不是运行时开销）。

### 9.3 帧流程与阶段三的 Pass 子集

`m1-frame-flow.md` 定义了完整 M1 帧流程（PrePass → SSAO → Shadow → Forward → Transparent → PostProcess），阶段三只实现其中子集：

```
阶段三实际帧流程：
PrePass (depth [+ normal]) → Forward (PBR + IBL) → [临时 tonemapping] → ImGui → Present
```

如果选择了 1.2(e) 的方案 B（不引入虚基类），那么 Renderer 的 `render()` 方法就是显式调用存在的 pass。不存在的 pass 就是还没有对应的代码，没有优雅性问题。引入虚基类后，disabled pass 通过 `enabled() = false` 跳过。

### 9.4 阶段二 forward.frag 的光照模型保留

阶段二的 Lambert 光照在阶段三被 Cook-Torrance 取代。

**建议**：不保留 Lambert 切换。Lambert 只是过渡用的简化模型，PBR 上线后没有调试价值。如果需要诊断 PBR 问题，debug UI 可以显示各分量（diffuse only、specular only、IBL only 等），比回退到 Lambert 更有用。

### 9.5 阶段三引入的 Compute Shader 基础设施

IBL 预计算需要 compute shader（cubemap 卷积、LUT 生成）。阶段二没有用过 compute shader。

**需要确认**：

- `Pipeline` 类是否已支持 compute pipeline 创建？（阶段一只实现了 graphics pipeline）
- `CommandBuffer` 是否已有 `dispatch()` 方法？
- 如果没有，这是阶段三的前置基础设施工作

---

## 十、Step 划分思路

基于以上讨论，阶段三可能的 Step 划分（待决策确认后精化）：

| Step | 内容 | 产出 |
|------|------|------|
| 1 | Renderer 提取（如果选方案 A） | 代码重构，功能不变 |
| 2 | Compute shader 基础设施 + BRDF LUT 生成 | compute pipeline 可用 |
| 3 | HDR Color Buffer + MSAA 资源 + Depth PrePass | 深度 prepass + MSAA 生效（画面暂时看起来一样） |
| 4 | IBL 管线（equirect→cubemap + irradiance + prefiltered） | 环境贴图加载和预计算 |
| 5 | Forward shader 升级为 Cook-Torrance + IBL 接入 | PBR 光照 + 环境光 |
| 6 | MSAA Resolve + 临时 Tonemapping + Debug UI | 完整画面可看 |
