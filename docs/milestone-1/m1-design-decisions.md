# Milestone 1：设计决策

> M1 各架构组件的设计选择与理由。接口定义见 `m1-interfaces.md`，长远架构目标见 `../project/architecture.md`，M1 功能范围见 `milestone-1.md`。

---

## 设计原则

既不过度设计（避免在项目初期花太多时间在框架上），也不欠缺考虑（避免以后吃亏需要推翻重来）。每个组件选择的实现级别都预留了向长远目标演进的通道。

---

## Vulkan 1.4 核心特性

M1 使用 Vulkan 1.4，以下 1.3+ 提升为核心的特性从一开始采用：

### Dynamic Rendering

使用 `vkCmdBeginRendering` / `vkCmdEndRendering` 替代传统的 VkRenderPass + VkFramebuffer。

**为什么采用：** 不需要创建和管理 VkRenderPass 和 VkFramebuffer 对象，attachment 在录制时内联指定，与 Render Graph 天然契合。代码量显著减少。

**移动端影响：** TBDR 架构的 tiling 优化可能受影响，但这是 RHI 内部实现细节，移动端适配时可在 RHI 层内替换，不影响上层代码。

### Synchronization2

使用 `vkCmdPipelineBarrier2` 和 `VkImageMemoryBarrier2` 替代旧的 barrier API。

**为什么采用：** 新 API 将 stage 和 access 放在同一结构体中，语义更清晰，更不容易写错组合。Render Graph 的 barrier 自动插入直接受益。

### Extended Dynamic State

Viewport、scissor、cull mode、depth test/write/compare 等状态设为动态，不烘焙进 pipeline。

**为什么采用：** 减少 pipeline 数量（阴影 pass 和主 pass 可共享 pipeline），pipeline 创建更简单。

---

## Render Graph

**选择：手动编排的 Pass 列表 + barrier 自动插入**

Pass 的执行顺序由代码手动定义（一个有序列表），但每个 Pass 声明自己的输入输出资源，系统根据这些声明自动插入 barrier 和管理资源状态。不做自动拓扑排序，不做资源别名优化。

**为什么不做完整自动化 Render Graph：** 完整系统（依赖图拓扑排序、资源别名分析、barrier 合并优化）开发量大，在还没有任何 pass 跑起来之前要先花相当长时间在基础设施上。

**为什么不完全手动管理：** 每个 pass 自己管理 barrier 和资源状态，后续维护痛苦会指数增长。

**M1 的优势：** pass 数量约 10 个，手动排序完全可控，同时获得了 barrier 自动化这个最大的减痛点。

**升级路径：** 每个 pass 已声明输入输出，后续加入拓扑排序和资源别名分析时，已有的 pass 声明格式不需要修改。

### 渐进式能力建设

Render Graph 的功能按需引入，不提前建设未使用的能力：

| 能力 | 引入阶段 | 理由 |
|------|---------|------|
| Barrier 自动插入 | 阶段二 | 核心价值，从第一天就需要 |
| 资源导入（import） | 阶段二 | 外部创建的资源（swapchain image、vertex buffer 等）导入 RG 追踪状态 |
| Managed 资源管理 | 阶段三 | 首批 RG 管理的资源（Depth/Normal/HDR Color Buffer）出现于阶段三 |
| Temporal 资源管理 | 阶段五 | 首个 temporal pass（SSAO temporal filter）出现于阶段五 |

阶段二的 RG 只管 barrier 和状态追踪，资源由外部创建后导入。这不影响后续扩展——pass 声明输入输出的接口从阶段二就固定下来，managed/temporal 是对 RG 内部的增量添加，已有 pass 不需要修改。

#### Managed 资源管理

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

实现：`use_managed_image()` 内部等效于 `import_image(handle, UNDEFINED, UNDEFINED)`，零帧间状态，不需要 final barrier。

**Temporal 资源兼容性：** 阶段五引入的 temporal 资源（需要保留上帧内容）有独立机制（`get_history_image()` 返回独立 `RGResourceId`），不走 managed 资源的 UNDEFINED 路径。

**现有 depth buffer 迁移：** 阶段三将阶段二手动管理的 depth buffer 迁移为 managed 资源，删除手动创建/销毁代码。

接口定义详见 `m1-interfaces.md`「Managed 资源 API」。

#### Backing Image 即时查询

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

#### MSAA Resolve 策略

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

#### Depth Resolve 模式

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

#### Temporal 资源管理（阶段五引入）

```cpp
// RGResourceDesc 中添加 is_temporal 标记
// 获取 temporal 资源的历史帧版本
ImageHandle get_history_image(RGResourceId id);
```

### 帧间生命周期

RG 每帧重建：`clear()` → `import_*()` → `add_pass()` → `compile()` → `execute()`。这是**长期方案**，不是过渡性设计。每帧重建确保 pass 列表和资源引用始终与当前帧状态一致，避免帧间残留状态带来的 bug。

### Command Buffer 传递

`execute(CommandBuffer& cmd)` 接收外部传入的 command buffer，RG 不持有任何 Vulkan 同步资源（fence、semaphore、command pool）。同步由外部帧循环管理，RG 专注于 pass 编排和 barrier 插入。

### import_image 初始 layout

`import_image()` 增加 `VkImageLayout initial_layout` 参数，调用方显式传入资源在导入时的当前 layout。RG 以此为起点计算 layout transition。

### import_image 最终 layout

`import_image()` 同时接受 `final_layout` 参数（必填，无默认值）。RG 在 `execute()` 结束后，为每个 imported image 插入从「最后使用 layout」到 `final_layout` 的 barrier。

**设计理由：** 帧间存活的 imported resource（swapchain image、depth buffer 等）需要在帧末处于确定的 layout，以对应下一帧 import 时的 `initial_layout`。`final_layout` 让 RG 自动管理这个转换，避免调用方手动插入 barrier。

**为什么不用默认值：** 所有 imported image 都是帧间存活的（阶段二没有 transient 资源），强制指定 `final_layout` 避免遗漏导致的 layout 不匹配 bug。

**用法示例：** swapchain image 传 `PRESENT_SRC_KHR`，depth buffer 传 `DEPTH_ATTACHMENT_OPTIMAL`。

### Barrier 粒度

阶段二只处理 image layout transition。Buffer barrier 在阶段三有 GPU 写 buffer 需求时再加——扩展只需在 barrier emit 处加一个 if/else 分支，约 10 行代码。

### Barrier 计算策略

`compile()` 根据 pass 声明的 `RGResourceUsage` 推导 barrier 参数。从 `(RGAccessType, RGStage)` 到 `(VkImageLayout, VkPipelineStageFlags2, VkAccessFlags2)` 的映射以函数形式封装，按需实现实际用到的组合，未实现的组合 assert 拦截。后续阶段加新组合只需加 case。

**Hazard 覆盖**：barrier 在 layout 变化或存在数据竞争时发出。四种 hazard 类型中，RAR（连续读取）无需 barrier，其余三种均处理：RAW/WAW 通过检测上一次 access 包含写 flag 触发；WAR 通过检测当前 access 包含写 flag 且资源此前已被访问触发。这确保 M2 引入 compute pass（storage image 在 GENERAL layout 下先读后写）时不会遗漏 execution dependency。

**RG 不管 loadOp/storeOp。** Pass 在 execute 回调中自己构造 `VkRenderingInfo` 时决定 loadOp/storeOp。RG 只管 layout transition 和内存依赖，保持职责单一。

#### Compute Pass 资源用途推导

Step 3 新增 compute pass 支持后，`resolve_usage()` 补充 Compute case 的映射：

| Access × Stage | Layout | Access Flags | 用途 |
|---------------|--------|-------------|------|
| Read + Compute | `SHADER_READ_ONLY_OPTIMAL` | `SHADER_SAMPLED_READ_BIT` | sampled image |
| Write + Compute | `GENERAL` | `SHADER_STORAGE_WRITE_BIT` | storage image |

覆盖 M1 全部 compute pass 需求（IBL 预计算的 sampled input + storage output）。不实现 ReadWrite Compute（`imageLoad` 只读 storage）——M1 无此需求，后续阶段按需加 case。

### Debug Utils 集成

RG `execute()` 自动为每个 pass 插入 `vkCmdBeginDebugUtilsLabelEXT` / `vkCmdEndDebugUtilsLabelEXT`，使用 pass 注册时的名称。RenderDoc 和 GPU profiler 中按 pass 名称分组显示 GPU 工作。

CommandBuffer 新增 `begin_debug_label(name, color)` / `end_debug_label()` 方法封装此功能。

### 资源引用方式

Pass 声明资源使用时通过 typed handle（`RGResourceId`）而非字符串引用资源。`declare_resource()` / `import_resource()` 返回 `RGResourceId`，pass 持有并传递这些 ID。每个资源使用声明（`RGResourceUsage`）包含 handle、access type（READ / WRITE / READ_WRITE）和 stage，合并为单个列表传入 `add_pass()`，不区分 inputs/outputs 参数。资源名称仅用于调试（debug name）。

**为什么不用字符串：** 拼写错误不被编译期捕获，且字符串查找不够优雅。typed handle 提供编译期类型安全和零运行时查找开销。

### READ_WRITE 语义

`READ_WRITE` 表示**同一张 image 在同一帧内同时读写**，典型场景是 depth attachment（depth test 读 + depth write 写）。RG 为此生成的 barrier 使用 `DEPTH_ATTACHMENT_OPTIMAL`（同时允许读写）。

**与 temporal 的区分：** 阶段五引入 temporal 资源后，历史帧数据通过 `get_history_image()` 获取独立的 `RGResourceId`，当前帧和历史帧是**两个不同的资源**，各自声明 `READ` 或 `WRITE`。`READ_WRITE` 不用于 temporal 场景。

### Render Graph 接管范围

Render Graph 接管 acquire image 和 present 之间的**所有** GPU 工作。帧循环变为：acquire image → CPU 准备 → RG compile → RG execute → submit → present。Swapchain image 作为 imported resource 导入 RG，RG 管理其渲染期间的 layout transition。

### ImGui 作为 Render Graph Pass

ImGui 注册为 Render Graph 中的最后一个 pass（独立 pass），声明对最终 color attachment 的读写。`begin_frame()` 在 CPU 准备阶段调用（RG execute 之前），`render()` 在 pass 的 execute 回调中调用。

**为什么不在其他 pass 内部附加：** ImGui 作为独立 pass 更透明，RG 统一管理所有渲染工作。

### Pass 类设计

阶段二只有一两个 pass，直接在 Render Graph 的 lambda 回调中编写渲染逻辑。阶段三 pass 增多后（DepthPrePass、ForwardPass、TonemappingPass），从 lambda 提取为独立的**具体类**。

#### 层归属

**所有 pass 统一放在 Layer 2（`passes/`）**，包括有 draw loop 的 pass（ForwardPass、DepthPrePass）和无 draw loop 的 pass（TonemappingPass）。

有 draw loop 的 pass 需要场景数据（meshes、materials、cull_result），这些类型全部定义在 Layer 1（`framework/mesh.h`、`framework/material_system.h`、`framework/scene_data.h`），依赖方向 Layer 2 → Layer 1 合法。数据实例从 Layer 3（Renderer）通过 `FrameContext` 向下传递，不产生反向依赖。

#### FrameContext

`FrameResources`（阶段二设计，只含 RG 资源 ID）扩展为 `FrameContext`，携带一帧渲染所需的全部上下文：RG 资源 ID + 场景数据引用 + 帧参数。定义在 Layer 1（`framework/frame_context.h`），Renderer 每帧构造并向下传递给所有 pass。

**服务引用不放入 FrameContext**：长期不变的服务引用（ResourceManager、DescriptorManager、ShaderCompiler 等）由 Pass 在 `setup()` 时存储指针，lambda 通过 `this` 访问。FrameContext 是纯每帧数据（资源 ID + 场景数据引用 + 帧参数），不做 service locator，避免混合两种职责导致退化为 God Object。

#### Descriptor Set 绑定方式

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

#### 具体类 vs 虚基类

| 方案 | 说明 |
|------|------|
| A. 虚基类 `RenderPass` | 标准多态，Renderer 持有 `vector<unique_ptr<RenderPass>>` |
| B. 具体类，不用虚函数 | 每个 Pass 是独立类，Renderer 持有具体类型成员 |
| C. 继续 lambda | 不提取类 |

