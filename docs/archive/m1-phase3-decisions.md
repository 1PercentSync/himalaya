# M1 阶段三设计决策：PBR 光照基础

> 阶段三：Forward Lighting（PBR）、方向光、IBL、Depth+Normal PrePass、MSAA、Tonemapping、App 层重构。
> 核心决策摘要见 `../milestone-1/m1-design-decisions-core.md`。

---

## 决策总览

| 组件 | 实现级别 | 核心理由 |
|------|----------|----------|
| Pass 抽象 | M1 全程具体类（阶段二 lambda → 阶段三具体类），format 硬编码（Swapchain 例外），各 pass 方法集允许不统一，`create_pipelines()` 预留热重载。M2 评估虚基类 | 5-8 个 pass 的显式调用仍可读，方法集差异大统一接口净增复杂度 |
| Skybox | 独立 RG pass（resolved 1x，GREATER_OR_EQUAL depth test），独立 skybox.vert（VS 方向计算 + depth=0.0），Step 6 引入 | M2 Bruneton 替换 |
| Descriptor Set 绑定 | Pass 通过 `dm_` getter 获取 set，FrameContext 保持纯每帧数据 | 避免 Vulkan 类型侵入层间合同 |
| Exposure 控制 | DebugUI 手动 EV 滑条（-4 到 +4），`pow(2, ev)` exposure 乘数 | PBR + IBL 输出亮度范围大，固定值不适用 |
| App 层 | Application + Renderer 分离，模块按需设计接口 | Composition Root |
| MSAA | 运行时可配置 1x/2x/4x/8x，默认 4x | 调试灵活，切换重建 pipeline + managed 资源 |
| Tonemapping | ACES fullscreen fragment shader | SRGB swapchain 不支持 compute，后续可替换 PBR Neutral |
| HDR Color Buffer | 全程 R16G16B16A16F | 精度避免 banding，M2 后处理链友好 |
| Normal Buffer | R10G10B10A2_UNORM, world-space | MSAA AVERAGE resolve 兼容，10-bit 精度足够 |
| IBL 管线 | GPU 运行时预计算，Framework 层，equirect .hdr 输入，IBL 自管理资源。Skybox mip 剥离 + GPU BC6H 压缩 | 一次性开销，BC6H 运行时 ~26 MB |
| IBL 环境光验证 | Step 6.5 独立验证 IBL 数据 | 隔离 IBL 数据问题和 BRDF 数学问题 |
| IBL 旋转 | GlobalUBO ibl_rotation_sin/cos | 调试 + 艺术控制 |
| Default light 退役 | Step 6.5 退役 default light，IBL 取代 | 避免双重计算太阳贡献 |

---

## Render Graph 扩展

### Managed 资源管理

阶段三引入 RG managed 资源——由 RG 根据声明创建和管理的 image，跨帧缓存，resize 时自动重建。与 imported 资源的区别：

| | imported | managed |
|---|---|---|
| 创建者 | 外部 | RG |
| 生命周期 | 外部管理 | RG 缓存，resize 自动重建 |
| initial/final layout | 调用者指定 | RG 推导 |
| 用途 | swapchain image 等外部资源 | depth、MSAA、HDR 等渲染中间产物 |

候选方案：

| 方案 | 说明 |
|------|------|
| A. 引入 managed 资源管理 | RG 新增 `create_managed_image()` 等 API，根据描述创建资源，跨帧缓存，resize 自动重建 |
| B. 延迟到阶段四或五 | 阶段三仍然外部创建 + import，和阶段二一样 |
| C. 引入简化版 | RG 只做"声明式创建"，不做内存别名 |

**选择 A。** 阶段三新增大量分辨率相关资源（MSAA buffers、HDR color、normal buffer），手动管理创建/销毁/resize 重建代码量大且重复。RG managed 资源集中管理这些逻辑，降低 Renderer 的复杂度。

**演进级别：**

| 级别 | 内容 | 时机 |
|------|------|------|
| L1：RG 管理创建/缓存 | RG 根据声明创建资源，跨帧缓存（desc 不变则复用），resize 时自动重建 | **阶段三** |
| L2：L1 + 内存别名 | 生命周期不重叠的资源共享 VkDeviceMemory（VMA `CAN_ALIAS_BIT`） | **阶段八**（Bloom 链是最自然的引入点） |
| L3：L2 + 自动生命周期推导 | RG 分析 pass 依赖图推导首次/末次使用，自动决定别名 | **M2 考虑** |

**两步注册模式：** `create_managed_image()` 在初始化时注册持久 handle；`use_managed_image()` 每帧调用，返回当前帧的 `RGResourceId`。初始化时注册、每帧使用、销毁时注销——handle 跨帧稳定，RGResourceId 每帧重建。

**Slot 状态判断：** Managed image slot 通过 `backing.valid()`（`ImageHandle::index != UINT32_MAX`）判断是否活跃，不引入额外 `active` 标志。`destroy_managed_image()` 重置 `backing = {}`（无效），`create_managed_image()` 赋予有效 backing handle。与 RHI 层句柄设计一致——`valid()` 是单次 `uint32_t` 比较，零开销，语义已充分表达 slot 状态。

**Initial/final layout 推导：** Managed 资源每帧统一以 `VK_IMAGE_LAYOUT_UNDEFINED` 作为 initial layout，帧末不插入 final layout transition。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 帧间追踪上一帧 final layout | RG 记录每帧 execute 结束时的 layout，下一帧用作 initial |
| B. 每帧 UNDEFINED | 一律 UNDEFINED，不追踪帧间状态 |
| C. 帧末统一转到固定 rest layout | 每帧结束后转到预定义 layout |

**选择 B。** 阶段三的所有 managed 资源（depth、MSAA color、HDR color、normal）都是帧内中间产物，每帧被 `loadOp = CLEAR` 完全重写。`UNDEFINED` 的 Vulkan 语义是"不关心旧内容"，让 driver 跳过旧数据的解压/保留，是 Vulkan 对每帧重写的 render target 的推荐做法。

性能：`UNDEFINED → 目标 layout` + `loadOp = CLEAR` 被 driver 合并处理为 no-op 或几个 cycle 的元数据标记操作（GPU 的压缩元数据如 HTILE/HiZ 在 CLEAR 时重新初始化）。每帧每个 managed 资源多一次 barrier，但 GPU 和带宽开销均为零。方案 A 在后续帧省掉的只是这个 no-op barrier，实际节省接近零。

实现：`use_managed_image(handle, final_layout)` initial layout 统一 UNDEFINED。`final_layout` 由调用方显式指定——非 temporal 传 `UNDEFINED`（不插入帧末 barrier，等效于阶段三的行为），temporal current 传 `SHADER_READ_ONLY_OPTIMAL`（帧末 transition，确保 swap 后 history layout 正确）。

**Temporal 资源兼容性：** 阶段五引入的 temporal 资源（需要保留上帧内容）通过 `use_managed_image` 的 `final_layout` 参数控制帧末 transition，`get_history_image()` 返回独立 `RGResourceId` 读取上帧内容。RG 内部不区分 temporal 和非 temporal——统一根据 `final_layout` 参数决定是否插入帧末 barrier。

**现有 depth buffer 迁移：** 阶段三将阶段二手动管理的 depth buffer 迁移为 managed 资源，删除手动创建/销毁代码。

接口定义详见 `../milestone-1/m1-interfaces.md`「Managed 资源 API」。

### Backing Image 即时查询

**选择：RG 暴露 `get_managed_backing_image()` API**

Resize / MSAA 切换时，managed 资源被 RG 内部重建（销毁旧 backing image，创建新的）。Renderer 需要获取新的 backing `ImageHandle` 来更新 Set 2 descriptor。

| 方案 | 说明 |
|------|------|
| A. 帧循环中通过 `use_managed_image()` → `get_image()` 获取 | 下一帧自然拿到新 handle |
| B. RG 新增 `get_managed_backing_image(RGManagedHandle)` 方法 | resize handler 中立即获取 |

**选择 B。** 方案 A 导致 resize 到下一帧之间存在一帧的 descriptor 失效窗口——Set 2 仍指向已销毁 image 的 view。方案 B 在 resize handler 中立即获取新 handle 并更新 Set 2，流程无中间错误状态。实现简单——managed 资源内部已持有 backing `ImageHandle`，只需暴露一个 getter。

**Set 2 更新流程（resize / MSAA 切换）：**

```
vkQueueWaitIdle()
    ↓
RG 重建 managed 资源（set_reference_resolution / update_managed_desc）
    ↓
Renderer 通过 get_managed_backing_image() 获取新 ImageHandle
    ↓
DescriptorManager 更新 Set 2 对应 binding（vkUpdateDescriptorSets）
    ↓
继续渲染
```

**闲置资源处理（M1 不实现，备忘）：** Pass 长期 disable 时，其 managed 资源的 backing VkImage 持续占用显存。L2 内存别名解决帧内生命周期不重叠时的复用；跨帧长期闲置可通过显式 `sleep_managed_image(handle)` / `wake_managed_image(handle)` 释放/重建 backing VkImage——使用者完全控制资源生命周期，不做隐式回收。阶段八实现 L2 时同步评估是否需要 sleep/wake。

### MSAA Resolve 策略

MSAA resolve 需要在 RG 中表达。

| 方案 | 说明 |
|------|------|
| A. Resolve 作为独立 RG pass | 每个 resolve 注册为独立 pass |
| B. Resolve 内建到 RG | RG 标记 sample count，compile 时自动插入 resolve |
| C. Resolve 集成到 Dynamic Rendering | 使用 `VkRenderingAttachmentInfo::resolveImageView` 在渲染结束时自动 resolve |

**选择纯 C。** 最初倾向 C（color resolve）+ A（depth/normal resolve）混合方案——depth resolve 需要 MIN/MAX 而非 AVERAGE，normal resolve 可能需要重新归一化，用独立 pass 更可控。但 Vulkan 1.2 核心支持 depth resolve mode（MIN/MAX/SAMPLE_ZERO），Normal resolve 用 AVERAGE + 消费方 shader `normalize()` 是标准做法，因此所有 resolve 均可通过 Dynamic Rendering 原生完成，zero 额外 pass 开销。

**RG 零改动：** Pass 声明写入 MSAA target 和 resolved target，RG 只看到"谁读谁写"，自动生成正确 barrier。Resolve destination 需要的 layout 与现有 `Write + ColorAttachment` / `Write + DepthAttachment` 生成的 barrier 完全一致。Resolve 配置完全在 pass callback 内部通过 `VkRenderingAttachmentInfo` 设置。

