# Milestone 1 阶段三：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## Step 1：Renderer 提取 + CLI11

- [x] 创建 `RenderInput` 结构体（`app/include/himalaya/app/renderer.h`）
- [x] 创建 `Renderer` 类骨架（init / render / destroy / on_swapchain_invalidated / on_swapchain_recreated）
- [x] 将渲染资源所有权从 Application 迁移到 Renderer（pipeline、depth image、default textures、default sampler、shader compiler、material system、render graph、swapchain image handles、per-frame UBO/SSBO）
- [x] 将渲染逻辑从 Application 迁移到 Renderer（RG 构建与执行、UBO/SSBO 填充、draw loop）
- [x] Application 改为持有 Renderer 并在主循环中调用 `renderer_.render(cmd, input)`
- [x] 两阶段 resize：`handle_resize()` 改为先调 `renderer_.on_swapchain_invalidated()`、再 `swapchain_.recreate()`、最后 `renderer_.on_swapchain_recreated()`
- [x] 引入 CLI11 命令行解析库（vcpkg.json + CMakeLists.txt 更新，需用户确认）
- [x] `main.cpp` 使用 CLI11 替代手动 `argc/argv` 解析：`--scene`（迁移现有路径参数）+ `--env`（预留，默认 `assets/environment.hdr`）
- [x] `Application::init()` 接收已解析的参数结构体，不感知命令行解析细节
- [x] 验证：编译通过，运行效果与阶段二一致，无 validation 报错

## Step 2：RG Managed 资源 + Debug Name

- [x] `RGImageDesc` 结构体（RGSizeMode Relative/Absolute、format、usage、sample_count、mip_levels）
- [x] `RGManagedHandle` 类型 + `create_managed_image()` / `destroy_managed_image()` API
- [x] `use_managed_image()` — 每帧调用，返回 `RGResourceId`（每帧以 UNDEFINED 为 initial layout，不追踪帧间状态，不插入 final barrier）
- [x] `set_reference_resolution(VkExtent2D)` — Relative 模式的基准分辨率
- [x] Resize 自动重建：`set_reference_resolution()` 被调用时，比较 desc 推导出的新旧尺寸，变化时销毁旧 backing image 并创建新的
- [x] `update_managed_desc(handle, new_desc)` — 更新描述符（MSAA 切换用），desc 变化时重建 backing image
- [x] `get_managed_backing_image(handle)` — 获取 backing ImageHandle（resize handler 中即时获取新 handle 更新 Set 2 descriptor）
- [x] 迁移现有 depth buffer 从手动管理到 managed 资源，删除 Renderer 中的手动 depth 创建/销毁代码
- [x] `create_image()`、`create_buffer()`、`create_sampler()` 新增必选 `debug_name` 参数，内部调用 `vkSetDebugUtilsObjectNameEXT`
- [x] 回溯修改所有已有 `create_image()` / `create_buffer()` / `create_sampler()` 调用点，补充 debug name
- [x] 验证：现有渲染正常工作，depth buffer 由 RG managed 管理，resize 时自动重建；Vulkan 对象在 validation 输出和 RenderDoc 中显示可读名称

## Step 3：Descriptor Layout + Compute Infra

