# Milestone 1：接口与目标结构

> 本文档包含 M1 的目标文件结构和关键接口定义。
> 设计决策与理由见 `m1-design-decisions.md`，长远架构目标见 `../project/architecture.md`，M1 功能范围见 `milestone-1.md`。

---

## M1 目标文件结构

### Layer 0 — RHI（rhi/）

```
rhi/
├── include/himalaya/rhi/
│   ├── types.h                  # 句柄类型（generation-based）、枚举、格式定义
│   ├── context.h                # Instance, Device, Queue, VMA, 帧同步, 删除队列
│   ├── swapchain.h              # Swapchain 管理
│   ├── resources.h              # Buffer, Image 创建与管理, 资源池
│   ├── descriptors.h            # Bindless descriptor 管理, Descriptor Set 分配
│   ├── pipeline.h               # Pipeline 创建与缓存
│   ├── commands.h               # Command Buffer 录制 wrapper
│   └── shader.h                 # 运行时编译（GLSL → SPIR-V）、缓存、热重载
└── src/
    ├── context.cpp
    ├── swapchain.cpp
    ├── resources.cpp
    ├── descriptors.cpp
    ├── pipeline.cpp
    ├── commands.cpp
    ├── shader.cpp
    └── vma_impl.cpp            # VMA 单头文件库实现
```

---

### Layer 1 — Framework（framework/）

```
framework/
├── include/himalaya/framework/
│   ├── render_graph.h           # Render Graph（编排 + barrier + temporal）
│   ├── material_system.h        # 材质模板、材质实例、参数布局
│   ├── mesh.h                   # 顶点格式、Mesh 数据管理
│   ├── texture.h                # 纹理加载、格式处理、mip 生成
│   ├── camera.h                 # 相机、投影、jitter
│   ├── scene_data.h             # 渲染列表、光源、探针数据结构定义（纯头文件）
│   ├── culling.h                # 视锥剔除
│   └── imgui_backend.h          # ImGui 集成
└── src/
    └── ...
```

`scene_data.h` 是纯头文件（只有数据结构定义），没有 .cpp——它定义的是渲染器和应用层之间的"合同"。AABB 等共享类型定义在此层。

---

### Layer 2 — Passes（passes/）

```
passes/
├── include/himalaya/passes/
│   ├── pass_interface.h         # Pass 基础接口定义（纯接口）
│   ├── depth_prepass.h          # Depth + Normal PrePass
│   ├── shadow_pass.h            # CSM 阴影
│   ├── forward_pass.h           # 主光照（Forward Lighting）
│   ├── transparent_pass.h       # 透明物体
│   ├── ssao_pass.h              # SSAO
│   ├── ssao_temporal.h          # SSAO Temporal Filter
│   ├── contact_shadows.h        # Contact Shadows
│   ├── bloom_pass.h             # Bloom 降采样 + 升采样链
│   ├── auto_exposure.h          # 自动曝光
│   ├── tonemapping.h            # Tonemapping（ACES）
│   ├── vignette.h               # Vignette
│   ├── color_grading.h          # Color Grading
│   ├── height_fog.h             # 高度雾
│   └── msaa_resolve.h           # MSAA Resolve（Depth、Normal、Color）
└── src/
    └── ...
```

---

### Layer 3 — App（app/）

```
app/
├── CMakeLists.txt
├── include/himalaya/app/
│   ├── application.h            # 主循环、窗口管理
│   ├── scene_loader.h           # glTF 加载 → 渲染列表
│   ├── camera_controller.h      # 自由漫游相机控制
│   └── debug_ui.h               # ImGui 面板、各 Pass 参数调整
└── src/
    ├── main.cpp                 # 入口
    ├── application.cpp
    ├── scene_loader.cpp
    ├── camera_controller.cpp
    └── debug_ui.cpp
```

App 层拥有 GLFW 窗口，传 `GLFWwindow*` 给 RHI 创建 Surface。

---

### Shaders

