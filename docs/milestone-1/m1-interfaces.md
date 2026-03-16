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
│   ├── frame_context.h          # FrameContext（一帧渲染所需的全部上下文，纯头文件）
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
│   ├── depth_prepass.h          # Depth + Normal PrePass
│   ├── shadow_pass.h            # CSM 阴影
│   ├── forward_pass.h           # 主光照（Forward Lighting）
│   ├── transparent_pass.h       # 透明物体
│   ├── ssao_pass.h              # SSAO
│   ├── ssao_temporal.h          # SSAO Temporal Filter
│   ├── contact_shadows.h        # Contact Shadows
│   ├── bloom_pass.h             # Bloom 降采样 + 升采样链
│   ├── auto_exposure.h          # 自动曝光
│   ├── tonemapping_pass.h       # Tonemapping（ACES）
│   ├── vignette.h               # Vignette
│   ├── color_grading.h          # Color Grading
│   └── height_fog.h             # 高度雾
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
│   ├── renderer.h               # 渲染子系统（RG 编排、pass 管理、GPU 数据填充）
│   ├── scene_loader.h           # glTF 加载 → 渲染列表
│   ├── camera_controller.h      # 自由漫游相机控制
│   └── debug_ui.h               # ImGui 面板、各 Pass 参数调整
└── src/
    ├── main.cpp                 # 入口
    ├── application.cpp
    ├── renderer.cpp
    ├── scene_loader.cpp
    ├── camera_controller.cpp
    └── debug_ui.cpp
```

App 层拥有 GLFW 窗口，传 `GLFWwindow*` 给 RHI 创建 Surface。Application 持有 RHI 基础设施和 App 模块，Renderer 持有渲染子系统，详见 `m1-design-decisions.md`「App 层设计」。

---

### Shaders

```
shaders/
├── common/                      # 共享头文件 / 函数
│   ├── constants.glsl           # 数学常量（PI、EPSILON 等）
│   ├── brdf.glsl                # BRDF 函数（D_GGX、V_SmithGGX、F_Schlick，include constants）
│   ├── normal.glsl              # TBN 构造、normal map 解码
│   ├── shadow.glsl              # 阴影采样（CSM、PCF）
│   └── bindings.glsl            # 全局绑定布局定义
├── depth_prepass.vert           # 共享 VS（invariant gl_Position）
├── depth_prepass.frag           # Opaque FS（无 discard）
├── depth_prepass_masked.frag    # Mask FS（alpha test + discard）
├── forward.vert/frag
├── shadow.vert                  # Shadow VS（position + uv0，cascade_vp 变换）
├── shadow_masked.frag           # Shadow mask FS（alpha test + discard，opaque 无 FS）
├── ssao.comp
├── ssao_temporal.comp
├── contact_shadows.comp
├── bloom_downsample.comp
├── bloom_upsample.comp
├── auto_exposure.comp
├── fullscreen.vert              # Fullscreen triangle（无顶点输入，后处理共用）
├── tonemapping.frag             # Tonemapping（ACES，fullscreen fragment）
├── transparent.vert/frag
├── vignette.comp
├── color_grading.comp
└── height_fog.comp
```

- `shaders/common/bindings.glsl` 定义全局绑定布局，所有 shader 通过 `#include` 引用，确保绑定一致性
- 后处理 shader 优先使用 Compute Shader（`.comp`）；直接写入 SRGB swapchain 的 pass 使用 fullscreen fragment shader（SRGB format 不支持 `STORAGE_BIT`）
- CMake 构建后拷贝到 build 目录，开发期通过编辑 build 副本触发热重载

---

## 关键接口与数据结构

> M1 层间接口的关键数据结构——多个模块共同依赖的"合同"。各 Pass 内部的辅助类和工具函数不在此定义，在实现时按需设计。

### Layer 0 — 句柄类型（rhi/types.h）

上层代码持有和传递这些轻量句柄，不直接接触 Vulkan 类型。内部实现是资源池的索引 + generation 计数器。generation 用于检测 use-after-free：资源销毁时 slot 的 generation 递增，使用句柄时比对 generation，不匹配则为过期句柄。

```cpp
// 资源句柄（generation-based）— 用于资源池管理的资源
struct ImageHandle    { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };
struct BufferHandle   { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };
struct SamplerHandle  { uint32_t index = UINT32_MAX; uint32_t generation = 0; bool valid() const; };

// Bindless 系统返回的纹理索引，shader 里用这个访问纹理
struct BindlessIndex { uint32_t index = UINT32_MAX; };
```

Pipeline 不使用 handle 体系——所有权单一明确（pass 直接持有 `Pipeline` 值类型），详见 `m1-design-decisions.md`「资源句柄设计」。

#### 资源创建描述