**Resolve 产生者分配（Step 5 引入 PrePass 后）：**

| Resolve 产物 | 产生者 | 机制 | 原因 |
|-------------|--------|------|------|
| Resolved depth | PrePass | Dynamic Rendering depth resolve MAX_BIT | PrePass 写入 MSAA depth，Forward Pass depth write OFF，MSAA depth 内容不变 |
| Resolved normal | PrePass | Dynamic Rendering normal resolve AVERAGE | normal 只在 PrePass 产生 |
| Resolved hdr_color | Forward Pass | Dynamic Rendering color resolve AVERAGE | Forward Pass 写入 MSAA color |

Forward Pass depth write OFF 后，如果仍配 depth resolve 会冗余执行一次 resolve（4x MSAA D32Sfloat 1080p ≈ 32MB 读 + 8MB 写），因此 Forward Pass 不配 depth resolve。

```cpp
// PrePass 资源声明（Step 5+）
depth_prepass_.record(rg, {
    {ctx.msaa_depth,  ReadWrite, DepthAttachment},  // depth test + write
    {ctx.msaa_normal, Write, ColorAttachment},       // normal output
    {ctx.depth,       Write, DepthAttachment},       // depth resolve 目标
    {ctx.normal,      Write, ColorAttachment},       // normal resolve 目标
});

// Forward pass 资源声明（Step 5+）
forward_pass_.record(rg, {
    {ctx.msaa_color, Write, ColorAttachment},       // MSAA 渲染目标
    {ctx.msaa_depth, Read, DepthAttachment},        // EQUAL test only, write OFF
    {ctx.hdr_color,  Write, ColorAttachment},       // color resolve 目标
});
```

**已知改造点（阶段七）：** 阶段七透明 Pass 也写入 MSAA color buffer，color resolve 必须在透明 pass 之后才能做。届时 Forward Pass 的 Dynamic Rendering color resolve 移除，改为在透明 pass 结束时配置 resolve。改造成本低——只是把 `resolveImageView` 配置从 Forward Pass 的 attachment info 移到透明 Pass 的 attachment info。

自定义 resolve（tone-mapped resolve、bilateral normal resolve 等）是高级视觉优化，M1/M2 不涉及。如需，可在对应 pass 内部替换为 custom shader，不影响 RG 和整体架构。

### Depth Resolve 模式

MSAA depth resolve 用于生成屏幕空间效果所需的 resolved depth buffer。

| 方案 | 说明 |
|------|------|
| A. `VK_RESOLVE_MODE_MIN_BIT` | Reverse-Z 下 MIN = 最远深度（保守遮蔽） |
| B. `VK_RESOLVE_MODE_MAX_BIT` | Reverse-Z 下 MAX = 最近深度（前景优先） |
| C. `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT` | 取 sample 0 |
| D. Custom resolve shader | 完全自定义 |

**选择 B（MAX_BIT — 前景深度）。** 最初倾向 A（MIN）——Reverse-Z 下 MIN = 最远深度，对 SSAO 来说是保守选择（宁可少遮蔽不要错误遮蔽）。但需要确认是否在所有 screen space effect 场景下都合适（如 Contact Shadows ray march 从最近表面出发更准确）。经分析 M1 和 M2 所有 resolved depth 消费者后翻转为 MAX：

| 效果 | 阶段 | 偏好 | 原因 |
|------|------|------|------|
| SSAO | M1 五 | MAX | 重建位置对应可见前景表面，range check 自然处理深度不连续。MIN 反而重建"幽灵"背景位置 |
| Contact Shadows | M1 五 | MAX | ray march 起点需准确前景表面位置 |
| DOF | M1 八 | MAX | 防止背景模糊渗入前景轮廓 |
| SSR / SSGI | M2 | MAX | ray march 起点需准确前景表面位置 |
| Camera Motion Blur | M2 | MAX | 防止背景速度渗入前景 |
| God Rays | M2 | MIN | 遮挡检测用最远深度更保守 |

7 个消费者中 6 个偏好 MAX。唯一偏好 MIN 的 God Rays 用 MAX 的瑕疵不可感知（低频柔和效果，radial blur 大量采样稀释边缘像素的深度偏差）。

### Compute Pass 资源用途推导

Step 3 新增 compute pass 支持后，`resolve_usage()` 补充 Compute case 的映射：

| Access × Stage | Layout | Access Flags | 用途 |
|---------------|--------|-------------|------|
| Read + Compute | `SHADER_READ_ONLY_OPTIMAL` | `SHADER_SAMPLED_READ_BIT` | sampled image |
| Write + Compute | `GENERAL` | `SHADER_STORAGE_WRITE_BIT` | storage image |

覆盖 M1 全部 compute pass 需求（IBL 预计算的 sampled input + storage output）。不实现 ReadWrite Compute（`imageLoad` 只读 storage）——M1 无此需求，后续阶段按需加 case。

---

## Pass 类设计

阶段二只有一两个 pass，直接在 Render Graph 的 lambda 回调中编写渲染逻辑。阶段三 pass 增多后（DepthPrePass、ForwardPass、TonemappingPass），从 lambda 提取为独立的**具体类**。

### 层归属

**所有 pass 统一放在 Layer 2（`passes/`）**，包括有 draw loop 的 pass（ForwardPass、DepthPrePass）和无 draw loop 的 pass（TonemappingPass）。

有 draw loop 的 pass 需要场景数据（meshes、materials、cull_result），这些类型全部定义在 Layer 1（`framework/mesh.h`、`framework/material_system.h`、`framework/scene_data.h`），依赖方向 Layer 2 → Layer 1 合法。数据实例从 Layer 3（Renderer）通过 `FrameContext` 向下传递，不产生反向依赖。

### FrameContext

`FrameResources`（阶段二设计，只含 RG 资源 ID）扩展为 `FrameContext`，携带一帧渲染所需的全部上下文：RG 资源 ID + 场景数据引用 + 帧参数。定义在 Layer 1（`framework/frame_context.h`），Renderer 每帧构造并向下传递给所有 pass。

**服务引用不放入 FrameContext**：长期不变的服务引用（ResourceManager、DescriptorManager、ShaderCompiler 等）由 Pass 在 `setup()` 时存储指针，lambda 通过 `this` 访问。FrameContext 是纯每帧数据（资源 ID + 场景数据引用 + 帧参数），不做 service locator，避免混合两种职责导致退化为 God Object。

### Descriptor Set 绑定方式

Pass 在 `record()` 的 execute lambda 中需要绑定 descriptor set（Set 0/1/2），需要确定 `VkDescriptorSet` 的获取方式。

| 方案 | 说明 |
|------|------|
| A. FrameContext 携带 `VkDescriptorSet` | 每帧构造时填入当前帧的 set |
| B. Pass 通过 `dm_` getter 获取 | Pass 在 `setup()` 时存储 `DescriptorManager*`，`record()` 中调用 getter |
| C. `DescriptorManager::bind_global_sets()` | DescriptorManager 提供绑定方法 |

**选择 B。** Pass 在 `setup()` 时存储 `DescriptorManager*`，`record()` 的 execute lambda 中调用 `dm_->get_set0(frame_index)` / `dm_->get_set1()` / `dm_->get_set2()` 获取需要的 descriptor set。各 pass 显式选择需要的 set 组合，语义精确。

排除 A：打破 FrameContext 纯净性——引入 `VkDescriptorSet`（Vulkan 类型）到层间合同（FrameContext 定义在 framework 层），开门效应使 FrameContext 逐渐退化为 Vulkan 数据搬运工。

排除 C：`DescriptorManager::bind_global_sets(cmd, frame_index)` 让 DescriptorManager 越界操作 CommandBuffer 录制——DescriptorManager 的职责是管理 descriptor 分配和更新，不是录制命令。

**演进**：如果将来 boilerplate 痛苦（每个 pass 都写 `cmd.bindDescriptorSets(...)` 三行），可引入自由函数 helper（如 `bind_global_sets(cmd, dm, frame_index)`）而非让 DescriptorManager 越界。

### 具体类 vs 虚基类

| 方案 | 说明 |
|------|------|
| A. 虚基类 `RenderPass` | 标准多态，Renderer 持有 `vector<unique_ptr<RenderPass>>` |
| B. 具体类，不用虚函数 | 每个 Pass 是独立类，Renderer 持有具体类型成员 |
| C. 继续 lambda | 不提取类 |

**选择 B。** 阶段三只有 2-3 个 pass，虚函数多态收益有限。具体类让类型更明确、编译期可见。阶段四 pass 数量增长后（shadow、SSAO 等）再引入多态基类——届时有更多样本验证接口设计。

### 方法职责分离

| 方法 | 职责 | 调用时机 |
|------|------|----------|
| `setup()` | Pipeline 创建等不依赖分辨率的一次性工作，存储服务指针 | 初始化 |
| `on_sample_count_changed()` | MSAA 切换时重建 pipeline | MSAA 切换时（低频） |
| `record()` | 向 RG 注册资源使用声明 + execute lambda | 每帧 |
| `rebuild_pipelines()` | 重编译 shader 重建 pipeline（热重载） | DebugUI 触发（低频） |
| `destroy()` | 销毁 pipeline + 私有资源 | 关闭 |

`setup()` 签名因 pass 而异。MSAA 相关 pass（ForwardPass、DepthPrePass）接收 `sample_count`：

```cpp
void setup(rhi::Context&, rhi::ResourceManager&,
           rhi::DescriptorManager&, rhi::ShaderCompiler&,
           uint32_t sample_count);
```

非 MSAA pass（TonemappingPass、SkyboxPass）不接收 `sample_count`：

```cpp
void setup(rhi::Context&, rhi::ResourceManager&,
           rhi::DescriptorManager&, rhi::ShaderCompiler&);
```

SkyboxPass 渲染到 resolved 1x `hdr_color`，`rasterizationSamples` 永远为 1，不受 MSAA 配置影响。

Pass 在 setup 中存储这些服务的指针（`ctx_`、`rm_`、`dm_`、`sc_`），lambda 通过 `this` 访问。ShaderCompiler 是 Layer 0 类型，Pass（Layer 2）依赖它不违反层级；热重载时 Pass 可自行重新编译 shader 重建 pipeline，Renderer 不需要介入。

`record()` 取代原设计中的 `register_resources()`。命名更准确——不仅注册资源，也提供 execute 回调。签名：`void record(RenderGraph& rg, const FrameContext& ctx)`。

**各 pass 方法集允许不统一**，只保留有实际作用的方法，但同功能方法保持同名。所有 pass 均有 `setup()` / `record()` / `rebuild_pipelines()` / `destroy()`。仅 MSAA 相关 pass（ForwardPass、DepthPrePass）额外有 `on_sample_count_changed()`。不保留空实现的占位方法。