```
shaders/
├── common/                      # 共享头文件 / 函数
│   ├── brdf.glsl                # BRDF 函数（Lambert、GGX、Smith、Schlick）
│   ├── lighting.glsl            # 光照计算（方向光、IBL）
│   ├── shadow.glsl              # 阴影采样（CSM、PCF）
│   └── bindings.glsl            # 全局绑定布局定义
├── depth_prepass.vert/frag
├── forward.vert/frag
├── shadow.vert/frag
├── ssao.comp
├── ssao_temporal.comp
├── contact_shadows.comp
├── bloom_downsample.comp
├── bloom_upsample.comp
├── auto_exposure.comp
├── tonemapping.comp
├── transparent.vert/frag
├── vignette.comp
├── color_grading.comp
└── height_fog.comp
```

- `shaders/common/bindings.glsl` 定义全局绑定布局，所有 shader 通过 `#include` 引用，确保绑定一致性
- 后处理 shader 全部用 Compute Shader（`.comp`），不使用全屏三角形
- CMake 构建后拷贝到 build 目录，开发期通过编辑 build 副本触发热重载

---

## 关键接口与数据结构

> M1 层间接口的关键数据结构——多个模块共同依赖的"合同"。各 Pass 内部的辅助类和工具函数不在此定义，在实现时按需设计。

### Layer 0 — 句柄类型（rhi/types.h）

上层代码持有和传递这些轻量句柄，不直接接触 Vulkan 类型。内部实现是资源池的索引 + generation 计数器。generation 用于检测 use-after-free：资源销毁时 slot 的 generation 递增，使用句柄时比对 generation，不匹配则为过期句柄。

```cpp
// 资源句柄（generation-based）
struct ImageHandle    { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };
struct BufferHandle   { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };
struct PipelineHandle { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };
struct SamplerHandle  { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };

// Bindless 系统返回的纹理索引，shader 里用这个访问纹理
struct BindlessIndex { uint32_t index = UINT32_MAX; };
```

#### 资源创建描述

```cpp
// 所有字段必须显式初始化，无默认值。
// create_image() 通过 assert 拦截 depth/mip_levels/sample_count 为 0 的情况。
struct ImageDesc {
    uint32_t width, height;
    uint32_t depth;             // 2D images: must be 1
    uint32_t mip_levels;        // single level: must be 1
    uint32_t sample_count;      // no MSAA: must be 1
    Format format;              // 自定义枚举，映射到 VkFormat
    ImageUsage usage;           // 自定义 flags，映射到 VkImageUsageFlags
};

struct BufferDesc {
    uint64_t size;
    BufferUsage usage;
    MemoryUsage memory;     // GPU_ONLY, CPU_TO_GPU, GPU_TO_CPU
};

struct SamplerDesc {
    Filter mag_filter;          // NEAREST, LINEAR
    Filter min_filter;          // NEAREST, LINEAR
    SamplerMipMode mip_mode;    // NEAREST, LINEAR
    SamplerWrapMode wrap_u;     // REPEAT, CLAMP_TO_EDGE, MIRRORED_REPEAT
    SamplerWrapMode wrap_v;     // REPEAT, CLAMP_TO_EDGE, MIRRORED_REPEAT
    float max_anisotropy;       // 0 表示不启用各向异性
};
```

#### Format 转换函数（rhi/types.h）

```cpp
// Format 枚举 → VkFormat 映射
VkFormat to_vk_format(Format format);

// Format 枚举 → VkImageAspectFlags 推导（depth → DEPTH_BIT, color → COLOR_BIT）
VkImageAspectFlags aspect_from_format(Format format);
```

与 `Format` 枚举定义在同一文件中。RG 的 barrier 计算、ResourceManager 的 image 创建等处统一使用。

#### ResourceManager 扩展接口

```cpp
class ResourceManager {
public:
    // --- 已有接口省略 ---

    // 外部 image 注册（swapchain image 等非 ResourceManager 创建的资源）
    // 分配 slot 记录 VkImage/VkImageView/desc，allocation 为 null（不持有 VMA 内存）
    ImageHandle register_external_image(VkImage image, VkImageView view, const ImageDesc& desc);

    // 取消注册外部 image（释放 slot，递增 generation，不调用 vmaDestroyImage）
    void unregister_external_image(ImageHandle handle);

    // Image 上传（staging buffer + vkCmdCopyBufferToImage，在 immediate scope 内调用）
    void upload_image(ImageHandle handle, const void* data, uint64_t size);

    // GPU 端 mip 生成（逐级 vkCmdBlitImage，在 immediate scope 内调用）
    void generate_mips(ImageHandle handle);
};
```

