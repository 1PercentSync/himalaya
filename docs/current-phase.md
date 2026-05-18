# 当前阶段：Reflector Phase 1 — 管线精简

> 目标：移除光栅化和烘焙管线，只保留 Path Tracing 渲染 GLTF 的能力。
> 精简后 RenderMode 仅含 PathTracing（GaussianSplatting 在后续 Phase 加入）。
> 任务清单见 `tasks/reflector-phase1.md`。

---

## 背景

Himalaya 渲染器在 main 分支上以光栅化为主管线，包含完整的 Forward+ 管线（shadow、AO、contact shadows 等）和 GPU PT 烘焙器（lightmap + reflection probe）。`reflector` 分支转向以 Path Tracing 和 Gaussian Splatting 为核心，需要先精简掉不再需要的光栅化和烘焙代码。

## 精简范围

### 删除

- **光栅化 pass**：DepthPrePass, ForwardPass, ShadowPass, SkyboxPass, GTAOPass, AOSpatialPass, AOTemporalPass, ContactShadowsPass
- **烘焙 pass**：LightmapBakerPass, ProbeBakerPass, PosNormalMapPass
- **framework 模块**：Shadow, Culling, BakeDataManager, BakeDenoiser, LightmapUV, ProbePlacement, RenderProgress
- **渲染路径**：`renderer_rasterization.cpp`, `renderer_bake.cpp`
- **shader**：光栅化/烘焙相关 shader 文件，bindings.glsl 中的光栅化声明
- **third_party**：xatlas（仅 lightmap UV 使用）

### 保留

- **PT 核心**：ReferenceViewPass, TonemappingPass, RT shader（reference_view.rgen, closesthit, anyhit, miss）
- **框架**：RenderGraph, MaterialSystem, IBL, Camera, Mesh, Texture, SceneASBuilder, EmissiveLightBuilder, Denoiser
- **RHI**：全部保留
- **工具链**：CachedShaderCompiler, KTX2, TextureCompress（含 BC6H）, CubemapFilter, ColorUtils, Cache

### 重构

- Descriptor Set 0/2 binding 重新编号（移除空洞）
- RenderMode 简化为 PathTracing + GaussianSplatting（GS 占位）
- GlobalUniformData / RenderFeatures / RenderInput 精简

## 实现步骤

共 8 个 Step（Step 0 文档 + Step 1-7 代码），从顶层到底层逐步精简，每步结束可编译。详见 `tasks/reflector-phase1.md`。