### MSAA Pipeline 重建

MSAA 切换需要重建 pipeline（`rasterizationSamples` 烘焙在 pipeline 中），但 `setup()` 是一次性初始化，不适合承载此职责。

| 方案 | 说明 |
|------|------|
| A. 新增 `on_sample_count_changed(uint32_t)` 方法 | 语义清晰，只有受影响的 pass 实现 |
| B. 扩展 `on_resize()` 为 `on_config_change(RenderConfig)` | 统一处理多种配置变化 |
| C. `destroy()` + `setup()` 重新初始化 | 不增加新方法 |

**选择 A。** 方法名描述"发生了什么"（sample count 变了），pass 内部决定如何响应（重建 pipeline）。排除 B：语义模糊，不关心 MSAA 的 pass（如 TonemappingPass）也要接收不属于自己的配置变化。排除 C：`setup()` 还做了编译 shader、存储服务指针等工作，MSAA 切换时完全不需要重做。

只有 pipeline 中 `rasterizationSamples` 依赖 sample count 的 pass 需要实现此方法——ForwardPass、DepthPrePass。SkyboxPass 和 TonemappingPass 不需要——SkyboxPass 渲染到 resolved 1x `hdr_color`，`rasterizationSamples` 永远为 1；TonemappingPass 始终 1x，处理 resolved 产物。

### Pipeline 创建与热重载

Pass 的 pipeline 创建逻辑抽为私有方法 `create_pipelines()`，`setup()`、`on_sample_count_changed()`、`rebuild_pipelines()` 均调用它。`create_pipelines()` 内部先销毁旧 pipeline 再创建新 pipeline，可安全重入。

MSAA 相关 pass（ForwardPass、DepthPrePass）的 `create_pipelines()` 接收 `sample_count` 参数。`rebuild_pipelines()` 使用存储的 `current_sample_count_`（由 `setup()` 和 `on_sample_count_changed()` 记录）。非 MSAA pass 的 `create_pipelines()` 无参。

```cpp
class ForwardPass {
public:
    void setup(..., uint32_t sample_count);   // 记录 sample_count，调用 create_pipelines()
    void on_sample_count_changed(uint32_t);   // 记录 sample_count，调用 create_pipelines()
    void rebuild_pipelines();                 // 用存储的 sample_count 调用 create_pipelines()
private:
    void create_pipelines(uint32_t sample_count);
    uint32_t current_sample_count_ = 0;
};
```

ShaderCompiler 缓存 key 基于源码文本，include 变化通过内容比对检测。热重载以 DebugUI "Reload Shaders" 按钮形式触发，Renderer 检测后 `vkQueueWaitIdle()` → 遍历所有 pass 调用 `rebuild_pipelines()`。

### Attachment Format 处理

Pipeline 创建时需要知道 color/depth attachment 的 `VkFormat`。M1 期间绝大部分格式是设计决策确定的常量，但 Swapchain 格式是例外：

| 格式 | 值 | 处理方式 | 使用者 |
|------|------|----------|--------|
| HDR color | `R16G16B16A16_SFLOAT` | Pass 内部硬编码 | ForwardPass, SkyboxPass |
| Depth | `D32_SFLOAT` | Pass 内部硬编码 | ForwardPass, DepthPrePass, SkyboxPass |
| Normal | `R10G10B10A2_UNORM` | Pass 内部硬编码 | DepthPrePass |
| Swapchain | 取决于物理设备协商 | `setup()` 参数传入 | TonemappingPass |

**渲染中间产物格式硬编码。** HDR color、Depth、Normal 的格式在 M1 期间不会变化，传参增加接口复杂度但无实际收益。这与"具体类，不用虚函数"的决策一致——每个 pass 完全了解自己的渲染配置。

**Swapchain 格式例外。** Swapchain 的实际格式取决于物理设备和表面协商，不同 GPU 可能返回 `B8G8R8A8_SRGB` 或 `R8G8B8A8_SRGB`（通道顺序不同）。TonemappingPass 是唯一直接写入 swapchain 的 pass，其 `setup()` 接收 `VkFormat swapchain_format` 参数。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 全部硬编码 | 假定 `B8G8R8A8_SRGB`，不匹配时 assert 失败 |
| B. Swapchain 格式查询 | TonemappingPass 的 `setup()` 接收 swapchain format 参数 |

**选择 B。** Swapchain 是唯一格式不完全可控的 attachment，传一个参数的成本极低。排除 A：隐性假设变成运行时 assert 而非编译期保证，且 assert 失败的错误信息对调试无帮助。

### 显式 `destroy()`

遵循项目约定（显式 `destroy()` 管理 Vulkan 对象生命周期），每个 Pass 持有 pipeline 和私有资源，需显式销毁。

方法约定详见 `../milestone-1/m1-interfaces.md`「Pass 类约定」。

**缺席 pass 无需处理机制**：选择了具体类（非虚基类），`Renderer::render()` 显式调用每个存在的 pass。尚未实现的 pass 不在代码中，无需任何"缺席"处理机制。

---

## 资源管理与描述符扩展

### GlobalUBO 容量

阶段二 GlobalUBO 304 bytes。阶段三新增 IBL 字段（4× uint = 16 bytes）、debug 字段（1× uint）和 IBL 旋转字段（2× float），总计 336 bytes（21 × 16，满足 std140 struct 对齐）：

```
offset   0: mat4 view
offset  64: mat4 projection
offset 128: mat4 view_projection
offset 192: mat4 inv_view_projection
offset 256: vec4 camera_position_and_exposure
offset 272: vec2 screen_size
offset 280: float time
offset 284: uint directional_light_count
offset 288: float ibl_intensity               ← 原 ambient_intensity，Step 6.5 重命名
offset 292: uint irradiance_cubemap_index      ← Step 6 新增
offset 296: uint prefiltered_cubemap_index     ← Step 6 新增
offset 300: uint brdf_lut_index                ← Step 6 新增
offset 304: uint prefiltered_mip_count         ← Step 6 新增
offset 308: uint skybox_cubemap_index          ← Step 6 新增
offset 312: float ibl_rotation_sin             ← Step 6 新增
offset 316: float ibl_rotation_cos             ← Step 6 新增
offset 320: uint debug_render_mode             ← Step 7 新增
offset 324: uint _pad[3]                       ← padding to 336
```

IBL 未初始化时字段值为 0（index 0 对应 default textures，行为正确）。`ambient_intensity` 在 Step 6.5 重命名为 `ibl_intensity`——Step 6.5 引入 IBL 环境光取代简单环境光项，原名不再准确。C++ 端 `GlobalUniformData` 和 shader 端 `bindings.glsl` 同步改名。

`skybox_cubemap_index` 存储天空盒 cubemap 在 Set 1 `cubemaps[]` 数组中的下标，与其他 IBL index 字段一致，shader 统一从 UBO 读取。M2 Bruneton 大气散射替换天空时，字段指向 Bruneton cubemap，shader 不用改。

候选方案：

| 方案 | 说明 |
|------|------|
| A. GlobalUBO 新增字段（复用 padding slot） | 与 IBL index 字段一致 |
| B. Push Constant | 每帧推送 skybox cubemap index |
| C. Specialization Constant | pipeline 创建时特化 |

**选择 A。** 布局 offset 312 从 `_pad[2]` 改为 `skybox_cubemap_index` + `_pad[1]`，总大小保持 320 bytes。与 `irradiance_cubemap_index`、`prefiltered_cubemap_index` 等 IBL 字段在同一 UBO 中，shader 统一读取，无需额外绑定机制。

排除 B：Push Constant 给 Skybox Pass 传 cubemap index 打破统一 pipeline layout 约定——所有 pass 共享同一 `PushConstantData` 结构体，SkyboxPass 不使用 per-draw 数据（model、material_index），增加 push constant range 为一个 uint 不值得。

排除 C：Specialization Constant 在 pipeline 创建时烘焙，场景切换（不同环境贴图 → 不同 cubemap index）需要重建 pipeline，过于刚性。

不新增独立的 IBL intensity 字段——`ibl_intensity`（原 `ambient_intensity`）直接复用为 IBL 环境光强度乘数。

`ibl_rotation_sin` / `ibl_rotation_cos` 存储 IBL 环境贴图的水平旋转（绕 Y 轴）。CPU 侧预计算 `sin(ibl_yaw_)` / `cos(ibl_yaw_)` 写入 UBO，shader 用 `rotate_y(dir, sin, cos)` 旋转采样方向。仅影响环境采样（skybox / irradiance / prefiltered），不影响 glTF 场景灯光方向。传两个 float 而非一个角度，避免 shader 中 per-fragment sin/cos——虽然 GPU 驱动对 uniform 的 sin/cos 可以常量折叠，显式传递更清晰且无性能疑问。

后续阶段（阴影、后处理等）还会继续增长。M1 全部完成后预估仍远小于 1KB，远在 UBO 最小保证大小 16KB（Vulkan spec）之下。暂不拆分。

### Set 2 Layout — Render Target Descriptor Set

**选择：专用 Descriptor Set 管理帧内 render target 的 shader 采样**

后处理 pass（Tonemapping、Bloom 等）和屏幕空间效果（SSAO、Contact Shadows）需要采样帧内中间渲染产物。同时 ForwardPass 也需要读取 AO/contact shadow 等 effect 结果。这些 render target 的生命周期与材质纹理完全不同（resize/MSAA 切换时重建），需要独立的绑定机制。

候选方案：

| 方案 | 说明 |
|------|------|
| A. 注册 render target 到 Set 1 bindless | 复用 bindless 基础设施 |
| B. Push Descriptors | 后处理 pass 每帧推送输入 |
| C. 专用 Descriptor Set（Set 2） | render target 独立管理 |

**选择 C。** 核心理由是**资源生命周期分层**——三类资源（每帧更新的 buffer、持久纹理资产、帧内 render target）有不同的创建/更新/销毁模式，每类对应一个 descriptor set 最清晰。

排除 A：render target 与材质纹理混在同一 bindless 数组中模糊了"持久资产"和"帧内产物"的边界。Resize/MSAA 切换时需要在 bindless 中 unregister→register 更新 entry，增加管理复杂度。shader 层面通过 array index 间接访问，不如命名 binding 直观。