```cpp
// 所有字段必须显式初始化，无默认值。
// create_image() 通过 assert 拦截 depth/mip_levels/sample_count 为 0 的情况。
// array_layers == 6 自动推断 cubemap（VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT + CUBE view type）。
struct ImageDesc {
    uint32_t width, height;
    uint32_t depth;             // 2D images: must be 1
    uint32_t mip_levels;        // single level: must be 1
    uint32_t array_layers;      // cubemap: 6, regular 2D: 1
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
    float max_lod;              // 0 = 仅基础级别（禁用 mip），VK_LOD_CLAMP_NONE = 不限制
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
    // --- 资源创建（debug_name 为必选参数，内部调用 vkSetDebugUtilsObjectNameEXT） ---
    ImageHandle  create_image(const ImageDesc& desc, const char* debug_name);
    BufferHandle create_buffer(const BufferDesc& desc, const char* debug_name);
    SamplerHandle create_sampler(const SamplerDesc& desc, const char* debug_name);

    // 外部 image 注册（swapchain image 等非 ResourceManager 创建的资源）
    // 分配 slot 记录 VkImage/VkImageView/desc，allocation 为 null（不持有 VMA 内存）
    ImageHandle register_external_image(VkImage image, VkImageView view, const ImageDesc& desc);

    // 取消注册外部 image（释放 slot，递增 generation，不调用 vmaDestroyImage）
    void unregister_external_image(ImageHandle handle);

    // Image 上传（staging buffer + vkCmdCopyBufferToImage，在 immediate scope 内调用）
    // dst_stage: 最终 barrier 的 dstStageMask（调用方按消费者指定）
    void upload_image(ImageHandle handle, const void* data, uint64_t size,
                      VkPipelineStageFlags2 dst_stage);

    // GPU 端 mip 生成（逐级 vkCmdBlitImage，在 immediate scope 内调用）
    void generate_mips(ImageHandle handle);
};
```

#### ShaderCompiler 接口

```cpp
class ShaderCompiler {
public:
    // 配置 #include 解析根目录（初始化时调用一次）
    void set_include_path(const std::string& path);

    // 从文件路径加载并编译 shader（唯一公开编译入口）
    // path 相对于 set_include_path() 设置的根目录，与 FileIncluder 的解析规则一致
    // 例：set_include_path("shaders") 后，compile_from_file("forward.vert", Vertex)
    //     读取 shaders/forward.vert，filename 传 "forward.vert" 给内部 compile()
    std::vector<uint32_t> compile_from_file(const std::string& path, ShaderStage stage);
};
```

#### DescriptorManager 接口

```cpp
class DescriptorManager {
public:
    // --- Descriptor Set getter ---
    // Pass 在 setup() 时存储 DescriptorManager*，
    // record() 的 execute lambda 中按需调用以下 getter 获取 descriptor set。

    // Set 0: per-frame（2 个 set 对应 2 frames in flight）
    VkDescriptorSet get_set0(uint32_t frame_index) const;

    // Set 1: 持久 Bindless 纹理（1 个 set，加载时写入）
    VkDescriptorSet get_set1() const;

    // Set 2: Render Target 中间产物（1 个 set，init/resize/MSAA 切换时更新）
    VkDescriptorSet get_set2() const;

    // 返回三个 Set 的 layout（Set 0 + Set 1 + Set 2），用于 pipeline 创建
    std::array<VkDescriptorSetLayout, 3> get_global_set_layouts() const;
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

```

> PointLight、ReflectionProbe、LightmapInfo 延迟到实际需要时再定义（YAGNI）。
> 接口设计保留在此文档作为参考，但不提前写入代码。

<details>
<summary>延迟定义的类型（参考）</summary>

