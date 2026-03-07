# Milestone 1 阶段二：基础渲染管线

> 阶段二的目标是搭建完整的渲染管线骨架：加载 glTF 场景、用基础 Lit shader 渲染、相机自由漫游。
> 涉及 Layer 0 补全（Descriptor 管理）、Layer 1 框架建设、Layer 3 重构与场景加载。
> 高层级阶段划分见 `milestone-1/m1-development-order.md`，架构决策见 `milestone-1/m1-architecture-choices.md`。

---

## 阶段二技术决策摘要

以下决策在阶段二设计讨论中确定，详细理由见 `milestone-1/m1-architecture-choices.md`：

| 决策项 | 选择 | 要点 |
|--------|------|------|
| Render Graph 资源管理 | 阶段二只管 barrier + 资源导入 | Transient 资源阶段三加，temporal 阶段五加 |
| RG 资源引用 | Typed handle（`RGResourceId`） | 编译期类型安全，不用字符串 |
| RG 接管范围 | acquire 和 present 之间的所有 GPU 工作 | Swapchain image 作为 imported resource |
| ImGui 在 RG 中 | 独立 pass（最后一个） | 统一由 RG 管理 |
| Pass 抽象基类 | 推迟到阶段三 | 阶段二用 lambda 回调 |
| Descriptor 管理 | DescriptorManager 集中管理，提供 `get_global_set_layouts()` | 一致性风险集中在一处 |
| Bindless 纹理上限 | 固定 4096 | `VARIABLE_DESCRIPTOR_COUNT` + `PARTIALLY_BOUND` |
| Bindless slot 回收 | Free list + deferred deletion | 支持场景切换 |
| glTF 加载库 | fastgltf | 现代 C++17 风格，解析后立即转入自有数据结构 |
| 图像解码 | stb_image | JPEG/PNG 解码 |
| 纹理格式 | 阶段二直接加载 JPEG/PNG | BC 压缩推迟，是后续优化项 |
| 顶点格式 | 统一（position + normal + uv0 + tangent + uv1） | 缺失属性填默认值 |
| 阶段二 shader | 基础 Lit（Lambert + 从 LightBuffer 读取方向光） | 验证完整数据流（含 bindless + light buffer） |
| 材质数据流 | 全局 Material SSBO + push constant（完整实现） | 验证 bindless 完整链路 |
| 相机 | Camera 数据在 framework，CameraController 在 app | 渲染器不知道输入如何产生 |
| Y-flip | 负 viewport height（延续阶段一） | 心智负担最小，Vulkan 核心保证 |
| 深度范围 | `GLM_FORCE_DEPTH_ZERO_TO_ONE` | Vulkan 深度 [0,1] |
| SceneRenderData | 完整结构一次定义，阶段二只填需要的字段 | 空 span 零成本 |
| 视锥剔除 | 阶段二实现 | AABB-frustum，验证 world_bounds 正确性 |
| Mip 生成 | GPU 端 vkCmdBlitImage | 标准做法，写一次复用 |
| App 层 | Application 持有全部（注释分组）+ 模块按需接口 | Composition Root，阶段三提取 Renderer |
| Depth Buffer | 阶段二创建（D32Sfloat） | imported resource 导入 RG，resize 时重建 |
| Reverse-Z | 阶段二启用 | near=1, far=0, clear 0.0f, compare GREATER |
| CommandBuffer 补全 | 分散到各 Step | Step 2: bind_descriptor_sets; Step 7: draw_indexed 等 |
| Shader #include | shaderc Includer | 自定义 includer 从 shaders/ 目录解析 |
| RG 初始 layout | import_image 参数 | 调用方传入 initial_layout |
| RG 帧间生命周期 | 每帧重建 | 长期方案，非过渡 |
| RG Command Buffer | 外部传入 | execute(CommandBuffer&) |
| RG Barrier 粒度 | 仅 image layout | buffer barrier 阶段三按需加 |
| GPU 上传 | 批量上传 | begin/end_immediate scope，一次 submit |
| Resize 策略 | Application 集中管理 | 阶段三重构为 on_resize() |
| glTF 多 primitive | 展开为独立 MeshInstance | 长期方案 |
| glTF 坐标系 | 无需额外处理 | 负 viewport + GLM 右手系自洽 |
| glTF 资源加载 | fastgltf 统一处理 | LoadExternalBuffers + LoadExternalImages |
| Material SSBO | 一次性分配 | 加载后知道数量再创建 |
| Set 0 更新 | descriptor 写一次 + 每帧 memcpy | GlobalUBO×2, LightBuffer×2, MaterialBuffer×1 |
| sRGB 处理 | 按 texture 角色选 format | base color→SRGB, normal→UNORM |
| Sampler 管理 | per-texture sampler | SamplerDesc + ResourceManager 管理 |
| Default 纹理 | 三个 1×1（white/flat normal/black） | Texture 模块持有，固定 BindlessIndex |
| RG final_layout | import_image 必填参数 | 所有 imported image 指定帧末 layout，RG 自动插入转换 |
| RG barrier 计算 | 按需实现映射组合 | 阶段二只实现用到的组合，assert 拦截未实现的。RG 不管 loadOp |
| RG debug label | 阶段二实现 | CommandBuffer 新增方法，RG execute 自动插入 |
| 纹理角色 | TextureRole 枚举 | texture 模块接受 Color/Linear，scene_loader 按 glTF 字段传入 |
| 缺失 tangent | MikkTSpace 生成 | vcpkg 引入 mikktspace，mesh 缺失 tangent 时自动生成 |
| Sampler 去重 | 不做 | glTF 层面自然去重够用，后续按需升级 |
| upload_buffer | 录制到 immediate scope | 不自行 submit，staging 由 Context 收集并在 end_immediate 后销毁 |
| Resize 流程 | handle_resize() 集中处理 | vkQueueWaitIdle 后立即销毁，acquire 失败和帧末两处共用 |
| GPUMaterialData | 完整字段 64 字节 | vec4×2 + float×2 + uint×5 + padding，含 occlusion/emissive 占位 |
| Forward shader 光照 | 从 LightBuffer 读取 | 不硬编码，验证完整数据流 |
| Descriptor Pool | 两个 pool 分离 | 普通 pool (Set 0) + UPDATE_AFTER_BIND pool (Set 1) |
| SceneLoader 所有权 | 持有全部资源句柄 | 提供 destroy() 一次性清理，支持场景切换 |
| stb_image 通道 | 强制 RGBA | stbi_load 第四参数传 4，统一 R8G8B8A8 |
| CameraController 输入 | 检查 ImGui WantCapture | 避免 ImGui 操作时相机误动 |
| 默认方向光 | Application 保底 | scene_loader 忠实还原 glTF，Application 检测空光源后填默认值 |
| Swapchain image 导入 RG | ResourceManager 外部注册 | `register_external_image()` 返回 ImageHandle，import_image 接口不变 |
| RG 解析 ImageHandle | RG 持有 ResourceManager* | framework→rhi 合法依赖方向 |
| RG barrier aspect | 从 format 推导 | `aspect_from_format()` 提取到 `rhi/types.h` 公共函数 |
| Format 转换函数 | 提取到 `rhi/types.h` | `to_vk_format()` + `aspect_from_format()` 供多处复用 |
| GlobalUBO 布局 | vec4 打包，保留 time | `camera_position_and_exposure`（xyz=pos, w=exposure）+ `screen_size` + `time`，放 scene_data.h |
| GPUDirectionalLight | 两个 vec4，含 cast_shadows | `direction_and_intensity` + `color_and_shadow`，放 scene_data.h |
| PushConstantData | 单 range VERTEX\|FRAGMENT | `mat4 model` + `uint material_index`，68 bytes，放 scene_data.h。阶段六迁移到 per-instance SSBO |
| Image upload | ResourceManager 新增方法 | `upload_image()` + `generate_mips()`，通用资源操作 |
| Staging buffer 收集 | Context pending list | `end_immediate()` 后统一销毁 |
| GLM 编译宏 | CMake PUBLIC 定义 | `GLM_FORCE_DEPTH_ZERO_TO_ONE` 定义在 himalaya_framework |
| fastgltf 归属 | app 层 | 链接到 himalaya_app，不是 framework |
| stb_image 实现 | `framework/src/stb_impl.cpp` | `#define STB_IMAGE_IMPLEMENTATION` |
| Framework Vulkan 类型 | RG + ImGui 为明确例外 | 非暂时性，详见 architecture.md |
| Shader 文件读取 | `ShaderCompiler::compile_from_file()` | 封装文件读取+编译 |
| 场景路径 | `argc/argv` + 默认路径 | 长期方向 GUI 文件选择器 |
| 加载错误处理 | log error + abort | 开发期不做 fallback，立刻暴露问题 |
| upload_buffer 迁移时机 | Step 5 | Step 5 引入 immediate scope 时适配旧三角形上传代码，Step 7 删除三角形后旧路径消失 |
| RG READ_WRITE 语义 | 同帧同 image 读写 | 不用于 temporal（历史帧是独立 RGResourceId） |

---

> **实现步骤、文件清单、技术笔记**已移至 `current-phase.md`。
