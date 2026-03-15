# 渲染器长远架构

> 本文档记录渲染器在长远视角下应具备的架构特征、层次结构和边界约束。
> 不包含特定 milestone 的实现级别选择，那些记录在各自的 milestone 文档中。

---

## 核心架构特征

### Render Graph

声明式的帧渲染管理系统。每个 pass 声明自己读什么资源、写什么资源，系统负责执行顺序、资源生命周期、同步屏障（barrier）、资源复用。

**为什么必须有：**

- 管线会有大量 pass（深度 PrePass、光照、SSAO、SSR、SSGI、Bloom 多级降采样升采样、Tonemapping 等），且随 milestone 推进不断增加调整
- 手动管理 VkImageMemoryBarrier 和资源状态转换在 pass 多了之后是持续的痛苦和 bug 来源
- 天然支持模块化——禁用一个 pass 就是从图里移除一个节点，其余自动调整

**演进方向：** 初期手动编排 pass 顺序 + barrier 自动插入，后期升级为自动拓扑排序 + 资源别名分析。因为每个 pass 从一开始就声明了输入输出，升级时已有数据不需要改 pass 代码。

参考：Frostbite GDC 演讲（经典参考），多个开源 Vulkan 实现。

#### 资源管理准则

GPU image 资源根据是否需要跟随屏幕尺寸 resize 选择管理方式：

| 准则 | 管理方式 | 典型资源 |
|------|---------|---------|
| 需要 resize（屏幕尺寸相关） | **RG Managed**：RG 创建、缓存、resize 自动重建 | Depth/Normal/HDR Color buffer、AO texture、Bloom chain |
| 不需要 resize（固定尺寸） | **Pass 自管理 + 每帧 `import_image()`**：Pass 完全拥有资源生命周期，RG 仅管 barrier | Shadow map array、Shadow Atlas、Froxel 3D texture |

RG Managed 的不可替代价值是 resize 自动重建。不需要 resize 的资源用 RG Managed 只会分裂所有权（image 归 RG、特殊 views 归 Pass），零收益多一层协调。自管理资源通过 `import_image()` 仍然获得 RG barrier 自动化。

RG Temporal 是 RG Managed 的子集——屏幕尺寸 + 需要历史帧数据的资源（如 AO history），由 RG 管理 double buffer 和帧间切换。

### 资源抽象层

封装 Vulkan 底层资源（VkImage、VkBuffer、VkSampler 等），不是为跨 API，而是为了：

- **生命周期管理** — 帧间资源创建销毁、transient 资源自动回收，与 Render Graph 配合
- **格式抽象** — ASTC/BC 双格式根据硬件自动选择，Lightmap HDR 格式（BC6H）处理
- **描述符管理** — Bindless descriptor（VK_EXT_descriptor_indexing）：所有纹理放入全局 descriptor array 用 index 访问，材质系统和 Render Graph 更简洁，Instancing、Shadow Atlas 等动态纹理访问无缝支持

### 材质系统

三个层次：

- **材质模板 / 着色模型** — 定义一种着色方式（标准 PBR、卡通渲染、SSS 等），对应一组 shader 变体。Forward+ 下不同着色模型就是不同的 pipeline
- **材质实例** — 基于模板设置具体参数（albedo 贴图、roughness 值等）。运行时大量物体共享同一个模板但有不同的参数
- **Shader 变体管理** — 编译时开关（有无法线贴图、有无 POM、是否接收阴影等）的编译和缓存系统

每种着色模型定义自己的 GPU 材质数据结构体（CPU 端 struct + shader 端 struct 一一对应）。添加新着色模型时，新增对应的数据结构和 shader 变体即可。不引入运行时参数描述系统——shader 端必须在编译时知道 struct 布局，运行时描述只增加 CPU 端复杂度而无法带来 GPU 端灵活性。

**Shader 编译演进：** 开发期运行时编译（GLSL→SPIR-V）+ 热重载，发布时预编译。

### 场景表示与数据流

渲染器不直接操作游戏逻辑对象，而是接收一个"渲染视图"：一组要渲染的物体（mesh + 材质 + 变换）、一组光源、一组探针、相机参数。

好处：

- **多视图渲染** — CSM 每个 cascade 从不同视角渲染、Reflection Probes 渲染六面 Cubemap、VR 双眼，都是"用不同相机参数对同一场景做渲染"
- **剔除结果分离** — 视锥剔除、遮挡剔除的结果是过滤后的索引列表，不修改场景数据本身。每个视图有自己的剔除结果
- **编辑器/工具支持** — 渲染器不关心场景数据从哪来（文件加载、编辑器修改、程序生成），只要接口一致

### Pass 可插拔性

每个渲染 Pass 是一个自包含的模块，声明自己的输入输出，注册到 Render Graph。添加或移除一个 pass 不需要修改其他 pass 的代码。