```cpp
struct PointLight {
    vec3 position;
    vec3 color;
    float intensity;
    float radius;
    bool cast_shadows;
};

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

</details>

#### 场景渲染数据（总入口）

```cpp
struct SceneRenderData {
    // 应用层填充，渲染器只读
    std::span<const MeshInstance> mesh_instances;
    std::span<const DirectionalLight> directional_lights;
    Camera camera;
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
// GlobalUBO — std140 layout, 624 bytes (aligned to 16)
// 对应 shader: Set 0, Binding 0
struct GlobalUniformData {
    glm::mat4 view;                             // offset   0
    glm::mat4 projection;                       // offset  64
    glm::mat4 view_projection;                  // offset 128
    glm::mat4 inv_view_projection;              // offset 192
    glm::vec4 camera_position_and_exposure;     // offset 256 — xyz = position, w = exposure
    glm::vec2 screen_size;                      // offset 272
    float time;                                 // offset 280 — 程序运行时间（秒），M2 水面/云层等动画用
    uint32_t directional_light_count;           // offset 284 — 活跃方向光数量
    float ibl_intensity;                        // offset 288 — IBL 环境光强度乘数
    uint32_t irradiance_cubemap_index;          // offset 292 — cubemaps[] 下标
    uint32_t prefiltered_cubemap_index;         // offset 296 — cubemaps[] 下标
    uint32_t brdf_lut_index;                    // offset 300 — textures[] 下标
    uint32_t prefiltered_mip_count;             // offset 304 — roughness → mip level 映射
    uint32_t skybox_cubemap_index;              // offset 308 — cubemaps[] 下标（Skybox Pass 天空渲染用）
    float ibl_rotation_sin;                     // offset 312 — IBL 水平旋转 sin(yaw)
    float ibl_rotation_cos;                     // offset 316 — IBL 水平旋转 cos(yaw)
    uint32_t debug_render_mode;                 // offset 320 — DEBUG_MODE_* 常量，见 bindings.glsl
    // --- 阶段四新增 ---
    uint32_t feature_flags;                     // offset 324 — bitmask: FEATURE_SHADOWS 等
    uint32_t shadow_cascade_count;              // offset 328 — 活跃 cascade 数量
    float shadow_normal_offset;                 // offset 332 — 法线偏移 bias 强度
    float shadow_texel_size;                    // offset 336 — 1.0 / shadow_map_resolution
    float shadow_max_distance;                  // offset 340 — cascade 覆盖最远距离
    float shadow_blend_width;                   // offset 344 — cascade blend region 占比
    uint32_t shadow_pcf_radius;                 // offset 348 — PCF kernel 半径 (1=3×3, 2=5×5)
    glm::mat4 cascade_view_proj[4];             // offset 352 — per-cascade 光空间 VP 矩阵
    glm::vec4 cascade_splits;                   // offset 608 — 4 个 cascade 远边界 (view-space depth)
};  // total: 624 bytes (39 × 16)

// GPU 方向光 — std430 layout, 32 bytes per element
// 对应 shader: Set 0, Binding 1 (LightBuffer SSBO)
struct alignas(16) GPUDirectionalLight {
    glm::vec4 direction_and_intensity;          // xyz = direction, w = intensity
    glm::vec4 color_and_shadow;                 // xyz = color, w = cast_shadows (0.0 / 1.0)
};

// Per-instance 数据 — std430 layout, 80 bytes per element
// 对应 shader: Set 0, Binding 3 (InstanceBuffer SSBO)
// 阶段四准备工作引入（提前合并原计划阶段六 per-instance SSBO 迁移 + M3 Instancing）
// 未来扩展：M2 的 prev_model (motion vectors)、阶段六的 lightmap UV transform
struct GPUInstanceData {
    glm::mat4 model;                            // 64 bytes — 世界变换矩阵
    uint32_t material_index;                    //  4 bytes — MaterialBuffer SSBO 索引
    uint32_t _padding[3];                       // 12 bytes — 对齐到 80 bytes (16 的倍数)
};

// Per-draw push constant — 4 bytes
// 对应 shader: push_constant, stage = VERTEX
// 仅 shadow pass 使用，forward / prepass 不 push
struct PushConstantData {
    uint32_t cascade_index;                     //  4 bytes — shadow.vert 使用
};
```

---

### Layer 1 — 缓存模块接口（framework/cache.h）

纹理压缩和 IBL 缓存共用的轻量工具模块。只提供路径和哈希工具，不关心具体缓存格式。设计决策见 `m1-design-decisions.md`「缓存基础设施」。

```cpp
namespace himalaya::framework {
    /// 返回缓存根目录 (%TEMP%\himalaya\)，首次调用时创建
    std::filesystem::path cache_root();

    /// 计算数据的内容哈希（XXH3_128，返回 32 字符十六进制字符串）
    std::string content_hash(const void* data, size_t size);

    /// 计算文件的内容哈希（读取全部内容后哈希）
    std::string content_hash(const std::filesystem::path& file);

    /// 拼接缓存文件路径: cache_root() / category / (hash + extension)
    /// 自动创建 category 子目录
    std::filesystem::path cache_path(std::string_view category,
                                     std::string_view hash,
                                     std::string_view extension);
}
```

---

### Layer 1 — IBL 模块接口（framework/ibl.h）

IBL 模块自管理全部资源——`init()` 中创建的所有 image 由模块自身持有，`destroy()` 先注销 bindless 条目再销毁底层资源。设计决策见 `m1-design-decisions.md`「IBL 资源所有权」。

```cpp
class IBL {
public:
    /// 加载 .hdr 文件，执行全部预计算（equirect → cubemap → irradiance/prefiltered/BRDF LUT），
    /// 注册产物到 Set 1 bindless 数组。在 begin_immediate() / end_immediate() scope 内执行。
    /// 返回 true 表示 HDR 加载成功，false 表示使用了 fallback cubemap。
    bool init(rhi::Context& ctx, rhi::ResourceManager& rm,
              rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
              const std::string& hdr_path);

    /// 注销 bindless 条目 + 销毁全部 image（中间 cubemap、irradiance、prefiltered、BRDF LUT）。
    /// equirect 输入 image 在 init() 内部已销毁，不出 init scope。
    void destroy();

