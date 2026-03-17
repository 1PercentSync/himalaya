# Final Project 总览

## 项目现状

**Himalaya** 是一个基于 Vulkan 1.4 的实时 PBR 渲染器，采用 C++20 开发，以光栅化渲染为起步。项目按 Milestone 划分，当前处于 **Milestone 1（静态场景演示）** 的开发过程中。

### 已完成的功能

| 类别 | 功能 | 说明 |
|------|------|------|
| **Vulkan 基础设施** | Instance / Device / Queue / Swapchain | Vulkan 1.4 核心，Dynamic Rendering + Synchronization2 + Extended Dynamic State |
| **内存管理** | VMA 集成 | Vulkan Memory Allocator |
| **Shader** | 运行时编译 | shaderc 集成，GLSL → SPIR-V |
| **资源管理** | Buffer / Image / Sampler | 创建、上传、销毁 |
| **描述符** | Bindless 纹理 + 传统 Descriptor Set | Set 0 全局数据 + Set 1 Bindless 纹理数组 |
| **Render Graph** | 手动编排 + barrier 自动插入 | Managed 资源 + Imported 资源，跨帧缓存 |
| **场景加载** | glTF（fastgltf） | 运行时 Windows 原生文件对话框选择场景/HDR |
| **材质** | PBR Metallic-Roughness | Lambert 漫反射 + Cook-Torrance 镜面反射（GGX / Smith / Schlick），法线贴图 |
| **光照** | 方向光直接光照 + IBL 环境光 | Split-Sum 近似，BRDF LUT 预计算，HDR Cubemap |
| **渲染管线** | Depth + Normal PrePass → Forward Pass → Skybox → Tonemapping → ImGui | MSAA 支持（4x/8x 可切换），Reverse-Z |
| **优化** | CPU 视锥剔除 + Instancing | Per-instance SSBO + mesh_id 分组 + instanced draw |
| **纹理压缩** | BC7 / BC5 + KTX2 缓存 | CPU 端 BC 压缩，首次加载后缓存为 KTX2，后续直接加载 |
| **配置** | JSON 持久化 | 场景/HDR 路径记忆，重启恢复 |
| **调试** | ImGui DebugUI + RenderDoc 兼容 | 渲染模式切换（base color / normal / metallic / roughness 等 passthrough） |

### 当前画面状态

场景有正确的 PBR 光照（直射光 + IBL 环境光），金属表面反射天空 Cubemap，HDR 渲染经过 ACES Tonemapping。**但没有阴影、没有 AO、没有后处理**——画面偏平，缺乏立体感和氛围。