排除 B：后处理 pass 每帧参与渲染循环，不属于"一次性 init 操作"（IBL 预计算的 push descriptor 例外仅限 init scope）。Push descriptor set 在每个 pipeline layout 中只能有一个，后处理 pass 若同时需要 GlobalUBO（exposure、debug mode 等）和 push descriptor 的输入 image，layout 设计变复杂。且所有 pipeline 的 layout 不再统一，切换 pipeline 时 descriptor set 绑定失效需要重新绑定。

**M1 完整 Set 2 Layout（一次性预留，`PARTIALLY_BOUND`，按阶段逐步写入）：**

| Binding | 类型 | 名称 | 产生者 | 消费者 | 引入阶段 |
|---------|------|------|--------|--------|---------|
| 0 | `sampler2D` | hdr_color | ForwardPass | Tonemapping | 三 |

后续阶段按需扩展 binding（阶段四起逐步填充）。

**Sampler 选择与所有权：** Renderer 持有 `linear_clamp` 和 `nearest_clamp` 两个后处理专用 sampler。写入 Set 2 时根据 render target 类型选择对应 sampler。

候选 sampler 所有权方案：

| 方案 | 说明 |
|------|------|
| A. Renderer 持有 | Renderer 是 Set 2 descriptor 更新的执行者 |
| B. DescriptorManager 持有 | 集中管理 descriptor 相关资源 |
| C. 各 Pass 自行创建 | Pass 各自管理采样策略 |

**选择 A。** Renderer 是 Set 2 descriptor 更新的执行者，持有 sampler 与角色一致。Pass 不需要知道 sampler，Set 2 绑定采样时 sampler 的选择是 Renderer 在写 descriptor 时的决策。

排除 B：DescriptorManager 是通用管理者（layout、pool、set 分配、bindless slot），不应包含渲染管线具体策略（哪个 render target 用 linear 还是 nearest）。

排除 C：Set 2 是共享 descriptor set 由 Renderer 统一更新，sampler 所有权和 descriptor 更新权分离增加复杂度，且 pass 不需要关心 Set 2 的 sampler。

**更新时机：** Set 2 descriptor 只在 resize / MSAA 切换时更新（managed 资源重建导致 image view 变化）。更新路径已有 `vkQueueWaitIdle()` 保证 GPU 空闲，Set 2 不需要 `UPDATE_AFTER_BIND`。

**已知局限：** 同一 render target 若需不同 sampler（如 depth 的 nearest 和 linear），需占用两个 binding。M1 中此情况罕见，8 个 binding 足够覆盖。

**Set 2 容量**：保持 8 个 binding，不为 M2 预留。8 个恰好覆盖 M1 全部需求（见上表），M2 需要扩展时再扩展。

**阶段八 compute 迁移影响：** 阶段八后处理链改为 compute shader 后，compute pass 的输出通过 storage image（`imageStore`）写入，不经过 Set 2。输入仍可通过 Set 2 采样（支持硬件双线性过滤）。Auto Exposure 输出走 GlobalUBO 的 exposure 字段（GPU 写回 buffer），不占 Set 2 binding。

---

## Shader 系统扩展

### 公共文件组织

**选择：按依赖链拆分 + 独立工具文件**

Shader 公共代码按依赖关系组织，include 顺序自明：

```
common/constants.glsl   ← PI, EPSILON 等纯数学常量（无依赖）
    ↓
common/brdf.glsl        ← D_GGX, V_SmithGGX, F_Schlick（纯函数，不依赖场景数据）
                           Lambertian 漫反射直接用 INV_PI（来自 constants.glsl）

common/bindings.glsl    ← Set 0/Set 1/push constant 布局定义（已有，独立于上面的链）
common/normal.glsl      ← TBN 构造、normal map 解码（独立文件，depth_prepass 和 forward 共用）
```

消费方式：

```glsl
// forward.frag
#include "common/bindings.glsl"
#include "common/normal.glsl"
#include "common/brdf.glsl"     // 内部已 include constants
```

`normal.glsl` 独立——depth_prepass.frag（输出法线时）和 forward.frag 都需要 TBN 构造，但 depth_prepass 不需要 brdf。

最初计划在 brdf.glsl 之上再加 `lighting.glsl`（evaluate_directional_light、evaluate_ibl），但 Step 7 debug 渲染模式需要分离 diffuse/specular 贡献，组合函数返回合并结果无法拆分。最终 forward.frag 直接 include brdf.glsl 内联计算，`lighting.glsl` 未保留。

### 材质变体策略

**选择：阶段三无变体，无条件采样一切**

候选方案：

| 方案 | 说明 |
|------|------|
| A. `#ifdef` 编译宏 | ShaderCompiler 传入 defines |
| B. 动态分支（if/else） | 运行时判断 |
| C. Specialization Constants | Pipeline 创建时特化 |

**选择 B 的极端形式——阶段三甚至不需要动态分支。** Bindless + default 纹理的设计红利使得 forward shader 可以无条件执行所有纹理采样和计算：

| 纹理 | default 值 | 无条件采样的数学效果 |
|------|-----------|-------------------|
| `base_color_tex` | white (1,1,1,1) | factor × 1.0 = factor 本身 |
| `normal_tex` | flat normal (0.5,0.5,1.0) | TBN 变换后 = 原始顶点法线 |
| `metallic_roughness_tex` | white (1,1,1,1) | factor × 1.0 = factor 本身 |
| `occlusion_tex` | white (1,1,1,1) | strength × 1.0 = 无遮蔽 |
| `emissive_tex` | black (0,0,0,1) | factor × 0.0 = 无自发光 |

- `occlusion_tex × occlusion_strength` 调制 IBL/ambient 光照（阶段三引入 IBL 时同步消费）
- `emissive_tex × emissive_factor` 加到最终颜色（一行代码）

Depth PrePass 的 opaque vs mask 不是 shader 变体，是两个独立 pipeline + 独立 shader。

**演进路线：**

| 阶段 | 策略 | 场景 |
|------|------|------|
| 阶段三 | 无条件采样 | 所有纹理通过 default 值实现 neutral 效果 |
| 阶段六 | 动态分支 | Lightmap 按 per-object 属性条件执行（warp 不发散，GPU 开销为零）。示例：`if (instance.lightmap_index != INVALID_INDEX) { indirect = texture(textures[instance.lightmap_index], uv1).rgb; } else { indirect = evaluate_ibl(...); }` |
| M2+ | `#define` 编译变体 | POM ray march 循环显著增加 shader 体积和寄存器压力，ShaderCompiler 已原生支持 `AddMacroDefinition` |
| M2+ 可选 | Specialization Constants | 从 `#define` 迁移以减少编译次数 |

---

## App 层设计

**选择：Application + Renderer 分离**

阶段二期间 `Application` 作为 Composition Root 持有所有子系统，通过注释分组表达层次。初始化和销毁顺序由 `init()` / `destroy()` 显式控制，不依赖成员声明顺序。

阶段三开始时提取 `Renderer` 类，将渲染子系统从 Application 分离。Application 管应用逻辑（窗口、输入、UI），Renderer 管渲染管线（RG、pass、资源）。

### Renderer 提取时机

| 方案 | 说明 | 优势 | 劣势 |
|------|------|------|------|
| A. 阶段三开头先重构 | 第一个 Step 专门做提取 | 后续 Step 在干净架构上工作 | 重构时还不知道阶段三具体需要什么 |
| B. 阶段三中间自然提取 | 写到明显臃肿时再提取 | 有实际经验指导 | 中间重构打断功能开发 |
| C. 阶段三结束后再提取 | 仍用 Application 持有一切 | 延迟决策 | Application 过于臃肿 |

**选择 A。** 阶段二结束后 Application 已有约 10 个成员，阶段三至少再加 5-6 个（MSAA 资源、Normal buffer、HDR buffer、IBL 资源、PrePass pipeline 等）。阶段二的完整经验已足够指导提取——已知哪些是 RHI 基础设施、哪些是框架组件、哪些是 app 逻辑。

### 所有权划分

**Renderer 持有（渲染子系统）：**

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

**Application 保留（平台 + App 逻辑）：**

| 类别 | 成员 |
|------|------|
| 平台 | `window_`, `framebuffer_resized_` |
| RHI 基础设施（顶层拥有） | `context_`, `swapchain_`, `resource_manager_`, `descriptor_manager_` |
| ImGui | `imgui_backend_` |
| App 模块 | `camera_`, `camera_controller_`, `debug_ui_`, `scene_loader_`, **`renderer_`** |
| 每帧场景数据 | `scene_render_data_`, `cull_result_` |
| IBL 参数 | `ibl_yaw_`（水平旋转）, `ibl_intensity_`, `disable_scene_lights_`（Step 6.5 重构：退役 `default_lights_` / pitch / intensity / `force_default_light_`，`ambient_intensity_` → `ibl_intensity_`） |
| 渲染参数 | `exposure_` |
| 帧状态 | `vsync_changed_`, `image_index_` |

> `resource_manager_` 和 `descriptor_manager_` 归 Application 持有，因为 `scene_loader_` 加载时也要用。Renderer 和 SceneLoader 都是使用者，不是拥有者。

### Resize 两阶段

`swapchain_image_handles_` 引用旧 VkImage，必须在 `swapchain_.recreate()` 之前注销；新资源在 recreate 之后创建：

```cpp
void Application::handle_resize() {
    vkQueueWaitIdle(context_.graphics_queue);
    renderer_.on_swapchain_invalidated();  // unregister old + destroy depth
    swapchain_.recreate(context_, window_);
    renderer_.on_swapchain_recreated();    // register new + create depth
}
```

### UBO/SSBO 填充归属

`GlobalUniformData` 的布局是 shader 约定，属于渲染层。Application 只传语义数据（`RenderInput`），Renderer 打包为 GPU 格式。

### App 层模块接口

App 层各模块（DebugUI、CameraController、SceneLoader、Renderer）不追求统一接口，各自按实际职责设计方法。

**为什么不统一：** 四个模块职责差异大，统一接口是为统一而统一，没有实际收益。

接口定义见 `../milestone-1/m1-interfaces.md`。

---

## MSAA 配置策略

**选择：运行时可配置（1x / 2x / 4x / 8x），默认 4x**

| 方案 | 说明 |
|------|------|
| A. 固定 4x | 最常见的平衡点 |
| B. 可配置（2x/4x/8x） | 运行时切换，需重建 MSAA 资源和 pipeline |
| C. 阶段三固定 4x，后续加配置 | 先简单后灵活 |

**选择 B，并扩展可选级别包含 1x。** 1x（无 MSAA）用于调试和性能基准对比。

### 1x MSAA 资源拓扑

