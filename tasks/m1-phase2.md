# Milestone 1 阶段二：任务清单

> 详细设计见 `docs/m1-phase2-plan.md`

---

## Step 1：App 层重构

- [ ] 从 `main.cpp` 拆分出 `app/application.h/cpp`（窗口管理、主循环骨架、初始化/销毁序列）
- [ ] 从 `main.cpp` 拆分出 `app/debug_ui.h/cpp`（ImGui 面板内容构建）
- [ ] `main.cpp` 只保留 `main()` 入口，创建 Application 并运行
- [ ] 现有功能（三角形渲染、debug 面板）在重构后保持不变
- [ ] 验证：编译通过，运行效果与重构前一致

## Step 2：Descriptor 管理

- [ ] 创建 `rhi/include/himalaya/rhi/descriptors.h` + `rhi/src/descriptors.cpp`
- [ ] 创建 Set 0 layout（GlobalUBO binding 0 + LightBuffer binding 1 + MaterialBuffer binding 2）
- [ ] 创建 Set 1 layout（bindless sampler2D array，上限 4096，`VARIABLE_DESCRIPTOR_COUNT` + `PARTIALLY_BOUND` + `UPDATE_AFTER_BIND`）
- [ ] 创建 Descriptor Pool
- [ ] 分配 Set 0 × 2（per-frame）+ Set 1 × 1
- [ ] `get_global_set_layouts()` 返回 {set0_layout, set1_layout}
- [ ] Bindless 纹理注册（`register_texture()` → `BindlessIndex`）
- [ ] Bindless 纹理注销（`unregister_texture()`，free list 回收 + deferred deletion）
- [ ] 显式 `destroy()` 方法
- [ ] 验证：DescriptorManager 初始化/销毁无 validation 报错

## Step 3：Render Graph 骨架

- [ ] 创建 `framework/include/himalaya/framework/render_graph.h` + `framework/src/render_graph.cpp`
- [ ] `RGResourceId` 类型定义
- [ ] `import_image()` / `import_buffer()`：导入外部资源，返回 `RGResourceId`
- [ ] `add_pass()`：注册 pass 名称、输入/输出 `RGResourceUsage` 列表、execute lambda
- [ ] `compile()`：根据 pass 声明的输入输出，计算每个 pass 之间需要的 barrier
- [ ] `execute()`：按注册顺序依次执行 pass（插入 barrier → 调用 execute lambda）
- [ ] `get_image()` / `get_buffer()`：在 execute 回调内通过 `RGResourceId` 获取底层句柄
- [ ] 帧循环重构：acquire → RG compile/execute → submit → present
- [ ] 三角形渲染迁移到 RG 的一个 pass 中
- [ ] ImGui 迁移到 RG 的最后一个独立 pass 中
- [ ] 验证：三角形 + ImGui 通过 RG 渲染，效果与之前一致，无 validation 报错

## Step 4：Camera + 场景数据接口

- [ ] 创建 `framework/include/himalaya/framework/camera.h`（Camera 结构体 + 矩阵计算方法）
- [ ] 创建 `framework/include/himalaya/framework/scene_data.h`（SceneRenderData、MeshInstance、DirectionalLight、PointLight、ReflectionProbe、LightmapInfo、CullResult、AABB）
- [ ] 创建 `app/camera_controller.h/cpp`（WASD 移动 + 鼠标旋转自由漫游）
- [ ] 负 viewport height 处理 Y-flip + `GLM_FORCE_DEPTH_ZERO_TO_ONE`
- [ ] 验证：相机能自由移动，三角形随视角变化正确透视

## Step 5：Mesh + 纹理加载

- [ ] 创建 `framework/include/himalaya/framework/mesh.h` + `framework/src/mesh.cpp`
- [ ] 统一顶点格式定义（position vec3 + normal vec3 + uv0 vec2 + tangent vec4 + uv1 vec2）
- [ ] Mesh 数据结构（vertex buffer handle、index buffer handle、顶点/索引数量）
- [ ] 创建 `framework/include/himalaya/framework/texture.h` + `framework/src/texture.cpp`
- [ ] stb_image 加载 JPEG/PNG → RGBA8 像素数据
- [ ] 上传到 GPU image（staging buffer）
- [ ] GPU 端 mip 生成（`vkCmdBlitImage` 逐级降采样 + layout transition 链）
- [ ] 注册到 DescriptorManager 获取 `BindlessIndex`
- [ ] 验证：能加载单张纹理并在 shader 中通过 bindless index 采样显示

## Step 6：材质系统 + glTF 场景加载

- [ ] 创建 `framework/include/himalaya/framework/material_system.h` + `framework/src/material_system.cpp`
- [ ] `GPUMaterialData` 结构体（base_color_factor、metallic/roughness_factor、各纹理 BindlessIndex）
- [ ] 全局 Material SSBO 管理（创建 buffer、写入材质数据）
- [ ] `MaterialInstance` 管理（template_id + buffer offset）
- [ ] 创建 `app/scene_loader.h/cpp`
- [ ] fastgltf 解析 glTF 文件
- [ ] 遍历 mesh：转换为统一顶点格式，创建 vertex/index buffer
- [ ] 遍历 material：提取 PBR 参数和纹理引用，创建 MaterialInstance
- [ ] 遍历 node：计算世界变换矩阵，填充 MeshInstance（含 world_bounds 计算）
- [ ] 填充 SceneRenderData
- [ ] 验证：glTF 场景加载后数据结构正确（ImGui 显示 mesh 数、材质数、纹理数）

## Step 7：Forward Pass + 视锥剔除

- [ ] 创建 `shaders/common/bindings.glsl`（全局绑定布局：Set 0 + Set 1 + push constant）
- [ ] 创建 `shaders/forward.vert`（MVP 变换，输出 world position / normal / uv0 / tangent）
- [ ] 创建 `shaders/forward.frag`（base color 纹理采样 + Lambert 光照 + 法线贴图 TBN）
- [ ] GlobalUBO 管理（每帧更新相机矩阵、屏幕尺寸等）
- [ ] LightBuffer 管理（填充方向光数据）
- [ ] Forward pass 注册到 Render Graph
- [ ] Pipeline 创建（使用 `DescriptorManager::get_global_set_layouts()`）
- [ ] Draw loop：遍历可见物体，push constant 传 model matrix + material_index，draw call
- [ ] 创建 `framework/include/himalaya/framework/culling.h` + `framework/src/culling.cpp`
- [ ] AABB vs 6 frustum planes 视锥剔除
- [ ] 移除旧的三角形渲染代码和 triangle shader
- [ ] 验证：glTF 场景正确渲染，有纹理和基础光照，相机移动时视锥剔除生效