**选择 B。** 阶段三只有 2-3 个 pass，虚函数多态收益有限。具体类让类型更明确、编译期可见。阶段四 pass 数量增长后（shadow、SSAO 等）再引入多态基类——届时有更多样本验证接口设计。

#### 方法职责分离

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

#### MSAA Pipeline 重建

MSAA 切换需要重建 pipeline（`rasterizationSamples` 烘焙在 pipeline 中），但 `setup()` 是一次性初始化，不适合承载此职责。

| 方案 | 说明 |
|------|------|
| A. 新增 `on_sample_count_changed(uint32_t)` 方法 | 语义清晰，只有受影响的 pass 实现 |
| B. 扩展 `on_resize()` 为 `on_config_change(RenderConfig)` | 统一处理多种配置变化 |
| C. `destroy()` + `setup()` 重新初始化 | 不增加新方法 |

**选择 A。** 方法名描述"发生了什么"（sample count 变了），pass 内部决定如何响应（重建 pipeline）。排除 B：语义模糊，不关心 MSAA 的 pass（如 TonemappingPass）也要接收不属于自己的配置变化。排除 C：`setup()` 还做了编译 shader、存储服务指针等工作，MSAA 切换时完全不需要重做。

只有 pipeline 中 `rasterizationSamples` 依赖 sample count 的 pass 需要实现此方法——ForwardPass、DepthPrePass。SkyboxPass 和 TonemappingPass 不需要——SkyboxPass 渲染到 resolved 1x `hdr_color`，`rasterizationSamples` 永远为 1；TonemappingPass 始终 1x，处理 resolved 产物。

#### Pipeline 创建与热重载

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

#### Attachment Format 处理

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

#### 显式 `destroy()`

遵循项目约定（显式 `destroy()` 管理 Vulkan 对象生命周期），每个 Pass 持有 pipeline 和私有资源，需显式销毁。

方法约定详见 `m1-interfaces.md`「Pass 类约定」。

> **评估结论（阶段四决策）**：M1 全程不引入虚基类，推迟到 M2 评估。阶段四新增 ShadowPass 后共 5 个 pass（DepthPrePass、ForwardPass、SkyboxPass、TonemappingPass、ShadowPass），4 种不同方法集。统一接口需要大量空默认实现或多个子列表，净增复杂度。阶段五再增 3 个 compute pass 到 8 个时多态收益更大，但 M1 场景下显式调用仍然可读且可维护。

**缺席 pass 无需处理机制**：选择了具体类（非虚基类），`Renderer::render()` 显式调用每个存在的 pass。尚未实现的 pass 不在代码中，无需任何"缺席"处理机制。

### Swapchain Image 导入 RG

Swapchain image 由 `VkSwapchainKHR` 持有，不在 ResourceManager 的资源池中。为保持 `import_image()` 接口统一使用 `ImageHandle`，ResourceManager 新增**外部 image 注册**能力：

- `register_external_image(VkImage, VkImageView, ImageDesc)` → `ImageHandle`：分配一个 slot，记录 VkImage/VkImageView/desc，`allocation` 设为 null（不持有 VMA 内存）
- `unregister_external_image(ImageHandle)`：释放 slot，递增 generation，**不调用** `vmaDestroyImage`
- `destroy_image()` 检查 `allocation` 是否为 null，防止误销毁外部资源

Swapchain 初始化时注册所有 swapchain image 获得 `ImageHandle` 数组，每帧用 `swapchain_handles_[image_index]` 调用 `import_image()`。Resize 时先 unregister 后重新注册。

**为什么不给 RG 加 VkImage 重载：** `get_image()` 返回 `ImageHandle`，pass lambda 通过 `resource_manager.get_image(handle)` 统一获取 VkImage/VkImageView。如果 swapchain image 没有 ImageHandle，`get_image()` 接口断裂，pass lambda 需要两条代码路径。外部注册保持了整个 Handle 体系的一致性。

### RG 与 ResourceManager 的关系

RG 在 `compile()` 时需要 `VkImage` 构造 `VkImageMemoryBarrier2`，在 `compile()` / `execute()` 时通过 `ResourceManager::get_image(handle)` 解析。因此 RG 构造/初始化时接收 `ResourceManager*` 引用。

依赖方向正确：RG（framework 层）→ ResourceManager（rhi 层）。这与 ResourceManager 自身接收 `Context*` 是同一模式。

### RG Barrier 的 Image Aspect 推导

RG 插入 `VkImageMemoryBarrier2` 时需要 `subresourceRange.aspectMask`（depth image 用 `DEPTH_BIT`，color image 用 `COLOR_BIT`）。RG 通过 `resource_manager.get_image(handle).desc.format` 获取资源格式，再由 `aspect_from_format()` 推导 aspect。

`to_vk_format(Format)` 和 `aspect_from_format(Format)` 从 `resources.cpp` 的 static 函数提取为 `rhi/types.h` 中的公共 inline 函数，与 `Format` 枚举定义在同一文件中，framework 层通过 `#include <himalaya/rhi/types.h>` 使用。

### Framework 层 Vulkan 类型使用

Render Graph 和 ImGui Backend 是 framework 层中允许直接使用 Vulkan 类型（VkImageLayout 等）的**明确例外**，不是暂时性的实现。

**理由：** RG 本质是 Vulkan barrier 管理器，用 VkImageLayout 比自造枚举再映射更直观且不易出错。ImGui Backend 的 Vulkan backend 天然需要 Vulkan 类型。其他 framework 模块的公开接口仍然不使用 Vulkan 类型。

详见 `../project/architecture.md` 的「Framework 层 Vulkan 类型例外」章节。

---

## 资源管理与描述符

**选择：Bindless 纹理 + Push Constant 混合方案**

- **材质纹理**：Bindless（创建全局 descriptor array，所有纹理注册进去用 index 访问）
- **Per-draw 动态数据**：Push Constant（模型矩阵、材质 index 等每次绘制变化的数据）

不使用传统的 per-material descriptor set。

**为什么 Bindless 从一开始做：** 这是一个早期投资但回报贯穿整个项目——材质系统简洁，不需要 per-material descriptor set 的分配和管理，以后做 Instancing、Shadow Atlas 等动态访问不同纹理的功能时无缝支持。Vulkan 对 bindless 支持好（VK_EXT_descriptor_indexing）。

**为什么不用传统 per-material descriptor set：** descriptor set 的分配和管理会随材质数量增长变得繁琐，频繁切换有 CPU/GPU 开销，以后做 Instancing 需要改造。

**注意事项：** 需要想清楚 descriptor 更新策略、纹理加载后注册到 array 的流程、shader 里的间接访问模式。对 GPU 调试不太友好（所有东西都是 index），需配合 debug name 使用。

### Descriptor Set 三层架构

三个 Descriptor Set 按资源生命周期分层，每层有清晰的语义边界：

| Set | 内容 | 生命周期 | 更新频率 |
|-----|------|---------|---------|
| 0 | 全局 Buffer（GlobalUBO + LightBuffer + MaterialBuffer） | per-frame 双缓冲 | 每帧 memcpy |
| 1 | 持久纹理资产（bindless 材质纹理 + cubemap） | 场景加载 → 卸载 | 加载时写入 |
| 2 | 帧内 Render Target（后处理 / 屏幕空间效果的中间产物） | init → destroy | resize / MSAA 切换时更新 |

均使用传统 Descriptor Set 分配（非 Push Descriptors）。Set 0 per-frame 分配 2 个（2 frames in flight），Set 1 和 Set 2 各分配 1 个长期持有。共 4 个 descriptor set：Set 0 × 2 + Set 1 × 1 + Set 2 × 1。

所有 pipeline（场景渲染、后处理、屏幕空间效果）使用统一的 pipeline layout `{Set 0, Set 1, Set 2}`，确保切换 pipeline 时所有 set 绑定保持有效。后处理 pass 不使用 Set 1 但 layout 中包含以保持兼容性。

### Descriptor Pool 分离

三个 Set 使用**独立的 Descriptor Pool**：

- **Set 0 Pool**（普通 pool）：容纳 2 UBO + 4 SSBO，maxSets = 2。分配 Set 0 × 2（per-frame）
- **Set 1 Pool**（`VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT`）：容纳 4352 COMBINED_IMAGE_SAMPLER（2D 纹理 4096 + Cubemap 256），maxSets = 1。分配 Set 1 × 1
- **Set 2 Pool**（普通 pool）：容纳 8 COMBINED_IMAGE_SAMPLER（M1 预留 8 个 render target binding），maxSets = 1。分配 Set 2 × 1

**为什么分离：** `UPDATE_AFTER_BIND_BIT` 加在 pool 上会影响从该 pool 分配的所有 set。Set 1 需要此 flag（bindless 纹理在使用时更新），Set 0 和 Set 2 不需要，分离后职责隔离更清晰。

Set 2 不需要 `UPDATE_AFTER_BIND`——render target descriptor 只在 resize/MSAA 切换时更新，此时已 `vkQueueWaitIdle()` 确保 GPU 空闲。

ImGui 专用 Descriptor Pool 独立于上述三个 pool（已在阶段一实现）。

### Set 0 Per-frame Buffer 策略

- **GlobalUBO × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **LightBuffer × 2**（per-frame）：`CpuToGpu` memory，host-visible persistent mapped
- **MaterialBuffer × 1**：场景加载后一次性创建

Set 0 descriptor 初始化时通过 `vkUpdateDescriptorSets` 写一次（per-frame set 各指向自己的 buffer），之后每帧只 `memcpy` buffer 内容，不需要再 update descriptor。

**Push Descriptors 评估后不采用：** Vulkan 1.4 核心的 Push Descriptors 可以省去 Set 0 的 descriptor pool 分配（约 30 行初始化代码），但每帧需要构造 VkWriteDescriptorSet（约 15 行），总代码量几乎打平。且每个 pipeline layout 只能有一个 push descriptor set，引入了第二种描述符管理模式，增加认知复杂度。收益不足以抵消一致性的损失。

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

### DescriptorManager 职责

`DescriptorManager`（`rhi/descriptors.h/cpp`）集中管理所有 descriptor 相关资源：

- **持有全局 Descriptor Set Layout**（Set 0、Set 1、Set 2），提供 `get_global_set_layouts()` 方法供 pipeline 创建时使用，确保所有 pipeline 使用一致的 layout
- **管理 Descriptor Pool** 和 **Descriptor Set 分配**（三个 pool 各自分配对应的 set）
- **管理 Bindless 数组**（Set 1：2D 纹理和 Cubemap 的注册、注销、slot 分配）
- **管理 Render Target Descriptor**（Set 2：render target binding 的写入和更新）

### Descriptor Set Layout 与 Pipeline Layout 的关系

DescriptorManager 持有全局 set layout，Pipeline 创建时通过 `GraphicsPipelineDesc::descriptor_set_layouts` 传入。每个 Pipeline 各自创建 `VkPipelineLayout`。虽然产生重复的 layout 对象（轻量元数据，开销忽略不计），但保留了灵活性——未来 compute pipeline 或特殊 pass 可能需要不同的 layout 组合。

`get_global_set_layouts()` 方法将一致性风险集中在 DescriptorManager 一处，各 pass 统一调用而非自己构造 layout 数组。

### 资源 Debug Name

`create_image()`、`create_buffer()`、`create_sampler()` 全部新增**必选** `debug_name` 参数（`std::string_view`）。内部调用 `vkSetDebugUtilsObjectNameEXT` 为 Vulkan 对象设置可读名称。

所有已有调用点回溯补上有意义的名字（如 `"depth_buffer"`、`"global_ubo_frame0"`），RenderDoc 和 Validation Layer 报错时显示这些名称。

**引入时机**：Step 2（managed 资源引入时同步实施，新增的 managed 资源和已有资源一起补齐 debug name）。

### Bindless 纹理数组

Set 1 包含两个 binding，分别管理 2D 纹理和 Cubemap：

```glsl
layout(set = 1, binding = 0) uniform sampler2D textures[];     // 2D 纹理，上限 4096
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];   // Cubemap，上限 256
```

两个 binding 均使用 `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`，固定上限。不使用 `VARIABLE_DESCRIPTOR_COUNT`——Vulkan 规定此 flag 只能设在 set 内最后一个 binding 上，两个 binding 后 binding 0 不能再用。4096 + 256 个 descriptor 的内存开销约 200-300 KB，不值得保留 `VARIABLE_DESCRIPTOR_COUNT` 的复杂性。

