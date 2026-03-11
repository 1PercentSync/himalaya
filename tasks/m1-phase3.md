# Milestone 1 阶段三：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## Step 1：Renderer 提取

- [ ] 创建 `RenderInput` 结构体（`app/include/himalaya/app/renderer.h`）
- [ ] 创建 `Renderer` 类骨架（init / render / destroy / on_swapchain_invalidated / on_swapchain_recreated）
- [ ] 将渲染资源所有权从 Application 迁移到 Renderer（pipeline、depth image、default textures、default sampler、shader compiler、material system、render graph、swapchain image handles、per-frame UBO/SSBO）
- [ ] 将渲染逻辑从 Application 迁移到 Renderer（RG 构建与执行、UBO/SSBO 填充、draw loop）
- [ ] Application 改为持有 Renderer 并在主循环中调用 `renderer_.render(cmd, input)`
- [ ] 两阶段 resize：`handle_resize()` 改为先调 `renderer_.on_swapchain_invalidated()`、再 `swapchain_.recreate()`、最后 `renderer_.on_swapchain_recreated()`
- [ ] 验证：编译通过，运行效果与阶段二一致，无 validation 报错

## Step 2：RG Managed 资源

- [ ] `RGImageDesc` 结构体（RGSizeMode Relative/Absolute、format、usage、sample_count、mip_levels）
- [ ] `RGManagedHandle` 类型 + `create_managed_image()` / `destroy_managed_image()` API
- [ ] `use_managed_image()` — 每帧调用，返回 `RGResourceId`（每帧以 UNDEFINED 为 initial layout，不追踪帧间状态，不插入 final barrier）
- [ ] `set_reference_resolution(VkExtent2D)` — Relative 模式的基准分辨率
- [ ] Resize 自动重建：`set_reference_resolution()` 被调用时，比较 desc 推导出的新旧尺寸，变化时销毁旧 backing image 并创建新的
- [ ] `update_managed_desc(handle, new_desc)` — 更新描述符（MSAA 切换用），desc 变化时重建 backing image
- [ ] 迁移现有 depth buffer 从手动管理到 managed 资源，删除 Renderer 中的手动 depth 创建/销毁代码
- [ ] 验证：现有渲染正常工作，depth buffer 由 RG managed 管理，resize 时自动重建

## Step 3：Descriptor Layout + Compute Infra

- [ ] Set 0 layout 新增 binding 3（`sampler2DArrayShadow`，阶段四 CSM 用；`PARTIALLY_BOUND` 允许当前不写入）
- [ ] Set 0 descriptor pool 调整（从 "2 UBO + 4 SSBO" 调整为 "2 UBO + 4 SSBO + 2 COMBINED_IMAGE_SAMPLER"）
- [ ] Set 1 layout 新增 binding 1（`samplerCube[]`，上限 256，`PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`）
- [ ] Set 1 descriptor pool 容量从 4096 扩展到 4352
- [ ] 去掉 Set 1 binding 0 的 `VARIABLE_DESCRIPTOR_COUNT`，改为固定上限 4096 + `PARTIALLY_BOUND`
- [ ] `DescriptorManager` 新增 `register_cubemap()` / `unregister_cubemap()` API（独立 free list 和 slot 空间）
- [ ] `pipeline.h` 新增 `ComputePipelineDesc` 结构体（含 `descriptor_set_layouts` 字段支持自定义 layout）+ `create_compute_pipeline()` 函数
- [ ] `commands.h` 新增 `CommandBuffer::dispatch(group_count_x, group_count_y, group_count_z)` + `CommandBuffer::push_descriptor_set()` 方法
- [ ] 验证：所有布局更新无 validation 报错，现有渲染正常；能创建并 dispatch 一个空 compute shader

## Step 4：MSAA + HDR + Tonemapping + Pass 类基础设施

- [ ] 创建 `framework/include/himalaya/framework/frame_context.h`（FrameContext 结构体：RG 资源 ID + 场景数据引用 + 帧参数）
- [ ] 创建 MSAA color buffer（R16G16B16A16F，4x，managed 资源；1x 时不创建）
- [ ] 创建 MSAA depth buffer（D32Sfloat，4x，managed 资源；1x 时不创建，使用 Step 2 的 1x depth）
- [ ] 创建 resolved HDR color buffer（R16G16B16A16F，1x，managed 资源）
- [ ] 提取 ForwardPass 类（`passes/forward_pass.h/cpp`）：从 Renderer 的 RG lambda 迁移渲染逻辑，实现 setup / record / destroy
- [ ] Forward pass 改为渲染到 MSAA color + MSAA depth，通过 Dynamic Rendering 配置 color resolve（AVERAGE）；1x 时直接渲染到 hdr_color，无 resolve
- [ ] Forward pass RG 资源声明：多采样时 3 个资源（msaa_color WRITE、msaa_depth READ_WRITE、hdr_color WRITE），1x 时 2 个资源（hdr_color WRITE、depth READ_WRITE）
- [ ] Tonemapping shader（`shaders/tonemapping.frag` + `shaders/fullscreen.vert`）
- [ ] TonemappingPass 类（`passes/tonemapping_pass.h/cpp`）：setup 创建 pipeline、record、destroy
- [ ] TonemappingPass 读 hdr_color 写 swapchain image（不感知 MSAA 模式）
- [ ] MSAA 运行时切换：Renderer::handle_msaa_change()（创建/销毁 MSAA managed 资源 + update_managed_desc + pipeline 重建）+ DebugUI MSAA 选择控件（1x/2x/4x/8x）
- [ ] 验证：场景以 MSAA + HDR + ACES tonemapping 渲染，高光不再截断为纯白，DebugUI 可切换 MSAA 采样数

