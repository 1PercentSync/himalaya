# Milestone 1：开发顺序

> 本文档记录 M1 的分阶段开发计划，每个阶段结束都有可见的、可验证的产出。
> 帧流程详见 `m1-frame-flow.md`，设计决策详见 `m1-design-decisions-core.md` + 各阶段 `m1-phase*-decisions.md`，接口定义详见 `m1-interfaces.md`。

---

## 设计逻辑

1. **每个阶段有可见产出** — 不会在看不到结果的基础设施上连续工作太久
2. **尊重依赖关系** — Render Graph 先于所有 pass，深度 PrePass 先于屏幕空间效果
3. **基础设施复用** — temporal filtering 在阶段五搭建后，M2 的 SSR/SSGI 可直接复用

---

## 阶段一：最小可见三角形

**Layer 0 核心**

- Vulkan 初始化（Instance、Device、Queue、Swapchain）
- VMA 内存分配集成
- Shader 运行时编译（GLSL → SPIR-V，shaderc 集成）
- Command Buffer 录制辅助
- 基础资源创建（Buffer、Image）
- 基础 Pipeline 创建
- 一个硬编码的三角形能显示在屏幕上

**产出：** 验证整个 Vulkan 基础设施能跑通。这是后续所有工作的地基。

---

## 阶段二：基础渲染管线

**Layer 0 补全 + Layer 1 框架 + Layer 3 重构与加载**

- Bindless descriptor 管理（DescriptorManager：set layout、descriptor set、bindless 纹理注册/注销）
- Mesh 加载（fastgltf 解析 glTF、统一顶点格式）
- 纹理加载（stb_image 解码 JPEG/PNG、GPU 端 mip 生成）
- 材质系统基础（全局 Material SSBO + push constant 数据流、标准 PBR 参数布局）
- 相机（framework 层 Camera 数据结构 + app 层自由漫游控制器）
- 场景渲染接口（完整 SceneRenderData 结构，阶段二只填充需要的字段）
- glTF 场景加载 → 填充渲染列表
- 视锥剔除（CPU 端 AABB-frustum）
- Render Graph 骨架（barrier 自动插入 + 资源导入，typed handle 引用）
- App 层拆分（main.cpp → application / scene_loader / camera_controller / debug_ui）

**产出：** 能加载一个 glTF 场景，用基础 Lit shader（Lambert + 从 LightBuffer 读取方向光）渲染出来，相机能移动，ImGui 能显示。画面上就是一个有纹理和基础光照的静态场景。

**暂不实现：** RG transient 资源管理（阶段三引入）、RG temporal 资源管理（阶段五引入）、Pass 抽象基类（阶段三引入）。BC 纹理压缩在阶段四准备工作引入（运行时压缩 + KTX2 缓存）。

> 详细步骤见 `../current-phase.md`

---

## 阶段三：PBR 光照基础

**Layer 2 核心 Pass**

- Forward Lighting Pass（Lambert 漫反射 + Cook-Torrance 镜面反射：GGX / Smith / Schlick）
- 方向光直接光照
- IBL 环境光（Split-Sum 近似、BRDF Integration LUT 预计算、HDR Cubemap 加载）
- Depth + Normal PrePass
- MSAA 开启 + Depth Resolve + Normal Resolve + Color Resolve
- Tonemapping Pass（ACES 拟合，fullscreen fragment shader，直接写 swapchain）

**产出：** 场景有了基本正确的 PBR 光照——直接光 + 环境光。金属表面反射天空 Cubemap。HDR 渲染结果经过 Tonemapping 映射到 LDR。但没有阴影、没有 AO，画面会偏平。

---

## 阶段四：阴影

- Instancing：per-instance SSBO + mesh_id 分组 + instanced draw，push constant 72→4 bytes
- 运行时场景/HDR 加载：ImGui + Windows 原生对话框 + JSON 配置持久化，CLI11 退役
- 缓存基础设施 + BC 纹理压缩 + IBL 缓存：BC7 + BC5 + KTX2 缓存 + IBL readback 缓存
- RenderFeatures 运行时开关机制（为后续可选效果奠基）
- 直接实现 CSM（cascade=1 起步，不经单张中间步骤，4 cascade 2048²，PSSM 分割、texel snapping）
- PCF 软阴影 + cascade blend + dual bias（slope + normal offset，constant 对 D32Sfloat 无效已移除）
- Alpha mask 阴影（双 pipeline：opaque + mask）
- Shadow 采样集成到 Forward Lighting Pass（feature_flags 条件分支）
- Culling 模块泛化（通用 frustum 剔除，shadow + camera 共用）
- Cascade 数量 / 分辨率运行时可调