- 渐进开发模式的核心支撑——每个 milestone 就是在 Render Graph 里增加节点
- 可配置需求——MSAA 开/关、SSAO 开/关、后处理链各效果开/关，都是 pass 级别启用禁用

**Pass 粒度原则：** 实现清晰优先，每个效果保持独立 pass。后期如有性能需求可合并（如 Vignette + Color Grading 合并到 Tonemapping shader），但初始设计以模块化为主。

### Temporal 数据管理

大量系统需要帧间数据——temporal filtering（SSAO、SSGI、SSR）、TAA/DLSS/FSR、自动曝光平滑、motion vectors。

需要统一机制管理 temporal 资源——double buffer 或历史 buffer 池，Render Graph 在帧结束时把当前帧输出标记为下一帧历史输入。在 pass 声明中标记"此资源需要历史帧"，系统管理交换。

### 配置与调参系统

渲染器有大量可调参数（CSM cascade 分割、SSAO 采样半径、Bloom 阈值、自动曝光 EV 范围、PCSS 光源尺寸等）。需要统一方式暴露和调整，最好能运行时热调整。

演进方向：初期 ImGui 面板 + 配置结构体（能调就行），后期加参数注册系统、序列化（保存/加载）、热重载。

---

## 四层架构

```
Layer 3（应用层）
  ↓ 填充渲染列表，调用渲染
Layer 2（渲染 Pass 层）
  ↓ 通过 Render Graph 注册和执行
Layer 1（渲染框架层）
  ↓ 使用资源和命令接口
Layer 0（Vulkan 抽象层 / RHI）
```

**严格单向依赖** — 上层依赖下层，下层不知道上层的存在。Layer 2 的各个 Pass 之间也没有直接依赖，只通过 Layer 1 的 Render Graph 间接关联。

### Layer 0：Vulkan 抽象层（RHI）

**职责：** 封装 Vulkan 底层 API，向上层提供简洁的资源创建和操作接口。

包含：
- Device / Instance / Queue 管理
- 资源创建（Buffer、Image、Sampler）
- Bindless descriptor 管理（全局纹理 array、注册/注销接口）
- Pipeline 创建与缓存
- Shader 编译（运行时 GLSL→SPIR-V + 热重载）
- Command Buffer 录制辅助
- Swapchain 管理
- 内存分配（VMA 集成）

**设计原则：** 不包含任何渲染逻辑。它不知道什么是 Shadow Map 或 SSAO，只知道怎么创建 Image、创建 Pipeline、录制 Command Buffer。

**抽象粒度：** 薄封装为主（保持和 Vulkan 概念的对应关系），但对特别繁琐的部分（descriptor 管理、pipeline 创建、barrier 插入）做适度便利封装。不引入大型抽象概念。

**对外暴露类型：** 句柄或轻量包装类型（ImageHandle、BufferHandle 等），上层持有和传递句柄。Command Buffer 通过轻量 wrapper 暴露给 Pass 层，wrapper 的 API 和 Vulkan 命令类似但使用自定义类型，不直接暴露 VkCommandBuffer。

### Layer 1：渲染框架层（Render Framework）

**职责：** 提供渲染相关的通用框架和基础设施，不涉及具体的渲染效果。

包含：
- Render Graph（编排 + barrier 辅助 + temporal 资源管理）
- 材质系统（材质模板、材质实例、参数布局）
- Mesh / Geometry 管理（顶点格式、index buffer、mesh 加载）
- 纹理加载与格式处理（BC/ASTC、HDR 格式、mip 生成）
- 相机（投影矩阵、视图矩阵、jitter）
- 光源数据结构（方向光、点光源、聚光灯的参数定义）
- 场景渲染接口（渲染列表——mesh 实例数组、光源数组、探针数组）
- ImGui 集成

**关键设计：** 定义了"渲染一帧"的骨架——接收渲染列表，经 Render Graph 调度各 pass，输出最终图像。但具体有哪些 pass、每个 pass 做什么，由上面一层定义。

使用 Layer 0 来创建资源、录制命令，但不直接接触 Vulkan 类型。

### Layer 2：渲染 Pass 层（Render Passes）

**职责：** 实现每一个具体的渲染 Pass。每个 Pass 独立模块，声明输入输出，注册到 Render Graph。

**每个 Pass 的标准接口：**
- 声明输入资源（读哪些 buffer/texture）
- 声明输出资源（写哪些 buffer/texture）
- Setup（创建 pipeline、固定资源等，初始化时调用一次）
- Execute（每帧调用，录制 command buffer）
- 可选的配置结构体（暴露给 ImGui 调参）
- enabled() — 支持运行时开关
- on_resize() — 窗口大小变化时重建依赖分辨率的资源

**设计原则：** Pass 之间不直接互相调用或引用。数据传递完全通过 Render Graph 的资源声明。