#### ShaderCompiler 扩展接口

```cpp
class ShaderCompiler {
public:
    // --- 已有接口省略 ---

    // 从文件路径加载并编译 shader（封装文件读取 + compile）
    std::vector<uint32_t> compile_from_file(const std::string& path, ShaderStage stage);
};
```

---

### Layer 1 — 场景数据接口（framework/scene_data.h）

渲染器的输入"合同"——应用层填充这些数据，渲染器只读消费。

#### Mesh 实例

```cpp
struct MeshInstance {
    uint32_t mesh_id;           // 引用 Mesh 资源
    uint32_t material_id;       // 引用材质实例
    mat4 transform;
    mat4 prev_transform;        // M1 不使用，为 M2+ motion vectors 预留
    AABB world_bounds;          // 视锥剔除用
};
```

#### 光源

```cpp
struct DirectionalLight {
    vec3 direction;
    vec3 color;
    float intensity;
    bool cast_shadows;
};

struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float radius;
    bool cast_shadows;
};
```

> PointLight 在 M1 不使用（只有方向光），但结构体存在不影响任何实现。

#### 探针与 Lightmap

```cpp
struct ReflectionProbe {
    vec3 position;
    AABB influence_bounds;      // 视差校正用
    BindlessIndex cubemap_index;
};

struct LightmapInfo {
    BindlessIndex lightmap_index;
    // UV2 在 mesh 顶点数据里
};
```

#### 场景渲染数据（总入口）

```cpp
struct SceneRenderData {
    // 应用层填充，渲染器只读
    std::span<const MeshInstance> mesh_instances;
    std::span<const DirectionalLight> directional_lights;
    std::span<const PointLight> point_lights;
    std::span<const ReflectionProbe> reflection_probes;
    Camera camera;
    LightmapInfo lightmap;
    BindlessIndex sky_cubemap;
};
```

> 用 `std::span` 而非 `std::vector` — 渲染器不拥有场景数据，只引用应用层的数据。强化"渲染列表不可变"的约束。

#### 剔除结果

```cpp
struct CullResult {
    // 剔除输出，不修改 SceneRenderData
    std::vector<uint32_t> visible_opaque_indices;
    std::vector<uint32_t> visible_transparent_indices;  // 已按距离排序
};
```

#### GPU 数据结构（scene_data.h）

CPU 侧数据结构的 GPU 布局镜像，必须与 shader 端一一对应。

```cpp
// GlobalUBO — std140 layout, 288 bytes (aligned to 16)
// 对应 shader: Set 0, Binding 0
struct GlobalUniformData {
    glm::mat4 view;                             // offset   0
    glm::mat4 projection;                       // offset  64
    glm::mat4 view_projection;                  // offset 128
    glm::mat4 inv_view_projection;              // offset 192
    glm::vec4 camera_position_and_exposure;     // offset 256 — xyz = position, w = exposure
    glm::vec2 screen_size;                      // offset 272
    float time;                                 // offset 280 — 程序运行时间（秒），M2 水面/云层等动画用
    // std140 padding to 288 bytes
};

// GPU 方向光 — std430 layout, 32 bytes per element
// 对应 shader: Set 0, Binding 1 (LightBuffer SSBO)
struct alignas(16) GPUDirectionalLight {
    glm::vec4 direction_and_intensity;          // xyz = direction, w = intensity
    glm::vec4 color_and_shadow;                 // xyz = color, w = cast_shadows (0.0 / 1.0)
};

// Per-draw push constant — 68 bytes, within 128-byte minimum guarantee
// 对应 shader: push_constant, stage = VERTEX | FRAGMENT
// 演进计划：阶段六迁移到 per-instance SSBO（push constant 缩减为 uint instance_id），
//   同时容纳 lightmap 数据，并为 M2 的 prev_model (motion vectors) 预留空间。
//   详见 m1-design-decisions.md「Per-draw 数据演进：Push Constant → Per-instance SSBO」
struct PushConstantData {
    glm::mat4 model;                            // 64 bytes — vertex shader 使用
    uint32_t material_index;                    //  4 bytes — fragment shader 使用
};
```