**产出：** 场景有了阴影，立体感大幅提升。但物体接地感还差一些（没有 AO 和 Contact Shadows）。

---

## 阶段五：屏幕空间效果

- RG temporal 机制 + per-frame-in-flight Set 2 + compute pass push descriptors（基础设施）
- DepthPrePass roughness 输出（M2 SSR 需要 per-pixel roughness，M1 预建）
- GTAO Pass（AO + bent normal 计算，RGBA8 输出）
- AO Temporal Filter Pass（reprojection + prev depth rejection + 邻域 clamp）
- AO Forward 集成（乘法复合 + multi-bounce 色彩补偿 + specular occlusion 调制 IBL specular）
- Contact Shadows Pass（screen-space ray march，距离衰减输出）

**产出：** 物体之间、角落里有了自然的暗部过渡（AO），物体贴地处有了精细的接触阴影。光滑金属在遮蔽区 specular 适当衰减（specular occlusion），浅色表面 AO 不过度压暗（multi-bounce）。画面层次感明显提升。

**战略价值：** temporal filtering 基础设施（RG temporal double buffer + per-frame Set 2 + compute push descriptors）在此搭建，M2 的 SSR、SSGI 可直接复用同一机制。DepthPrePass roughness buffer 同样为 M2 SSR 所需。注意 DLSS/FSR 的全画面 temporal accumulation 是 SDK 自带实现，依赖 Motion Vectors 而非此基础设施。

---

## 阶段六：RT 基础设施 + PT 参考视图

**Layer 0 RT 扩展 + Layer 1 场景 AS + Layer 2 PT Pass**

- Vulkan RT 扩展启用（`acceleration_structure` + `ray_tracing_pipeline` + `ray_query`，可选——不支持时禁用 RT 功能）
- RHI 层 AS 封装（BLAS/TLAS 创建、构建、销毁）
- RHI 层 RT Pipeline 封装（Shader Group + Shader Binding Table 管理）
- RHI 层 Command 扩展（`trace_rays()`、`build_acceleration_structure()`）
- Scene AS Builder（Framework 层：输入 scene data → 构建 per-mesh BLAS + scene TLAS）
- Set 0 扩展（binding 4: TLAS, binding 5: Geometry Info SSBO）
- PT 核心 shader（raygen/miss/closesthit，NEE + MIS + Russian Roulette + cosine/GGX VNDF importance sampling + Sobol + Blue noise + multi-lobe BRDF 选择）
- PT 着色质量基础（ray origin offset、normal mapping、subpixel jitter、emissive 贡献、firefly clamping、shading normal 一致性）
- PT 参考视图 Pass（屏幕空间射线发射，accumulation buffer 跨帧累积，相机移动时重置）
- OIDN GPU 集成（CPU 内存中转 + albedo/normal 辅助降噪通道）
- 独立渲染路径（PT 参考视图模式：PT Pass → OIDN → Tonemapping → Swapchain）
- ImGui 渲染模式切换（光栅化 ↔ PT 参考视图）+ PT 参数面板
- Area Light NEE（emissive 面光源 importance sampling + MIS，EmissiveLightBuilder）
- Texture LOD（Ray Cones，纹理 mip 选择 + LOD bias 调参）

**产出：** 能在 PT 参考视图模式下看到路径追踪渲染的画面。相机静止时画面逐渐收敛变清晰，OIDN 降噪提供即时预览。Emissive 表面正确照亮周围，纹理采样有合理的 LOD 选择。验证整个 RT 技术栈（AS 构建、RT Pipeline、PT 采样、降噪）端到端可用。

**战略价值：** RT 基础设施（AS、RT Pipeline、PT 核心 shader）为阶段七的烘焙器和 M2 的实时 PT 直接复用。独立渲染路径架构为 M2 实时 PT 模式提供框架。Area light NEE 和 Ray Cones 同样为 M2 实时 PT 直接复用。

---