- [x] Set 1 layout 新增 binding 1（`samplerCube[]`，上限 256，`PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`）
- [x] Set 1 descriptor pool 容量从 4096 扩展到 4352
- [x] 去掉 Set 1 binding 0 的 `VARIABLE_DESCRIPTOR_COUNT`，改为固定上限 4096 + `PARTIALLY_BOUND`
- [x] 同步在 `bindings.glsl` 中声明 `samplerCube cubemaps[]`（Set 1 binding 1），使 shader 侧布局与 CPU 侧一致
- [x] 新增 Set 2 descriptor set layout（M1 全部 8 个 binding 预留，`PARTIALLY_BOUND`）+ Set 2 pool（普通 pool，8 COMBINED_IMAGE_SAMPLER）+ 分配 Set 2 × 1
- [x] `DescriptorManager` 新增 `register_cubemap()` / `unregister_cubemap()` API（独立 free list 和 slot 空间）
- [x] `DescriptorManager` 新增 Set 2 管理：`update_render_target()` 更新指定 binding 的 image + sampler
- [x] `get_global_set_layouts()` 返回三个 layout（Set 0 + Set 1 + Set 2），所有 pipeline layout 统一
- [x] `pipeline.h` 新增 `ComputePipelineDesc` 结构体（含 `descriptor_set_layouts` 字段支持自定义 layout）+ `create_compute_pipeline()` 函数
- [x] `commands.h` 新增 `CommandBuffer::dispatch(group_count_x, group_count_y, group_count_z)` + `CommandBuffer::push_descriptor_set()` 方法
- [x] 补充 `RenderGraph::resolve_usage()` 的 Compute case：Read → `SHADER_READ_ONLY_OPTIMAL` + `SHADER_SAMPLED_READ_BIT`，Write → `GENERAL` + `SHADER_STORAGE_WRITE_BIT`
- [x] 验证：所有布局更新无 validation 报错，现有渲染正常；Compute 基础设施实际 dispatch 验证通过

## Step 4a：FrameContext + Pass 类 + HDR 管线重组

- [x] 创建 `framework/include/himalaya/framework/frame_context.h`（FrameContext 结构体：RG 资源 ID + 场景数据引用 + 帧参数）
- [x] 提取 ForwardPass 类（`passes/forward_pass.h/cpp`）：从 Renderer 的 RG lambda 迁移渲染逻辑，实现 setup / record / destroy
- [x] 创建 hdr_color managed 资源（R16G16B16A16_SFLOAT，1x）
- [x] 创建 `shaders/fullscreen.vert`（fullscreen triangle，无顶点输入）
- [x] 创建 `shaders/tonemapping.frag`（passthrough 版本：采样 hdr_color 直接输出，不做 ACES）
- [x] TonemappingPass 类（`passes/tonemapping_pass.h/cpp`）：setup 接收 swapchain format 参数（物理设备协商结果，非硬编码）、创建 pipeline、record、destroy
- [x] ForwardPass 改为渲染到 hdr_color（不再直接渲染到 swapchain），沿用阶段二深度行为（depth compare GREATER、depth write ON）
- [x] TonemappingPass 读 hdr_color 写 swapchain image
- [x] 验证：管线正确运行，无 validation 报错，场景可见（高光过曝可接受，因为没有 tone mapping）

## Step 4b：ACES Tonemapping + Exposure

- [x] 替换 `tonemapping.frag` 的 passthrough 为 ACES tonemapping
- [x] Exposure 控制：DebugUI 手动 EV 滑条（范围 -4 到 +4），`pow(2, ev)` 计算 exposure
- [x] 数据通路：`RenderInput::exposure` → `GlobalUniformData::camera_position_and_exposure.w` → `tonemapping.frag`
- [x] 验证：高光不截断，画面正确 tonemapped，DebugUI EV 滑条可调

## Step 4c：MSAA

- [x] 创建 MSAA color buffer（R16G16B16A16F，4x，managed 资源；1x 时不创建）
- [x] 创建 MSAA depth buffer（D32Sfloat，4x，managed 资源；1x 时不创建，使用 Step 2 的 1x depth）
- [x] Forward pass 改为渲染到 MSAA color + MSAA depth，通过 Dynamic Rendering 配置 color resolve（AVERAGE）；1x 时直接渲染到 hdr_color，无 resolve
- [x] Forward pass RG 资源声明：多采样时 3 个资源（msaa_color WRITE、msaa_depth READ_WRITE、hdr_color WRITE），1x 时 2 个资源（hdr_color WRITE、depth READ_WRITE）
- [x] MSAA 运行时切换：`Renderer::handle_msaa_change()`（`vkQueueWaitIdle` 保障 GPU 空闲 → 创建/销毁 MSAA managed 资源 + `update_managed_desc` + `on_sample_count_changed()` pipeline 重建）+ DebugUI MSAA 选择控件（1x/2x/4x/8x）
- [x] 验证：MSAA 渲染正确，DebugUI 可切换 1x/2x/4x/8x 采样数