Cubemap 不能放入 `sampler2D[]` 数组（类型不兼容），需要独立 binding。一步到位做 bindless cubemap 数组，M1 阶段六 Reflection Probes 直接复用。

候选方案：

| 方案 | 说明 |
|------|------|
| A. Set 0 新增 binding 放 cubemap samplers | 独立 binding 最直接 |
| B. Set 1 新增 binding（bindless cubemap 数组） | 一步到位，Reflection Probes 复用 |
| C. 新的 Set 2（IBL 专用 descriptor set） | IBL 资源独立管理 |

**选择 B。** 阶段三只有 2-3 个 cubemap（irradiance + prefiltered），但 Set 1 bindless 数组为 M1 阶段六 Reflection Probes 提供零改动的扩展路径。C 增加额外 descriptor set 管理复杂度，IBL 不需要独立于其他纹理的生命周期。

> **限制**：2D 纹理 slot 上限 4096，Cubemap slot 上限 256。如果未来需要更多（不太可能在光栅化管线中发生），需要重新创建更大的 descriptor set。

**Slot 回收**：纹理销毁后，其 bindless slot 通过 free list 回收，新纹理注册时优先复用空闲 slot。回收配合 deferred deletion 确保 GPU 不再引用旧 descriptor。这支持场景切换时的纹理卸载/重载。2D 纹理和 Cubemap 的 slot 管理独立（各自的 free list + 各自的 slot 空间），`BindlessIndex` 的 `index` 值分别对应各自数组的下标。

### Set 0 Layout — 纯 Buffer

Set 0 只包含 buffer 类型的 binding，不包含任何 image/sampler 类型：

| Binding | 类型 | 内容 | 引入阶段 |
|---------|------|------|----------|
| 0 | UBO | GlobalUBO | 阶段二（已有） |
| 1 | SSBO | LightBuffer | 阶段二（已有） |
| 2 | SSBO | MaterialBuffer | 阶段二（已有） |

阶段二的 Set 0 layout 保持不变——阶段三不需要新增 Set 0 binding。Shadow map（`sampler2DArrayShadow`）归入 Set 2（render target，见下文），不占用 Set 0 binding。

### Set 1 Layout — 持久纹理资产

Set 1 bindless 数组只存放**持久纹理资产**——场景加载或初始化时创建，整个运行期间不变：

| 纹理 | Set 1 binding | 阶段 |
|------|--------------|------|
| 材质纹理（base color、normal、metallic-roughness 等） | `sampler2D[]` (binding 0) | 阶段二（已有） |
| BRDF LUT | `sampler2D[]` (binding 0) | 阶段三 |
| Lightmap | `sampler2D[]` (binding 0) | 阶段六 |
| IBL irradiance / prefiltered | `samplerCube[]` (binding 1) | 阶段三 |
| Reflection Probes | `samplerCube[]` (binding 1) | 阶段六 |

帧内 render target（AO texture、contact shadow mask、hdr_color、depth 等）不进入 Set 1，走 Set 2。

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
| 0 | `sampler2D` | hdr_color | ForwardPass | Tonemapping, Bloom, HeightFog, AutoExposure | 三 |
| 1 | `sampler2D` | depth_resolved | PrePass | SSAO, ContactShadows, HeightFog | 五 |
| 2 | `sampler2D` | normal_resolved | PrePass | SSAO | 五 |
| 3 | `sampler2D` | ao_texture | SSAOTemporalPass | ForwardPass | 五 |
| 4 | `sampler2D` | contact_shadow_mask | ContactShadowsPass | ForwardPass | 五 |
| 5 | `sampler2DArrayShadow` | shadow_map | ShadowPass | ForwardPass | 四 |
| 6 | `sampler2D` | bloom_texture | BloomUpsample | Tonemapping | 八 |
| 7 | `sampler2D` | refraction_source | HDR Copy | TransparentPass | 七 |

**Sampler 选择与所有权：** Renderer 持有 `linear_clamp` 和 `nearest_clamp` 两个后处理专用 sampler。写入 Set 2 时根据 render target 类型选择对应 sampler。Shadow map binding 5 使用硬件比较采样器。

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

## 顶点格式

**选择：统一顶点格式**

所有 mesh 使用同一种顶点格式：position (vec3) + normal (vec3) + uv0 (vec2) + tangent (vec4) + uv1 (vec2)。glTF 加载时缺失的属性填默认值（normal 填 (0,0,1)，tangent 填 (1,0,0,1)，uv 填 0）。

**为什么统一：** 所有 mesh 共享同一种 pipeline vertex input 配置，简化 pipeline 管理。M1 的标准 PBR 需要全部属性（position + normal + uv0 + tangent 用于法线贴图 TBN 矩阵，uv1 为阶段六 lightmap 预留）。

**内存代价：** 每顶点约 24 字节的额外开销（tangent + uv1），一个几万顶点的 mesh 多几百 KB，可接受。

> **潜在优化**：当引入 skinned mesh（额外需要 joint indices + joint weights 每顶点 32 字节）或大规模场景显存紧张时，可拆分为多种顶点格式 + 对应 pipeline variant。统一格式继续给标准 PBR mesh 用，新格式给特定需求。

---

## 材质系统

**选择：代码定义 + 固定数据结构**

M1 只有一种材质（标准 PBR），材质参数用固定的 `GPUMaterialData` 结构体定义（CPU 端 struct + shader 端 struct 一一对应）。不引入运行时参数描述系统（`MaterialParamType` / `MaterialParamDesc`）——shader 端必须在编译时知道 struct 布局，运行时描述只增加 CPU 端复杂度而无法带来 GPU 端灵活性。

**GPUMaterialData 定义位置**：`material_system.h`（而非 `scene_data.h`）。`scene_data.h` 中的 GPU 结构体（`GlobalUniformData`、`GPUDirectionalLight`、`PushConstantData`）是每帧/每次绘制流经渲染管线的数据——renderer-app contract。`GPUMaterialData` 则是材质系统的内部 GPU 布局：由 `MaterialSystem` 创建、写入 Material SSBO、管理生命周期，其他模块仅通过 push constant 的 `material_index` 间接访问。放在 `material_system.h` 遵循"谁拥有谁定义"原则，MaterialSystem 是唯一生产者。

**为什么不做完整数据驱动：** M1 只有标准 PBR 一种材质，复杂的数据驱动系统（材质定义文件、自动生成 shader 变体）投入产出比不高。

**升级路径（固定 stride + 多 shader 解读）：** 引入第二种着色模型（卡通、SSS 等）时，采用固定 stride 方案：所有材质 struct 填充到相同大小（当前 80 字节），每个 shader variant 在同一 binding 上定义自己的 typed struct（如 `PBRMaterial`、`ToonMaterial`），通过 `materials[material_index]` 统一寻址。descriptor、pipeline layout、寻址方式均不变。如果新模型超出当前 stride，整体提升（如 80→128 字节）。内存浪费可忽略（几百个材质 × 几十字节 = 几 KB）。

**排除的方案：** 每种模型各自 SSBO（破坏全局 Set 0 layout 一致性，descriptor 管理变复杂）；变长 buffer + byte offset 寻址（shader 失去 typed struct，需手动解包，易错）。

### 材质数据流

从阶段二开始建立完整的材质数据流：全局 MaterialBuffer SSBO（Set 0, Binding 2）存储所有材质实例数据，shader 通过 push constant 中的 `material_index` 索引。

**为什么阶段二就完整实现：** push constant 保证的最小大小是 128 字节，mat4 model 已占 64 字节，剩余空间不足以内联材质参数。全局 SSBO 是必经之路，早建立能验证 bindless 纹理采样的完整链路（shader 中 `materials[material_index].base_color_tex` 索引到 bindless 数组），后续每个 pass 直接复用。

### Per-draw 数据演进：Push Constant → Per-instance SSBO

阶段二的 `PushConstantData` 为 `mat4 model` + `uint material_index` = 68 字节，在 128 字节最低保证内。但后续阶段会引入更多 per-instance 数据：

| 阶段 | 新增数据 | 累计大小 |
|------|---------|---------|
| 阶段二 | model(64) + material_index(4) | 68 字节 |
| 阶段六 | + lightmap bindless index + UV scale/offset | ~84+ 字节 |
| M2 | + prev_model(64, motion vectors) | ~148 字节 ← 超出 128 |

**迁移计划（阶段六执行）**：引入 per-instance SSBO，将 `model`、`material_index`、lightmap 数据等搬入。Push constant 缩减为 `uint instance_id`（4 字节），shader 通过 `instance_data[instance_id]` 读取所有 per-instance 数据。这同时为 M2 的 `prev_model` 预留空间，不再受 push constant 大小限制。

---

## Shader 系统

### 编译与热重载

**选择：运行时编译 + 热重载（仅开发模式）**

Shader 以 GLSL 源码形式存在，运行时根据需要的变体组合用 shaderc 编译成 SPIR-V，编译结果缓存。修改 shader 不需要重新构建项目。

**为什么不做构建时预编译：** 开发效率优先——M1 阶段会频繁修改 shader，热重载的价值极大。

**M1 的可行性：** 变体组合很少（标准 PBR 有无法线贴图、有无阴影接收，大概几个变体），运行时编译开销可忽略。

**升级路径：** 发布时的预编译路径可以后加，开发期和发布期使用不同的 shader 加载策略。

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

## 场景数据接口

**选择：简单渲染列表**

渲染器的输入就是几个数组：mesh 实例数组（mesh 引用 + 材质引用 + 变换矩阵）、光源数组、探针数组、相机结构体。场景加载后填充这些数组，渲染器每帧消费。

**为什么不做 ECS 或 Scene Graph：** M1 是渲染器演示而非引擎，不需要游戏对象管理系统。

**长远兼容性：** 这个接口就是渲染器的"合同"——不管上层以后用什么方式管理场景（ECS、Scene Graph、自定义），最终喂给渲染器的都是这些数组。接口设计不需要以后改。

---

## Temporal 数据管理

**选择：手动 Double Buffer 管理**

需要历史数据的 pass 自己维护一对 buffer，每帧手动交换 current/previous 的引用。在 pass 声明里标记"这个资源需要历史帧"，系统帮管理交换。

**为什么不做更复杂的方案：** M1 只有 SSAO 一个 temporal pass（加上自动曝光的 1×1 buffer），复杂度完全可控。

**升级路径：** M2 引入 SSR、SSGI、FSR 后 temporal pass 增多，可在 Render Graph 内建 temporal 资源支持（自动管理 double buffer 和帧间切换）。

---

## ImGui 集成

**选择：Framework 层集成，直接使用 Vulkan 类型**

ImGui backend（初始化、每帧渲染循环、销毁）放在 `framework/` 层（`imgui_backend.h/cpp`），调试面板内容由 `app/` 层构建。

### Framework 层使用 Vulkan 类型（ImGui）

ImGui 的 Vulkan backend 天然需要 `VkDevice`、`VkInstance`、`VkCommandBuffer` 等。这与 Render Graph 一样属于 framework 层的**明确例外**——详见上方「Framework 层 Vulkan 类型使用」章节和 `../project/architecture.md` 的「Framework 层 Vulkan 类型例外」。

### Dynamic Rendering

ImGui Vulkan backend 配置为 Dynamic Rendering 模式，不使用 `VkRenderPass`。

阶段一中 ImGui 在与场景相同的 dynamic rendering pass 中渲染。阶段二引入 Render Graph 后，ImGui 注册为 RG 中的独立 pass（见上方「Render Graph — ImGui 作为 Render Graph Pass」章节）。

### Descriptor Pool

ImGui 使用专用 Descriptor Pool，与渲染器自身的 descriptor 管理完全隔离。

### CommandBuffer::handle()

为 `CommandBuffer` wrapper 添加 `handle()` 方法暴露底层 `VkCommandBuffer`，供 ImGui 等第三方库直接录制命令。

### 调试面板

**数据指标**

| 数据 | 来源 |
|------|------|
| FPS + 帧时间 | `FrameStats` 采样器（1 秒周期结算，取平均值） |
| 1% Low FPS + 帧时间 | `FrameStats` 采样器（周期内最慢 1% 帧的平均值） |
| VRAM 占用 | `Context::query_vram_usage()`（基于 `vmaGetHeapBudgets()`，`VK_EXT_memory_budget`），不做 fallback |
| GPU 名称 | `Context::gpu_name`（init 时缓存自 `vkGetPhysicalDeviceProperties()`） |
| 窗口分辨率 | `Swapchain::extent` |