## 阶段七：PT 烘焙器

**Layer 1 重构 + Layer 1 烘焙基础设施 + Layer 2 Baker Pass + Layer 3 烘焙模式**

- BC6H 压缩通用工具提取（从 IBL 提取为 framework 层独立模块，cubemap + 2D 通用）
- Cubemap prefilter 通用工具提取（从 IBL 提取为 framework 层独立模块）
- xatlas vcpkg 集成 + Lightmap UV 生成器（TEXCOORD_1 优先使用 → 无则 xatlas 生成 → 缓存）
- Lightmap UV 拓扑应用（xatlas 输出替换内存中 Mesh 的 vertex/index buffer，`uv1` 写入 lightmap UV）
- Lightmap 分辨率自动计算（世界空间表面积 × texels_per_meter，clamp 到 min/max，对齐到 4）
- BakeDenoiser（framework 层，同步阻塞 OIDN 降噪，独立于 reference view 的异步 Denoiser）
- Position/Normal Map 光栅化 pass（UV 空间渲染：vertex shader 将 lightmap UV 映射到 NDC，fragment shader 输出世界空间 position + normal）
- PT Push Constants 扩展（`lightmap_width` + `lightmap_height` + probe 字段，60B 超集布局，closesthit 无条件分支）
- Lightmap Baker Pass（UV 空间 RT dispatch：读 position/normal map → 逐 texel 发射射线 → accumulation → BakeDenoiser → BC6H → KTX2 持久化）
- Probe 自动放置（场景 AABB 内均匀网格 1m 间距 + RT 射线几何过滤剔除墙内探针）
- Reflection Probe Baker Pass（cubemap 6 面 RT dispatch → accumulation → BakeDenoiser → prefilter mip chain → BC6H → KTX2 持久化）
- 烘焙模式渲染路径（RenderMode::Baking，逐项顺序执行：lightmaps → probes → finalize）
- ImGui 烘焙控制面板（触发烘焙、参数配置、进度显示 + accumulation buffer ImGui 预览）

**产出：** 能在引擎内烘焙出 Lightmap 和 Reflection Probe 数据，保存为 KTX2 文件。烘焙过程中可在 ImGui 中观察 UV 空间的累积进度。Per-instance lightmap 分辨率按世界空间面积自动适配。

**战略价值：** BC6H 和 prefilter 通用工具为���续（M2+ 的 probe 更新、其他 HDR 压缩需求）直接复用。BakeDenoiser 的同步模型为任何离线处理管线提供���噪能力。xatlas lightmap UV 基础设施为 Phase 8 的 forward shader 集成提供零开销的 `uv1` 顶点属性访问。

---

## 阶段八：间接光照集成

**Layer 1 BakeDataManager + Layer 2 Forward 集成 + Layer 3 模式切换 UI**

- `indirect_intensity` 参数重命名（`ibl_intensity` → `indirect_intensity`，全代码库重构）
- `IndirectLightingMode` enum + `FEATURE_LIGHTMAP_PROBE` feature flag
- GPUInstanceData 扩展（`lightmap_index` + `probe_index`，哨兵值 `0xFFFFFFFF`）
- instance_index vert→frag 重构（三对 shader 统一传 `gl_InstanceIndex`，frag 读 InstanceBuffer）
- GPUProbeData struct + ProbeBuffer SSBO（Set 0 binding 9，非 RT-only，Phase 8.5 视差校正 AABB 预留）
- BakeDataManager（framework 层：bake 缓存扫描与完整性校验，KTX2 加载/卸载 + bindless 注册，CPU probe-to-instance 分配）
- Forward shader 间接光照集成：Lightmap 采样（入射辐照度 × diffuse_color）+ Probe cubemap 采样（roughness-based mip LOD）
- IBL / Lightmap+Probe 模式切换 UI + 角度选择 + bake 完成后自动启用
- AO/SO 按模式自动预设（切换时加载该模式的 AOConfig，两模式各自记忆）
- Lightmap/Probe 模式 IBL 旋转跳变（拖拽超阈值跳到下一个已 bake 角度）

**产出：** 光栅化模式下室内场景有了间接光照（Lightmap），不再是纯黑的角落。光滑表面反射周围环境（Reflection Probes），不再只反射天空。IBL / Lightmap+Probe 两种模式可切换，多角度 bake 数据可选择。画面写实度的一个重要跳跃。