    /// 获取 IBL 产物的 bindless index（Renderer 填充 GlobalUBO 用）
    BindlessIndex irradiance_cubemap_index() const;
    BindlessIndex prefiltered_cubemap_index() const;
    BindlessIndex brdf_lut_index() const;
    BindlessIndex skybox_cubemap_index() const;  // 中间 cubemap，用于 Skybox Pass
    uint32_t prefiltered_mip_count() const;
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

#### Managed 资源类型

```cpp
enum class RGSizeMode : uint8_t { Relative, Absolute };

struct RGImageDesc {
    RGSizeMode size_mode;
    // Relative mode（render target 等屏幕尺寸相关资源；阶段三全部、M2 半分辨率 SSAO/SSR）
    float width_scale;
    float height_scale;
    // Absolute mode（Shadow Map 等固定尺寸资源；阶段四 Shadow Map 固定 2048/4096）
    uint32_t width;
    uint32_t height;
    // Common
    rhi::Format format;
    rhi::ImageUsage usage;
    uint32_t sample_count;
    uint32_t mip_levels;
};
// 所有字段必须显式指定，无默认值。与 RHI 层 ImageDesc 设计一致，防止遗漏。

// 持久 handle，跨帧稳定（初始化时获取，每帧通过 use_managed_image 转为 RGResourceId）
struct RGManagedHandle {
    uint32_t index = UINT32_MAX;
    bool valid() const { return index != UINT32_MAX; }
};
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

    // --- Managed 资源管理（阶段三引入） ---

    // 设置基准分辨率（Relative 模式的参照）
    void set_reference_resolution(VkExtent2D extent);

    // 注册 managed image（初始化时调用，返回持久 handle）
    RGManagedHandle create_managed_image(const char* debug_name, const RGImageDesc& desc);

    // 每帧使用 managed image（返回当前帧的 RGResourceId）
    // 每帧统一以 UNDEFINED 作为 initial layout，不追踪帧间状态，不插入 final barrier
    RGResourceId use_managed_image(RGManagedHandle handle);

    // 更新 managed 资源描述（desc 变化时销毁旧 backing image 并创建新的，handle 不变）
    void update_managed_desc(RGManagedHandle handle, const RGImageDesc& desc);

    // 获取 managed 资源的 backing image（resize handler 中即时获取新 handle 更新 Set 2 descriptor）
    ImageHandle get_managed_backing_image(RGManagedHandle handle) const;

    // 销毁 managed image（Renderer::destroy() 时调用）
    void destroy_managed_image(RGManagedHandle handle);
};
```

#### CommandBuffer Debug Utils 扩展

```cpp
// Debug utils (VK_EXT_debug_utils)
void begin_debug_label(const std::string& name, std::array<float, 4> color);
void end_debug_label();
```

RG `execute()` 自动为每个 pass 调用 `begin_debug_label(pass_name)` / `end_debug_label()`，在 RenderDoc 和 GPU profiler 中按 pass 名称分组显示。

#### CommandBuffer Compute + Push Descriptor 扩展

```cpp
// Compute dispatch
void dispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);

// Push descriptors (Vulkan 1.4 core, IBL 预计算专用)
// 注意：此方法直接接受 Vulkan 类型参数（VkPipelineLayout、VkWriteDescriptorSet），
// 是项目中上层使用 Vulkan 类型的例外——仅限 IBL 一次性 init compute dispatch 场景。
void push_descriptor_set(VkPipelineLayout layout, uint32_t set,
                         std::span<const VkWriteDescriptorSet> writes) const;
```

---

### Pass 类约定

> 阶段三引入。阶段二直接在 RG lambda 回调中编写渲染逻辑。
> 阶段三所有 pass 统一放在 Layer 2（`passes/`），使用具体类（非虚基类），Renderer 持有具体类型成员。设计决策见 `m1-design-decisions.md`「Pass 类设计」。

每个 Pass 使用具体类（非虚函数），`setup()` 签名因 pass 而异。Attachment format 在 pass 内部硬编码。各 pass 的方法集允许不统一（如 SkyboxPass 无 `on_resize()`），但同功能的方法保持同名（如 `record()`、`destroy()`）。

```cpp
// MSAA 相关 pass（ForwardPass、DepthPrePass）
class ForwardPass {  // 具体类，无基类
public:
    /// 一次性：创建 pipeline，存储服务指针（ctx_, rm_, dm_, sc_）
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
               uint32_t sample_count);

    /// 创建/重建 resolution-dependent 资源（init 时在 setup 后调用，resize 时单独调用）
    void on_resize(uint32_t width, uint32_t height);

    /// MSAA 切换时重建 pipeline
    void on_sample_count_changed(uint32_t sample_count);

    /// 每帧：向 RG 注册资源使用声明 + execute lambda
    void record(RenderGraph& rg, const FrameContext& ctx);

    /// 销毁 pipeline + 私有资源
    void destroy();
};

// 非 MSAA pass（TonemappingPass）— setup() 不接收 sample_count，无 on_sample_count_changed()
// Swapchain 格式由 setup() 传入（物理设备协商结果，非硬编码常量）
class TonemappingPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
               VkFormat swapchain_format);
    void on_resize(uint32_t width, uint32_t height);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void destroy();
};

// 轻量 pass（SkyboxPass）— 无分辨率/MSAA 相关私有资源
class SkyboxPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void destroy();
};
```

#### FrameContext

Renderer 每帧构造，传给各 pass 的 `record()`。携带一帧渲染所需的全部上下文。定义在 `framework/frame_context.h`（Layer 1）。

```cpp
/// 一帧渲染所需的全部上下文（纯每帧数据），由 Renderer 填充，各 pass 按需取用。
/// 长期服务引用（ResourceManager、DescriptorManager 等）不在此处，由 Pass 在 setup() 时存储指针。
/// 随阶段推进扩展。
struct FrameContext {
    // --- RG 资源 ID ---
    RGResourceId swapchain;
    RGResourceId hdr_color;
    RGResourceId depth;
    RGResourceId msaa_color;     // 1x MSAA 时 invalid
    RGResourceId msaa_depth;     // 1x MSAA 时 invalid
    RGResourceId msaa_normal;    // 1x MSAA 时 invalid
    RGResourceId normal;
    RGResourceId shadow_map;     // 阶段四新增，!features.shadows 时 invalid

    // --- 场景数据（非拥有引用） ---
    std::span<const Mesh> meshes;
    std::span<const MaterialInstance> materials;
    const CullResult* cull_result = nullptr;
    std::span<const MeshInstance> mesh_instances;

    // --- 渲染配置（非拥有引用，阶段四新增） ---
    const RenderFeatures* features = nullptr;
    const ShadowConfig* shadow_config = nullptr;

    // --- 帧参数 ---
    uint32_t frame_index = 0;
    uint32_t sample_count = 1;
};
```

#### RenderFeatures（阶段四引入）

Pass 运行时开关的控制结构体。定义在 `framework/scene_data.h`（与其他渲染合同一起）。DebugUI 直接操作 bool 字段，Renderer 据此决定是否调用 pass 的 `record()`。设计决策见 `m1-design-decisions.md`「Pass 运行时开关」。

```cpp
/// 可选渲染效果的运行时开关。DebugUI 操作，Renderer 消费。
/// 随阶段推进扩展。
struct RenderFeatures {
    bool skybox          = true;
    bool shadows         = true;   // 阶段四引入
    bool ssao            = true;   // 阶段五引入
    bool contact_shadows = true;   // 阶段五引入
    // 后处理 flags 随阶段八扩展
};
```

GlobalUBO 对应字段（阶段四引入）：

```cpp
uint32_t feature_flags;   // bitmask，shader 动态分支用
```

Shader 端常量定义在 `bindings.glsl`：

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_SSAO            (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
```

#### ShadowConfig（阶段四引入）

Shadow 系统的运行时可调参数。定义在 `framework/scene_data.h`（与 RenderFeatures 同位）。Application 持有实例，DebugUI 直接操作字段。

```cpp
/// CSM 阴影配置参数。Application 持有，DebugUI 操作，Renderer/ShadowPass 消费。
struct ShadowConfig {
    float split_lambda    = 0.75f;   // PSSM 对数/线性混合 (0=线性, 1=对数)
    float max_distance    = 200.0f;  // cascade 覆盖最远距离 (m)
    float constant_bias   = 0.002f;  // 硬件 depth bias constant factor
    float slope_bias      = 1.5f;    // 硬件 depth bias slope factor
    float normal_offset   = 1.0f;    // shader-side 法线偏移强度
    uint32_t pcf_radius   = 1;       // PCF kernel 半径 (0=off, 1=3×3, 2=5×5, ..., 5=11×11)
    float blend_width     = 0.1f;    // cascade blend region 占 cascade 范围的比例
};
```

#### ShadowPass 接口（阶段四引入）

CSM 阴影 pass。自管理 shadow map 资源（非 RG managed），每帧 import 到 RG。设计决策见 `m1-design-decisions.md`「CSM 阴影系统」。

```cpp
class ShadowPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);

