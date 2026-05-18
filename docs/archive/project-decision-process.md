# 渲染技术栈决策过程

> 本文档记录每项技术选型的推理过程：考虑了哪些方案、关键权衡因素、为什么选定或排除。
> 最终决策结果见 `technical-decisions.md`，需求与设计理念见 `requirements-and-philosophy.md`。

---

## 1. 渲染框架：为什么选 Forward+

### 候选方案

| 方案 | 核心特点 |
|------|----------|
| 传统 Deferred Shading | 多光源性能好，GBuffer 自然支持屏幕空间效果 |
| Forward+（Clustered/Tiled Forward） | 材质自由度高，天然 MSAA，带宽小 |
| Visibility Buffer（Deferred Texturing） | 带宽极小，GPU-driven 友好，UE5 Nanite 方向 |

### 排除过程

**Visibility Buffer** 最先排除：公开的完整实现和资料相对少，不符合"资料丰富、AI 辅助可靠"的约束。

**Deferred Shading** 的核心优势——多光源解耦——在本项目场景下被弱化：动态光源不多且会使用烘焙技术。而它的劣势在本项目约束下反而被放大：

- **移动端 TBDR 架构根本性不兼容**：移动端 GPU（Mali、Adreno、PowerVR）的 TBDR 架构核心设计是数据留在片上 Tile Memory。Deferred 的 GBuffer 多 RT 写出再读回从根本上打破了 TBDR 工作模式，性能损失可达翻倍级别。这不是优化问题而是架构适配问题——选择 Deferred 意味着以后支持移动端需要重写整个光照管线。这恰好是"以后没法通过模块化替换"的典型情况。
- **MSAA 不兼容**：VR 场景下 TAA 的 temporal ghosting 在立体视觉中更明显，MSAA 仍是重要选项。Deferred + MSAA 需要 per-sample 着色（性能爆炸）或复杂 workaround。
- **材质灵活性受限**：保留卡通渲染能力的需求在 Deferred 中意味着 GBuffer 编码 ShadingModelID + 光照 Pass 分支，Forward+ 中不同物体直接绑不同 shader 即可。

### Forward+ 的"牺牲"评估

在本项目约束下逐项审视 Forward+ 的代价：

- **屏幕空间效果需要额外 PrePass**：Forward+ 做 SSAO/SSR 需要 Depth+Normal PrePass。但 Z-PrePass 在现代渲染器中几乎是标配（Early-Z 优化），额外输出 Normal 代价很小。一次性基础设施工作。
- **混合管线需要额外 thin GBuffer**：成本只在真正启用 RT 时存在，且 RT Pass 本身是性能大头，额外属性 Pass 占比小。
- **Clustered Light Culling 实现**：比 Deferred "全屏 quad 逐光源画"的模式多了一层抽象。但有自然的渐进路径（暴力→Tiled→Clustered），DOOM 2016 推广后资料充足，且一旦写好基本不用改。

### 最终判断

Forward+ 获得的（平台兼容、MSAA）是架构级不可逆收益，牺牲的在本项目场景下几乎都可通过额外的、可控的工作弥补。没有哪一项牺牲构成"感知强烈的质量损失"。

### 光源 Culling 渐进路径

暴力 Forward 在光源个位数到十几个时完全够用，足以撑住相当长的开发周期。每一步到下一步不需要重构——暴力到 Tiled 是加一个 Compute Pass，Tiled 到 Clustered 是多一个维度。渲染管线其余部分完全不受影响。

---

## 2. PBR / BRDF：标准工业配置

### 决策逻辑

PBR 的选型争议很小，基本是业界标准的直接采用：

**漫反射**：Lambert 和 Burley Diffuse 的视觉差异主要体现在粗糙表面掠射角（retroreflection），大部分场景下几乎看不出区别。Burley 多几行 shader 代码、性能无差别，作为低成本升级放在 Pass 2。排除 Oren-Nayar（被 Burley 取代，用得少）。