> 详细步骤见 `../current-phase.md`

---

## 阶段八点五：间接光照质量提升

**Layer 1 probe 放置改进 + Layer 1 空间索引 + Layer 2 Forward 集成 + Layer 3 参数 UI**

- Probe relocation — pre-bake（两层测试失败 → 移动到最近 hit 点 + 法线偏移 → 重试）
- Probe relocation — post-bake（部分面全黑 → 反向移动 → 重新 bake → 仍失败则剔除）
- PCC AABB 计算（Fibonacci + 6 轴射线按主轴分组取中位数，manifest v2 存储）
- Per-pixel probe 选取（5×5×5 世界空间 3D grid 空间加速，法线半球硬过滤，`pow(normal_dot, normal_bias) / dist_sq` 评分）
- Top-2 probe blend（roughness 平滑过渡：`roughness_single` → `roughness_full`，`blend_curve` 控制曲线形状）
- PCC 视差校正（Parallax Corrected Cubemap，AABB proxy 反射向量修正）
- GPUInstanceData 清理（`probe_index` 移除，CPU per-instance 分配逻辑删除）

**产出：** Probe 放置质量提升（relocation 减少无效 probe），反射效果贴合房间墙壁（视差校正），大物体反射有空间变化且连续过渡（per-pixel 选取 + blend），整体间接光照质量显著提升。

> 详细步骤见 `../current-phase.md`

---

## 阶段九：透明

- 透明物体排序
- HDR Color Buffer 拷贝（折射源）
- Transparent Pass（Alpha Blending + 屏幕空间折射）

**产出：** 玻璃窗、半透明物体正确显示，有折射效果。

**开发前检查：** transparent.frag 需要复用 forward.frag 的大量光照代码（PBR 直射光、IBL、阴影、AO 等），开发前评估是否需要将 forward.frag 光照逻辑重构为可复用的 `common/lighting.glsl`。

---

## 阶段十：后处理链 + 大气

- 自动曝光（亮度降采样到 1×1 + 时域平滑）
- Bloom（降采样链 + 升采样链）
- 高度雾 Pass
- Tonemapping 演进（阶段三已实现基础 ACES 拟合，此处升级）：
  - 从 fullscreen fragment shader 迁移到 compute shader（LDR buffer 非 SRGB swapchain，可以用 STORAGE_BIT）
  - 从直接写 swapchain 改为写中间 LDR buffer（后处理链串联：HDR → Bloom → Tonemapping → LDR → Vignette → Color Grading → Final Output → Swapchain）
- Vignette Pass
- Color Grading Pass

**产出：** M1 完成。画面有完整的 HDR → LDR 管线，高光有光晕，曝光自动适应，远景融入雾中，画面有风格感。

---

## 阶段间依赖总结

```
阶段一 ──→ 阶段二 ──→ 阶段三 ──→ 阶段四
              │                      │
              │                      ↓
              │            阶段五（依赖阶段三的 Depth/Normal）
              │                      │
              │                      ↓
              │            阶段六（RT 基础设施 + PT 参考视图）
              │                      │
              │                      ↓
              │            阶段七（PT 烘焙器，依赖阶段六的 RT 基础设施）
              │                      │
              │                      ↓
              │            阶段八（间接光照集成，依赖阶段七的烘焙产出）
              │                      │
              │                      ↓
              │            阶段八点五（间接光照质量提升，依赖阶段八基础设施）
              │                      │
              │                      ↓
              │            阶段九（依赖阶段三的 HDR Color、Depth）
              │                      │
              │                      ↓
              └──────────→ 阶段十（依赖阶段二的 Render Graph）
```

阶段四、五在代码依赖上都需要阶段三完成，但它们之间没有硬依赖——理论上可以调换顺序。选择当前顺序的理由是：阶段四的阴影对画面立体感的提升最直接，阶段五的 temporal filtering 是重要基础设施。阶段六引入 RT 基础设施，阶段七的烘焙器依赖阶段六，阶段八的间接光照集成依赖阶段七的烘焙产出。阶段八点五是阶段八的质量提升迭代，视差校正和多 probe 混合等增强依赖阶段八的基础设施。阶段九的透明和阶段十的后处理与 RT 无硬依赖，顺延即可。