    /// 每帧：import shadow map → add RG pass → 内部循环 cascade 渲染
    void record(RenderGraph& rg, const FrameContext& ctx);

    void destroy();

    /// 运行时 cascade 数量/分辨率变更：销毁旧资源 + 创建新资源 + 重建 per-layer views
    /// 调用者保证 GPU 空闲（Renderer::handle_shadow_config_changed 的 vkQueueWaitIdle 保障）
    void on_shadow_config_changed(uint32_t cascade_count, uint32_t resolution);

    /// 获取 shadow map 的 backing image（Renderer 更新 Set 2 binding 5 用）
    rhi::ImageHandle shadow_map_image() const;
};
```

#### Culling 泛化接口（阶段四 Step 6 引入）

纯几何 frustum 剔除。定义在 `framework/culling.h`。设计决策见 `m1-design-decisions.md`「Per-cascade 剔除与 Culling 模块重构」。

`culling.h` 只包含几何剔除逻辑。材质分桶（opaque/transparent、opaque/mask）和透明排序不属于 culling 模块，由调用方内联——不同消费者的分桶标准不同（camera: opaque vs transparent，shadow: opaque vs mask）。

```cpp
/// 6 平面 frustum（从 view-projection 矩阵提取，正交/透视均适用）
struct Frustum {
    std::array<glm::vec4, 6> planes;  // ax+by+cz+d=0, (a,b,c) 朝 frustum 内部
};

/// 从 VP 矩阵提取 frustum 6 平面
Frustum extract_frustum(const glm::mat4& view_projection);

/// AABB-frustum 剔除，通过测试的实例索引写入 out_visible。
/// out_visible 由调用方持有并跨帧复用（clear + push_back，首帧后零分配）。
void cull_against_frustum(
    std::span<const MeshInstance> instances,
    const Frustum& frustum,
    std::vector<uint32_t>& out_visible);
```

---

### Layer 3 — Renderer 接口（app/renderer.h）

> 阶段三引入。Application 与 Renderer 的所有权划分见 `m1-design-decisions.md`「App 层设计」。

#### RenderInput

Application 每帧传递给 Renderer 的语义数据（不含 GPU 布局知识）：

```cpp
struct RenderInput {
    uint32_t image_index;
    uint32_t frame_index;
    const framework::Camera& camera;
    std::span<const framework::DirectionalLight> lights;
    const framework::CullResult& cull_result;
    std::span<const framework::Mesh> meshes;
    std::span<const framework::MaterialInstance> materials;
    std::span<const framework::MeshInstance> mesh_instances;
    float ibl_intensity;
    float exposure;
    float ibl_rotation_sin;
    float ibl_rotation_cos;
    // --- 阶段四新增 ---
    const framework::RenderFeatures& features;
    const framework::ShadowConfig& shadow_config;
};
```

#### Renderer

```cpp
class Renderer {
public:
    /// hdr_env_path: .hdr 环境贴图路径（传递给内部 IBL 初始化，生成 irradiance / prefiltered cubemap）
    void init(rhi::Context& ctx, rhi::Swapchain& swapchain,
              rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
              framework::ImGuiBackend& imgui,
              const std::string& hdr_env_path);