1x 时不创建 MSAA 资源（msaa_color、msaa_depth、msaa_normal），Forward Pass / PrePass 直接渲染到 1x resolved target。MSAA 复杂度不向下游 pass（Tonemapping 等后处理）泄漏——它们的输入永远是 1x 的 `hdr_color`。

| | 1x | 多采样（2x/4x/8x） |
|---|---|---|
| managed 资源 | hdr_color, depth, normal | hdr_color, depth, normal + msaa_color, msaa_depth, msaa_normal |
| Forward Pass 资源声明 | hdr_color(Write), depth(Read) | msaa_color(Write), msaa_depth(Read), hdr_color(Write) |
| Forward Pass resolve 配置 | 无 | color resolve AVERAGE |
| PrePass 资源声明 | depth(ReadWrite), normal(Write) | msaa_depth(ReadWrite), msaa_normal(Write), depth(Write), normal(Write) |
| PrePass resolve 配置 | 无 | depth resolve MAX_BIT, normal resolve AVERAGE |
| Pipeline rasterizationSamples | 1 | N |

FrameResources 中 msaa 字段使用 `RGResourceId` 的 `valid()` 检查（`index != UINT32_MAX`）表示不存在，与已有设计一致。

MSAA 切换（1x ↔ 多采样）时 `handle_msaa_change()` 创建/销毁 MSAA managed 资源。

不使用 `VK_EXT_extended_dynamic_state3`（动态 rasterization samples）。该扩展非 Vulkan 1.4 核心，MSAA 切换是低频操作，重建 pipeline 开销可忽略。

### Pipeline 销毁安全性

`on_sample_count_changed()` 的**前提条件**：调用者必须保证 GPU 空闲。`Renderer::handle_msaa_change()` 第一行 `vkQueueWaitIdle()` 是保障。

在此前提下，直接 `vkDestroyPipeline(old)` + 创建新 pipeline，不走 deferred deletion。GPU 空闲意味着没有 command buffer 正在引用旧 pipeline，销毁安全。

此前提应在文档和代码注释中标注，避免未来有人在非 idle 上下文中调用 `on_sample_count_changed()` 导致 use-after-free。

### 切换机制

切换 MSAA 时需要重建：

| 需重建 | 原因 |
|--------|------|
| 所有 MSAA pipeline | `rasterizationSamples` 烘焙在 pipeline 中 |
| 所有 MSAA managed 资源 | sample_count 变化 |

不需要重建：descriptor layout、resolved target、非 MSAA pipeline。

