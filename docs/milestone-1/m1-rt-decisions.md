# M1 路径追踪架构决策

> 阶段六~八（RT 基础设施 + PT 参考视图 + PT 烘焙器 + 间接光照集成）的架构决策。
> Milestone 级 RT 技术选型见 `../project/technical-decisions.md` 第 13 章和 `../project/decision-process.md` 第 13 章。

---

## 四层架构放置

RT 能力在每层添加新内容，遵循已有的单向依赖约束：

| 层 | 新增内容 | 说明 |
|----|---------|------|
| Layer 0 (RHI) | AS 资源类型、RT Pipeline 创建、SBT 管理、Command 扩展 | 不知道什么是路径追踪，只知道怎么创建 AS、构建 SBT、录制 trace_rays |
| Layer 1 (Framework) | Scene AS Builder、PT 配置结构 | 输入 scene data → 构建 BLAS/TLAS；烘焙器和参考视图共用的参数/配置 |
| Layer 2 (Passes) | Reference View Pass、Lightmap Baker Pass、Probe Baker Pass | 独立模块，声明输入输出，注册到 Render Graph |
| Layer 3 (App) | 渲染模式切换、烘焙 UI 控制 | ImGui 面板触发烘焙、配置参数、切换渲染模式 |

---

## 三条独立渲染路径

Renderer 内部维护三条独立路径，每条有自己的帧流程：

| 模式 | 帧流程 | 激活条件 |
|------|--------|---------|
| 光栅化 | 现有完整管线（shadow → prepass → AO → forward → skybox → postprocess） | 默认 |
| PT 参考视图 | PT Pass → OIDN → Tonemapping → Swapchain | 用户切换 |
| 烘焙模式 | Baker dispatch → 累积 → 预览进度 → Swapchain | 用户触发烘焙 |

选择独立路径而非条件跳过的理由：PT 帧流程和光栅化差异极大（无 depth prepass、无 shadow、无 AO、无 forward pass），强行塞进同一套 pass 注册逻辑会让条件分支遍布 Renderer::render()。

---

## 场景数据访问

RT shader 通过已有 descriptor 架构访问场景数据：

- **Set 0 binding 0-3**：复用（GlobalUBO + LightBuffer + MaterialBuffer + InstanceBuffer）
- **Set 0 binding 4**：TLAS（`accelerationStructureEXT`，新增）
- **Set 0 binding 5**：Geometry Info SSBO（per-mesh vertex/index buffer device address + material ID，新增）
- **Set 1**：复用（bindless textures + cubemaps）

Closest-hit shader 通过 `gl_InstanceCustomIndexEXT` 查 Geometry Info 获取 mesh 的 buffer address 和 material ID，再走与 forward.frag 相同的路径读材质参数和采样纹理。

光栅化 shader 不引用 binding 4/5，存在于 set layout 中不产生开销。

---

## OIDN 集成

**数据流**：Vulkan readback → CPU 内存 → OIDN GPU 降噪 → CPU 内存 → Vulkan upload。

"CPU 中转"指 Vulkan 与 OIDN 之间的数据通道走 CPU 内存，降噪计算本身在 GPU 上执行（CUDA/HIP/SYCL）。

选择 CPU 中转而非 VK_KHR_external_memory 零拷贝的理由：M1 的烘焙器是离线的，参考视图降噪不需要每帧执行（每隔 N 次采样触发一次），CPU 中转延迟完全可接受。省去外部内存互操作的复杂度。

**降噪频率**：参考视图不必每帧降噪，可配置为每 N 次采样后触发一次 OIDN，或由用户手动触发。

---

## 烘焙器执行模型

烘焙模式激活时接管渲染：光栅化和 PT 参考视图均跳过，GPU 全力用于烘焙。

每帧 dispatch N 个采样 → 累积到 accumulation buffer → 展示当前进度到 swapchain。达到目标采样数后执行 OIDN 降噪 → BC6H 压缩 → KTX2 持久化。

---

## 加速结构生命周期

- **BLAS**：场景加载时 per unique mesh 构建一次，`PREFER_FAST_TRACE`，场景销毁时释放
- **TLAS**：场景加载时构建一次，包含所有 mesh instance 变换，场景销毁时释放
- M1 场景静态，不需要运行时重建。架构上不阻碍后续添加动态 TLAS 更新

---

## Lightmap UV2

**生成**：xatlas 运行时自动生成。per-mesh 按需标记是否需要 lightmap UV，M1 全部静态 mesh 标记。

**缓存**：xxHash 内容哈希 + 自定义二进制格式。

缓存文件内容：
- Header：magic + version + 源 mesh xxHash + atlas 尺寸 + 顶点数 + 索引数
- Data：UV2 坐标数组 + 新 index buffer + 顶点重映射表（新顶点 → 原始顶点）

缓存命中时直接读取，未命中时跑 xatlas 后保存。mesh 几何变化 → hash 变化 → 自动重新生成（旧 lightmap 也需重新烘焙）。

---

## Reflection Probe 放置

**网格 + 几何过滤**：场景 AABB 内按固定间距放置 3D 网格探针，利用 RT 射线检测剔除落在几何体内部的探针。密度通过 ImGui 参数可调。

---

## 烘焙产物格式

| 资源 | 累积格式 | 使用格式 | 持久化格式 |
|------|---------|---------|-----------|
| Lightmap | RGBA32F | BC6H | KTX2 (BC6H) |
| Reflection Probe | RGBA32F | BC6H + mip chain | KTX2 (BC6H + mip chain) |

复用已有 BC6H 压缩 shader 和 KTX2 读写基础设施。Probe 的 prefilter mip chain 复用 IBL prefilter 管线。

---

## RT 扩展可选性

RT 扩展（`acceleration_structure` + `ray_tracing_pipeline` + `ray_query`）为可选：

- 设备支持 RT → 启用全部 RT 功能（参考视图、烘焙器、M2+ 实时 PT）
- 设备不支持 RT → 禁用 RT 功能，仅光栅化模式可用
- 阶段八间接光照集成不受影响——Lightmap/Probe 数据从 KTX2 加载，无论是自家烘焙器还是外部工具生成的

---

## 参考视图后处理

PT 参考视图仅经过 Tonemapping → Swapchain，不走其他后处理（Bloom、Vignette、Color Grading 等）。

理由：参考视图的目的是 debug 和验证 PT 光照正确性，额外后处理会干扰对光照结果的判断。
