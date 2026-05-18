# Reflector Phase 1：管线精简

> 目标：移除光栅化和烘焙管线，只保留 Path Tracing 渲染 GLTF 的能力。
> 精简后 RenderMode 仅含 PathTracing（GaussianSplatting 在后续 Phase 加入）。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | 文档归档范围 | 全部归档写新文档，4 个活跃文档 |
| 2 | 代码精简粒度 | 彻底删除光栅化 + 烘焙代码 |
| 3 | RenderMode | `PathTracing \| GaussianSplatting`，GS 走 compute + raster splatting |
| 4 | 框架层保留 | PT 依赖模块 + KTX2 + TextureCompress + CubemapFilter + ColorUtils + Cache |
| 5 | Descriptor 布局 | 重新编号 |
| 6 | 精简顺序 | 逐步精简，每步可编译 |
| 7 | 方向光 | PT 始终用 HDR 环境光，移除方向光 NEE 及全部方向光 UI/状态 |

---

## Step 0：文档归档与重建

- [x] 归档 `docs/current-phase.md` 和 `tasks/m1-phase8.5.md`
- [x] 创建 `tasks/reflector-phase1.md` + 更新 `docs/agent-context.md`
- [x] 创建 `docs/current-phase.md`（Phase 1 精简步骤）
### 纯归档（无需迁移内容）

- [x] 归档 `docs/milestone-1/milestone-1.md`
- [x] 归档 `docs/milestone-1/m1-interfaces.md`
- [x] 归档 `docs/milestone-1/m1-frame-flow.md`
- [x] 归档 `docs/milestone-1/m1-phase-future-decisions.md`
- [x] 归档 `docs/roadmap/`（3 files）

### 创建 `docs/roadmap.md` + 归档源文档

- [x] 创建 `docs/roadmap.md`（reflector phase 概览）+ 归档 `docs/milestone-1/m1-development-order.md`

### 创建 `docs/architecture.md` + 归档源文档

- [x] 创建 `docs/architecture.md`（从 `project/architecture.md` + `project/requirements-and-philosophy.md` 提取 PT/GS 适用内容）
- [x] 归档 `docs/project/architecture.md`、`docs/project/requirements-and-philosophy.md`

### 创建 `docs/technical-decisions.md` + 归档源文档

- [x] 创建 `docs/technical-decisions.md`（从 `project/technical-decisions.md` + `project/decision-process.md` + `milestone-1/m1-rt-decisions.md` + `milestone-1/m1-design-decisions-core.md` 提取 PT/GS 相关）
- [x] 归档 `docs/project/technical-decisions.md`、`docs/project/decision-process.md`、`docs/milestone-1/m1-rt-decisions.md`、`docs/milestone-1/m1-design-decisions-core.md`

### 收尾

- [x] 更新 `docs/agent-context.md` 文档目录（指向新文档）

## Step 1：清理 app 层（移除光栅化/烘焙路径）

> 从最顶层开始，删除文件并清理引用，使 app 不再依赖光栅化/烘焙模块。
> passes 和 framework 的被删模块暂时保留（未被引用，不影响编译）。

- [x] 删除 `renderer_rasterization.cpp`、`renderer_bake.cpp`
- [x] 清理 `renderer.h`（移除已删 pass 的 include/成员/方法声明，移除烘焙状态，移除光栅化 buffer）
- [x] 简化 `renderer.cpp` render() 调度（仅 PT 路径）
- [x] 清理 `renderer_init.cpp`（移除光栅化/烘焙资源创建、managed image、sampler）
- [x] 清理 `renderer_pt.cpp`（移除对已删模块的引用，如有）
- [x] 精简 `RenderInput`（移除光栅化/烘焙专用字段）
- [x] 清理 `application.h/cpp`（移除烘焙触发、光栅化模式切换、bake data 管理）
- [x] 清理 `debug_ui.h/cpp`（移除光栅化/烘焙 UI 面板）
- [x] 清理 `config.h/cpp`（移除光栅化/烘焙配置持久化）
- [x] 清理 `scene_loader.h/cpp`（移除 lightmap UV 相关逻辑，如有）
- [x] 更新 `app/CMakeLists.txt`（移除已删源文件）
- [x] 移除 app 层方向光（Lighting 面板、LightSourceMode、fallback/hdr_sun 状态、RenderInput.lights、LightBuffer 填充、PT directional_lights 开关、config 持久化）