    /// 填充 GPU buffers（UBO/SSBO），构建 RG，执行所有 pass。
    void render(rhi::CommandBuffer& cmd, const RenderInput& input);

    /// Resize 两阶段：在 swapchain_.recreate() 之前调用。
    void on_swapchain_invalidated();
    /// Resize 两阶段：在 swapchain_.recreate() 之后调用。
    void on_swapchain_recreated();

    void destroy();

    /// MSAA 运行时切换：等待 GPU 空闲，创建/销毁/更新 MSAA managed 资源，重建 pipeline。
    void handle_msaa_change(uint32_t new_sample_count);

    /// 当前 MSAA 采样数（1 = 无 MSAA）。
    uint32_t current_sample_count() const;

    /// Shadow 运行时配置变更：等待 GPU 空闲，ShadowPass 重建资源，更新 Set 2。（阶段四新增）
    void handle_shadow_config_changed(uint32_t new_cascade_count, uint32_t new_resolution);

    // --- 场景加载用 accessor ---
    rhi::SamplerHandle default_sampler() const;
    const framework::DefaultTextures& default_textures() const;
    framework::MaterialSystem& material_system();
};
```

命名空间：`himalaya::app`。文件位置：`app/include/himalaya/app/renderer.h` + `app/src/renderer.cpp`。

#### SceneLoader 场景数据（app/scene_loader.h）

SceneLoader 加载 glTF 场景后暴露场景级元数据。设计决策见 `m1-design-decisions.md`「Shadow Max Distance 初始化」。

```cpp
class SceneLoader {
public:
    // ... 已有接口（场景加载、mesh/material 数据访问等）...