**镜面反射 D 项**：GGX 是当前工业标准，长尾分布、高光拖尾自然，没有理由不用。排除 Beckmann（被 GGX 取代）。Multiscatter GGX 解决高粗糙度金属偏暗的问题，实现成本低（一张 LUT + 十几行 shader），与 IBL BRDF Integration LUT 共享基础设施，作为 Pass 2 升级。

**G 项和 F 项**：Smith Height-Correlated GGX 是 GGX NDF 数学上配套的选择（UE4/5、Filament 等均采用），Schlick 近似是几乎所有引擎的标准答案。两者不受 Multiscatter 影响，直接锁定。

**参数化**：Metallic-Roughness 是当前行业标准，glTF 原生支持，资产生态最好。排除 Specular-Glossiness（更容易出现物理不正确的参数组合）。

---

## 3. 阴影：四个维度的独立决策

### 维度一：方向光阴影

**CSM 是绝对主流**，几乎所有引擎都用。排除的方案：

- **Virtual Shadow Maps（UE5）**：画质上限极高但实现复杂度也极高——需要虚拟纹理系统、页面管理、GPU-driven culling。个人项目从零实现工作量非常大。
- **SDF 混合**：需要完整 SDF 基础设施，单独为阴影建 SDF 不划算。但如果 GI 走 SDF 路线，SDF 数据已存在时集成代价很低（光照 shader 加几十行 Ray March + 距离混合）。因此设为条件性 Pass 3。

CSM → SDF 混合的演进是自然的：两者职责范围不重叠（CSM 管近中距离，SDF 管远距离），CSM 不需要任何改动，集成点在着色阶段而非阴影生成阶段。

### 维度二：点光源/聚光灯阴影

动态光源不多，Pass 1 阶段专注方向光即可。Shadow Atlas 解决的是"光源多了以后的统一管理和调度"，在光源少的场景下可能不需要。因此 Pass 2 直接带缓存策略（缓存逻辑轻量不必单独拆步），Shadow Atlas 放 Pass 3。

排除 Dual Paraboloid（接缝精度问题不值得省下的性能）。Multi-View Cubemap 优化等点光源 Cubemap 6 面开销问题在光源少时不是瓶颈，留待需要时再考虑。

### 维度三：软阴影

**PCSS 相对 PCF 是质变级提升**——半影宽度随遮挡距离变化是写实感的重要因素。性能开销在目标硬件上可控。排除 VSM/ESM/MSM——各有 artifact 问题（光渗、深度差问题），会成为持续的调参负担。

**Contact Shadows** 性价比极高——实现简单、和 Shadow Map 系统完全解耦、视觉提升明显。与 PCF/PCSS 不在同一演进线上（独立的屏幕空间补充），因此早于 PCSS 实现。

### 维度四：glitch 防控

所有方案在"无明显 glitch"约束下的检查：
- CSM 移动闪烁：texel snapping（必做标配）
- Shadow Acne / Peter Panning：slope-scaled bias + 常量 bias
- PCSS 采样噪声：增加采样数或 temporal filtering
- Contact Shadows 屏幕边缘：作用距离短，实际中很难注意到

---

## 4. 全局光照：烘焙为主、屏幕空间补充

### 核心思路

GI 的需求拆成三个层次：静态场景（Lightmap 覆盖）、时间变化（多套 Lightmap blend）、场景拓扑变化（需讨论）。

### 多套 Lightmap Blend

对于昼夜循环：烘焙几个关键时间点的 Lightmap，运行时相邻两套插值。Far Cry 系列做过类似方案。代价是存储（N 套 Lightmap），运行时性能几乎零额外开销（采样两张做 lerp）。

对于门开关：门完全开和完全关各烘焙一套，过渡期 SSGI 补充。**组合爆炸问题**通过设计约束解决——限制同时仅一扇可见门可交互。这样基础状态一套 + 每个可交互门各一套，存储可控。