**重建触发流程**与 resize/vsync 统一模式。RG `update_managed_desc()` 更新 MSAA 资源描述，handle 保持不变（见「Managed 资源管理」章节）。Pipeline 重建通过各 pass 的 `on_sample_count_changed()` 完成（见「Pass 类设计 — MSAA Pipeline 重建」章节）。

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
    // 2. 通知受影响的 pass 重建 pipeline
    forward_pass_.on_sample_count_changed(new_sample_count);
    depth_prepass_.on_sample_count_changed(new_sample_count);
    // skybox_pass_ 和 tonemapping_pass_ 不受影响——均始终 1x
}
```

---

## Tonemapping

**选择：ACES fullscreen fragment shader**

阶段三引入 HDR color buffer 后，需要 tonemapping 将 HDR 映射到 SDR 显示范围。

| 方案 | 说明 |
|------|------|
| A. 临时 blit/copy（HDR→SDR 截断） | 高光全部 clamp 为纯白，效果差 |
| B. 简易 Reinhard | `color / (color + 1.0)`，偏平 |
| C. 先不用 HDR buffer | Forward pass 直接输出 LDR（swapchain format） |

**选择：直接实现 ACES（跳过 B 和 C）。** PBR 光照输出亮度范围是 HDR 的（镜面高光轻松超过 1.0），直接截断到 [0,1] 丢失大量信息。阶段三直接实现 ACES，不用 Reinhard 过渡。后续阶段八可替换为 Khronos PBR Neutral（项目 Tonemapping 演进见 `../project/technical-decisions.md`）。

**为什么 fullscreen fragment 而非 compute shader：** SRGB swapchain format（`VK_FORMAT_B8G8R8A8_SRGB`）通常不支持 `VK_IMAGE_USAGE_STORAGE_BIT`，无法作为 compute shader 的 storage image 输出。Fullscreen fragment shader 通过硬件自动 linear→sRGB 转换写入 SRGB swapchain。

**Fullscreen triangle：** 使用 hardcoded fullscreen triangle（vertex shader 中根据 `gl_VertexIndex` 生成覆盖全屏的三角形，无顶点输入），比全屏 quad 少一个三角形、避免对角线像素被光栅化两次。

**已知演进点（阶段八）：** 阶段八引入完整后处理链（Tonemapping → Vignette → Color Grading）后，所有后处理改为 compute shader 输出到中间 LDR buffer，末端新增 Final Output fullscreen fragment pass 拷贝到 SRGB swapchain。届时 `tonemapping.frag` 改为 `tonemapping.comp`，`fullscreen.vert` 保留给 Final Output pass 复用。

### Exposure 控制

| 方案 | 说明 |
|------|------|
| A. DebugUI 手动 EV 滑条 | `pow(2, ev)` 计算 exposure 乘数 |
| B. 固定 exposure | 硬编码 1.0 |
| C. 简单自动曝光（平均亮度） | 降采样计算平均亮度 → 反馈 exposure |

**选择 A。** PBR + IBL 输出亮度范围大，固定值不可能适配所有场景和环境贴图。DebugUI 提供 EV 滑条（范围 -4 到 +4），`pow(2, ev)` 计算 exposure 乘数。

**数据通路：** `RenderInput::exposure` → `GlobalUniformData::camera_position_and_exposure.w` → `tonemapping.frag` 消费。Exposure 打包在 `camera_position_and_exposure` 的 w 分量中，复用已有 vec4 slot，不新增字段。

排除 B：PBR 光照 + IBL 环境光的输出亮度受光源强度、材质属性、环境贴图亮度多重因素影响，固定 exposure 在不同场景下要么过曝要么过暗。

排除 C：简单自动曝光增加不必要复杂度（降采样 pass、temporal 平滑），是阶段八的工作。

**演进**：阶段八自动曝光实现后，手动滑条仍有价值——作为覆盖（override）和对比基线，帮助调试自动曝光算法。

### ImGui 与 Tonemapping 的关系

| 方案 | 说明 |
|------|------|
| A. 渲染到 resolved 后的 LDR buffer | 正常路径，MSAA forward → resolve → 后处理 → ImGui 叠加 |
| B. 直接渲染到 swapchain image | 与阶段二一致 |

**选择 B。** ImGui 作为 debug overlay 不需要 MSAA，直接画到 swapchain image 上。渲染到中间 LDR buffer 无实际优势。

---

## HDR Color Buffer 格式

**选择：全程 R16G16B16A16F**

| 格式 | 大小 | 精度 | Alpha | 负值 | 适用 |
|------|------|------|-------|------|------|
| R11G11B10F | 4B/px | 6-bit mantissa (R/G), 5-bit (B) | 无 | 不支持 | 带宽敏感、不需要 alpha 的场景 |
| R16G16B16A16F | 8B/px | 10-bit mantissa | 有 | 支持 | 最通用，后处理链友好 |

需要考虑的因素：
- MSAA 下带宽加倍（4x MSAA + R16G16B16A16F = 32B/px）
- 透明 pass（阶段七）是否需要 alpha？→ 不需要（blending 的 alpha 在 blend equation 中，render target alpha 不是必须的）
- 后处理链是否需要负值？→ 正常 HDR 渲染不产生负值
- Bloom 降采样链是否对精度敏感？→ 6-bit mantissa 对 Bloom 够用

**选择 R16G16B16A16F。** 带宽分析（1920×1080, 4x MSAA）：约为 R11G11B10F 的 2 倍（~269 MB/帧 vs ~135 MB/帧），但额外的 ~134 MB 在 60fps 下仅占桌面 GPU 显存带宽的 1-3%，不构成瓶颈。

精度分析：R11G11B10F 的 6-bit 尾数在暗部渐变区域可能产生可见 banding（色带），R16G16B16A16F 无此风险。M2 后处理链（SSR、SSGI、DOF 等）多次读写 HDR buffer 会累积量化误差，高精度更可控。

最初考虑两个策略：策略 1（混合格式）— R11G11B10F 作 MSAA render target，R16G16B16A16F 作 resolve 后的后处理链输入；策略 2（统一格式）— 全程 R16G16B16A16F。混合格式（MSAA 用 R11G11B10F，resolved 用 R16G16B16A16F）不可行——Dynamic Rendering 的 resolve 要求格式兼容，两者不兼容。

---

## Normal Buffer 格式与编码

**选择：R10G10B10A2_UNORM, world-space**

**长远考虑**（格式选择影响后续所有 pass）：
- SSAO（阶段五）需要 normal，view-space 更直接
- SSR（M2）需要 normal，world-space 更通用
- SSGI（M2）需要 normal
- 阶段三只是 PrePass 写入 + 未来 pass 消费，格式选择影响后续所有 pass

| 方案 | 格式 | 大小 | 精度 | 编解码 |
|------|------|------|------|--------|
| A. World-space xyz + padding | R16G16B16A16F | 8B/px | 极高 | 无 |
| B. World-space xyz | R10G10B10A2_UNORM | 4B/px | 10-bit/通道 | `n*0.5+0.5` / `n*2.0-1.0` |
| C/D. Octahedron 编码 | R16G16_SFLOAT | 4B/px | 高 | 八面体编解码 |
| E. xy + reconstruct z | R16G16_SFLOAT | 4B/px | 高 | z 符号丢失 |

**选择 B。** 精度：10-bit/通道 → 角度分辨率 ~0.1°，对 SSAO/SSR 等所有消费者足够。编解码：1 MAD 指令，开销可忽略。World-space 更通用——SSAO（阶段五）需要时通过 view matrix 转为 view-space（一次矩阵乘法），SSR/SSGI（M2）直接使用 world-space。

**MSAA resolve 兼容性：** `n*0.5+0.5` 是线性映射，AVERAGE resolve 正确（平均编码值 = 平均法线分量），消费方解码后 `normalize()` 即可。排除 C/D（octahedron）的关键原因：八面体映射是非线性的，AVERAGE resolve 产生错误法线，需要 custom resolve pass，与 MSAA Resolve 策略（纯 Dynamic Rendering resolve）矛盾。排除 E：world-space Z 符号丢失。

2-bit A 通道可在未来存储 material flag（区分皮肤/金属等），用于屏幕空间效果的特殊处理。

---

## Depth + Normal PrePass

### PrePass 范围

**选择：Depth + Normal PrePass 一步到位**

| 方案 | 说明 |
|------|------|
| A. 仅 Depth PrePass | 只输出深度，不输出法线 |
| B. Depth + Normal PrePass | 同时输出深度和法线 |
| C. 阶段三做 A，阶段五时升级为 B | 延迟法线输出 |

**选择 B。** 阶段五（SSAO / Contact Shadows）和 M2（SSR / SSGI）都依赖 resolved normal buffer。阶段三直接输出法线，pipeline 和 shader 无需后续修改。Normal buffer 格式见「Normal Buffer 格式与编码」（R10G10B10A2_UNORM, world-space）。

### Alpha Mask 物体处理

**选择：两批绘制（Opaque pipeline + Mask pipeline）**

| 方案 | 说明 |
|------|------|
| A. PrePass 跳过 Alpha Mask 物体 | Mask 物体只在 forward pass 中画 |
| B. PrePass 两批绘制 | 同一 RG pass 内先画 Opaque（无 discard），再画 Mask（有 discard），两个 pipeline |
| C. 统一用带 discard 的 shader | 所有物体都走含 discard 的 fragment shader |

**选择 B。** 核心理由：`discard` 对 GPU Early-Z 的影响。

GPU Early-Z 在 fragment shader 执行之前做深度测试。`discard` 关键字破坏此优化——即使 Opaque 物体永远不走 discard 分支，只要 shader 二进制中包含 `discard` 指令，GPU/driver 就可能对该 pipeline 的所有调用禁用 Early-Z（编译期决定，非运行时分支）。因此 Opaque 和 Mask 必须使用不同 pipeline + 不同 shader。

排除方案 A：PrePass 跳过 Mask 物体导致 Forward Pass 无法对其使用 EQUAL depth test、阶段五 SSAO 在 Mask 区域读到错误深度、Normal buffer 缺少 Mask 物体法线。

两个 Pipeline 对比：

| | Opaque Pipeline | Mask Pipeline |
|---|---|---|
| VS 输出 | position, normal, tangent, uv0 | 同左 |
| FS 采样 | normal_tex（无条件，default = flat normal） | normal_tex + base_color_tex（alpha test） |
| FS 操作 | TBN → encode world normal → 写 color attachment | alpha test → discard → TBN → encode → 写 color |
| `discard` | **无** | **有** |
| Early-Z | **保证** | **可能被禁用** |
| Depth write | ON, Compare GREATER | ON, Compare GREATER |

绘制顺序（同一 RG pass 内）：先画 Opaque（填充 depth buffer），再画 Mask（利用已有深度拒绝被遮挡 fragment）。

Shader 文件：

```
shaders/
├── depth_prepass.vert          # 共享（输出 position + normal/tangent + uv0）
├── depth_prepass.frag          # Opaque: 采样 normal_tex → TBN → encode world normal
└── depth_prepass_masked.frag   # Mask: alpha test + discard → 采样 normal_tex → TBN → encode
```

VS 共享——Opaque FS 也需要 uv0（用于 normal_tex 采样）。两个 FS 的差异仅为 alpha test 几行代码。

### Forward Pass 深度配合

**选择：EQUAL + depth write OFF + `invariant gl_Position`**

| 方案 | 说明 |
|------|------|
| A. EQUAL + depth write OFF + `invariant gl_Position` | 经典 Z-PrePass 配合，确定性 zero overdraw |
| B. GREATER_OR_EQUAL + depth write OFF | 容忍微小浮点差异，近零 overdraw |

注：GREATER + write ON 在有 PrePass 时是错误的——Reverse-Z 下 GREATER 要求 fragment depth 严格大于 buffer depth，PrePass 已写入的同一表面深度 D 无法通过 D > D 测试，导致全部 fragment 被拒绝。

**选择 A。** 工作原理：

```
PrePass:  depth compare GREATER, write ON  → 写入最近表面深度 D
Forward:  depth compare EQUAL,   write OFF → fragment depth == D ? 着色 : 拒绝
```

确定性 zero overdraw：只有恰好位于最近表面上的 fragment 通过深度测试并执行 PBR 着色。不写深度（depth buffer 已正确），省带宽。

`invariant gl_Position` 保证 bit-identical 深度——GLSL `invariant` 限定符保证不同 shader program 中，相同表达式 + 相同输入 → bit-identical 输出：

```glsl
// depth_prepass.vert & forward.vert
invariant gl_Position;
gl_Position = global.view_projection * push.model * vec4(in_position, 1.0);
```

MSAA 兼容：`invariant` 保证两个 pass 产生相同的 sample coverage 和 per-sample depth。前提是两个 pipeline 的光栅化配置相同（相同 sample count、相同 cull mode），在本项目中自然满足。

排除方案 B：GREATER_OR_EQUAL 只在一个方向容忍误差，另一方向仍有孔洞风险。`invariant` 从根本上消除浮点差异，EQUAL 是正确且完整的方案。性能代价：`invariant` 可能轻微限制编译器优化，实践中性能影响不可测量。

---

## Forward Pass 升级

**选择：分两步升级 forward.frag（Step 6.5 IBL 验证 → Step 7 Cook-Torrance）**

forward.vert 保持不变（输出 world position / normal / uv0 / tangent）。forward.frag 光照升级分两步：

1. **Step 6.5**：保留 Lambert 直射光循环，新增 IBL 环境光（irradiance diffuse + prefiltered specular + BRDF LUT Split-Sum），内联在 forward.frag 中。采样 `metallic_roughness_tex` 实现 metallic 工作流分离。验证 IBL 数据正确性
2. **Step 7**：Lambert 直射光 → Cook-Torrance（GGX / Smith / Schlick），IBL 内联代码重构到 `common/lighting.glsl` 的 `evaluate_ibl()`，消费 GPUMaterialData 全部 5 个纹理字段

分两步的理由：Step 6 生成 IBL 纹理但只能通过 RenderDoc 检查，直到 Step 7 同时引入 BRDF 数学 + IBL 采样，出错时无法区分是 IBL 数据问题还是 BRDF 数学问题。Step 6.5 独立验证 IBL 数据，Step 7 出问题时可断定是 BRDF 数学。

不保留 Lambert 切换——如需诊断 PBR 问题，debug UI 可显示各光照分量（diffuse only、specular only、IBL only 等），比回退 Lambert 更有用。

### Debug 渲染模式

Step 7 通过 `GlobalUBO.debug_render_mode` 控制 forward.frag 输出不同分量（Diffuse Only / Normal / Metallic / Roughness / IBL Only 等）。Debug 输出通常是 [0,1] 线性值，经过 ACES tonemapping 会失真（如 normal 编码的 0.5 经 ACES 变成 ~0.47）。

**选择：HDR / passthrough 二分 + 命名常量。** 分类依据是输出性质而非功能类别——HDR 模式输出高动态范围值需要 exposure + ACES，passthrough 模式输出 [0,1] 线性值直接写入（只保留硬件 linear→sRGB）。

`bindings.glsl` 中定义命名常量和阈值：

```glsl
#define DEBUG_MODE_FULL_PBR          0
#define DEBUG_MODE_DIFFUSE_ONLY      1
#define DEBUG_MODE_SPECULAR_ONLY     2
#define DEBUG_MODE_IBL_ONLY          3
// --- HDR modes above, passthrough modes below ---
#define DEBUG_MODE_PASSTHROUGH_START 4
#define DEBUG_MODE_NORMAL            4
#define DEBUG_MODE_METALLIC          5
#define DEBUG_MODE_ROUGHNESS         6
// 后续阶段按排列约定追加 passthrough 模式
```

TonemappingPass 检查 `debug_render_mode >= DEBUG_MODE_PASSTHROUGH_START` 决定是否 passthrough。

**排列约定**：HDR 模式（光照分量隔离）排在 `PASSTHROUGH_START` 之前，passthrough 模式追加到末尾。新增 HDR 模式插入并递增 `PASSTHROUGH_START`，新增 passthrough 模式直接追加。HDR 模式是有限集合（Direct / Specular / IBL / Indirect），passthrough 模式会随阶段持续增长。

C++ 侧使用对应 enum，DebugUI 下拉列表用 `ImGui::Separator()` 按类别分组显示，不影响编号。

### Forward Shader 跨阶段演进

forward.frag 在每个阶段都会被修改——阶段四加阴影采样、阶段五加 AO/Contact Shadow、阶段六加 lightmap。**不预留注入点**（`#include` 占位、函数调用占位）。每个阶段按需修改是自然的增量开发，`brdf.glsl` 的函数在各阶段按需组合。过度预留增加当前复杂度且预留结构可能与实际需求不匹配。

### 透明物体处理（Phase 3 → Phase 7）

Step 5 将 Forward Pass 改为 `depth compare EQUAL + depth write OFF`。`AlphaMode::Blend` 的透明物体不在 PrePass 中写 depth，因此在 Forward EQUAL test 中 100% 被拒绝。Phase 7 引入正式 Transparent Pass 修复。

**选择：接受回归。** M1 测试场景（Sponza 等建筑场景）主体是 opaque + alpha masked，不含 `AlphaMode::Blend` 物体。Phase 3 Step 5 到 Phase 7 期间透明物体不渲染，已记录为已知局限性（见 `../milestone-1/milestone-1.md`）。

---

## IBL 管线

### 环境贴图输入

**选择：Equirectangular .hdr 文件**

| 方案 | 说明 |
|------|------|
| A. Equirectangular .hdr 文件 | stb_image 加载，GPU 转 cubemap |
| B. 预处理好的 cubemap 6 面 | 离线工具处理，直接加载 |
| C. .ktx2 / .dds cubemap | 预压缩格式 |

**选择 A。** .hdr equirectangular 是最常见的 HDR 环境贴图格式，网上免费资源丰富。Equirect→cubemap 转换约 20-30 行 compute shader，一次性工作。stb_image 已集成，支持 .hdr 格式（`stbi_loadf`）。

### Equirect 输入 GPU 格式

`stbi_loadf` 返回 float32 RGB 数据，上传 GPU 需要选择格式：

| 格式 | 大小 | 精度 |
|------|------|------|
| R32G32B32A32_SFLOAT | 16 B/px | 完整 float32 |
| R16G16B16A16_SFLOAT | 8 B/px | half float（max ~65504） |

**选择 R16G16B16A16_SFLOAT。** Equirect 输入是一次性使用（转 cubemap 后销毁），HDR 环境贴图亮度极少超过 65504。R16 省一半上传带宽，精度完全够用。两者都需要 CPU 侧 RGB→RGBA 扩展（加 alpha=1.0）。