## Step 5：Depth + Normal PrePass

- [ ] 创建 `shaders/common/normal.glsl`（TBN 构造、normal map 采样解码、R10G10B10A2 world-space 编码 `n*0.5+0.5`）
- [ ] 创建 `shaders/depth_prepass.vert`（输出 position/normal/tangent/uv0，`invariant gl_Position`）
- [ ] 创建 `shaders/depth_prepass.frag`（Opaque：采样 normal_tex → TBN → encode world normal，无 discard）
- [ ] 创建 `shaders/depth_prepass_masked.frag`（Mask：采样 base_color_tex alpha test → discard → 采样 normal_tex → TBN → encode）
- [ ] 创建 resolved depth buffer（D32Sfloat，1x，managed 资源）
- [ ] 创建 MSAA normal buffer（R10G10B10A2_UNORM，managed 资源；1x 时不创建）+ resolved normal buffer（R10G10B10A2_UNORM，managed 资源）
- [ ] DepthPrePass 类（`passes/depth_prepass.h/cpp`）：setup 创建 Opaque pipeline + Mask pipeline、on_resize、record、destroy
- [ ] DepthPrePass 配置 Dynamic Rendering 同时 resolve：depth MAX_BIT + normal AVERAGE（1x 时无 resolve，直接写 1x target）
- [ ] PrePass 绘制：先 Opaque 批次（Early-Z 保证），再 Mask 批次（含 discard）
- [ ] Forward pass 改为 depth compare EQUAL + depth write OFF + 移除 depth resolve 配置，资源声明改为 3 个（msaa_color Write、msaa_depth Read、hdr_color Write），forward.vert 添加 `invariant gl_Position`
- [ ] 验证：PrePass 正确填充 depth 和 normal buffer（RenderDoc 检查），Forward pass zero-overdraw 无视觉瑕疵

## Step 6：IBL Pipeline

- [ ] `framework/include/himalaya/framework/ibl.h` + `framework/src/ibl.cpp` 模块骨架
- [ ] stb_image `.hdr` 文件加载（`stbi_loadf`，RGB float 数据）+ 上传到 equirectangular 2D GPU image
- [ ] Equirectangular → Cubemap 转换 compute shader（`shaders/ibl/equirect_to_cubemap.comp`）
- [ ] Irradiance 余弦卷积 compute shader（`shaders/ibl/irradiance.comp`，32×32 per face，R11G11B10F）
- [ ] Prefiltered environment map compute shader（`shaders/ibl/prefilter.comp`，256×256 per face，多 mip 级别对应不同 roughness，R16G16B16A16F）
- [ ] BRDF Integration LUT compute shader（`shaders/ibl/brdf_lut.comp`，256×256，R16G16_UNORM）
- [ ] IBL 模块 `init()` 方法：在 `begin_immediate()` / `end_immediate()` scope 内执行全部预计算，使用 Push Descriptors 绑定输入/输出 image
- [ ] 将 irradiance/prefiltered cubemap 注册到 Set 1 binding 1（`register_cubemap()`），BRDF LUT 注册到 Set 1 binding 0（`register_texture()`）
- [ ] `GlobalUniformData` 新增 IBL 字段（irradiance_cubemap_index、prefiltered_cubemap_index、brdf_lut_index、prefiltered_mip_count）+ 更新 `bindings.glsl` 布局（Step 6 完成，不等 Step 7）
- [ ] Renderer 在 `init()` 中调用 IBL 预计算，在 `destroy()` 中清理 IBL 资源
- [ ] 验证：IBL 预计算无 validation 报错，RenderDoc 检查 cubemap 各面和 mip 级别内容正确、BRDF LUT 呈现预期的渐变图案

## Step 7：PBR Shader 升级

- [ ] 创建 `shaders/common/constants.glsl`（PI、EPSILON 等数学常量）
- [ ] 创建 `shaders/common/brdf.glsl`（D_GGX、G_SmithGGX、F_Schlick、Lambert_diffuse，纯函数无场景数据依赖）
- [ ] 创建 `shaders/common/lighting.glsl`（evaluate_directional_light、evaluate_ibl，内部 include constants + brdf，依赖 bindings.glsl 的光源/IBL 结构体）
- [ ] 升级 `forward.frag`：替换 Lambert 为 Cook-Torrance（GGX / Smith Height-Correlated / Schlick）+ IBL 环境光（irradiance + prefiltered + BRDF LUT Split-Sum）
- [ ] `forward.frag` 消费全部 5 个材质纹理：occlusion_tex 调制 IBL/ambient，emissive_tex × emissive_factor 加到最终颜色
- [ ] `bindings.glsl` 更新：新增 `samplerCube cubemaps[]` 声明（Set 1 binding 1）+ GlobalUBO IBL 字段
- [ ] DebugUI 渲染模式：增加可视化选项（Diffuse Only / Specular Only / IBL Only / Normal / Metallic / Roughness / AO）通过 GlobalUBO 传递 debug mode 标志，forward.frag 根据标志输出对应分量
- [ ] 验证：glTF 场景正确 PBR 渲染，金属表面反射环境，粗糙表面漫反射，Debug 各模式可用