这个方案的核心优势是**用离线烘焙的质量解决了通常需要实时 GI 才能处理的问题**，同时避免了实时 GI 方案（Irradiance Probes、SDF GI 等）的巨大复杂度。

### SSGI 的定位

不是主 GI 方案，而是叠加在 Lightmap 之上的补充。Lightmap 提供基础间接光照水平，SSGI 处理动态变化的局部响应（color bleeding、门过渡态等）。屏幕空间信息缺失问题被 Lightmap 基底大幅缓解。

与 SSR 共享大量基础代码（都是屏幕空间 Ray March），先做 SSR 后 SSGI 是半价实现。

### 排除/延后的方案

- **Irradiance Probes + SDF GI**：在已有 Lightmap + SSGI 的基础上，边际收益集中在"动态变化 + 屏幕外"这个交集——触发频率低、视觉影响细微。代价是 SDF 基础设施 + 探针管理 + 漏光处理。投入产出比偏低，标记为远期可选。
- **Voxel Cone Tracing**：需要完整体素基础设施，归入 SDF GI 一并考虑。
- **RSM / LPV**：未深入讨论，被 Lightmap + SSGI 方案覆盖。

### 镜面反射 GI

SSR + Reflection Probes 混合是业界最常见的性价比方案。SSR 有结果时用 SSR（精确近距离反射），没结果时 fallback 到 Probe。SSR 需要 Reflection Probes 作为回退，因此 Probes 先于或同时于 SSR。

### Pass 归属考虑

Pass 2 将 Lightmap 和 Light Probes 放在一起——两者都属于"烘焙/预计算"思路且 Light Probes 依赖 Lightmap 数据。Reflection Probes 独立于漫反射侧，归入镜面反射演进线。SSR 和 SSGI 共享基础设施放在同一个 Pass 3 阶段，多套 Lightmap blend 是在单套基础上的扩展同样放在 Pass 3。

---

## 5. 环境光遮蔽：跳过 HBAO 直达 GTAO

### 演进线

SSAO → HBAO → GTAO 是一条清晰的演进线，每步改进数学模型，实现结构大体相同。HBAO 到 GTAO 的跨度很小——如果能实现 HBAO，GTAO 只是把遮挡计算公式换了。因此直接跳过 HBAO。

### Temporal filtering 的战略价值

Per-effect temporal filtering 是屏幕空间效果的共享基础设施，SSAO/SSGI/SSR 均复用同一模式（per-effect 历史 buffer + reprojection + blend）。实现成本低，但收益贯穿所有屏幕空间效果。

**适用范围**：per-effect temporal 作用于**独立的中间效果纹理**（AO texture、SSR result 等），对每个效果单独降噪。它**不能**平滑嵌在最终光照颜色中的信号噪声（如 specular aliasing）——那些需要 FSR/DLSS 的**全画面 temporal accumulation**（对最终颜色做时域积累，SDK 自带实现，依赖 Motion Vectors，不复用 per-effect 基础设施）。

### 排除的方案

- **VXAO / DFAO**：需要体素/SDF 基础设施，单独为 AO 建设不划算。
- **RTAO**：混合管线阶段的事情，当前不考虑。但所有屏幕空间 AO 方案在混合管线里可被 RTAO 无缝替换（接口一致：输入场景，输出单通道遮挡纹理）。

---

## 6. 抗锯齿与上采样：SDK 替代自研

### 核心决策：不自研 TAA

FSR 在输入输出分辨率 1:1 时等价于高质量 TAA（AMD 官方文档确认）。一个 SDK 同时解决 native AA 和 upscaling。效果比大多数自研 TAA 好，没有理由自己写。

### MSAA + TAA 共存