## Step 5：Depth + Normal PrePass

- [x] 创建 `shaders/common/normal.glsl`（TBN 构造、normal map 采样解码、R10G10B10A2 world-space 编码 `n*0.5+0.5`）
- [x] 创建 `shaders/depth_prepass.vert`（输出 position/normal/tangent/uv0，`invariant gl_Position`）
- [x] 创建 `shaders/depth_prepass.frag`（Opaque：采样 normal_tex → TBN → encode world normal，无 discard）
- [x] 创建 `shaders/depth_prepass_masked.frag`（Mask：采样 base_color_tex alpha test → discard → 采样 normal_tex → TBN → encode）
- [x] 创建 resolved depth buffer（D32Sfloat，1x，managed 资源）
- [x] 创建 MSAA normal buffer（R10G10B10A2_UNORM，managed 资源；1x 时不创建）+ resolved normal buffer（R10G10B10A2_UNORM，managed 资源）
- [x] DepthPrePass 类（`passes/depth_prepass.h/cpp`）：setup 创建 Opaque pipeline + Mask pipeline、on_resize、record、destroy
- [x] DepthPrePass 配置 Dynamic Rendering 同时 resolve：depth MAX_BIT + normal AVERAGE（1x 时无 resolve，直接写 1x target）
- [x] PrePass 绘制：先 Opaque 批次（Early-Z 保证），再 Mask 批次（含 discard）
- [x] Forward pass 深度行为变更：从 Step 4a/4b 的 GREATER + write ON 改为 EQUAL + write OFF + 移除 depth resolve 配置，资源声明改为 3 个（msaa_color Write、msaa_depth Read、hdr_color Write），forward.vert 添加 `invariant gl_Position`
- [x] 验证：PrePass 正确填充 depth 和 normal buffer（RenderDoc 检查），Forward pass zero-overdraw 无视觉瑕疵

## Step 6：IBL Pipeline + Skybox

- [x] `framework/include/himalaya/framework/ibl.h` + `framework/src/ibl.cpp` 模块骨架（公开接口 + 私有成员 + CMakeLists.txt 集成）
- [x] `load_equirect()` 私有方法：stb_image `.hdr` 加载（`stbi_loadf`，RGB float）→ RGBA f16 转换 → R16G16B16A16F 2D GPU image 创建与上传，返回 equirect ImageHandle
- [x] `shaders/ibl/equirect_to_cubemap.comp` + `convert_equirect_to_cubemap()` 私有方法（cubemap R16G16B16A16F 动态分辨率创建、compute pipeline with push descriptors、dispatch、barrier）
- [x] `shaders/ibl/irradiance.comp` + `compute_irradiance()` 私有方法（irradiance cubemap 32² R11G11B10F 创建、余弦卷积 dispatch）
- [x] `shaders/ibl/prefilter.comp` + `compute_prefiltered()` 私有方法（prefiltered cubemap 512² R16G16B16A16F 多 mip 创建、per-mip roughness push constant、dispatch）
- [x] `shaders/ibl/brdf_lut.comp` + `compute_brdf_lut()` 私有方法（BRDF LUT 256² R16G16_UNORM 创建、dispatch）
- [ ] `init()` 编排 + `register_bindless_resources()` + `destroy()` 实现（串联预计算阶段、创建 sampler、注册产物到 Set 1 bindless、equirect 销毁、资源清理）
- [ ] `GlobalUniformData` 新增 IBL 字段（irradiance_cubemap_index、prefiltered_cubemap_index、brdf_lut_index、prefiltered_mip_count、skybox_cubemap_index）+ 更新 `bindings.glsl` GlobalUBO 布局（`cubemaps[]` 声明已在 Step 3 完成，此处仅更新 GlobalUBO）
- [ ] Renderer 在 `init()` 中调用 IBL 预计算，在 `destroy()` 中清理 IBL 资源
- [ ] 创建 `shaders/skybox.vert` + `shaders/skybox.frag`（独立 VS 计算世界方向 varying + `gl_Position.z = 0.0`，FS rotate_y + normalize + cubemap 采样）
- [ ] SkyboxPass 类（`passes/skybox_pass.h/cpp`）：方法集 setup / record / destroy（不属于 MSAA 相关 pass），渲染到 resolved 1x hdr_color，读 resolved depth（GREATER_OR_EQUAL + depth write OFF）
- [ ] `GlobalUniformData` 新增 `ibl_rotation_sin` / `ibl_rotation_cos` + 更新 `bindings.glsl` GlobalUBO 布局
- [ ] 左键拖拽改为 IBL 水平旋转（`light_yaw_` → `ibl_yaw_`，移除 pitch 逻辑），DebugUI Lighting 面板显示 IBL Rotation 角度
- [ ] 验证：IBL 预计算无 validation 报错，RenderDoc 检查 cubemap 各面和 mip 级别内容正确、BRDF LUT 呈现预期的渐变图案；天空背景正确显示且可水平旋转