**运行时控件**

| 控件 | 作用 |
|------|------|
| VSync | 切换 `Swapchain::vsync`，触发 swapchain 重建（FIFO / MAILBOX） |
| Log Level | 运行时切换 `spdlog` 日志级别 |

### Docking

启用 `ImGuiConfigFlags_DockingEnable`，支持 ImGui 窗口停靠布局，为后续多面板调参做准备。

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

接口定义见 `m1-interfaces.md`。

---

## 配置与调参系统

**选择：ImGui 面板 + 配置结构体**

各个参数统一放在几个结构体里（ShadowConfig、SSAOConfig、PostProcessConfig 等），ImGui 直接操作这些结构体的字段。不做正式的参数注册系统。

**M1 原则：** 能调就行，不需要优雅。

**升级路径：** 需要序列化（保存/加载配置）或参数热重载时再做正式系统。

---

## 深度缓冲与精度

### 格式

**D32Sfloat**（32-bit 浮点深度，无 stencil）。M1 不需要 stencil 操作，纯深度格式能获得更好的深度精度。对后续 CSM 和屏幕空间效果（SSAO、contact shadows）的深度采样更有利。MSAA depth buffer 沿用 D32Sfloat 不变（Reverse-Z 需要浮点深度才能发挥精度分布优势；D24UnormS8 是定点格式且浪费 8-bit stencil，带宽与 D32Sfloat 相同；D16Unorm 精度不足）。

### Reverse-Z

从阶段二起启用 Reverse-Z，避免事后修改所有 depth 相关代码：

| 参数 | 值 |
|------|-----|
| Near plane | 1.0 |
| Far plane | 0.0 |
| 投影矩阵 | 自定义 reverse-Z perspective |
| Depth clear value | 0.0f |
| Depth compare op | `VK_COMPARE_OP_GREATER` |

**长期影响**：所有 depth 相关 pass（CSM、SSAO、contact shadows、depth prepass）统一使用 reverse-Z 约定。远处深度精度显著提升（浮点在 0 附近精度最高）。

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
#define DEBUG_MODE_AO                7
#define DEBUG_MODE_SHADOW_CASCADES   8   // 阶段四新增
```

TonemappingPass 检查 `debug_render_mode >= DEBUG_MODE_PASSTHROUGH_START` 决定是否 passthrough。

**排列约定**：HDR 模式（光照分量隔离）排在 `PASSTHROUGH_START` 之前，passthrough 模式追加到末尾。新增 HDR 模式插入并递增 `PASSTHROUGH_START`，新增 passthrough 模式直接追加。HDR 模式是有限集合（Direct / Specular / IBL / Indirect），passthrough 模式会随阶段持续增长。

C++ 侧使用对应 enum，DebugUI 下拉列表用 `ImGui::Separator()` 按类别分组显示，不影响编号。

### Forward Shader 跨阶段演进

forward.frag 在每个阶段都会被修改——阶段四加阴影采样、阶段五加 AO/Contact Shadow、阶段六加 lightmap。**不预留注入点**（`#include` 占位、函数调用占位）。每个阶段按需修改是自然的增量开发，`brdf.glsl` 的函数在各阶段按需组合。过度预留增加当前复杂度且预留结构可能与实际需求不匹配。

### 透明物体处理（Phase 3 → Phase 7）

Step 5 将 Forward Pass 改为 `depth compare EQUAL + depth write OFF`。`AlphaMode::Blend` 的透明物体不在 PrePass 中写 depth，因此在 Forward EQUAL test 中 100% 被拒绝。Phase 7 引入正式 Transparent Pass 修复。

**选择：接受回归。** M1 测试场景（Sponza 等建筑场景）主体是 opaque + alpha masked，不含 `AlphaMode::Blend` 物体。Phase 3 Step 5 到 Phase 7 期间透明物体不渲染，已记录为已知局限性（见 `milestone-1.md`）。

---

## Pass 运行时开关

**选择：FeatureFlags 结构体 + GlobalUBO feature_flags bitmask + shader 动态分支（阶段四引入）**

M1 的多个 pass 是可选效果（阴影、SSAO、Contact Shadows、Skybox、后处理等），运行时开关对调试有重要价值——对比开启/关闭某个效果的画面差异，排查视觉问题的来源。

### 可开关 pass 分析

| Pass | 可独立关闭 | 关闭后影响 |
|------|-----------|-----------|
| DepthPrePass | 不能 | Forward 依赖 EQUAL depth test |
| ForwardPass | 不能 | 主光照 |
| TonemappingPass | 不能 | HDR→LDR 必需 |
| SkyboxPass | 可以 | 天空变黑，不影响其他 pass |
| ShadowPass | 可以 | 无阴影，画面变平 |
| SSAO | 可以 | 无环境光遮蔽 |
| Contact Shadows | 可以 | 无接触阴影 |
| 后处理各 pass | 可以 | 各自效果消失 |

### 核心问题：shader 如何安全采样可能不存在的 render target

禁用 pass 后其输出 render target 不存在。Set 2 使用 `PARTIALLY_BOUND`，访问未绑定 binding 是未定义行为。

| 路线 | 说明 |
|------|------|
| 路线 1：Feature flags + shader 动态分支 | GlobalUBO 的 `feature_flags` bitmask 控制 shader 是否采样 |
| 路线 2：Dummy textures + shader 无条件采样 | 禁用时绑定白色 dummy 到 Set 2 binding |

**选择路线 1。** 理由：

- Shadow map 类型是 `sampler2DArrayShadow`（硬件比较 + array），创建匹配 dummy 复杂
- Toggle 不需要 GPU idle——`feature_flags` 是 GlobalUBO 的一个 uint，下一帧自然生效
- 现代 GPU 对 uniform 条件分支（warp 内全走同一分支）零性能开销
- 不需要在 toggle 时更新 Set 2 descriptor

排除路线 2：每种特殊类型 render target（shadow array、未来可能的 3D texture 等）都需要专门 dummy。Toggle 需要 `vkQueueWaitIdle` + `vkUpdateDescriptorSets`，比路线 1 重。

### 设计机制

**1. RenderFeatures 结构体**（定义在 `framework/scene_data.h`）：

```cpp
struct RenderFeatures {
    bool skybox          = true;
    bool shadows         = true;   // 阶段四引入
    bool ssao            = true;   // 阶段五引入
    bool contact_shadows = true;   // 阶段五引入
    // 后处理 flags 随阶段八扩展
};
```