两者解决不同类型锯齿——MSAA 处理几何边缘（光栅化阶段多采样），TAA 处理时域闪烁和着色锯齿（高光闪烁、法线贴图噪声）。TAA 输入已是 MSAA resolve 后几何边缘干净的图像，模糊程度降低。Forward+ 下 MSAA 开销相对可控。低分辨率 + MSAA + upscale 的组合性能可接受。

### SDK 选择

| SDK | 特点 | 决策 |
|-----|------|------|
| DLSS/DLAA | 效果最好，RTX 专有 | N 卡首选 |
| FSR 2/3 | 开源跨平台，所有 GPU | 通用回退 |
| XeSS | 跨平台但生态不如 FSR | 远期可选 |

两个 SDK 需要的输入基本一致（jittered color、depth、motion vectors、exposure），统一接口层让切换 SDK 成为配置项。

### Motion Vectors

不管选哪个 SDK 或 temporal 效果都需要。静态场景仅需相机 reprojection，动态物体阶段需 per-object。越早搭建越好。

---

## 7. 色彩管线：sRGB 够用，广色域留接口

### 色域选择

广色域（ACEScg/Rec.2020）的边际收益在实时渲染中有限——主要避免高饱和度光照下色彩裁切。但输入贴图大概率是 sRGB authored 的，需要额外的色域转换，增加复杂度和心智负担。sRGB 色域作为工作空间完全够用。

### Tonemapping

**ACES 作为起步**：资料最多、一行代码。但 ACES 有色相偏移（蓝色偏紫）和饱和度压缩，近年有反思。

**Khronos PBR Neutral 作为进阶**：专为 PBR 设计，最小化 tonemapping 对材质外观的干扰。与本项目"主体标准 PBR 追求物理正确"高度契合——花大量精力在 PBR 材质和光照上追求物理正确，tonemapping 不应再扭曲。

排除 AgX（仍有风格化 S 曲线，不如 PBR Neutral 对 PBR 管线的中性度）。如需风格化对比度和色彩，可在 Color Grading 中单独控制，比在 tonemapper 中隐式引入更可控。

---

## 8. 后处理：按性价比分层

### 必做效果的理由

- **Bloom**：没有 Bloom 的 HDR 画面"数字感"强。物理正确版本不设硬阈值。
- **自动曝光**：HDR 管线几乎必须——否则每个场景手动设 EV 值。用户曾在 UE 中感觉"画面灰"，原因是范围限制和补偿没调好，不是自动曝光本身的问题。通过限制 EV min/max、曝光补偿、加权测光解决。
- **Color Grading / Vignette / Chromatic Aberration / Film Grain**：每个都是几行代码，加起来可能半天工作量但画面观感差异大。

### DOF 演进

scatter-based Bokeh 非常贵（变相粒子系统），排除。现代 gather-based 方案半分辨率下 0.5-1.5ms 可接受。跳过"半分辨率 Gaussian"中间步骤——反正要做 Bokeh，直接在半分辨率上做 gather-based Bokeh 一步到位。Bokeh 标记为可选——DOF 在游戏中争议大（干扰操作），如果演示场景不以浅景深为卖点可以停在 Gaussian。

### God Rays 定位调整

从"低优先级后处理"调整为"体积光渐进路线的占位"——核心就是一个后处理 pass 里的 for 循环（三四十行 shader），Froxel Volumetric Fog 实现后退役。

### Lens Flare

基于 Bloom 降采样的屏幕空间方案效果微妙——弥散的彩色光斑增加镜头感，做得克制不出戏。但不会产生明确的六角形光斑，期望不宜过高。保持低优先级。

### 不做 Sharpening

DLSS/FSR 内建一定程度的 sharpening，额外做的必要性低。

---

## 9. 大气与天空：跳过 Hosek-Wilkie

### 天空方案对比

**Hosek-Wilkie**（程序化天空）和 **Bruneton**（物理大气散射）是完全独立的两套方案——前者经验拟合，后者物理模拟，代码/数据结构完全不同。从 Hosek-Wilkie 到 Bruneton 是替换不是升级。