## Step 6.5：IBL 环境光验证 + 灯光体系重构

- [ ] `forward.frag` 采样 `metallic_roughness_tex`，metallic 工作流分离（F0 / diffuse_color）
- [ ] `forward.frag` IBL 漫反射 + 镜面反射（irradiance + prefiltered + BRDF LUT Split-Sum，含 `rotate_y`），用 `ibl_intensity` 调制，移除固定 ambient 项
- [ ] 灯光体系重构：`ambient_intensity` → `ibl_intensity` 全通路重命名（GlobalUniformData、bindings.glsl、RenderInput、Application、DebugUI）+ 退役 default light（移除 `default_lights_`、`light_pitch_`、`light_intensity_`、`force_default_light_` 及相关逻辑）+ 新增 `disable_scene_lights_`（DebugUI "Disable Scene Lights" checkbox + RenderInput 通路）+ DebugUI Lighting 面板全面更新
- [ ] 验证：物体表面 IBL 光照正确（金属面反射环境、粗糙面模糊反射、非金属环境漫反射），`ibl_intensity` 滑条可调，IBL 可旋转，glTF 方向光可通过 checkbox 切换

## Step 7：PBR Shader 升级

- [ ] 创建 `shaders/common/constants.glsl`（PI、EPSILON 等数学常量）
- [ ] 创建 `shaders/common/brdf.glsl`（D_GGX、G_SmithGGX、F_Schlick、Lambert_diffuse，纯函数无场景数据依赖）
- [ ] 创建 `shaders/common/lighting.glsl`（evaluate_directional_light、evaluate_ibl，从 forward.frag 重构而来，内部 include constants + brdf）
- [ ] 升级 `forward.frag`：Lambert 直射光 → Cook-Torrance（GGX / Smith Height-Correlated / Schlick），IBL 内联代码重构为 `evaluate_ibl()` 调用（逻辑不变）+ 新增 occlusion_tex 调制 IBL + emissive_tex × emissive_factor
- [ ] DebugUI 渲染模式：增加可视化选项（Diffuse Only / Specular Only / IBL Only / Normal / Metallic / Roughness / AO）通过 GlobalUBO 传递 debug mode 标志，forward.frag 根据标志输出对应分量
- [ ] 验证：glTF 场景正确 PBR 渲染，Cook-Torrance 直射光 + IBL 环境光，金属表面反射环境，粗糙表面漫反射，Debug 各模式可用
- [ ] 最终验证：补充 .hdr 环境贴图和额外 glTF PBR 测试模型（DamagedHelmet 或类似），结合现有 Sponza 场景全面验证