**2. GlobalUBO 新增 `feature_flags` 字段**（`uint32_t`，bitmask）：

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_SSAO            (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
```

**3. Renderer 编排**：根据 `RenderFeatures` 决定是否调用 pass 的 `record()`。禁用的 pass 不注册到 RG，其输出 `RGResourceId` 在 FrameContext 中保持 invalid。

**4. 消费端条件资源声明**：ForwardPass 等消费 pass 的 `record()` 检查 FrameContext 资源 ID 有效性，仅对有效资源声明 RG 依赖：

```cpp
auto resources = base_resources(ctx);
if (ctx.shadow_map.valid())
    resources.push_back({ctx.shadow_map, Read, Fragment});
if (ctx.ao_texture.valid())
    resources.push_back({ctx.ao_texture, Read, Fragment});
```

**5. Shader 端动态分支**：

```glsl
float shadow = 1.0;
if ((global.feature_flags & FEATURE_SHADOWS) != 0u) {
    shadow = sample_shadow_map(...);
}
float ao = 1.0;
if ((global.feature_flags & FEATURE_SSAO) != 0u) {
    ao = texture(rt_ao_texture, uv).r;
}
```

### 引入时机

**阶段四引入骨架**。阶段三没有有调试价值的可开关 pass（Skybox 是唯一可选但调试价值不大）。阶段四引入阴影时是第一个有意义的开关场景，此时同步引入 `RenderFeatures` + `feature_flags`。后续阶段按需扩展。

### DebugUI 集成

阶段四起 DebugUI 新增 feature toggle 面板，直接操作 `RenderFeatures` 的 bool 字段。Toggle 生效路径：DebugUI → `RenderFeatures` → Renderer 跳过 pass → FrameContext 资源 invalid → ForwardPass 条件声明 → GlobalUBO `feature_flags` → shader 动态分支。无 GPU idle，无 descriptor 更新，下一帧即生效。

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

**保留中间 cubemap**：M1 规划包含"静态 HDR Cubemap 天空"（见 `milestone-1.md`），中间 cubemap 是 Skybox Pass 的源数据。M2 Bruneton 大气散射替换天空后可改为销毁。

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

> **项目例外**：常规渲染管线不使用 Push Descriptors（见「Descriptor Set 管理方式」章节评估），但 IBL 预计算完全隔离在 `ibl.cpp` 内部，不参与常规帧循环，不影响全局 descriptor 管理一致性。此例外仅适用于一次性 init 阶段的 compute dispatch。

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

---

## Sampler 管理

**选择：Per-texture sampler**

ResourceManager 扩展 sampler 管理，使用已定义的 `SamplerHandle`（generation-based）：

- 新增 `SamplerDesc` 结构体（filter、wrap mode、mip mode）
- `create_sampler()` / `destroy_sampler()` / `get_sampler()` 方法
- `DescriptorManager::register_texture()` 同时接收 `ImageHandle` + `SamplerHandle`
- scene_loader 从 glTF sampler 定义创建对应 sampler
- 缺失 sampler 的 glTF texture 使用 default sampler（linear filter, repeat wrap, mip-linear）

**为什么 per-texture：** glTF 材质中不同纹理可能有不同的采样需求（如 normal map 用 linear + repeat，lightmap 用 linear + clamp），per-texture sampler 忠实还原原始资产意图。

---

## Default 纹理

Texture 模块初始化时创建三个 1×1 default 纹理，注册到 bindless array 获得固定 `BindlessIndex`：

| 名称 | 颜色值 (RGBA) | 用途 |
|------|--------------|------|
| White | (1, 1, 1, 1) | base color / metallic-roughness / occlusion / lightmap 的 neutral 值 |
| Flat Normal | (0.5, 0.5, 1.0, 1.0) | normal map 无扰动（切线空间 Z 朝上） |
| Black | (0, 0, 0, 1) | emissive 无自发光 |

`GPUMaterialData` 中缺失纹理字段填入对应 default 纹理的 `BindlessIndex`，shader 无需特判——统一采样即可获得正确的 neutral 效果。

---

## 纹理格式选择

根据 glTF texture 角色选择 `VkFormat`：

| 角色 | Format | 理由 |
|------|--------|------|
| base color, emissive | `R8G8B8A8_SRGB` | 颜色数据，GPU 自动 gamma 解码 |
| normal, metallic-roughness, occlusion | `R8G8B8A8_UNORM` | 线性数据，原样读取 |

scene_loader 在加载 glTF texture 时根据其引用关系（哪个材质字段引用了这张纹理）决定 format。

---

## GPU 上传策略

**选择：批量上传（immediate command scope）**

Context 提供 immediate command scope：

- `begin_immediate()` → 纯状态切换（reset/begin 内部 command buffer，不返回 `CommandBuffer`）
- `end_immediate()` → submit + `vkQueueWaitIdle`

场景加载时一次 `begin_immediate()`，录制所有 buffer copy + image copy + mip 生成命令，一次 `end_immediate()` 提交。减少 submit 次数和 GPU idle 等待。

`upload_buffer()` 改为录制模式：在活跃的 begin/end_immediate scope 内调用时，只录制 copy 命令到当前 command buffer，不自行 submit。staging buffer 由 Context 收集，`end_immediate()` submit + wait 完成后统一销毁。scope 外调用 `upload_buffer()` 会 assert 失败。

### Image 上传与 Mip 生成

ResourceManager 新增 `upload_image()` 和 `generate_mips()` 方法，与 `upload_buffer()` 同级——通用资源操作，不绑定在特定模块（Texture、IBL、Lightmap 等均复用）。

- `upload_image(ImageHandle, data, size, dst_stage)`：创建 staging buffer + 录制 `vkCmdCopyBufferToImage`（含 UNDEFINED → TRANSFER_DST layout transition）。`dst_stage` 指定最终 barrier 的 `dstStageMask`，调用方按消费者阶段显式指定（fragment 传 `FRAGMENT_SHADER_BIT`，compute 传 `COMPUTE_SHADER_BIT`）
- `generate_mips(ImageHandle)`：逐级 `vkCmdBlitImage` + 每级 layout transition（TRANSFER_DST → TRANSFER_SRC），最终转为 SHADER_READ_ONLY_OPTIMAL

两者均在 begin/end_immediate scope 内调用，遵循录制模式。

### Staging Buffer 收集策略

Context 内部维护 `std::vector<std::pair<VkBuffer, VmaAllocation>> pending_staging_buffers_`。`upload_buffer()` 和 `upload_image()` 每次创建 staging buffer 后 push 进此列表。`end_immediate()` submit + `vkQueueWaitIdle` 完成后，遍历销毁所有 pending staging buffer 并清空列表。

**长期定位**：这是初始加载的长期方案。未来运行时流式加载使用 Transfer Queue 异步上传，是独立系统，不影响此方案。

---

## 场景加载与错误处理

**场景路径**：命令行参数 `--scene`，默认 `assets/Sponza/Sponza.gltf`。长期方向是 GUI 文件选择器。

**参数传递方式**：`main.cpp` 使用 CLI11 解析命令行参数（`--scene`、`--env` 等），`Application::init()` 接收已解析的参数结构体。Application 不感知命令行解析细节。CLI11 依赖详见「IBL 管线 — HDR 环境贴图加载路径」章节。

**错误处理策略**：加载失败（glTF 解析失败、纹理文件缺失、shader 编译失败）一律 log error + abort。开发期使用已知资产，加载失败 = 代码或路径有 bug，应立刻暴露。不做 fallback 到 default 资源——那会掩盖问题。

---

## 资源句柄设计

**选择：Generation-based 句柄**

资源池管理的资源句柄（Image、Buffer、Sampler）包含 index + generation 两个字段。资源池每个 slot 维护 generation 计数器，资源销毁时 generation 递增，使用句柄时比对 generation。

Pipeline 不纳入资源池管理——Pipeline 所有权始终是单一且明确的（pass 持有），不像 Image/Buffer 被多个 pass、descriptor set、render graph 交叉引用。生命周期保护由 Vulkan Validation Layer 兜底（检测已销毁 VkPipeline 的使用、设备销毁时的泄漏报告），无需 generation 机制。

**为什么加 generation：** 检测所有 use-after-free 错误——旧句柄引用已销毁并被新资源复用的 slot 时立即报错，而非静默访问错误资源。开销仅为一次 `uint32_t` 比较。

---

## 对象生命周期

**选择：显式 `destroy()` 方法**

不使用 RAII 析构函数管理 Vulkan 对象。所有持有 Vulkan 资源的类提供 `destroy()` 方法，调用方负责在正确时机调用。

**为什么不用 RAII：** Vulkan 对象销毁顺序重要且复杂（Device 必须最后销毁、依赖资源必须先于被依赖资源销毁），析构函数的隐式调用顺序可能踩坑。显式方法让销毁时机完全可控。

---

## 帧同步

**选择：2 Frames in Flight**

每帧的 CPU 侧资源（command pool、command buffer、fence、semaphore）分为 2 套，通过帧索引轮换。每帧开始时等前 N 帧的 fence，执行该帧的延迟删除队列，然后录制新命令。

**为什么不是 3 帧：** CPU 端无复杂游戏逻辑，帧时间不会剧烈波动。2 帧的更低延迟对交互式场景漫游更好。后续如需改为 3 帧只需修改常量。

---

## 错误处理与调试

**Vulkan 错误处理：** `VK_CHECK` 宏 + abort。Vulkan API 错误在开发期几乎都是编程错误，不需要运行时恢复。

**Validation Layer：** 开发期常开，所有 Vulkan 调用经过验证。

**Debug Utils：** 集成 `VK_EXT_debug_utils`：
- 为资源添加 debug name（RenderDoc 和 Validation Layer 报错时显示可读名称）
- 为 command buffer 区域添加标记（GPU profiler 中显示 pass 名称）

---

## CSM 阴影系统

阶段四引入。方向光 CSM（Cascaded Shadow Mapping）+ PCF 软阴影 + cascade blend。

### 实现策略：直接 CSM

**选择：直接实现 CSM，cascade=1 起步，不经过"单张 Shadow Map"中间步骤。**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 先单张 Shadow Map，再升级 CSM | 两步验证 |
| B. 直接 CSM，cascade=1 起步 | 一步到位 |

**选择 B。** cascade=1 的 CSM 运行时行为等价于单张 shadow map，但代码结构直接是最终形态。从单张升级到 CSM 需要重写 7 个改动项中的 5 个（资源类型、descriptor 类型、UBO layout、shadow pass 结构、光空间矩阵计算、shader cascade 查找），是"替换"而非"增量"。方案 B 的每一步都是增量的、不丢弃的。

调试隔离性差异很小——cascade=1 多引入的变量（array texture layer=0、matrix array 对齐）要么 trivial 要么在 cascade=1 时也能验证。

### Shadow Map 数据结构

**选择：Texture 2D Array（每 cascade 一个 layer）**

候选方案：

| 方案 | 说明 |
|------|------|
| A. Texture 2D Array | 每 cascade 一个 layer，`sampler2DArrayShadow` 硬件 PCF |
| B. 单张大 Atlas | 2D 纹理划分区域，UV 重映射 |

**选择 A。** PCF 采样无边界溢出问题（每 layer 独立纹理空间），采样代码简洁（layer index = cascade index），与 `m1-interfaces.md` 预定义的 `sampler2DArrayShadow` 一致。M3 Shadow Atlas 是面向多种光源的统一管理系统，与 CSM texture array 在管理复杂度上差距巨大，无论现在选哪种方案 M3 都需要另建 atlas 管理系统。

Atlas 相比 Array 的唯一优势是支持 per-cascade 不同分辨率。但 per-cascade 不同分辨率是不必要的——**PSSM 对数分割本身就通过给近处 cascade 分配更小的世界空间覆盖范围来实现非均匀 texel density**：同样的 2048² 像素覆盖 3m（cascade 0，~680 px/m）vs 覆盖 140m（cascade 3，~15 px/m），密度差 ~45 倍。在 PSSM 已经最优化像素预算分布的基础上叠加非均匀分辨率，既不能改善近处质量（PSSM 已给了最多像素），又会恶化远处质量（缩小远处 cascade 分辨率进一步降低本就偏低的密度）。真正影响近处阴影质量的参数是 `shadow_max_distance`（见「Shadow Max Distance 初始化」），而非 per-cascade 分辨率。

### Shadow Map 参数

| 参数 | 值 | 可调性 |
|------|-----|-------|
| Cascade 数量 | 4（默认） | 运行时可调（1/2/3/4），纯渲染参数（控制循环次数），不触发资源重建 |
| Shadow map 层数 | 4（固定 = MAX_SHADOW_CASCADES） | 不随 cascade 数量变化。未使用的层包含陈旧数据但不被采样 |
| 每 cascade 分辨率 | 2048²（所有 cascade 统一） | 运行时可调（512/1024/2048/4096），触发资源重建 |
| Depth 格式 | D32Sfloat | 固定（与主相机 depth 一致） |

所有 cascade 统一分辨率：Texture 2D Array 要求每 layer 同尺寸，且 PSSM 已通过覆盖范围差异提供非均匀 texel density（见上方分析），统一分辨率不构成浪费。4 × 2048² × 4B（D32Sfloat）= 64MB，桌面 8GB+ 显存下完全可接受。

Shadow map 层数固定为 4（MAX_SHADOW_CASCADES）：cascade_count 是纯渲染参数（控制 ShadowPass 循环次数 + shader 采样范围），不影响资源。这使得 cascade 数量切换为零开销操作（改一个整数，下一帧生效），避免了按需重建 image + views + descriptor 的复杂流程。`create_image()` 对 `array_layers = 4`（> 1 且 ≠ 6）自动推断 `VK_IMAGE_VIEW_TYPE_2D_ARRAY`，无需额外 flag。

运行时分辨率变更流程与 MSAA 切换统一模式：`vkQueueWaitIdle` → 重建资源（ShadowPass `on_resolution_changed()`）→ 更新 Set 2 descriptor。Cascade 数量变更不走此流程。

### Shadow Map 资源管理

**选择：ShadowPass 自管理（非 RG managed），每帧 import 到 RG**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 扩展 RGImageDesc 加 array_layers，作为 managed 资源 | RG 管理生命周期 |
| B. ShadowPass 自行创建/销毁，每帧 `import_image()` 到 RG | ShadowPass 完全拥有 |

**选择 B。** Shadow map 性质不同于屏幕尺寸 render target：Absolute 固定尺寸、array 纹理、需要 per-layer view。Managed 资源系统的核心价值（Relative 模式 + resize 自动重建）对 shadow map 无增值。方案 A 导致所有权分裂（主 image 归 RG，per-layer views 归 ShadowPass），config change 时需要协调两个管理者。方案 B 统一所有权——ShadowPass 同时管理主 image 和 per-layer views，创建/销毁/重建在一处完成。

每帧通过 `import_image("Shadow Map", shadow_image_, UNDEFINED, SHADER_READ_ONLY_OPTIMAL)` 导入 RG，与 swapchain image 的 import 模式一致。

### ShadowPass 类设计

**选择：单个 RG pass，内部循环 cascade**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 一个 RG pass，lambda 内循环 cascade | 单次 barrier |
| B. 每 cascade 一个 RG pass | 更细粒度 profiling |

**选择 A。** 方案 B 导致 4 个 pass 依次 Write 同一 shadow map RGResourceId，RG 在它们之间插入冗余 WAW barrier（RG 不追踪 subresource，无法区分不同 layer）。方案 A 一个 pass 一组 barrier，内部循环 cascade，per-cascade 用 `cmd.begin_debug_label("Cascade N")` 提供 RenderDoc 分组。

方法集：`setup()` / `record()` / `destroy()` / `on_resolution_changed()` / `rebuild_pipelines()`。不属于 MSAA 相关 pass（shadow map 始终 1x），无 `on_resize()`（Absolute 固定尺寸），无 `on_sample_count_changed()`。Cascade 数量为纯渲染参数，不需要专门方法。

ShadowPass 持有：shadow map ImageHandle、per-layer VkImageView 数组、opaque pipeline、mask pipeline。提供 `shadow_map_image()` getter 供 Renderer 更新 Set 2 binding 5。

### Alpha Mask 阴影

**选择：支持，从阶段四开始**

| 方案 | 说明 |
|------|------|
| A. 不处理，实心阴影 | 最简单 |
| B. 双 pipeline（opaque + mask） | 与 DepthPrePass 模式一致 |

**选择 B。** Sponza 场景有大量 alpha mask 植物，实心矩形阴影非常显眼。增量 ~50 行代码，模式复用 DepthPrePass 的 opaque/mask 分离。

- Opaque pipeline：仅 VS（`shadow.vert`），无 FS，深度由光栅器写入，完整 Early-Z
- Mask pipeline：VS + FS（`shadow_masked.frag`，alpha test + discard），Early-Z 可能被禁用
- 绘制顺序：先 opaque 后 mask（与 DepthPrePass 一致）

### Shadow Map 深度约定

**选择：Reverse-Z（与主相机一致）**

正交投影下 Reverse-Z 无精度优势（深度线性分布），选择纯粹基于一致性——项目全面 Reverse-Z，shadow map 特殊化引入认知负担。

- Clear value：0.0（far）
- Depth compare：GREATER
- Shadow comparison sampler：`GREATER_OR_EQUAL`（fragment depth ≥ shadow depth → lit）

### Cascade 分割策略

**选择：Practical（PSSM），lambda 默认 0.75，DebugUI 可调**

`C_i = λ × C_log + (1 - λ) × C_lin`

业界最常用方案（GPU Gems、NVIDIA、Unity/UE 均采用变体）。`shadow_max_distance` 限制分割范围（不一定等于相机 far plane），避免远距离浪费分辨率。

### Shadow Max Distance 初始化

**选择：场景加载时根据 scene AABB 自动初始化**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 固定默认值 200m | 简单，但对小场景（Sponza ~30m）浪费大量分辨率 |
| B. 场景加载时自动计算 | SceneLoader 暴露 scene AABB，Application 设置 `max_distance = diagonal × 1.5` |
| C. 运行时自动追踪 | 每帧根据可见物体动态调整 |

**选择 B。** `shadow_max_distance` 直接决定 PSSM 分割质量。以 Sponza（~30m 跨度）为例，固定 200m 时 cascade 0 的 texel density 仅 ~159 px/m（cascade 2-3 浪费在空旷区域），自动计算后（max_distance ≈ 55m）cascade 0 密度达 ~554 px/m，近处阴影质量提升 3.5 倍。

排除 A：不同场景尺度差异巨大（室内 30m vs 室外 500m），固定值对多数场景不是最优。排除 C：M1 静态场景，max_distance 不需要帧间变化。

**计算方式**：`max_distance = scene_aabb.diagonal_length() × 1.5`。乘 1.5 覆盖方向光在 ~60° 入射角下的阴影投射范围（此时阴影长度约为物体高度的 1.73 倍）。倍率硬编码，不作为可调参数——这是"合理默认值"的计算系数，不是用户关注点。

**退化防护**：Application 初始化 `max_distance = 100.0f`（退化 fallback）。场景 AABB 有效时覆盖为 `diagonal × 1.5`，退化时保持 100m。不设人为 clamp 范围——基于实际场景几何的计算结果对该场景总是合理的。

**实现位置**：SceneLoader 加载完成后计算场景 AABB（所有 mesh instance 的 `world_bounds` 求并集），暴露 `scene_bounds()` getter。Application 在场景加载后、首帧渲染前：`if (diagonal > epsilon) max_distance = diagonal × 1.5`，否则保持初始 100m。DebugUI 仍允许手动覆盖（对数滑条）。RenderFeatures 和 ShadowConfig 均无 struct 默认值，Application 使用 designated initializers 显式初始化全部字段。

### 相机自动定位与 F 键 Focus

**选择：Camera::compute_focus_position() 纯计算 + CameraController F 键触发**

两个需求共享同一核心计算——给定 AABB + 朝向 + FOV → 计算能看到整个场景的相机位置：

| 场景 | yaw/pitch | position |
|------|-----------|----------|
| 场景加载自动定位 | 设为 0° / -45° | 由 AABB 计算 |
| F 键 focus | 保持当前值不变 | 由 AABB 计算 |

**分层设计**：

- `Camera::compute_focus_position(const AABB& bounds) const`（framework 层）：纯几何计算，AABB 包围球半径 `r` → `distance = r / sin(fov/2)` → `center - forward() * distance`。不修改相机状态。退化 AABB 时返回当前 position。
- `CameraController::set_focus_target(const AABB* bounds)`（app 层）：持有 focus target 指针。`update()` 中检测 F 键单次按下（`ImGui::IsKeyPressed(ImGuiKey_F, false)`，在 `!io.WantTextInput` 保护下），调用 `compute_focus_position()` 更新 position。
- Application：场景加载后设置 `camera_controller_.set_focus_target(&scene_loader_.scene_bounds())`。自动定位时先设 pitch/yaw 再调用 `compute_focus_position()`。

**为什么不放在 Application 层检测 F 键**：键盘输入处理是 CameraController 的职责，Application 不应直接轮询按键。CameraController 通过指针获取 AABB 数据，不引入对 SceneLoader 的依赖。

### Cascade 混合策略

**选择：Lerp blend**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 硬切换 | 有可见接缝 |
| B. 相邻 cascade lerp | blend region 内 2x 采样 |
| C. Dithering + temporal | 无额外采样，需 temporal filtering |

**选择 B。** 排除 A：cascade 间质量差异在无过渡时产生可见接缝。不采用 C：dithering 依赖全画面 temporal accumulation（FSR/DLSS）平滑噪点，M1 不具备此基础设施。即使有 DLSS/FSR，disocclusion 时 dithering 噪声在 cascade 边界仍短暂可见（~1-2 帧，无法完全消除）。是否可接受需实测判断。

Lerp blend 每帧都产出干净结果，零 temporal 依赖，无 disocclusion 瑕疵。性能开销局限在 blend region（~10% 阴影像素 2x 采样），可接受。

**Dithering 切换实现约束**：shadow.glsl 中 cascade blend 逻辑封装为独立函数 `blend_cascade_shadow(shadow_current, shadow_next, blend_factor)`，forward.frag 通过此函数获取最终 shadow 值。切换 dithering 时只需修改此函数内部（从 `mix()` 改为 dither pattern 比较 + 单次采样），forward.frag 调用点不变。

#### Blend Width 默认值分析

`blend_width = 0.1`（cascade blend region 占 cascade 范围的 10%）。以 auto max_distance ≈ 55m 的 Sponza 场景为例分析绝对 blend 距离：

| 过渡 | cascade 范围 | blend 绝对宽度 | 过渡位置 | 相邻密度比 |
|------|-------------|---------------|---------|-----------|
| 0→1 | 3.7m | 0.37m | ~3.4m | 1.3:1 |
| 1→2 | 4.9m | 0.49m | ~8.2m | 2.1:1 |
| 2→3 | 10.5m | 1.05m | ~18.2m | 3.5:1 |

比例制的隐含优势：近处 cascade（密度比小、质量差异小）获得窄 blend，远处 cascade（密度比大但视距远、屏幕占比小）获得宽 blend，匹配人眼对不同距离的敏感度差异。0.1 作为默认值合理，DebugUI 可调。

### Shadow Bias 策略

**选择：三者组合（constant + slope 硬件 + normal offset shader）**

| Bias 类型 | 作用端 | 覆盖角度 |
|-----------|--------|---------|
| Constant | 渲染端（`vkCmdSetDepthBias`） | 正对光源 |
| Slope-scaled | 渲染端（`vkCmdSetDepthBias`） | 中等倾斜 |
| Normal offset | 采样端（`shadow.glsl`） | 极端掠射角 |

Normal offset 与 cascade texel 世界尺寸成正比——远处 cascade 的 texel 覆盖更大世界面积，需要更大偏移。texel world size 从 `cascade_view_proj` 矩阵提取（正交投影范围 / shadow map 分辨率），不需要额外 UBO 字段。

所有 bias 参数为全局值（非 per-cascade），DebugUI 可调。

### Shader 接口设计

**选择：分步函数，forward.frag 自行组装**

```glsl
// common/shadow.glsl
int select_cascade(float view_depth, out float blend_factor);
float sample_shadow_pcf(vec3 world_pos, vec3 world_normal, int cascade);
float blend_cascade_shadow(float shadow_current, float shadow_next, float blend_factor);
float shadow_distance_fade(float view_depth);
```

不做单一 `evaluate_shadow()` 包装——cascade index 被隐藏会妨碍 debug cascade 可视化。forward.frag 的 shadow 调用流程完全透明，方便调参和排查。`blend_cascade_shadow` 隔离 blend 策略（见「Cascade 混合策略 — Dithering 切换实现约束」）。

### GlobalUBO Shadow 数据布局

**选择：全部放入 GlobalUBO**

候选方案：

| 方案 | 说明 |
|------|------|
| A. 扩展 GlobalUBO | 336 → 624 bytes |
| B. 独立 Shadow UBO（新增 Set 0 binding） | 清晰分离 |

**选择 A。** 624 bytes 远在 16KB 最低保证之下。新增 binding 需修改 Set 0 layout → 所有 pipeline layout 变更 → 全部 pipeline 重建，代价远超 UBO 多几百字节。

布局详见 `m1-interfaces.md`「GlobalUniformData」。

### PushConstantData

**选择：仅 cascade_index，4 bytes**

Instancing 准备工作（阶段四准备工作 B）将 model + material_index 迁移到 per-instance SSBO 后，push constant 只剩 `cascade_index`（4 bytes）。Shadow VS 需要知道当前渲染的 cascade index 以从 GlobalUBO 读取对应的 `cascade_view_proj[cascade_index]`，每个 cascade 循环开头 push 一次，该 cascade 所有 draw call 共享。Forward/PrePass 不使用 push constant。

### Per-cascade 剔除与 Culling 模块重构

**选择：Step 1c 提前重构 Culling 模块为纯几何剔除，ShadowPass 暴力全画验证正确性后 Step 6 接入**

| 阶段 | 策略 |
|------|------|
| Step 1c | 重构 Culling 模块为纯几何接口 + 现有相机 cull 迁移（shadow 之前完成，隔离验证） |
| Step 2-5 | ShadowPass 暴力全画全部场景物体，GPU 自动裁剪 frustum 外几何。Sponza 规模下 ~2000 depth-only draw calls 无性能问题 |
| Step 6 | ShadowPass 接入 per-cascade 剔除（Culling 接口已就绪） |

**Culling 模块职责边界**：现有 `perform_culling()` 混合了三个职责——几何剔除（AABB-frustum 测试）、材质分桶（按 alpha_mode 分 opaque/transparent）、透明排序。Step 1c 拆分为：

| 职责 | 归属 | 理由 |
|------|------|------|
| 几何剔除 | `culling.h`（`Frustum` + `extract_frustum` + `cull_against_frustum`） | 纯几何，与材质无关，camera/shadow/probe 统一复用 |
| 材质分桶 | 调用方内联 | 不同消费者分桶标准不同（camera: opaque vs transparent，shadow: opaque vs mask，Blend 不投射阴影） |
| 透明排序 | 调用方内联 | 只有 camera 路径需要，shadow/probe 不需要 |

`CullResult` 继续在 `scene_data.h`——它是 camera cull + 分桶后的渲染合同，不属于 culling 模块。

**接口设计**：`cull_against_frustum` 使用调用方持有的预分配 buffer（`void` + `out_visible` 参数），调用方跨帧复用 `vector`（`clear()` 不释放内存，首帧分配后后续帧零分配），避免每帧 camera 1 次 + shadow 4 次 + 未来 probe 6 次的重复 vector 分配。接口定义见 `m1-interfaces.md`「Culling 泛化接口」。

**Shadow cull 输入**：必须是全部场景物体（不是相机剔除子集）——相机 frustum 外的物体可能在 shadow map 中可见（如屏幕外的树投影到可见地面上）。Per-cascade 光空间剔除从全部物体中过滤，cull 结果再按 alpha_mode 分桶为 opaque/mask 列表供双 pipeline 绘制。

---

## Instancing

### 背景

大型场景 hundreds of unique mesh 被实例化为数千 instance。当前每个 visible instance 独立 draw call（push constant + bind VB/IB + drawIndexed），密集区域数千 draw calls。瓶颈在 CPU 侧 draw call 提交开销。Phase 4 CSM shadow pass 将进一步增加 draw call（4 cascade × 全部场景物体）。

### 设计

**选择：CPU 侧 mesh_id 分组 + instanced draw + per-instance SSBO**

同一 mesh 的所有 instance 通常共享同一 material（glTF primitive 级别绑定），pipeline state 完全一致。按 `(mesh_id, alpha_mode, double_sided)` 分组后，每组一次 `vkCmdDrawIndexed(instanceCount=N)`，draw call 显著减少（unique mesh 上限，实际更少因为 frustum cull）。分组键包含 `alpha_mode` 和 `double_sided` 以确保同一 group 内 pipeline 和 cull mode 一致——即使同一几何体被不同材质引用也能正确处理。

**Per-instance SSBO 替代 push constant**：

```cpp
// Set 0, Binding 3
struct GPUInstanceData {              // 128 bytes, std430
    mat4 model;                       // 64 bytes
    mat3 normal_matrix;               // 48 bytes — transpose(inverse(mat3(model))), precomputed
    uint material_index;              //  4 bytes
    uint _padding[3];                 // 12 bytes
};
```

Shader 通过 `gl_InstanceIndex` 索引（`vkCmdDrawIndexed` 的 `firstInstance` 参数设置起始偏移）。Normal matrix 在 CPU 端 per-instance 预计算，避免每顶点 mat3 逆运算。Push constant 仅 4 bytes（`cascade_index`，shadow pass 用）。

**分组数据结构**（CPU 侧，不上 GPU）：

```cpp
struct MeshDrawGroup {
    uint32_t mesh_id;
    uint32_t first_instance;          // InstanceBuffer 偏移
    uint32_t instance_count;
    bool double_sided;                // 从 material 缓存
};
```

**帧流程**：
1. Culling 输出 `visible_opaque_indices`（flat list，不变）
2. Renderer 按 `(mesh_id, alpha_mode, double_sided)` 排序 → 构建 MeshDrawGroup + 填充 InstanceBuffer
3. Pass 迭代 draw groups：`set_cull_mode` → `bind_vb/ib` → `draw_indexed(instanceCount=N, firstInstance=offset)`

**InstanceBuffer 管理**：CpuToGpu 内存，per-frame buffer（2 frames in flight），固定大小（kMaxInstances=65536 × 128 bytes = 8 MB），每帧 memcpy 覆写。超出上限时 warn 并丢弃剩余 group。

**透明物体例外**：Blend 不做 instancing——数量少且需要 back-to-front 排序，排序破坏 mesh_id 分组。

**提前计划的迁移**：此变更合并了原计划阶段六的 per-instance SSBO 迁移和 M3 的 Instancing，提前到阶段四准备工作解决大型场景性能痛点。

---

## 运行时场景/HDR 加载

### 动机

M1 引入多个测试场景（DamagedHelmet、Sponza、Intel Sponza），频繁切换需要重新编译/重启。CLI11 命令行参数方式不灵活，且场景文件不应由 CMake 拷贝到 build 目录（资产体积大、路径耦合构建系统）。

### 设计

**选择：ImGui 运行时选择 + Windows 原生文件对话框 + JSON 配置持久化**

- **文件选择**：`GetOpenFileNameW` 原生对话框。零额外依赖，用户熟悉的 UI，过滤 .gltf/.glb 和 .hdr
- **配置位置**：`%LOCALAPPDATA%\himalaya\config.json`（Windows 用户数据标准目录，非可执行文件旁）
- **JSON 库**：nlohmann/json（vcpkg，header-only，C++ 事实标准）
- **CLI11 完全移除**：不保留命令行 override，所有配置走 config.json + ImGui

**启动流程**：
1. 读 `config.json` → 获取 `scene_path` 和 `env_path`
2. 分别尝试加载 scene 和 HDR（**独立失败**：scene 失败不影响 HDR，反之亦然）
3. Scene 失败 → 空场景（0 instance），仅 skybox
4. HDR 失败 → 灰色 fallback cubemap（IBL 已有 fallback 机制）
5. 首次运行（无 config）→ 空场景 + fallback cubemap

**运行时切换**：复用 MSAA 切换的 `vkQueueWaitIdle` 模式——`vkQueueWaitIdle` → destroy 旧资源 → load 新资源 → 更新 descriptors → 保存 config.json。

**DebugUI**：Scene / Environment 面板各显示当前文件路径 + "Load..." 按钮。加载失败时显示错误提示（不 abort）。

---

## 缓存基础设施

### 动机

阶段四引入两种缓存需求：BC 纹理压缩缓存（首次 CPU 压缩后缓存 KTX2）和 IBL 预计算缓存（首次 GPU 计算后缓存产物）。两者共享缓存目录解析、内容哈希、路径管理等逻辑。

### 设计

**选择：framework 层轻量缓存工具模块（`framework/cache.h`）**

```cpp
namespace himalaya::framework {
    std::filesystem::path cache_root();                              // %TEMP%\himalaya\
    std::string content_hash(const void* data, size_t size);         // XXH3_128 hex string
    std::string content_hash(const std::filesystem::path& file);     // 文件内容哈希
    std::filesystem::path cache_path(std::string_view category,      // root/category/hash.ext
                                     std::string_view hash,
                                     std::string_view extension);
}
```

模块只提供路径和哈希工具，不知道具体缓存格式（KTX2 / 二进制等）。消费者各自处理序列化。

**缓存位置**：`%TEMP%\himalaya\`（Windows `GetTempPath()`）。Disk Cleanup / Storage Sense 可自动清理，缓存丢了就重算，和 shader cache 同一性质。

**哈希算法**：XXH3_128（xxHash 库）。极快（>10 GB/s on modern CPU）、128 位低碰撞、BSD-2 许可。用 128 位十六进制字符串做文件名。

排除方案：
- `std::hash`：非确定性（跨编译器/运行不一致）
- CRC32：32 位碰撞率过高
- SHA-256：性能过剩（密码学级别不必要）

---

## 纹理压缩与缓存

### 背景

大型场景数百张纹理以 RGBA8 未压缩 + 全 mip chain 上传 GPU，纹理 VRAM 可达数 GB。BC 块压缩是 GPU 纹理的标准解决方案——4:1 压缩比、硬件原生解码、零性能开销。

### 压缩格式选择

**选择：BC7（通用）+ BC5（法线）**

| 纹理类别 | BC 格式 | VkFormat | 理由 |
|----------|---------|----------|------|
| baseColor | BC7 | `BC7_SRGB_BLOCK` | 高质量 4 通道，SRGB 颜色空间 |
| metalRough | BC7 | `BC7_UNORM_BLOCK` | 线性数据，G=roughness B=metallic |
| normal | BC5 | `BC5_UNORM_BLOCK` | 2 通道专用（RG），shader 重建 Z |
| emissive | BC7 | `BC7_SRGB_BLOCK` | SRGB 颜色 |

**为什么法线用 BC5 而非 BC7**：BC5 为 2 通道专用编码（每通道 8 bit），BC7 将精度分散到 4 通道（每通道 ~5-7 bit）。法线只需 RG 两通道，BC5 质量更高。shader 重建 `Z = sqrt(1 - R² - G²)` 改动极小。两者同为 1 byte/pixel，无大小差异。

**为什么不用 BC1**：BC1 是 0.5 byte/pixel（BC7 的一半），但每个 4×4 block 只有 4 个颜色，渐变和微妙色差出现可见 banding。质量代价不值得。

### 压缩编码器

**选择：bc7e.ispc（ISPC SIMD 向量化） + rgbcx（BC4/BC5）**

BC7 编码器使用 Binomial 的 bc7e.ispc（Apache 2.0），通过 ISPC 编译器生成 SSE2/SSE4/AVX2/AVX-512 四种 SIMD 变体，运行时自动选择最佳指令集。支持全部 8 种 BC7 mode，质量预设按构建类型区分：Release 使用 slowest（uber_level=4 + pbit search + 全 partition 搜索），Debug 使用 slow（uber_level=0 + pbit search）以加快迭代。BC4/BC5 编码沿用 rgbcx（算法简单，SIMD 加速无意义）。

ISPC 编译器作为构建依赖，CMake 4.1 原生支持 ISPC 语言（`project(... LANGUAGES CXX ISPC)`），通过 `CMAKE_ISPC_COMPILER` 指定路径。

**演进历史**：初始选择 bc7enc（纯 C++ 标量，4 mode），后因首次压缩耗时过长升级为 bc7e.ispc。bc7e 在最高质量设置下仍显著快于旧 bc7enc（SIMD 并行 + 更优算法），同时质量更高（8 mode 全支持）。

排除方案：
- **bc7enc**（原方案）：纯 C++ 标量实现，仅 4 mode，最高质量（uber_level=4）下速度慢
- **ISPCTextureCompressor**：Intel 已于 2024.9 归档停更
- **GPU compute BC7**：Vulkan 上比 CPU 慢（bc7e-on-gpu 实验结论），BC7 搜索空间大不适合 GPU
- **KTX-Software 内置 UASTC**：需每次加载 transcode，直接 BC7 缓存加载更快

### KTX2 缓存

**选择：首次压缩后缓存为 KTX2，后续直接加载 BC 数据**

```
首次: hash 源字节 → 缓存 miss → 解码 PNG → CPU mip → BC 压缩 → 上传 GPU + write_ktx2() 写缓存
缓存: hash 源字节 → 缓存 hit → read_ktx2() → upload_image_all_levels()（跳过解码和压缩）
```

KTX2 是 Khronos 官方纹理容器格式，天然支持 BC 格式 + 多 mip 存储。KTX2 读写通过自写最小模块（`framework/ktx2.h/cpp`）实现，不依赖 libktx。设计细节见下文「KTX2 读写模块」。Vulkan 上传走 `ResourceManager::upload_image_all_levels()`，设计细节见下文「多级上传 API」。

**缓存位置**：`%TEMP%\himalaya\textures\`。Windows Disk Cleanup / Storage Sense 可自动清理，不污染用户数据目录。缓存丢了就重新压缩，和 shader cache 同一性质。

**缓存 key**：源文件原始字节（JPEG/PNG 编码数据）的 XXH3_128 哈希 + 格式后缀（`_bc7s`/`_bc7u`/`_bc5u`）。基于源字节而非解码后像素，缓存命中时完全跳过图像解码。文件内容变化自动重新压缩。

### Mip 生成

**选择：CPU 端 stb_image_resize2 替代 GPU vkCmdBlitImage**

BC 纹理不支持 `vkCmdBlitImage`（GPU 不能逐像素写入 BC block）。CPU 端逐级缩放后每级独立压缩。stb_image_resize2 是 stb 系列头文件库，已有 stb_image 基础，集成成本为零。Color 纹理（BC7_SRGB）使用 `stbir_resize_uint8_srgb()` 做 gamma-correct 降采样，Linear/Normal 纹理使用 `stbir_resize_uint8_linear()`。

### 非 4 对齐纹理

**选择：resize 到 4 的倍数**

BC block 是 4×4 像素。宽或高不是 4 的倍数的纹理在压缩前 resize 到 `(dim + 3) & ~3`。UV 偏差 < 0.1%，肉眼不可见。大多数场景纹理恰好都是 4 对齐的，这是通用防护。

### 压缩并行度

**选择：纹理级 OpenMP 并行（`#pragma omp parallel for schedule(dynamic)`）**

多张纹理同时压缩，利用多核 CPU。`schedule(dynamic)` 适应纹理大小差异（大纹理比小纹理慢得多）。首次加载数百张纹理的压缩时间从分钟级降到可接受范围。实现简单，无需 block 级并行。

---

## KTX2 读写模块

### 背景

纹理压缩缓存（D-2）和 IBL 缓存（D-3）都需要将 GPU 纹理数据持久化为文件再读回。KTX2 是 Khronos 官方纹理容器格式，天然支持 BC 压缩 + cubemap + mip chain。

### 设计

**选择：自写最小 KTX2 读写模块（`framework/ktx2.h/cpp`），不依赖 libktx**

libktx（vcpkg `ktx` port）构建失败（GitHub 源码包 tar 解压已知问题：vcpkg#47514、#42381），且我们实际只需 6 种格式的 2D/cubemap + mip chain 读写，不需要 Basis Universal、supercompression、格式转码等 libktx 的大部分功能。自写模块约 200-300 行代码，零外部依赖，完全可控。

排除方案：
- **libktx（vcpkg）**：vcpkg 构建失败（tar 解压已知 bug），FetchContent 可能遇到相同构建问题
- **自定义二进制格式**：外部工具（RenderDoc、KTX viewer）无法检视缓存文件，不利于调试

**支持范围**：
- 6 种格式：BC7_SRGB、BC7_UNORM、BC5_UNORM、R16G16B16A16_SFLOAT、B10G11R11_UFLOAT_PACK32、R16G16_UNORM
- 2D 纹理（faceCount=1）和 cubemap（faceCount=6）
- 任意 mip level 数量
- 无 supercompression（scheme=0）
- 无纹理数组（layerCount=0）

**DFD（Data Format Descriptor）处理**：KTX2 规范要求每个文件包含有效 DFD。对 6 种支持格式通过 `build_dfd()` 按 Khronos Data Format Specification 参数化构建。读取时不解析 DFD——仅用 header 的 `vkFormat` 字段识别格式。DFD 的存在是为了让外部工具能正确识别文件格式。

**文件布局**（按 KTX2 规范）：identifier → header → index section → level index → DFD → mip data（从最小 mip 到最大 mip）。mip 数据间按 `lcm(block_bytes, 4)` 对齐。

**读取路径**：读整个文件 → 校验 identifier + header（`Ktx2FileHeader` packed struct） → 解析 level index → 提取 mip 数据到连续 buffer（剥离 KTX2 元数据） → 返回 `Ktx2Data`（仅含 mip 数据 + 每级 offset/size 索引），消费者直接传给 `upload_image_all_levels()`。

**写入路径**：write-to-temp + `std::filesystem::rename` 原子写入，防止崩溃产生半写文件。

---

## 多级上传 API

### 背景

现有 `upload_image()` 只上传 mip 0，然后 `generate_mips()` 通过 GPU blit 生成后续 mip。BC 纹理不支持 `vkCmdBlitImage`（GPU 无法逐像素写入 BC block），BC 和 IBL 缓存的 mip chain 是预建好的，需要一次性全部上传。

### 设计

**选择：`ResourceManager::upload_image_all_levels()`，单 staging buffer + 批量 `vkCmdCopyBufferToImage2`**

一个 staging buffer 装全部 mip 数据（memcpy 一次），对每个 mip level 构建一个 `VkBufferImageCopy2` region，`layerCount` 设为 image 的 `array_layers`（cubemap=6, 2D=1），单次 `vkCmdCopyBufferToImage2` 提交所有 region。

KTX2 的每级内数据布局（face 0, face 1, ..., face 5 连续排列）与 Vulkan 的 `layerCount > 1` buffer-to-image copy 约定天然一致——零数据重排。

排除方案：
- **多次 `upload_image()`**：每次创建独立 staging buffer + barrier + copy，N mip × 6 faces = 大量冗余同步
- **扩展现有 `upload_image()` 参数**：会使简单 case（mip 0 only）的 API 变复杂，保持两个函数各司其职更清晰

---

## IBL 缓存

### 背景

IBL 管线每次启动重新计算全部产物。主要耗时在 prefiltered environment map（512² × 6 faces × 10 mip × 1024 samples/texel），占启动时间约 95%。environment.hdr 89 MB 对应 2048 face cubemap。

### 设计

**选择：GPU 预计算后 readback 缓存为 KTX2，后续直接加载**

缓存分为两组，独立检查：

| 组 | 产物 | 格式 | 大小 | cache key |
|----|------|------|------|-----------|
| BRDF | BRDF LUT | R16G16_UNORM, 256² | ~0.25 MB | 固定 key（与 HDR 无关） |
| Cubemaps | Skybox cubemap | BC6H UFloat, 2048², 6 faces, mip 0 only | ~24 MB | HDR 内容哈希 |
| Cubemaps | Irradiance | R11G11B10F, 32², 6 faces | ~0.03 MB | HDR 内容哈希 |
| Cubemaps | Prefiltered | BC6H UFloat, 512², 6 faces + mips | ~2 MB | HDR 内容哈希 |

```
init() 流程:
  1. 哈希 HDR 文件 → hdr_hash
  2. 检查 BRDF 缓存（固定 key）和 Cubemaps 缓存（hdr_hash）
  3. 两组各自决定：
     命中 → read_ktx2(BC6H) + upload
     未命中 → compute → strip_skybox_mips → BC6H compress → readback + write_ktx2
  4. 注册 bindless

场景                           BRDF         Cubemaps
───────────────────────────────────────────────────────
首次启动（无缓存）              compute      compute
同 HDR 重启（全命中）           缓存加载     缓存加载
切换新 HDR（BRDF 命中）         缓存加载     compute
缓存被清除                      compute      compute
```

KTX2 天然支持 cubemap 存储（`numFaces = 6` + mip chain），与纹理缓存共用自写 KTX2 读写模块。

**Readback 方案**：compute 路径在 `end_immediate()` 之前，通过 per-product staging buffer + `vkCmdCopyImageToBuffer` 读回需要缓存的产物。immediate scope 内一次性开销，不影响运行时帧率。per-product 独立 staging buffer。BC6H 压缩后 readback 总量从 ~208 MB 降至 ~26 MB。

**BRDF LUT 永久缓存**：BRDF Integration LUT 仅依赖 GGX BRDF 公式，与输入 HDR 无关。使用固定 cache key，更换 HDR 时不重算——切换 HDR 只需重算 3 个 cubemap。

---

## 总结

| 组件 | M1 实现级别 | 核心理由 |
|------|------------|----------|
| Vulkan 特性 | Dynamic Rendering + Sync2 + Extended Dynamic State | 1.4 核心，简化代码 |
| Render Graph | 手动编排 + barrier 辅助（Sync2 API），渐进增加 transient/temporal | 减痛点但不过度投资 |
| 资源 / 描述符 | Bindless（2D 纹理 4096 + Cubemap 256）+ 传统 Descriptor Set + Push Constant + DescriptorManager 集中管理 | Bindless 早期投资值得，不用 Push Descriptors |
| 资源句柄 | Generation-based（index + generation） | 捕获 use-after-free |
| 对象生命周期 | 显式 `destroy()` | 销毁顺序可控 |
| 帧同步 | 2 Frames in Flight | 低延迟，够用 |
| 顶点格式 | 统一格式（position + normal + uv0 + tangent + uv1） | 简化 pipeline 管理，M1 够用 |
| 材质系统 | 代码定义 + 固定数据结构 + 全局 SSBO（阶段二起） | 简单直接，GPU 端无法运行时灵活 |
| Shader 系统 | 运行时编译 + 热重载，公共文件按依赖链组织，阶段三无变体（无条件采样） | 开发效率优先，bindless + default 纹理消除变体需求 |
| 场景数据 | 渲染列表（完整结构一次定义，按需填充） | 简单解耦够用 |
| Temporal 数据 | 手动 Double Buffer | M1 只有 SSAO 需要 |
| 调参 | ImGui + 配置结构体 | 能调就行 |
| ImGui 集成 | Framework 层，Dynamic Rendering，专用 Descriptor Pool，RG 独立 pass | 统一由 RG 管理 |
| Pass 抽象 | M1 全程具体类（阶段二 lambda → 阶段三具体类），format 硬编码（Swapchain 例外），各 pass 方法集允许不统一，`create_pipelines()` 预留热重载。M2 评估虚基类 | 5-8 个 pass 的显式调用仍可读，方法集差异大统一接口净增复杂度 |
| Skybox | 独立 RG pass（resolved 1x，GREATER_OR_EQUAL depth test），独立 skybox.vert（VS 方向计算 + depth=0.0），Step 6 引入 | M2 Bruneton 替换 |
| Descriptor Set 绑定 | Pass 通过 `dm_` getter 获取 set，FrameContext 保持纯每帧数据 | 避免 Vulkan 类型侵入层间合同 |
| Exposure 控制 | DebugUI 手动 EV 滑条（-4 到 +4），`pow(2, ev)` exposure 乘数 | PBR + IBL 输出亮度范围大，固定值不适用 |
| 资源 Debug Name | create_image/buffer/sampler 必选 debug_name 参数 | RenderDoc + Validation Layer 可读性 |
| App 层 | Application 持有全部（注释分组）+ 模块按需设计接口 | Composition Root，阶段三提取 Renderer |
| 错误处理 | VK_CHECK + Validation Layer + Debug Utils | 开发期全面检测 |
| 深度缓冲 | D32Sfloat + Reverse-Z（阶段二起） | 远处精度提升，所有 depth pass 统一约定 |
| MSAA | 运行时可配置 1x/2x/4x/8x，默认 4x | 调试灵活，切换重建 pipeline + managed 资源 |
| Tonemapping | ACES fullscreen fragment shader | SRGB swapchain 不支持 compute，后续可替换 PBR Neutral |
| HDR Color Buffer | 全程 R16G16B16A16F | 精度避免 banding，M2 后处理链友好 |
| Normal Buffer | R10G10B10A2_UNORM, world-space | MSAA AVERAGE resolve 兼容，10-bit 精度足够 |
| IBL 管线 | GPU 运行时预计算，Framework 层，equirect .hdr 输入，IBL 自行加载并自管理全部资源，config.json 配置路径。Skybox mip 剥离 + GPU BC6H 压缩（Betsy GLSL compute） | 一次性开销，BC6H 运行时显存 ~26 MB（vs 原 ~208 MB），M2 Reflection Probes 复用 |
| IBL 环境光验证 | Step 6.5 在 Cook-Torrance 之前独立验证 IBL 数据，forward.frag Lambert 直射光 + IBL Split-Sum 环境光 | 隔离 IBL 数据问题和 BRDF 数学问题 |
| IBL 旋转 | GlobalUBO ibl_rotation_sin/cos，CPU 预计算，shader rotate_y()，仅影响环境采样 | 调试 + 艺术控制 |
| Default light 退役 | Step 6.5 退役 default light，IBL 取代固定 ambient + fallback 方向灯，保留 glTF scene lights | 无 shadow mapping 时叠加方向灯双重计算太阳贡献 |
| Sampler | Per-texture sampler，ResourceManager 管理 | glTF sampler 忠实还原，default: linear repeat mip-linear |
| Default 纹理 | 3 个 1×1（white / flat normal / black） | 材质缺失纹理填 default index，shader 无需特判 |
| 纹理格式 | 按角色选（SRGB / UNORM） | 颜色数据 gamma 解码，线性数据原样读取 |
| GPU 上传 | 批量 immediate command scope | 一次 submit，流式加载是独立系统 |
| Pass 运行时开关 | FeatureFlags 结构体 + GlobalUBO feature_flags bitmask + shader 动态分支（阶段四引入） | 调试价值高，零性能开销，无需 GPU idle |
| CSM 阴影 | 直接 CSM（不经单张中间步骤），Texture 2D Array，4 cascade 2048²（运行时可调），PSSM 分割，ShadowPass 自管理资源每帧 import RG | 一步到位避免重写，array 无 PCF 边界问题 |
| Shadow 质量 | PCF + lerp cascade blend + triple bias（constant+slope 硬件 + normal offset shader）+ alpha mask 阴影 | 性价比最优组合 |
| Shadow shader | `common/shadow.glsl` 分步函数（select_cascade / sample_shadow_pcf / distance_fade），数据全部在 GlobalUBO | forward.frag 组装调用，debug cascade 可视化无障碍 |
| PushConstantData | 4 bytes（仅 cascade_index，shadow pass pipeline 专用）。model + material_index 移至 InstanceBuffer SSBO | shadow VS 读 cascade_vp[pc.cascade_index]，forward/prepass 无 push constant |
| Per-cascade 剔除 + Culling 重构 | Step 2-5 暴力全画，Step 6 重构 culling 为纯几何（预分配 buffer），分桶/排序在调用方 | 职责分离：几何剔除与材质路由解耦，camera/shadow/probe 统一接口 |
| Shadow max_distance 初始化 | 场景加载时 `scene_aabb.diagonal() × 1.5`，退化时 fallback 100m，DebugUI 对数滑条可覆盖 | PSSM 分割质量直接取决于 max_distance，auto 初始化避免不同场景手动调参 |
| Cascade blend_width | 0.1（10% 比例制），近处窄 blend + 远处宽 blend 匹配人眼敏感度 | DebugUI 可调，默认值经分析合理 |
| Instancing | CPU 侧 mesh_id 分组 + per-instance SSBO（Set 0 Binding 3, 128 bytes/instance，含预计算 normal matrix）+ instanced draw，push constant 仅 4 bytes（cascade_index），透明物体例外 | draw call 显著减少，提前合并阶段六 SSBO 迁移和 M3 Instancing |
| 运行时加载 | ImGui + Windows 原生文件对话框 + nlohmann/json 配置持久化（`%LOCALAPPDATA%`），CLI11 完全退役，scene/HDR 独立加载/失败 | 开发体验：运行时切换场景/环境，无需重启 |
| 缓存基础设施 | `framework/cache.h` 工具模块（`%TEMP%\himalaya\` + XXH3_128 哈希 + 路径管理），纹理和 IBL 共用 | 统一缓存管理，缓存丢了就重算 |
| 纹理压缩 | BC7（通用）+ BC5（法线），bc7e.ispc SIMD 编码（SSE2/SSE4/AVX2/AVX-512 多目标），rgbcx 编码 BC4/BC5，源文件字节哈希做缓存 key（命中跳过解码），sRGB-correct mip 生成，OpenMP 纹理级并行压缩 | RGBA8 → BC 约 4:1 压缩比，GPU 原生解码零开销 |
| KTX2 读写 | 自写最小模块（7 种格式含 BC6H，DFD 硬编码），不依赖 libktx。纹理和 IBL 缓存共用 | libktx vcpkg 构建失败，实际需求远小于 libktx 功能集 |
| 多级上传 | `upload_image_all_levels()` 单 staging buffer + 批量 copy region，KTX2 布局与 Vulkan copy 天然兼容 | 预建 mip chain（BC / IBL readback）一次性上传 |
| IBL 缓存 | 两组独立缓存（BRDF 固定 key + 3 cubemaps HDR hash），BC6H 压缩后 readback（~26 MB vs 原 ~208 MB），切换 HDR 时只重算 cubemaps | 启动从分钟级降至秒级，磁盘缓存大幅缩小 |