Hosek-Wilkie 的唯一过渡价值是更早获得动态天空配合昼夜调试，但其实现代码在切换后完全废弃。Bruneton 资料充足（Hillaire 论文 + 多个 Vulkan 参考实现），直接跳过反而更省总工期。

### 雾与大气散射的协同

Bruneton 实现后 aerial perspective 是免费副产品——远处物体颜色自然融入大气，不需要单独做"雾"。高度雾在 Bruneton 之前作为占位，之后自然替换。天空 Pass 2 和雾 Pass 2 是同一步工作。

### 云的优先级判断

体积云是整个技术栈中**复杂度最高的单一模块**——Ray March、3D 噪声、时域 reprojection、半分辨率渲染、与大气散射的光照交互。用渐进原则审视：2D 云 → 体积云不是渐进演进而是替换重写。从复杂度-收益比角度降级为远期可选，2D 云层配合 Bruneton 已可给出不错的室外画面。

---

## 10. 透明、半透明与体积渲染

### 透明排序

简单排序在透明物体少且不互相穿插时完全够用。WBOIT 作为按需方案——一个 pass 完成、不需排序、性能固定且低、近似误差实际中难以注意到。两者在架构上是替换关系（blend 模式、RT 配置、shader 不同），但透明 pass 代码量小，重写成本可控。

排除 MBOIT（复杂度高收益仅在极端情况）、Per-Pixel Linked List（性能不可控）。

### 次表面散射

**Pre-Integrated SSS** 适合作为 Pass 1：成本极低（LUT 替代 Lambert，几行 shader），Forward+ 天然适配，没有屏幕空间 SSS 的轮廓渗色问题。但质量明显低于 Screen-Space SSS——只能表现明暗过渡柔和感，不能表现光在表面的横向扩散。

**Screen-Space SSS** 作为 Pass 2 质变级提升：轮廓渗色通过深度/法线不连续性检测解决（标准做法），透射需额外 thickness map。两者可叠加但通常无必要——Screen-Space SSS 已包含 Pre-Integrated 试图解决的效果。

### 体积渲染

**Froxel-Based Volumetric Fog** 是光栅化管线下体积渲染的终点，再往上就是 RT 体积渲染。与 Clustered Light Culling 有概念相似性（3D 格子结构）。实现后 Screen-Space God Rays 直接退役。

### 水体

静态水体的大部分需求被已有基础设施覆盖（SSR → 反射、屏幕空间折射 → 折射、PBR Fresnel → 菲涅尔）。额外工作集中在深度着色（几行代码）、法线贴图（标配）、焦散和岸线泡沫。

焦散选择"根据法线计算投影"而非"纯贴图投影"——焦散形状与水面法线一致更物理正确，且静态水体可预烘焙为纹理，运行时零额外计算。

---

## 11. 几何管线：不做 GPU-Driven

### GPU-Driven 的排除

GPU-Driven Rendering 是所有已讨论系统中**实现复杂度最高的**，且复杂度不集中在一个点上而是弥散到整个架构：

- 场景数据组织方式需要全面重新设计（GPU 可读的连续 buffer）
- 材质系统需配合改造（bindless texture / texture array）
- 绘制逻辑搬到 Compute Shader（调试困难度高）
- 从传统模式后期改造基本是重写

它解决的核心问题——CPU drawcall 瓶颈——在 Vulkan 多线程 Command Buffer + 几千到上万 drawcall 的场景下不是真正的瓶颈。只有几十万独立实例级别才需要。

### 遮挡剔除的选择

排除 Hi-Z（最自然的使用场景是 GPU-Driven Compute Shader + 间接绘制，独立使用架构别扭）。

**Hardware Occlusion Query vs Software Occlusion Culling**：