### Layer 3：应用层（Application）

**职责：** 场景加载、资产管理、相机控制、用户输入、Demo 逻辑。

包含：
- 场景加载（glTF → 填充渲染列表）
- Lightmap / Reflection Probe 烘焙数据加载
- 相机控制器
- ImGui 面板组织（把各 Pass 的配置结构体暴露出来）
- Demo 逻辑（场景切换等）

填充渲染列表（mesh 实例、光源、探针、相机），然后调用 Layer 1 的"渲染一帧"接口。不关心渲染器内部有哪些 pass、怎么调度。

---

## 架构约束

| 约束 | 保护的架构属性 | 说明 |
|------|---------------|------|
| 上层不接触 Vulkan 类型 | Layer 0 内部实现自由度 | Layer 1/2 代码中不出现 VkImage、VkBuffer 等类型，通过句柄操作。**例外**：Render Graph 和 ImGui Backend 直接操作 Vulkan 同步/渲染原语，允许使用 VkImageLayout 等类型（见下方说明） |
| Pass 间只通过资源声明通信 | Pass 可插拔性 | SSAO 不持有 Depth PrePass 的引用，只声明"我需要 DepthBuffer 输入"，Render Graph 连接 |
| 渲染列表帧内不可变 | 多视图复用、数据安全 | 剔除结果是独立的索引列表，不从渲染列表中删除物体 |
| 配置参数单向传递 | 数据流清晰可追踪 | 应用层 → 配置结构体 → Pass 读取。Pass 私有运行时状态（如当前 EV 值）不反馈到应用层 |
| Temporal 数据归属 Pass | 模块启用禁用的干净性 | 由 Render Graph temporal 机制管理。禁用 Pass 时其 temporal 数据自然失效，重新启用时干净积累 |
| Shader 不硬编码绑定 | 材质系统灵活性 | 纹理通过 bindless index 访问，uniform 通过定义好的 buffer 布局访问 |
| Validation Layer 常开 | 开发期 bug 可见性 | Layer 0 每个 API 调用检查返回值。关键资源加 debug name（VK_EXT_debug_utils）|

### Framework 层 Vulkan 类型例外

以下 Framework 层组件允许直接使用 Vulkan 类型（VkImageLayout 等），不受"上层不接触 Vulkan 类型"约束：

| 组件 | 理由 |
|------|------|
| Render Graph | 本质是 Vulkan barrier 管理器，用 VkImageLayout 比自造枚举再映射更直观且不易出错 |
| ImGui Backend | 第三方库的 Vulkan backend 天然需要 Vulkan 类型 |
| Vertex 管线描述 | `binding_description()` / `attribute_descriptions()` 返回 `VkVertexInput*` 类型，仅用于 Pipeline 创建。再包一层自定义类型纯增复杂度 |
| `upload_image` dst_stage 参数 | 同步原语（`VkPipelineStageFlags2`），与 Render Graph 使用 VkImageLayout 属同一范畴。bitmask 类型封装成本高（枚举 + operator\| + 映射），调用点仅 texture.cpp 和 ibl.cpp 两处 |

其他 Framework 模块（Camera、Texture、MaterialSystem 等）的公开接口仍然不使用 Vulkan 类型，通过 RHI 句柄和自定义枚举操作。

### 资源命名约定

Pass 间通过名字（或 ID）引用资源，需要清晰的命名约定避免冲突。
格式：`场景.属性`，如 SceneDepth、SceneNormal、HDRColor、ShadowAtlas、AOResult 等。
在项目初期定义好并记录。

### Shader 数据分层

三层数据按更新频率分开管理：

| 层级 | 内容 | 更新频率 | 绑定方式 |
|------|------|----------|----------|
| 全局数据 | 相机矩阵、光源数组、曝光值 | 每帧一次 | 全局 uniform buffer（所有 shader 共享）|
| 材质数据 | PBR 参数、纹理 index | 加载时一次 | 全局 SSBO，通过 material index 读取 |
| Per-draw 数据 | 模型矩阵、材质 index | 每次绘制 | push constant |

### MSAA 与屏幕空间效果的处理原则

Depth/Normal PrePass 在 MSAA 下输出多采样 buffer，但屏幕空间效果（SSAO、Contact Shadows、SSR、SSGI 等）全部在 **resolve 后的单采样** buffer 上操作。

理由：
- 屏幕空间效果 shader 简单，与所有参考实现一致
- MSAA resolve 丢失的子像素信息对低频近似效果（AO 等）的影响在视觉上不可感知
- 避免 sampler2DMS 的性能和复杂度开销

具体流程：Depth/Normal 先 resolve → 屏幕空间效果在单采样上操作 → Forward Pass（MSAA）→ 透明 Pass（MSAA）→ Color resolve → 后处理链。