---

### Layer 1 — Render Graph 接口（framework/render_graph.h）

#### 资源标识

```cpp
// 由 import_resource() 返回，pass 声明输入输出时使用
// 资源名称仅用于调试（debug name），不参与运行时查找
struct RGResourceId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};
```

#### Pass 资源使用声明

```cpp
struct RGResourceUsage {
    RGResourceId resource;
    RGAccessType access;        // READ, WRITE, READ_WRITE
    RGStage stage;              // COMPUTE, FRAGMENT, VERTEX,
                                // COLOR_ATTACHMENT, DEPTH_ATTACHMENT, TRANSFER
};
// READ_WRITE 语义：同一张 image 在同一帧内同时读写（如 depth attachment: test + write）。
// 不用于 temporal 场景——历史帧通过 get_history_image() 获取独立 RGResourceId，
// 当前帧和历史帧各自声明 READ 或 WRITE。
```

#### Render Graph 核心接口

```cpp
class RenderGraph {
public:
    // 导入外部创建的资源（阶段二：所有资源走此路径）
    // initial_layout：资源导入时的当前 layout，RG 以此为起点计算 layout transition
    // final_layout：RG execute 结束后将资源转换到的目标 layout（必填，无默认值）
    //   所有 imported image 都是帧间存活的，必须指定帧末 layout 以匹配下一帧的 initial_layout
    RGResourceId import_image(const std::string& debug_name, ImageHandle handle,
                              VkImageLayout initial_layout,
                              VkImageLayout final_layout);
    RGResourceId import_buffer(const std::string& debug_name, BufferHandle handle);

    // 注册一个 pass（资源的读写语义由 RGAccessType 区分）
    void add_pass(const std::string& name,
                  std::span<const RGResourceUsage> resources,
                  std::function<void(CommandBuffer&)> execute);

    // 获取资源句柄（pass execute 回调内调用）
    ImageHandle get_image(RGResourceId id);
    BufferHandle get_buffer(RGResourceId id);

    // 每帧重建前调用，清除所有 pass 和资源引用
    void clear();

    // 编译（barrier 自动插入）
    void compile();

    // 执行（接收外部 command buffer，RG 不持有同步资源）
    void execute(CommandBuffer& cmd);
};
```

#### CommandBuffer Debug Utils 扩展

```cpp
// Debug utils (VK_EXT_debug_utils)
void begin_debug_label(const std::string& name, std::array<float, 4> color = {0, 0, 0, 1});
void end_debug_label();
```

RG `execute()` 自动为每个 pass 调用 `begin_debug_label(pass_name)` / `end_debug_label()`，在 RenderDoc 和 GPU profiler 中按 pass 名称分组显示。

---

### Layer 2 — Pass 接口（passes/pass_interface.h）

> 阶段三引入。阶段二直接在 RG lambda 回调中编写渲染逻辑。

```cpp
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // 名字，用于 Render Graph 注册和调试
    virtual std::string name() const = 0;

    // 初始化：创建 pipeline、固定资源等
    // 调用一次，或资源变化时重新调用
    virtual void setup(RHIDevice& device) = 0;

    // 注册到 Render Graph：声明输入输出资源
    virtual void register_resources(RenderGraph& graph) = 0;

    // 每帧执行：录制 command buffer
    virtual void execute(CommandBuffer& cmd, RenderGraph& graph) = 0;

    // 是否启用（支持运行时开关）
    virtual bool enabled() const { return true; }

    // 窗口大小变化时重建依赖分辨率的资源
    virtual void on_resize(uint32_t width, uint32_t height) {}
};
```

---

### Layer 1 — 材质系统（framework/material_system.h）

#### 参数描述

```cpp
enum class MaterialParamType {
    FLOAT, VEC2, VEC3, VEC4, TEXTURE
};

struct MaterialParamDesc {
    std::string name;
    MaterialParamType type;
    uint32_t offset;            // 在材质数据 buffer 中的偏移
};
```

#### 材质模板（定义着色模型）

```cpp
struct MaterialTemplate {
    std::string name;           // 如 "StandardPBR"
    PipelineHandle pipeline;
    PipelineHandle depth_prepass_pipeline;
    PipelineHandle shadow_pipeline;
    std::vector<MaterialParamDesc> params;
};
```