| | Hardware OQ | Software OC |
|--|-------------|-------------|
| 优势 | 复用深度 PrePass，不需额外遮挡体选择 | 无 GPU 延迟，当帧计算 |
| 劣势 | 两趟绘制同步点 | 需要自建软光栅器 + 遮挡体选择 |

选择 Hardware OQ：与已有管线契合度更高（深度 PrePass 已存在），实现成本低。保守两趟策略保证零瑕疵——上一帧可见的一定画（不突然消失），上一帧不可见的同帧补画（不晚一帧出现）。

### 其他排除

- **Mesh Shader**：移动端不支持，无 GPU-Driven 时收益有限（只是替代传统顶点管线的另一种写法）。
- **HLOD**：离线处理工具链开发成本高，场景规模不大时不需要。
- **Nanite-style 虚拟几何**：个人项目从零实现不现实。

---

## 12. 纹理、烘焙、地形、粒子、Streaming

### 纹理格式

ASTC 是未来方向（压缩率和质量比 BC 灵活，移动端标准），但桌面端支持面不是 100%。务实策略是 ASTC 为主格式 + BC 为回退，构建时同时生成两套。

### Virtual Texturing

VT 的需求来源主要是多套 Lightmap 的存储压力。但 BC6H 压缩后单套 ~100MB 级 Lightmap，4 套时间状态 + 几套门状态压缩后总共约 100-150MB，对桌面端 8GB+ 显存完全可容纳。压缩 + 按区域分块加载足以替代 VT，而 VT 的实现成本（页面管理、反馈 buffer、间接寻址表、所有采样 shader 改造）很高。

决策：不做 VT，但 Lightmap 采样封装为统一接口，预留将来替换为 VT 间接寻址的可能。

### 烘焙管线

Pass 1 用离线工具的理由：先解决"有 Lightmap 可用"的问题，Blender Cycles 路径追踪质量很高。

Pass 2 RT 烘焙的战略价值：烘焙器就是 RT 管线的第一个应用场景——同一套 BVH 构建和路径追踪基础设施。烘焙器开发和 RT 管线开发是同一件事，不是额外工作量。

### 地形

Heightmap 是唯一务实的大面积地形方案。Voxel 地形支持任意形状但实现复杂度高很多。Heightmap 的非高度场限制（不能表现洞穴悬崖）通过普通 Mesh 补全——两个系统完全独立，Heightmap 是一个模块，Mesh 走已有渲染管线。接缝通过材质混合掩盖，LOD 通过距离参数统一协调。

### Streaming 优先级

初期全部预加载（演示场景规模不会爆显存）。异步资源加载框架是所有 Streaming 的前置依赖——后台线程加载 + Vulkan Transfer Queue 异步上传。纹理 Mip Streaming 最先需要（显存压力主要来自纹理），其他按需跟进。

---

## 13. 路径追踪与混合管线：渐进式引入 RT

### 核心思路

光栅化管线保留为完整的独立渲染模式，RT 作为并行的新能力渐进引入。烘焙器是 RT 管线的第一个应用场景（M1），实时 PT 和混合效果在后续 milestone 展开。两种模式共存而非替换。

### 跳过离线烘焙工具

原计划 M1 用 Blender 等离线工具烘焙 Lightmap / Reflection Probes，M1 后期再做 RT 烘焙器。调整为 M1 直接实现 GPU PT 烘焙器：

- 烘焙器本身就是 RT 基础设施（加速结构、RT pipeline）的第一个消费者，两者一起做最自然
- 避免离线工具 → 自研烘焙器的迁移成本（数据格式、工作流更换）
- 为 M2 实时 PT 提前积累 RT 基础设施

### RT API 双轨选择