    /// 场景 AABB（所有 mesh instance 的 world_bounds 求并集），加载完成后可用。
    /// 用途：Application 初始化 ShadowConfig.max_distance。
    const AABB& scene_bounds() const;
};
```

---

### Layer 1 — 材质系统（framework/material_system.h）

#### 材质实例（具体参数值）

> M1 只有标准 PBR 一种着色模型，各 pass 自行持有 pipeline（按 alpha_mode 选择 opaque/mask 变体）。
> M3 引入场景卡通渲染（第二种着色模型）时设计多 pipeline 变体的管理方式。

```cpp
struct MaterialInstance {
    uint32_t template_id;              // 着色模型标识（0 = standard PBR）
    uint32_t buffer_offset;            // GPUMaterialData 数组索引
    AlphaMode alpha_mode;              // pass 路由（opaque / mask / transparent）
    bool double_sided;                 // 控制面剔除
};
```

#### GPU 端材质数据布局（shader 读取）

```cpp
// std430 layout, total 80 bytes, aligned to 16 bytes
struct alignas(16) GPUMaterialData {
    vec4  base_color_factor;           // offset  0  — glTF baseColorFactor (RGBA)
    vec4  emissive_factor;             // offset 16  — xyz = emissiveFactor, w unused

    float metallic_factor;             // offset 32  — glTF metallicFactor
    float roughness_factor;            // offset 36  — glTF roughnessFactor
    float normal_scale;                // offset 40  — glTF normalTexture.scale
    float occlusion_strength;          // offset 44  — glTF occlusionTexture.strength

    uint  base_color_tex;              // offset 48  — bindless index
    uint  emissive_tex;                // offset 52  — bindless index
    uint  metallic_roughness_tex;      // offset 56  — bindless index
    uint  normal_tex;                  // offset 60  — bindless index

    uint  occlusion_tex;               // offset 64  — bindless index
    float alpha_cutoff;                // offset 68  — glTF alphaCutoff (Mask mode)
    uint  alpha_mode;                  // offset 72  — 0=Opaque, 1=Mask, 2=Blend
    uint  _padding;                    // offset 76  — padding to 80 bytes
};
```

> emissive_factor 使用 vec4 而非 vec3 以避免 std430 对齐问题（vec3 对齐 16 字节会引入隐式 padding）。alpha_mode 同时存在于 GPU（shader 判断 discard）和 CPU（MaterialInstance，draw call 路由）两侧。M1 阶段 GPUMaterialData 是定长结构体，以后材质类型增多时可扩展或改为更灵活的布局。

---

### Layer 0 — Immediate Command Scope（rhi/context.h）

场景加载等批量上传场景使用。一次 begin，录制所有传输命令，一次 end 提交并等待完成。

```cpp
// Context 新增方法
class Context {
public:
    // 开始一段 immediate command 录制（纯状态切换，不返回 CommandBuffer）
    void begin_immediate();

    // 提交录制的命令并 vkQueueWaitIdle 等待完成
    void end_immediate();
};
```

`begin_immediate()` 是纯状态切换（设置 scope 标志、reset/begin 内部 command buffer），不返回 `CommandBuffer`。所有命令录制通过 `ResourceManager` 的 `upload_buffer()` / `upload_image()` / `generate_mips()` 方法完成，它们内部直接访问 `Context::immediate_command_buffer` 录制。调用方无需也不应手动录制 immediate command。

`upload_buffer()` 等方法为录制模式：在活跃的 begin/end_immediate scope 内调用时，只录制 copy 命令到内部 command buffer，不自行 submit。staging buffer 由 Context 收集，`end_immediate()` submit + wait 完成后统一销毁。scope 外调用 `upload_buffer()` 会 assert 失败。

---

### Shader 端 — 全局绑定布局（shaders/common/bindings.glsl）

```glsl
// Set 0: 全局 Buffer（每帧更新一次）
// Feature flags 常量（阶段四引入）
#define FEATURE_SHADOWS         (1u << 0)
// #define FEATURE_SSAO         (1u << 1)   // 阶段五
// #define FEATURE_CONTACT_SHADOWS (1u << 2) // 阶段五

// Shadow cascade 常量
#define MAX_SHADOW_CASCADES 4

// Debug render mode 常量（HDR modes < PASSTHROUGH_START, passthrough modes >= PASSTHROUGH_START）
#define DEBUG_MODE_FULL_PBR          0
#define DEBUG_MODE_DIFFUSE_ONLY      1
#define DEBUG_MODE_SPECULAR_ONLY     2
#define DEBUG_MODE_IBL_ONLY          3
#define DEBUG_MODE_PASSTHROUGH_START 4
#define DEBUG_MODE_NORMAL            4
#define DEBUG_MODE_METALLIC          5
#define DEBUG_MODE_ROUGHNESS         6
#define DEBUG_MODE_AO                7
#define DEBUG_MODE_SHADOW_CASCADES   8

layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 view_projection;
    mat4 inv_view_projection;
    vec4 camera_position_and_exposure;      // xyz = position, w = exposure
    vec2 screen_size;
    float time;                             // 程序运行时间（秒）
    uint directional_light_count;           // 活跃方向光数量
    float ibl_intensity;                    // IBL 环境光强度乘数
    uint irradiance_cubemap_index;          // cubemaps[] 下标
    uint prefiltered_cubemap_index;         // cubemaps[] 下标
    uint brdf_lut_index;                    // textures[] 下标
    uint prefiltered_mip_count;             // roughness → mip level 映射
    uint skybox_cubemap_index;              // cubemaps[] 下标（Skybox Pass 天空渲染用）
    float ibl_rotation_sin;                 // IBL 水平旋转 sin(yaw)
    float ibl_rotation_cos;                 // IBL 水平旋转 cos(yaw)
    uint debug_render_mode;                 // DEBUG_MODE_* 常量（见上方定义）
    // --- 阶段四新增 ---
    uint feature_flags;                     // bitmask: FEATURE_SHADOWS 等
    uint shadow_cascade_count;              // 活跃 cascade 数量
    float shadow_normal_offset;             // 法线偏移 bias 强度
    float shadow_texel_size;                // 1.0 / shadow_map_resolution
    float shadow_max_distance;              // cascade 覆盖最远距离
    float shadow_blend_width;               // cascade blend region 占比
    uint shadow_pcf_radius;                 // PCF kernel 半径 (0=off, 1=3×3, ..., 5=11×11)
    mat4 cascade_view_proj[MAX_SHADOW_CASCADES]; // per-cascade 光空间 VP 矩阵
    vec4 cascade_splits;                    // 4 个 cascade 远边界 (view-space depth)
} global;