### 中间 Cubemap（Equirect → Cubemap 转换产物）

| 参数 | 值 | 理由 |
|------|------|------|
| 分辨率 | `min(bit_ceil(equirect_width / 4), 2048)` per face | 匹配输入角分辨率（equirect 360° → face 90°），上限 2048 防止显存爆炸 |
| 格式 | R16G16B16A16_SFLOAT | 与 prefiltered map 格式一致，避免精度损失 |
| 预计算后 | **保留** | 用于天空盒渲染（Skybox Pass） |

**分辨率由输入 equirect 宽度决定**：cubemap 同时用于 IBL 预计算源数据和 Skybox 天空渲染。Skybox 直接暴露给玩家，固定分辨率在高质量输入下会丢失细节。`equirect_width / 4` 匹配角分辨率（360° 到 90°），`std::bit_ceil` 向上取整到 2 的幂，上限 2048 防止显存爆炸（无下限——低分辨率输入不会凭空产生细节）。常见 4096×2048 HDR → 1024 per face（~48 MB），8192×4096 → 2048 per face（~192 MB）。

**保留中间 cubemap**：M1 规划包含"静态 HDR Cubemap 天空"（见 `../milestone-1/milestone-1.md`），中间 cubemap 是 Skybox Pass 的源数据。M2 Bruneton 大气散射替换天空后可改为销毁。

**Mip 剥离优化**：中间 cubemap 创建时带完整 mip chain（供 `compute_prefiltered` 的 `textureLod` 采样降噪），预滤波完成后 mip 1..N 不再需要。`strip_skybox_mips()` 在预滤波后创建 mip-0-only 副本替换原 cubemap，释放约 25% 显存（8K HDR 下 ~64 MB）。Skybox 渲染仅用 `texture()` 自动 LOD 采样基础级别，mip 剥离不影响画面。缓存写入在剥离之后，因此缓存文件已是 mip-0-only。

**BC6H 压缩**：mip 剥离后，skybox 和 prefiltered cubemap 通过 GPU compute shader 压缩为 BC6H unsigned float 格式（`compress_cubemaps_bc6h()`）。压缩使用 Betsy/GPURealTimeBC6H 的 GLSL 移植版（`shaders/compress/bc6h.comp`），quality 模式（mode 11 + 32 partition × mode 2/6），并扩展了最小二乘 endpoint 精细化（3 轮迭代）：初始 min/max endpoint 选择后，通过 LS 拟合最小化全 16 texel 的插值误差来重新计算最优 endpoint，显著改善 HDR 高动态范围 block 的压缩质量。每个 face×mip 独立 dispatch，通过 SSBO + `vkCmdCopyBufferToImage2` 写入 BC6H cubemap。压缩后替换原 uncompressed 版本，运行时渲染直接使用 BC6H 纹理（硬件原生解码）。

BC6H 选型理由：
- 桌面 Vulkan 唯一可用的 HDR 块压缩格式（ASTC HDR 桌面支持不完整）
- Unsigned float 覆盖 IBL 正值域 [0, 65504]
- 8:1 压缩比（vs R16G16B16A16F），显存从 ~208 MB 降至 ~26 MB（8K HDR）
- GPU compute 压缩避免 readback→CPU 压缩→上传的往返开销
- 质量接近 Intel ispc_texcomp slow profile（RMSLE 差距 <10%），IBL 用途完全够用

### 预计算策略

**选择：GPU 计算 + 运行时缓存**

| 方案 | 说明 |
|------|------|
| A. GPU Compute Shader（每次重算） | 环境贴图变化时重新计算 |
| B. 离线预计算 + 文件缓存 | 首次计算后保存到磁盘 |
| C. GPU 计算 + 运行时缓存 | 每次启动重新计算 |

**选择 C。** 三个预计算总共几十到几百毫秒（一次性），不影响运行时性能。GPU compute shader 在 `begin_immediate()` / `end_immediate()` scope 内执行。A（GPU 从头计算）是 C 的核心实现，也是 B 的 fallback——永远保留。

磁盘缓存（B）不做正式规划——C→B 是纯优化（加一层磁盘读写），不影响架构。M2 的 Bruneton 大气散射使 IBL 变为动态计算（太阳角度变化时重算），磁盘缓存对动态输入无意义。B 真正有价值的时机是多个静态环境快速切换或 Reflection Probes 数量增多时。

#### Split-Sum 预计算产物

| 产物 | 功能 |
|------|------|
| Irradiance Map | 余弦加权半球卷积 |
| Prefiltered Env Map | 不同 roughness 级别的模糊环境 |
| BRDF Integration LUT | 基于 roughness + NdotV 的积分查表 |

#### 预计算产物参数

| 产物 | 分辨率 | 计算格式 | 运行时格式 | 运行时内存 | 说明 |
|------|--------|----------|-----------|-----------|------|
| Skybox Cubemap | 2048×2048 per face, mip 0 only | R16G16B16A16F | BC6H UFloat | ~24 MB | mip 剥离 + BC6H 压缩后 |
| Irradiance Map | 32×32 per face | R11G11B10F | R11G11B10F（不压缩） | ~24 KB | 太小，压缩无意义 |
| Prefiltered Env Map | 512×512 per face + mip chain | R16G16B16A16F | BC6H UFloat | ~2 MB | GPU 压缩后 |
| BRDF Integration LUT | 256×256 | R16G16_UNORM | R16G16_UNORM（不压缩） | ~256 KB | 非 HDR，值域 [0,1] |

Prefiltered map 计算时用 R16G16B16A16F（高精度避免 banding），计算完成后 GPU 压缩为 BC6H。Irradiance map 是余弦积分后的极低频信号，32×32 极小，不压缩。

### 模块归属

**选择：Framework 层 `framework/ibl.h`**

| 方案 | 说明 |
|------|------|
| A. Framework 层 | 与 texture/mesh 同级的通用基础设施 |
| B. App 层 | 与 scene_loader 一起 |
| C. Passes 层 | 独立"预计算 pass" |

**选择 A。** IBL 预计算是渲染框架级别的基础设施（M2 的 Reflection Probes 复用相同的 cubemap 处理），不是 app 逻辑，也不是每帧运行的 pass。

### HDR 环境贴图加载路径

#### 加载者

| 方案 | 说明 |
|------|------|
| A. IBL 模块自行加载 | `IBL::init()` 接收文件路径，内部 `stbi_loadf`，自包含 |
| B. SceneLoader 扩展 | SceneLoader 新增环境贴图加载能力 |
| C. Application 层加载 | Application 调用 `stbi_loadf`，传 pixel data 给 IBL |

**选择 A。** IBL 模块接收文件路径，内部完成加载、上传、预计算全流程，自包含。环境贴图不属于 glTF 场景，由 SceneLoader 加载会模糊其边界；Application 不应处理图像加载细节。

#### 路径传递

引入 **CLI11** 命令行解析库，替代当前手动解析 `argc/argv` 的方式。CLI11 是新依赖，需要更新 `vcpkg.json` 和 `CMakeLists.txt`。

命名参数：`--scene`（场景 glTF 路径）、`--env`（HDR 环境贴图路径，默认 `assets/environment.hdr`）。现有的 `scene_path` 也迁移到 CLI11 统一管理。

M1 不支持运行时切换环境贴图，固定一张。

### HDR 加载失败 Fallback

**问题**：`--env` 指定的 HDR 文件不存在或加载失败时，`load_equirect()` 返回无效 handle，后续 compute pipeline 在无效 handle 上操作导致崩溃。

**选择：Framework 层生成中性 fallback cubemap**

| 方案 | 说明 |
|------|------|
| A. Fatal error 终止程序 | 环境贴图是必需资源，缺失则无法运行 |
| B. Shader 层 fallback（条件分支） | shader 检查 index == UINT32_MAX 跳过 IBL |
| C. Framework 层生成 fallback cubemap | 创建 1×1 中性灰 cubemap，管线照常运行 |

**选择 C。** `IBL::init()` 在 `load_equirect()` 失败时，用 `vkCmdClearColorImage` 创建 1×1 中性灰 cubemap 替代。整个管线（bindless 注册、skybox 采样、forward IBL 采样）照常运行，shader 无需任何条件分支。效果为无环境反射的均匀灰色天空，渲染器仍可正常工作。

Fallback 产物参数：

| 产物 | 分辨率 | 格式 | 说明 |
|------|--------|------|------|
| Skybox cubemap | 1×1 per face | R16G16B16A16F | 中性灰 `(0.1, 0.1, 0.1)` |
| Irradiance cubemap | 1×1 per face | R11G11B10F | 同色 |
| Prefiltered cubemap | 1×1 per face, 1 mip | R16G16B16A16F | 同色 |
| BRDF LUT | 正常 compute 计算 | R16G16_UNORM | 环境无关，数学纯函数 |

**排除 A**：M1 定位为学习/演示项目，缺少 HDR 文件不应阻止渲染器运行。
**排除 B**：在 shader 中添加条件分支会增加 Step 6.5/7 的复杂度，且 fallback 只影响 init 时的数据准备，不应泄漏到每帧渲染路径。

实现边界：fallback 逻辑完全封装在 `IBL::init()` 内部（`create_fallback_cubemaps()` 私有方法），Renderer、Pass、Shader 层不感知 fallback 存在。

### Compute Shader 基础设施

IBL 预计算需要 compute shader（cubemap 卷积、LUT 生成）。阶段二没有用过 compute shader，经代码检查确认均不存在：

| 项目 | 状态 |
|------|------|
| `ComputePipelineDesc` / `create_compute_pipeline()` | 不存在，`pipeline.h` 仅有 `GraphicsPipelineDesc` + `create_graphics_pipeline()` |
| `CommandBuffer::dispatch()` | 不存在，`commands.h` 仅有 graphics 命令 |
| `.comp` shader 文件 | 不存在 |

阶段三需要新增：`ComputePipelineDesc` + `create_compute_pipeline()`、`CommandBuffer::dispatch()`、确认 shaderc `shaderc_compute_shader` stage 支持。

### Descriptor Binding

IBL 有两层 descriptor 需求：

**1. 预计算阶段（init 时 compute dispatch）**：每个 compute shader 需要绑定输入 image（sampled）和输出 image（storage），这些不属于全局 Set 0/Set 1。

**选择：Push Descriptors。** IBL 预计算是一次性操作（`begin_immediate()` / `end_immediate()` scope 内），使用 Push Descriptors 零分配、代码简洁。

> **项目例外**：常规渲染管线不使用 Push Descriptors（见「Descriptor Set 管理方式」章节评估），但 IBL 预计算完全隔离在 `ibl*.cpp` 内部，不参与常规帧循环，不影响全局 descriptor 管理一致性。此例外仅适用于一次性 init 阶段的 compute dispatch。