| 候选 | 评估 |
|------|------|
| 仅 RT Pipeline | 烘焙器/参考视图适合，但混合效果（RT Reflections/Shadows）需要在 fragment shader 内联查询，用 RT Pipeline 不自然 |
| 仅 Ray Query | 混合效果适合，但烘焙器/参考视图需要自己在 compute shader 中管理射线调度，不如硬件 RT Pipeline 高效 |
| **双轨** | RT Pipeline 用于大量射线场景（烘焙器、参考视图、实时 PT），Ray Query 用于混合效果（fragment/compute 内联），各取所长 |

选择双轨。两者共享 `VK_KHR_acceleration_structure`，BLAS/TLAS 构建一次两边复用。

### M1 PT 技术选型

**采样基础**全部采纳（NEE、MIS、Russian Roulette、cosine-weighted hemisphere、GGX importance sampling）——实现难度均为低，是任何 PT 的标配，缺少任何一个都会严重影响收敛速度。

**采样质量**选择低差异序列 + Blue noise：
- 低差异序列（Sobol/Halton）替代纯随机数，同样采样数下收敛明显更快，实现就是换随机数生成器
- Blue noise 时空抖动让噪点模式对人眼更友好、对降噪器输入更好，实现就是加载纹理 + 几行偏移
- **Light BVH 不做**：M1 光源少（方向光 + 少量发光面），暴力遍历够用。M2 ReSTIR DI 自然覆盖多光源采样

**降噪选择 OIDN GPU**（烘焙器 + 参考视图统一）：
- OIDN 2.0+ 支持 GPU 降噪（CUDA/HIP/SYCL/Metal），可完全在 GPU 上运行
- 烘焙器离线场景和参考视图 viewport denoising（类似 Blender 视口降噪）都适合 AI 单帧降噪
- 排除 NRD：NRD 是帧间 temporal 降噪器，为实时游戏设计。M1 的烘焙器不需要帧间一致性，参考视图靠累积采样收敛 + OIDN 单帧降噪即可
- NRD 留到 M2 实时 PT 阶段接入——实时场景需要 temporal 稳定性（相机移动时不闪烁）

### M2 实时 PT 技术选型

**直接光照选择 ReSTIR DI**：算法革命级——通过 reservoir 数据结构在像素间时空共享光源采样，等效于追踪了远超实际数量的 shadow ray。百万级光源场景仅需几条 ray/pixel。NVIDIA RTXDI SDK 提供生产实现。

**间接光照选择 SHaRC**（Spatial Hash Radiance Cache）：
- 候选：暴力多 bounce PT、SHaRC、DDGI、NRC
- 暴力多 bounce 排除：1 SPP 下 2-3 bounce 的间接光极度嘈杂
- DDGI 排除：仅处理漫反射 GI，镜面间接光需额外方案；探针漏光需要额外处理
- NRC 排除：依赖 Tensor Core 和 Cooperative Vectors，放在 Future
- **SHaRC 入选**：世界空间哈希网格缓存辐射度，96% 路径 1-2 bounce 即终止。开源 SDK，DOOM: The Dark Ages 验证。间接光偏"柔和模糊"（网格分辨率限制），但性价比最优

**降噪选择 NRD**：实时场景需要帧间 temporal 稳定性，OIDN 单帧降噪会帧间闪烁。NRD 的 ReBLUR/ReLAX/SIGMA 分别针对间接光/ReSTIR 信号/阴影优化。

**画面水平**：≈ DOOM: The Dark Ages 路径追踪模式。直接光部分（ReSTIR DI）接近 Cyberpunk RT Overdrive，间接光部分（SHaRC vs ReSTIR GI）低一档。

### 混合 RT 效果筛选

原则：仅选择光栅化有**明显可见瑕疵**、RT 能显著改善的效果。

