# Milestone 1 阶段二：任务清单

> 实现步骤见 `docs/current-phase.md`，接口定义见 `docs/milestone-1/m1-interfaces.md`

---

## Step 1：App 层重构

- [x] 从 `main.cpp` 拆分出 `app/application.h/cpp`（窗口管理、主循环骨架、初始化/销毁序列）
- [x] 从 `main.cpp` 拆分出 `app/debug_ui.h/cpp`（ImGui 面板内容构建）
- [x] `main.cpp` 只保留 `main()` 入口，创建 Application 并运行
- [x] 现有功能（三角形渲染、debug 面板）在重构后保持不变
- [x] 验证：编译通过，运行效果与重构前一致

## Step 2：Descriptor 管理

- [x] 创建 `rhi/include/himalaya/rhi/descriptors.h` + `rhi/src/descriptors.cpp`
- [x] 创建 Set 0 layout（GlobalUBO binding 0 + LightBuffer binding 1 + MaterialBuffer binding 2）
- [x] 创建 Set 1 layout（bindless sampler2D array，上限 4096，`VARIABLE_DESCRIPTOR_COUNT` + `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`）
- [x] 创建两个独立 Descriptor Pool（普通 pool for Set 0 + UPDATE_AFTER_BIND pool for Set 1）
- [x] 分配 Set 0 × 2（per-frame）+ Set 1 × 1
- [x] `get_global_set_layouts()` 返回 {set0_layout, set1_layout}
- [x] Bindless 纹理注册（`register_texture(ImageHandle, SamplerHandle)` → `BindlessIndex`）
- [x] Bindless 纹理注销（`unregister_texture()`，free list 回收 + deferred deletion）
- [x] 显式 `destroy()` 方法
- [x] ResourceManager 扩展 sampler 管理：`SamplerDesc`、`create_sampler()`、`destroy_sampler()`、`get_sampler()`
- [x] CommandBuffer 新增 `bind_descriptor_sets()` 方法
- [x] 验证：DescriptorManager 初始化/销毁无 validation 报错

## Step 3：Render Graph 骨架

- [x] 创建 `framework/include/himalaya/framework/render_graph.h` + `framework/src/render_graph.cpp`
- [x] `RGResourceId` 类型定义
- [x] `import_image(debug_name, handle, initial_layout, final_layout)` / `import_buffer()`：导入外部资源，返回 `RGResourceId`
- [x] `add_pass()`：注册 pass 名称、`RGResourceUsage` 列表（读写语义由 `RGAccessType` 区分）、execute lambda
- [x] `compile()`：根据 pass 声明的输入输出，计算每个 pass 之间需要的 barrier（仅 image layout transition）
- [x] `execute(CommandBuffer& cmd)`：接收外部 command buffer，按注册顺序依次执行 pass
- [x] `clear()`：每帧重建前清除所有 pass 和资源引用
- [x] `get_image()` / `get_buffer()`：在 execute 回调内通过 `RGResourceId` 获取底层句柄
- [x] 帧循环重构：acquire → RG compile/execute → submit → present
- [x] 三角形渲染迁移到 RG 的一个 pass 中
- [x] ImGui 迁移到 RG 的最后一个独立 pass 中
- [x] Shader 编译增加 shaderc includer 支持（自定义 includer，从 shaders/ 解析 `#include`）
- [x] CommandBuffer 新增 `begin_debug_label()` / `end_debug_label()` 方法
- [x] RG `execute()` 自动为每个 pass 插入 debug label
- [x] `compile()` barrier 计算：按需实现 (RGAccessType, RGStage) → barrier 参数映射，未实现的 assert 拦截
- [x] 验证：三角形 + ImGui 通过 RG 渲染，效果与之前一致，无 validation 报错

## Step 4：Camera + 场景数据接口

- [x] 创建 `framework/include/himalaya/framework/camera.h` + `framework/src/camera.cpp`（Camera 结构体 + reverse-Z 投影矩阵）
- [x] 创建 `framework/include/himalaya/framework/scene_data.h`（SceneRenderData、MeshInstance、DirectionalLight、CullResult、AABB、GPU 数据结构）
- [x] 创建 `app/camera_controller.h/cpp`（WASD 移动 + 鼠标旋转自由漫游）
- [x] CameraController 检查 ImGui WantCaptureMouse/WantCaptureKeyboard，为 true 时跳过相机输入
- [x] `GLM_FORCE_DEPTH_ZERO_TO_ONE` 编译定义（`rhi/CMakeLists.txt` PUBLIC 传播）
- [x] 三角形接入 Camera：顶点改 3D、shader 读 GlobalUBO VP 矩阵、每帧填充并绑定 Set 0
- [x] 验证：相机能自由移动，三角形随视角变化正确透视

## Step 5：Mesh + 纹理加载