衍生需求：`create_compute_pipeline()` 的 pipeline layout 需要支持传入自定义 descriptor set layout 数组，其中 set 0 标记为 push descriptor set（`VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT`）。`ComputePipelineDesc` 需要 `descriptor_set_layouts` 字段。

#### push_descriptor_set API

| 方案 | 说明 |
|------|------|
| A. 轻量封装 | 直接包装 `vkCmdPushDescriptorSet`（Vulkan 1.4 核心） |
| B. 高层封装 | Builder / helper 类构造 `VkWriteDescriptorSet` |

**选择 A。** 签名：`void push_descriptor_set(VkPipelineLayout layout, uint32_t set, std::span<const VkWriteDescriptorSet> writes) const;`

IBL 是唯一消费者，一次性 init 代码，过度封装无益。与 CommandBuffer 其他方法（轻量包装 Vulkan API）风格一致。

排除 B：IBL 每个 dispatch 绑定不同数量和类型的 image（sampled / storage），高层封装参数多或需多个重载，不如直接构造 `VkWriteDescriptorSet` 清晰。

> **Vulkan 类型例外**：`push_descriptor_set` 的参数使用 `VkPipelineLayout` 和 `VkWriteDescriptorSet`，这是 CommandBuffer 在 RHI 层中使用 Vulkan 类型的正常行为。上层（IBL 在 framework 层）通过此方法传入 Vulkan 类型——这与 IBL Push Descriptors 例外一致（见上文），仅限一次性 init 操作。

### IBL Barrier 管理

IBL 预计算在单个 `begin_immediate()` / `end_immediate()` scope 内执行，所有 GPU 命令（upload + 多次 compute dispatch）录入同一 command buffer。各步骤之间用 `vkCmdPipelineBarrier2` 手动管理资源转换。

线性流水线（equirect → cubemap → irradiance / prefiltered → SHADER_READ_ONLY_OPTIMAL），每步之间的 barrier 简单明确，不需要 Render Graph 参与。

**DeferredCleanup 模式**：compute dispatch 引用的临时对象（pipeline、image view、sampler）在 command buffer 执行期间不得销毁（Vulkan spec 要求）。各私有方法将清理 lambda 推入 `DeferredCleanup` 向量，`init()` 在 `end_immediate()`（含 `vkQueueWaitIdle`）后统一执行。可立即销毁的对象（`VkShaderModule`、`VkDescriptorSetLayout`——不在 spec 的 pending-state 引用列表中）仍就地销毁。

最终产物转到 `SHADER_READ_ONLY_OPTIMAL` 后注册到 Set 1 bindless 数组。

### cubemaps[] 声明时机

Step 3 修改 Set 1 layout（新增 cubemap binding）时，同步在 `bindings.glsl` 中声明 `cubemaps[]`。Layout 和 shader 声明严格同步，避免中间状态。`PARTIALLY_BOUND` 保证声明但未使用（Step 3 时尚无实际 cubemap 注册）不会有验证层报错。

**2. 运行时消费（shader 采样预计算产物）**：走现有 bindless 体系。

**Cubemap 走 Set 1 bindless**（见「Bindless 纹理数组」章节），BRDF LUT 走 Set 1 binding 0（普通 2D 纹理）。

GlobalUBO 新增 IBL 字段（**Step 6 添加**，Step 6.5 forward.frag 消费验证，Step 7 重构到 `lighting.glsl`）：

```cpp
uint32_t irradiance_cubemap_index;    // cubemaps[] 下标
uint32_t prefiltered_cubemap_index;   // cubemaps[] 下标
uint32_t brdf_lut_index;             // textures[] 下标
uint32_t prefiltered_mip_count;      // roughness → mip level 映射
float ibl_rotation_sin;              // IBL 水平旋转 sin(yaw)
float ibl_rotation_cos;              // IBL 水平旋转 cos(yaw)
```

字段和 `bindings.glsl` 布局是一体的（C++ struct 必须与 GLSL uniform block 严格匹配），Step 6 产出 IBL 资源后立即更新 UBO 布局，避免 bindless index 无处存放的中间状态。Step 6.5 验证 IBL 数据正确性（forward.frag 内联 Split-Sum），Step 7 将内联代码重构到 `lighting.glsl` 并升级直射光为 Cook-Torrance。

Shader 端通过 `cubemaps[global.irradiance_cubemap_index]` 和 `textures[global.brdf_lut_index]` 访问。

DescriptorManager 新增 cubemap 管理 API（与现有 `register_texture()` / `unregister_texture()` 对称）：

```cpp
BindlessIndex register_cubemap(ImageHandle cubemap, SamplerHandle sampler);
void unregister_cubemap(BindlessIndex index);
```

### Skybox Pass

IBL 预计算的中间 cubemap 保留用于天空渲染（M1 规划的"静态 HDR Cubemap 天空"）。

**选择：独立 Skybox Pass**

| 方案 | 说明 |
|------|------|
| A. 集成在 Forward Pass 内部 | 先 EQUAL 画几何，再切 pipeline 画天空 |
| B. 独立 RG pass | Forward Pass 之后、Tonemapping 之前 |

**选择 B。** Forward Pass 使用 `depth compare EQUAL`，skybox 的 fragment depth（Reverse-Z 下为 0.0）在 depth buffer 中没有对应条目，EQUAL 100% 拒绝。如果集成在 Forward Pass 内部，需要在同一 pass 中切换 pipeline（EQUAL → GREATER_OR_EQUAL），混合两个不同的渲染关注点。

Skybox Pass 渲染到 resolved 1x `hdr_color`，读 resolved 1x `depth`：

- **深度测试**：`GREATER_OR_EQUAL + depth write OFF`。Reverse-Z 下 clear value = 0.0，skybox depth = 0.0，只有无几何覆盖的像素（depth == 0.0）通过 `0.0 >= 0.0`
- **不需要 MSAA**：天空没有锯齿问题（平滑渐变），在 resolved buffer 上渲染即可
- **资源声明**：`hdr_color` Write + `depth` Read

#### Skybox Vertex Shader

| 方案 | 说明 |
|------|------|
| A. 复用 `fullscreen.vert` | `gl_FragCoord` + `inv_view_projection` 在 FS 中反推世界方向 |
| B. 独立 `skybox.vert` | VS 计算世界方向作为 varying，FS 纯采样 |

**选择 B。** 语义更清晰——VS 负责方向计算，FS 纯 cubemap 采样，职责分离干净。Fullscreen triangle + VS 方向计算在透视校正插值下与逐像素反投影数学一致。不依赖 `screen_size`。

排除 A：虽然少一个文件更简洁，但 `skybox.frag` 依赖 `screen_size` + `inv_view_projection` 反投影，语义耦合更重——改变投影参数时需同步关注 FS 的正确性。

#### Skybox Fragment Depth

| 方案 | 说明 |
|------|------|
| A. VS 输出 `gl_Position.z = 0.0` | NDC depth = 0.0，Early-Z 正常工作 |
| B. FS 写 `gl_FragDepth = 0.0` | Late-Z，被几何覆盖的天空像素也要执行 FS |

**选择 A。** Reverse-Z 下 depth clear = 0.0，`gl_Position = vec4(pos.xy, 0.0, 1.0)` 使 NDC depth = 0.0，`GREATER_OR_EQUAL` 通过。Early-Z 正常工作——被几何覆盖的像素（depth > 0.0）在 FS 执行前被拒绝。

排除 B：写 `gl_FragDepth` 强制 Late-Z，被几何覆盖的天空像素也要执行 FS（cubemap 采样），白白浪费性能。

Shader 文件：`skybox.vert`（方向计算 + depth = 0.0）+ `skybox.frag`（IBL 旋转 + cubemap 采样）。`skybox.frag` 对方向 `rotate_y()` 后再采样 cubemap，与 forward.frag 中 IBL 采样使用相同旋转。

```cpp
skybox_pass_.record(rg, {
    {ctx.hdr_color, Write, ColorAttachment},
    {ctx.depth,     Read,  DepthAttachment},
});
```

**引入时机**：Step 6（IBL Pipeline 完成，中间 cubemap 就绪）。

**演进**：M2 Bruneton 大气散射替换时，直接换掉 SkyboxPass 类，其余渲染管线不受影响。

### IBL 资源所有权

**选择：IBL 模块自管理全部资源**

IBL 模块在 `init()` 中创建的所有 image（equirect 输入、中间 cubemap、irradiance、prefiltered、BRDF LUT）由 IBL 模块自身持有并管理。

| 方案 | 说明 |
|------|------|
| A. IBL 模块自管理 | IBL 类持有所有 ImageHandle，`destroy()` 先 unregister bindless 再 destroy image |
| B. Renderer 管理 | IBL 的 `init()` 返回 handle 列表，Renderer 持有 |
| C. 混合 | IBL 持有非共享资源，Renderer 持有被其他 pass 引用的资源 |

**选择 A。** IBL 模块知道自己创建了什么，自己清理，内聚性最好。Renderer 调用 `ibl_.destroy()` 即可。

排除 B：将 IBL 内部资源的所有权泄漏给 Renderer，增加 Renderer 的管理负担。

排除 C：中间 cubemap 虽然被 SkyboxPass 使用，但 SkyboxPass 仅通过 bindless index（`GlobalUBO.skybox_cubemap_index`）间接访问，不持有 handle——所有权仍在 IBL 模块内。

**资源生命周期：**

| 资源 | 创建时机 | 销毁时机 | 说明 |
|------|---------|---------|------|
| Equirect 输入 image | `init()` 开头 | `init()` 末尾 | 转 cubemap 后立即销毁，不出 init scope |
| 中间 cubemap（全 mip） | `init()` 内 | `init()` 内（预滤波后剥离） | `strip_skybox_mips()` 替换为 mip-0-only 副本 |
| Skybox cubemap（mip 0） | `init()` 内（剥离后） | `destroy()` | 保留用于 Skybox 渲染 |
| Irradiance cubemap | `init()` | `destroy()` | 注册到 Set 1 binding 1 |
| Prefiltered cubemap | `init()` | `destroy()` | 注册到 Set 1 binding 1 |
| BRDF LUT | `init()` | `destroy()` | 注册到 Set 1 binding 0 |

**销毁顺序**：`IBL::destroy()` 先 `unregister_cubemap()` / `unregister_texture()` 注销 bindless 条目，再 `destroy_image()` 销毁底层资源。确保 descriptor 不指向已销毁 image。