#### 材质实例（具体参数值）

```cpp
struct MaterialInstance {
    uint32_t template_id;
    uint32_t material_buffer_offset;  // 在全局材质 SSBO 中的偏移
    // 参数值写在全局材质 SSBO 的对应偏移处
};
```

#### GPU 端材质数据布局（shader 读取）

```cpp
// std430 layout, total 64 bytes, aligned to 16 bytes
struct alignas(16) GPUMaterialData {
    vec4  base_color_factor;           // offset  0  — glTF baseColorFactor
    vec4  emissive_factor;             // offset 16  — xyz = glTF emissiveFactor, w unused (= 0)
    float metallic_factor;             // offset 32  — glTF metallicFactor
    float roughness_factor;            // offset 36  — glTF roughnessFactor
    uint  base_color_tex;              // offset 40  — bindless index
    uint  normal_tex;                  // offset 44  — bindless index
    uint  metallic_roughness_tex;      // offset 48  — bindless index
    uint  occlusion_tex;               // offset 52  — bindless index
    uint  emissive_tex;                // offset 56  — bindless index
    uint  _padding;                    // offset 60  — padding to 64 bytes (struct align = 16)
};
```

> emissive_factor 使用 vec4 而非 vec3 以避免 std430 对齐问题（vec3 对齐 16 字节会引入隐式 padding）。occlusion_tex 和 emissive_tex 在阶段二数据流中填充，shader 中阶段三 PBR 光照完善时启用读取。M1 阶段 GPUMaterialData 是定长结构体。以后材质类型增多时可扩展（加字段）或改为更灵活的布局。

---

### Layer 0 — Immediate Command Scope（rhi/context.h）

场景加载等批量上传场景使用。一次 begin，录制所有传输命令，一次 end 提交并等待完成。

```cpp
// Context 新增方法
class Context {
public:
    // 开始一段 immediate command 录制，返回可用的 CommandBuffer
    CommandBuffer begin_immediate();

    // 提交录制的命令并 vkQueueWaitIdle 等待完成
    void end_immediate(CommandBuffer& cmd);
};
```

`upload_buffer()` 改为录制模式：在活跃的 begin/end_immediate scope 内调用时，只录制 copy 命令到当前 command buffer，不自行 submit。staging buffer 由 Context 收集，`end_immediate()` submit + wait 完成后统一销毁。scope 外调用 `upload_buffer()` 会 assert 失败。

---

### Shader 端 — 全局绑定布局（shaders/common/bindings.glsl）

```glsl
// Set 0: 全局数据（每帧更新一次）
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inv_view_projection;
    vec4 camera_position_and_exposure;      // xyz = position, w = exposure
    vec2 screen_size;
    float time;                             // 程序运行时间（秒）
} global;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    // 每个元素: vec4 direction_and_intensity + vec4 color_and_shadow
    GPUDirectionalLight directional_lights[];
};

layout(set = 0, binding = 2) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

// Set 1: Bindless 纹理数组
layout(set = 1, binding = 0) uniform sampler2D textures[];

// Per-draw 数据
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint material_index;
};
```

#### Descriptor Set 管理方式

Set 0 和 Set 1 均使用传统 Descriptor Set（非 Push Descriptors）。

- **Set 0**：per-frame 分配 2 个 descriptor set（对应 2 frames in flight），每帧绑定当前帧的 set
- **Set 1**：分配 1 个 descriptor set，加载时写入，长期持有

#### 数据分层对应关系

| 数据层 | 更新频率 | 绑定方式 | 内容 |
|--------|---------|----------|------|
| 全局数据 | 每帧一次 | Set 0, Binding 0 (UBO) | 相机矩阵、屏幕尺寸、曝光值 |
| 光源数据 | 每帧一次 | Set 0, Binding 1 (SSBO) | 方向光、点光源数组 |
| 材质数据 | 加载时一次 | Set 0, Binding 2 (SSBO) | PBR 参数、纹理 index |
| 纹理数据 | 加载时一次 | Set 1, Binding 0 (Bindless) | 所有纹理通过 index 访问 |
| Per-draw 数据 | 每次绘制 | Push Constant | 模型矩阵、材质 index |