## Step 2：清理 passes 层

- [x] 删除光栅化 pass（8 对 .h/.cpp）：DepthPrePass, ForwardPass, ShadowPass, SkyboxPass, GTAOPass, AOSpatialPass, AOTemporalPass, ContactShadowsPass
- [x] 删除烘焙 pass（3 对 .h/.cpp）：LightmapBakerPass, ProbeBakerPass, PosNormalMapPass
- [x] 确认并处理 `pt_push_constants.h`（PT 使用则保留，否则删除）
- [x] 更新 `passes/CMakeLists.txt`

## Step 3：清理 framework 层

- [x] 删除 `shadow.h/cpp`、`culling.h/cpp`、`bake_data_manager.h/cpp`、`bake_denoiser.h/cpp`、`lightmap_uv.h/cpp`、`probe_placement.h/cpp`、`render_progress.h`
- [x] 清理 `scene_data.h`：移除 BakeState, BakeMode, BakeConfig, ShadowConfig, AOConfig, ContactShadowConfig, ProbeBlendConfig, IndirectLightingMode, DirectionalLight, GPUDirectionalLight
- [x] 清理 `scene_data.h`：移除 PTConfig.directional_lights 字段
- [x] 清理 `scene_data.h`：精简 RenderFeatures（移除光栅化专用 flag）
- [x] 清理 `scene_data.h`：精简 RenderMode 枚举（仅 PathTracing + GaussianSplatting）
- [x] 清理 `scene_data.h`：精简 GlobalUniformData（移除 shadow cascade、AO、probe blend、directional_light_count 字段）
- [x] 清理 `frame_context.h`：移除光栅化专用字段（shadow groups、AO resources、MSAA resources 等）
- [ ] 更新 `framework/CMakeLists.txt`

## Step 4：清理 shader

- [ ] 删除光栅化 shader：`depth_prepass*`, `forward.*`, `shadow*`, `skybox.*`, `gtao.comp`, `ao_spatial.comp`, `ao_temporal.comp`, `contact_shadows.comp`
- [ ] 删除烘焙 shader：`bake/pos_normal_map.*`, `bake/probe_filter.comp`, `rt/lightmap_baker.rgen`, `rt/probe_baker.rgen`
- [ ] 删除 `shaders/common/probe_grid.glsl`、`shaders/common/shadow.glsl`
- [ ] 清理 `bindings.glsl`：移除 ProbeBuffer/ProbeGridBuffer 声明、shadow/AO 相关 Set 2 声明
- [ ] 清理 `bindings.glsl`：精简 GlobalUBO（与 scene_data.h 同步）
- [ ] 清理 `bindings.glsl`：移除光栅化专用 `FEATURE_*` flag
- [ ] 清理 RT shader：移除 closesthit 方向光 NEE、reference_view.rgen directional_lights push constant
- [ ] 清理 `bindings.glsl`：移除 LightBuffer 声明

## Step 5：清理 third_party

- [ ] 删除 `third_party/xatlas/`
- [ ] 更新顶层 `CMakeLists.txt`（移除 xatlas `add_subdirectory`）
- [ ] 检查 `vcpkg.json`（是否有可移除的依赖）
- [ ] 编译验证

## Step 6：重新编号 descriptor 布局

- [ ] 规划新的 Set 0 / Set 2 binding 编号方案（含回收 LightBuffer binding 1）
- [ ] 更新 `descriptors.h/cpp`
- [ ] 更新 `bindings.glsl`
- [ ] 更新所有引用变更 binding 的 C++ 和 shader 代码
- [ ] 编译验证

## Step 7：收尾

- [ ] 更新 `CLAUDE.md`（项目描述、结构、约定、第三方库）