| 效果 | 光栅化瑕疵程度 | 决策 |
|------|-------------|------|
| RT Reflections | **高**——SSR 屏幕边缘反射突然消失、背后物体完全缺失，Probes 低频近似形状失真 | M2 采纳 |
| RT Shadows | **中高**——Shadow map 分辨率锯齿、cascade 边界过渡、远处小物体阴影消失 | M2 采纳 |
| RTAO | **低**——GTAO + temporal filter 观感尚可，薄物体光晕等问题实际中不太容易察觉 | 不做 |
| RT GI | **低**——Lightmap 基底大幅缓解了 SSGI 屏幕空间限制 | 不做（M3 ReSTIR GI 直接覆盖） |

### M3 间接光升级

ReSTIR GI 替换 SHaRC——同属 ReSTIR 家族，共享概念和基础设施（reservoir 数据结构、时空重采样框架），是 M2 ReSTIR DI 的自然延伸。间接光从哈希网格的低频近似提升到像素级路径重采样，高频 color bleeding 和小缝隙间接光变化精确可见。

### Future 两条演进线

**神经网络渲染**：NRC 替代 SHaRC/ReSTIR GI 的缓存层（连续函数 vs 离散网格/采样），DLSS Ray Reconstruction 替代 NRD（单 transformer 推理 vs 多 pass 手工滤波），Cooperative Vectors 标准化 shader 内神经网络推理使其跨厂商可用。

**算法统一**：ReSTIR PT 基于 GRIS 框架对完整光传输路径做重采样，统一替代 ReSTIR DI + ReSTIR GI，一个算法处理所有光传输现象（直接光、间接光、焦散）。

---

## 附：跨模块决策关联

以下决策之间存在相互影响：

| 决策 A | 影响 → 决策 B | 关联方式 |
|--------|---------------|----------|
| Forward+ | → MSAA 可用 | 架构天然支持 |
| Forward+ | → 屏幕空间效果需 PrePass | Depth+Normal PrePass 成为标配 |
| SDF GI（远期） | → SDF 阴影混合可用 | 共享 SDF 基础设施 |
| SSR | → SSGI 半价实现 | 共享屏幕空间 Ray March 代码 |
| per-effect temporal filtering | → SSAO/SSGI/SSR 受益 | 共享 per-effect 历史 buffer + reprojection，仅作用于独立中间效果纹理，不覆盖最终颜色中的噪声（如 specular aliasing） |
| 全画面 temporal（FSR/DLSS） | → specular aliasing 等最终颜色噪声 | SDK 自带实现，对最终颜色做时域积累，依赖 Motion Vectors，与 per-effect temporal 是独立系统 |
| Motion Vectors | → 所有 temporal 效果 + SDK upscaler | 底层共享依赖 |
| Lightmap blend | → 门状态设计约束 | 设计层面解决技术复杂度 |
| Bruneton 大气散射 | → 高度雾退役 + IBL/Probe 数据源 | 天空和雾同步完成 |
| Froxel 体积雾 | → God Rays 退役 | 替换关系 |
| IBL BRDF LUT | → Multiscatter GGX LUT | 共享 LUT 生成基础设施 |
| 深度 PrePass | → Hardware OQ 复用深度 | 无额外数据准备 |
| RT 烘焙 | → 混合管线 RT 基础 | 同一套 BVH + 路径追踪 |
| M1 RT 基础设施（AS + RT Pipeline） | → M2 实时 PT + 混合效果 | BLAS/TLAS 构建、RT pipeline 抽象直接复用 |
| M1 OIDN 降噪 | → M2 NRD 降噪 | 不复用，M2 替换为 NRD（temporal 需求） |
| M2 ReSTIR DI | → M3 ReSTIR GI | 同一 family 算法，共享 reservoir 概念和基础设施 |
| M2 SHaRC | → M3 ReSTIR GI 替换 | 替换关系，SHaRC 退役 |
| M2 NRD | → Future DLSS RR 替换（NVIDIA 路径） | NRD 跨平台保留，DLSS RR 作为 NVIDIA 增强 |
| Future Cooperative Vectors | → NRC、神经材质、神经纹理压缩 | 标准化 shader 内推理，使多项神经技术跨厂商可用 |