layout(set = 0, binding = 1) readonly buffer LightBuffer {
    // 每个元素: vec4 direction_and_intensity + vec4 color_and_shadow
    GPUDirectionalLight directional_lights[];
};

layout(set = 0, binding = 2) readonly buffer MaterialBuffer {
    GPUMaterialData materials[];
};

// Set 0, Binding 3: Per-instance 数据（阶段四准备工作引入）
// CpuToGpu per-frame buffer, Renderer 每帧填充
layout(set = 0, binding = 3) readonly buffer InstanceBuffer {
    GPUInstanceData instances[];
};

// Set 1: 持久纹理资产（Bindless 数组）
layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];

// Set 2: Render Target（帧内中间产物，PARTIALLY_BOUND，按阶段逐步写入）
layout(set = 2, binding = 0) uniform sampler2D rt_hdr_color;            // 阶段三
layout(set = 2, binding = 1) uniform sampler2D rt_depth_resolved;       // 阶段五
layout(set = 2, binding = 2) uniform sampler2D rt_normal_resolved;      // 阶段五
layout(set = 2, binding = 3) uniform sampler2D rt_ao_texture;           // 阶段五
layout(set = 2, binding = 4) uniform sampler2D rt_contact_shadow_mask;  // 阶段五
layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map;// 阶段四
layout(set = 2, binding = 6) uniform sampler2D rt_bloom_texture;        // 阶段八
layout(set = 2, binding = 7) uniform sampler2D rt_refraction_source;    // 阶段七

// Per-draw 数据 (4 bytes, 阶段四准备工作缩减：model + material_index 迁移到 InstanceBuffer)
layout(push_constant) uniform PushConstants {
    uint cascade_index;          // shadow.vert 使用，forward/prepass 不 push
};
```

#### Descriptor Set 管理方式

三个 Set 均使用传统 Descriptor Set（非 Push Descriptors），所有 pipeline 共享统一 layout `{Set 0, Set 1, Set 2}`。

- **Set 0**：per-frame 分配 2 个 descriptor set（对应 2 frames in flight），每帧绑定当前帧的 set
- **Set 1**：分配 1 个 descriptor set，加载时写入，长期持有
- **Set 2**：分配 1 个 descriptor set，init 时写入，resize / MSAA 切换时更新

设计决策见 `m1-design-decisions.md`「Descriptor Set 三层架构」+「Set 2 — Render Target Descriptor Set」。

#### 数据分层对应关系

| 数据层 | 更新频率 | 绑定方式 | 内容 |
|--------|---------|----------|------|
| 全局数据 | 每帧一次 | Set 0, Binding 0 (UBO) | 相机矩阵、屏幕尺寸、曝光值 |
| 光源数据 | 每帧一次 | Set 0, Binding 1 (SSBO) | 方向光、点光源数组 |
| 材质数据 | 加载时一次 | Set 0, Binding 2 (SSBO) | PBR 参数、纹理 index |
| 2D 纹理数据 | 加载时一次 | Set 1, Binding 0 (Bindless) | 材质纹理、BRDF LUT、Lightmap |
| Cubemap 数据 | 初始化 / 加载时 | Set 1, Binding 1 (Bindless) | IBL cubemap、Reflection Probes |
| Render Target | init 时 / resize 时 | Set 2, Binding 0-7 (Named) | HDR color、depth、normal、AO、shadow map 等 |
| Per-instance 数据 | 每帧一次（cull 后填充） | Set 0, Binding 3 (SSBO) | 模型矩阵、材质 index（instancing 用）|
| Per-draw 数据 | 每次绘制（仅 shadow） | Push Constant | cascade index |
