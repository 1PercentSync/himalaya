# Milestone 1：关键接口与数据结构

> 本文档定义 M1 层间接口的关键数据结构——多个模块共同依赖的"合同"。
> 各 Pass 内部的辅助类和工具函数不在此定义，在实现时按需设计。

---

## Layer 0 — 句柄类型（rhi/types.h）

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

### 资源创建描述

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
```

---

## Layer 1 — 场景数据接口（framework/scene_data.h）

渲染器的输入"合同"——应用层填充这些数据，渲染器只读消费。

### Mesh 实例

```cpp
struct MeshInstance {
    uint32_t mesh_id;           // 引用 Mesh 资源
    uint32_t material_id;       // 引用材质实例
    mat4 transform;
    mat4 prev_transform;        // M1 不使用，为 M2+ motion vectors 预留
    AABB world_bounds;          // 视锥剔除用
};
```

### 光源

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

### 探针与 Lightmap

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

### 场景渲染数据（总入口）

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

### 剔除结果

```cpp
struct CullResult {
    // 剔除输出，不修改 SceneRenderData
    std::vector<uint32_t> visible_opaque_indices;
    std::vector<uint32_t> visible_transparent_indices;  // 已按距离排序
};
```

---

## Layer 1 — Render Graph 接口（framework/render_graph.h）

### 资源标识

```cpp
// 由 import_resource() 返回，pass 声明输入输出时使用
// 资源名称仅用于调试（debug name），不参与运行时查找
struct RGResourceId {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};
```

### Pass 资源使用声明

```cpp
struct RGResourceUsage {
    RGResourceId resource;
    RGAccessType access;        // READ, WRITE, READ_WRITE
    RGStage stage;              // COMPUTE, FRAGMENT, VERTEX,
                                // COLOR_ATTACHMENT, DEPTH_ATTACHMENT, TRANSFER
};
```

### Render Graph 核心接口

```cpp
class RenderGraph {
public:
    // 导入外部创建的资源（阶段二：所有资源走此路径）
    RGResourceId import_image(const std::string& debug_name, ImageHandle handle);
    RGResourceId import_buffer(const std::string& debug_name, BufferHandle handle);

    // 注册一个 pass
    void add_pass(const std::string& name,
                  std::span<const RGResourceUsage> inputs,
                  std::span<const RGResourceUsage> outputs,
                  std::function<void(CommandBuffer&)> execute);

    // 获取资源句柄（pass execute 回调内调用）
    ImageHandle get_image(RGResourceId id);
    BufferHandle get_buffer(RGResourceId id);

    // 编译并执行（barrier 自动插入）
    void compile();
    void execute();
};
```

### 后续扩展（不在阶段二实现）

阶段三引入 transient 资源时新增：

```cpp
// 声明 RG 管理的 transient 资源（帧内创建，帧结束可回收）
RGResourceId declare_resource(const RGResourceDesc& desc);
```

阶段五引入 temporal 资源时新增：

```cpp
// RGResourceDesc 中添加 is_temporal 标记
// 获取 temporal 资源的历史帧版本
ImageHandle get_history_image(RGResourceId id);
```

---

## Layer 2 — Pass 接口（passes/pass_interface.h）

> **推迟到阶段三引入**。阶段二 pass 少（一两个），直接在 Render Graph 的 lambda 回调中编写渲染逻辑。阶段三有了多个 pass 的实际经验后再提取此抽象。

每个渲染 Pass 实现此接口（阶段三起）。

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

## Layer 1 — 材质系统（framework/material_system.h）

### 参数描述

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

### 材质模板（定义着色模型）

```cpp
struct MaterialTemplate {
    std::string name;           // 如 "StandardPBR"
    PipelineHandle pipeline;
    PipelineHandle depth_prepass_pipeline;
    PipelineHandle shadow_pipeline;
    std::vector<MaterialParamDesc> params;
};
```

### 材质实例（具体参数值）

```cpp
struct MaterialInstance {
    uint32_t template_id;
    uint32_t material_buffer_offset;  // 在全局材质 SSBO 中的偏移
    // 参数值写在全局材质 SSBO 的对应偏移处
};
```

### GPU 端材质数据布局（shader 读取）

```cpp
struct GPUMaterialData {
    vec4 base_color_factor;
    float metallic_factor;
    float roughness_factor;
    BindlessIndex base_color_tex;
    BindlessIndex normal_tex;
    BindlessIndex metallic_roughness_tex;
    BindlessIndex lightmap_tex;
    // ... 可扩展
};
```

> M1 阶段 GPUMaterialData 是定长结构体。以后材质类型增多时可扩展（加字段）或改为更灵活的布局。

---

## Shader 端 — 全局绑定布局（shaders/common/bindings.glsl）

```glsl
// Set 0: 全局数据（每帧更新一次）
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inv_view_projection;
    vec3 camera_position;
    float time;
    vec2 screen_size;
    float exposure;
    // ...
} global;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    DirectionalLight directional_lights[];
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

### Descriptor Set 管理方式

Set 0 和 Set 1 均使用传统 Descriptor Set（非 Push Descriptors）。

- **Set 0**：per-frame 分配 2 个 descriptor set（对应 2 frames in flight），每帧绑定当前帧的 set
- **Set 1**：分配 1 个 descriptor set，加载时写入，长期持有

### 数据分层对应关系

| 数据层 | 更新频率 | 绑定方式 | 内容 |
|--------|---------|----------|------|
| 全局数据 | 每帧一次 | Set 0, Binding 0 (UBO) | 相机矩阵、屏幕尺寸、曝光值 |
| 光源数据 | 每帧一次 | Set 0, Binding 1 (SSBO) | 方向光、点光源数组 |
| 材质数据 | 加载时一次 | Set 0, Binding 2 (SSBO) | PBR 参数、纹理 index |
| 纹理数据 | 加载时一次 | Set 1, Binding 0 (Bindless) | 所有纹理通过 index 访问 |
| Per-draw 数据 | 每次绘制 | Push Constant | 模型矩阵、材质 index |