- [x] 创建 `framework/include/himalaya/framework/mesh.h` + `framework/src/mesh.cpp`
- [x] 统一顶点格式定义（position vec3 + normal vec3 + uv0 vec2 + tangent vec4 + uv1 vec2）
- [x] Mesh 数据结构（vertex buffer handle、index buffer handle、顶点/索引数量）
- [x] MikkTSpace 集成：mesh 缺失 tangent 时从 position + normal + uv0 自动生成
- [x] 创建 `framework/include/himalaya/framework/texture.h` + `framework/src/texture.cpp`
- [x] stb_image 加载 JPEG/PNG → RGBA8 像素数据（强制 4 通道）
- [x] 上传到 GPU image（staging buffer）
- [x] GPU 端 mip 生成（`vkCmdBlitImage` 逐级降采样 + layout transition 链）
- [x] 纹理加载接口接受 `TextureRole` 枚举（Color/Linear），根据角色选择 format
- [x] 注册到 DescriptorManager 获取 `BindlessIndex`
- [x] 批量上传：Context 新增 `begin_immediate()` / `end_immediate()` scope，`upload_buffer()` 改为录制模式，旧三角形上传代码同步适配
- [x] Default 纹理创建（1×1 white、flat normal、black），Texture 模块初始化时注册到 bindless
- [x] Sampler 创建：从 glTF sampler 定义创建对应 VkSampler（缺失时使用 default）
- [x] 验证：改造三角形 shader 添加 hardcoded bindless index 采样，纹理正确显示在三角形上

## Step 6：材质系统 + glTF 场景加载 + Unlit 渲染

- [x] 创建 `framework/include/himalaya/framework/material_system.h` + `framework/src/material_system.cpp`
- [x] `GPUMaterialData` 结构体（80 字节, alignas(16)：含 normal_scale/occlusion_strength/alpha_cutoff/alpha_mode）
- [x] `AlphaMode` 枚举（Opaque/Mask/Blend）
- [x] 全局 Material SSBO 管理（场景加载后一次性分配恰好大小的 buffer）
- [x] `MaterialInstance` 管理（template_id + buffer_offset + alpha_mode + double_sided）
- [x] 缺失纹理字段填入 default 纹理的 BindlessIndex
- [x] `DescriptorManager::write_set0_buffer()` — 将 buffer descriptor 写入收敛到 RHI 层
- [x] 创建 `app/scene_loader.h/cpp`
- [x] 场景路径由 `Application::init(scene_path)` 接收，main.cpp 解析 argc/argv，默认 `assets/Sponza/Sponza.gltf`
- [ ] 加载失败 log error + abort，不做 fallback
- [ ] fastgltf 解析 glTF 文件
- [ ] 遍历 mesh：转换为统一顶点格式，创建 vertex/index buffer
- [ ] 遍历 material：提取 PBR 参数和纹理引用，创建 MaterialInstance
- [ ] 遍历 node：计算世界变换矩阵，填充 MeshInstance（含 world_bounds 计算）
- [ ] glTF 每个 primitive 展开为独立 MeshInstance
- [ ] 使用 fastgltf `LoadExternalBuffers` + `LoadExternalImages` 统一处理嵌入式与外部资源
- [ ] Sampler 不做 ResourceManager 层去重（glTF 同一 sampler index 自然去重）
- [ ] SceneLoader 持有所有资源句柄列表，提供 `destroy()` 一次性清理
- [ ] 填充 SceneRenderData
- [ ] CommandBuffer 新增 `bind_index_buffer()`、`draw_indexed()`、`push_constants()` 方法
- [ ] 创建 `shaders/common/bindings.glsl`（全局绑定布局：Set 0 + Set 1 + push constant）
- [ ] 创建 `shaders/forward.vert`（MVP 变换，输出 world position / normal / uv0 / tangent）
- [ ] 创建 unlit shader（forward.frag 雏形）：采样 base_color_tex × base_color_factor，无光照
- [ ] Depth buffer 创建（D32Sfloat，imported resource 导入 RG，resize 时 Application 重建）
- [ ] Reverse-Z 配置：depth clear 0.0f，compare op GREATER（投影矩阵已在 Step 4 Camera 中实现）
- [ ] 提取 `handle_resize()` 私有方法（vkQueueWaitIdle 后立即销毁旧资源，acquire 失败和帧末共用）
- [ ] GlobalUBO × 2 + LightBuffer × 2 管理（CpuToGpu memory，descriptor 初始化写一次，每帧 memcpy）
- [ ] Unlit pass 注册到 Render Graph（使用 depth attachment）
- [ ] Pipeline 创建（使用 `DescriptorManager::get_global_set_layouts()`）
- [ ] Draw loop：遍历物体，push constant 传 model matrix + material_index，draw call
- [ ] 移除旧的三角形渲染代码和 triangle shader
- [ ] 验证：glTF 场景渲染出有纹理的画面（无光照，验证数据管线完整性）

## Step 7：Forward 光照 + 视锥剔除

- [ ] 升级 `shaders/forward.frag`：加入 Lambert 光照（从 LightBuffer 读取方向光）+ 法线贴图 TBN
- [ ] Application 提供保底方向光（检测无光源时填默认值），scene_loader 忠实还原 glTF
- [ ] 创建 `framework/include/himalaya/framework/culling.h` + `framework/src/culling.cpp`
- [ ] AABB vs 6 frustum planes 视锥剔除
- [ ] Draw loop 改为遍历 CullResult 的 visible indices
- [ ] 验证：glTF 场景正确渲染，有纹理和基础光照，相机移动时视锥剔除生效
