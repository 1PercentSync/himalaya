# Milestone 1：接口与目标结构

> 本文档包含 M1 的目标文件结构和关键接口定义。
> 设计决策与理由见 `m1-design-decisions-core.md`，长远架构目标见 `../project/architecture.md`，M1 功能范围见 `milestone-1.md`。

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
│   ├── shader.h                 # 运行时编译（GLSL → SPIR-V）、缓存、热重载
│   ├── acceleration_structure.h # BLAS/TLAS 创建、构建、销毁（阶段六引入）
│   └── rt_pipeline.h            # RT Pipeline 创建、SBT 管理（阶段六引入）
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
│   ├── shadow.h                 # CSM cascade 计算（PSSM 分割、正交投影、texel snapping）
│   ├── imgui_backend.h          # ImGui 集成
│   ├── color_utils.h            # 色温 → 线性 RGB 转换
│   ├── scene_as_builder.h       # 场景加速结构构建器（阶段六引入）
│   ├── cached_shader_compiler.h # 带磁盘缓存的 ShaderCompiler（阶段六 Step 10.5 引入）
│   ├── emissive_light_builder.h # Emissive 面光源采样构建器（阶段六 Step 12 引入）
│   ├── texture_compress.h       # BC6H GPU 压缩通用工具（阶段七，从 IBL 提取）
│   ├── cubemap_filter.h         # Cubemap prefilter 通用工具（阶段七，从 IBL 提取）
│   ├── bake_denoiser.h          # 同步 OIDN 降噪（阶段七引入）
│   ├── lightmap_uv.h            # Lightmap UV 生成 + xatlas 集成 + 缓存（阶段七引入）
│   ├── probe_placement.h       # Probe 自动放置（均匀网格 + RT 几何过滤）（阶段七引入）
│   └── bake_data_manager.h     # Bake 数据管理（扫描/校验/加载/卸载/probe 分配）（阶段八引入）
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
│   ├── gtao_pass.h              # GTAO (AO compute pass)
│   ├── ao_temporal_pass.h       # AO Temporal Filter
│   ├── contact_shadows.h        # Contact Shadows
│   ├── reference_view_pass.h    # PT 参考视图（阶段六引入）
│   ├── lightmap_baker_pass.h    # Lightmap 烘焙器（阶段七引入）
│   ├── probe_baker_pass.h       # Reflection Probe 烘焙器（阶段七引入）
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
│   ├── debug_ui.h               # ImGui 面板、各 Pass 参数调整
│   └── config.h                 # 应用配置持久化（scene/env 路径、HDR Sun 坐标）
└── src/
    ├── main.cpp                 # 入口
    ├── application.cpp
    ├── renderer.cpp             # render() dispatch、fill_common_gpu_data()、accessors
    ├── renderer_init.cpp        # init/destroy/resize/reload、descriptor helpers
    ├── renderer_rasterization.cpp  # 光栅化渲染路径
    ├── renderer_pt.cpp          # PT 渲染路径
    ├── renderer_bake.cpp        # 烘焙渲染路径（阶段七引入）
    ├── scene_loader.cpp
    ├── camera_controller.cpp
    ├── debug_ui.cpp
    └── config.cpp
```

App 层拥有 GLFW 窗口，传 `GLFWwindow*` 给 RHI 创建 Surface。Application 持有 RHI 基础设施和 App 模块，Renderer 持有渲染子系统，详见 `m1-design-decisions-core.md`「App 层设计」。

---

### Shaders

```
shaders/
├── common/                      # 共享头文件 / 函数
│   ├── constants.glsl           # 数学常量（PI、EPSILON 等）
│   ├── brdf.glsl                # BRDF 函数（D_GGX、V_SmithGGX、F_Schlick，include constants）
│   ├── normal.glsl              # TBN 构造、normal map 解码
│   ├── shadow.glsl              # 阴影采样（CSM、PCF、PCSS）
│   └── bindings.glsl            # 全局绑定布局定义
├── depth_prepass.vert           # 共享 VS（invariant gl_Position）
├── depth_prepass.frag           # Opaque FS（无 discard）
├── depth_prepass_masked.frag    # Mask FS（alpha test + discard）
├── forward.vert/frag
├── shadow.vert                  # Shadow VS（position + uv0，cascade_vp 变换）
├── shadow_masked.frag           # Shadow mask FS（alpha test + discard，opaque 无 FS）
├── gtao.comp
├── ao_temporal.comp
├── contact_shadows.comp
├── bloom_downsample.comp
├── bloom_upsample.comp
├── auto_exposure.comp
├── fullscreen.vert              # Fullscreen triangle（无顶点输入，后处理共用）
├── tonemapping.frag             # Tonemapping（ACES，fullscreen fragment）
├── transparent.vert/frag
├── vignette.comp
├── color_grading.comp
├── height_fog.comp
├── rt/                          # 路径追踪 shader（阶段六引入）
│   ├── pt_common.glsl           # PT 核心共享（采样、MIS、Russian Roulette）
│   ├── reference_view.rgen      # 参考视图 raygen shader
│   ├── lightmap_baker.rgen      # Lightmap 烘焙 raygen shader（阶段七）
│   ├── probe_baker.rgen         # Probe 烘焙 raygen shader（阶段七）
│   ├── closesthit.rchit         # 通用 closest-hit shader（材质采样 + NEE）
│   ├── anyhit.rahit             # Alpha test + stochastic alpha（Mask/Blend 几何体）
│   ├── miss.rmiss               # Miss shader（环境光 / IBL 采样）
│   └── shadow_miss.rmiss        # Shadow miss shader（未遮挡标记）
├── bake/                        # 烘焙 shader（阶段七引入）
│   ├── pos_normal_map.vert      # UV 空间光栅化（lightmap UV → NDC，输出 world pos/normal）
│   ├── pos_normal_map.frag      # 写入 position + normal render target
│   └── probe_filter.comp        # Probe 几何过滤（rayQueryEXT，剔除墙内探针）
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

Pipeline 不使用 handle 体系——所有权单一明确（pass 直接持有 `Pipeline` 值类型），详见 `m1-design-decisions-core.md`「资源句柄设计」。

#### 资源创建描述

```cpp
// 所有字段必须显式初始化，无默认值。
// create_image() 通过 assert 拦截 depth/mip_levels/sample_count 为 0 的情况。
// array_layers == 6 自动推断 cubemap（VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT + CUBE view type）。
// array_layers > 1 && != 6 自动推断 2D Array（VK_IMAGE_VIEW_TYPE_2D_ARRAY）。
struct ImageDesc {
    uint32_t width, height;
    uint32_t depth;             // 2D images: must be 1
    uint32_t mip_levels;        // single level: must be 1
    uint32_t array_layers;      // cubemap: 6, regular 2D: 1, shadow map: 4 (MAX_SHADOW_CASCADES)
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
    bool compare_enable;        // true: 启用深度比较（shadow map 用），默认 false
    CompareOp compare_op;       // 比较操作（compare_enable=true 时有效），默认 Never
};
```

#### Format 转换函数（rhi/types.h）

```cpp
// Format 枚举 → VkFormat 映射
VkFormat to_vk_format(Format format);

// VkFormat → Format 枚举反向映射（KTX2 读取时使用，不识别的值返回 Undefined）
Format from_vk_format(VkFormat vk_format);

// Format 枚举 → VkImageAspectFlags 推导（depth → DEPTH_BIT, color → COLOR_BIT）
VkImageAspectFlags aspect_from_format(Format format);

// 格式工具函数
uint32_t format_bytes_per_block(Format format);                   // 每 texel block 字节数（BC=16, RGBA8=4, ...）
std::pair<uint32_t, uint32_t> format_block_extent(Format format); // block texel 维度（BC={4,4}, 其他={1,1}）
bool format_is_block_compressed(Format format);                   // BC 格式返回 true
```

与 `Format` 枚举定义在同一文件中。RG 的 barrier 计算、ResourceManager 的 image 创建、KTX2 读写等处统一使用。设计决策见 `m1-design-decisions-core.md`「KTX2 读写模块」「多级上传 API」。

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

    // Image 上传 — mip 0 only（staging buffer + vkCmdCopyBufferToImage，在 immediate scope 内调用）
    // dst_stage: 最终 barrier 的 dstStageMask（调用方按消费者指定）
    void upload_image(ImageHandle handle, const void* data, uint64_t size,
                      VkPipelineStageFlags2 dst_stage);

    // Image 上传 — 预建 mip chain（2D / cubemap 通用，在 immediate scope 内调用）
    // 单 staging buffer 装全部 mip 数据，多 VkBufferImageCopy2 region 一次提交。
    // cubemap 时 layerCount=6，每级内 face 0..5 连续排列（与 KTX2 布局一致）。
    struct MipUploadRegion {
        uint64_t buffer_offset;  ///< 该 mip 数据在 data 中的起始偏移
        uint32_t width;          ///< 该 mip 的像素宽度
        uint32_t height;         ///< 该 mip 的像素高度
    };
    void upload_image_all_levels(ImageHandle handle,
                                 const void* data, uint64_t total_size,
                                 std::span<const MipUploadRegion> mip_regions,
                                 VkPipelineStageFlags2 dst_stage);

    // GPU 端 mip 生成（逐级 vkCmdBlitImage，在 immediate scope 内调用）
    void generate_mips(ImageHandle handle);

    // --- Sub-resource view 创建（阶段四引入） ---

    // 创建 array image 的单层 2D view（shadow map per-cascade 渲染用）
    // 从 ImageHandle 获取底层 VkImage、format、aspect，创建 baseArrayLayer=layer, layerCount=1 的 2D view
    // 调用方持有返回的 VkImageView，通过 destroy_layer_view() 销毁
    VkImageView create_layer_view(ImageHandle handle, uint32_t layer, const char* debug_name);

    // 销毁 create_layer_view() 创建的 view
    void destroy_layer_view(VkImageView view);
};
```

#### ShaderCompiler 接口

```cpp
class ShaderCompiler {
public:
    virtual ~ShaderCompiler() = default;

    // 配置 #include 解析根目录（初始化时调用一次）
    void set_include_path(const std::string& path);

    // 从文件路径加载并编译 shader（唯一公开编译入口）
    // path 相对于 set_include_path() 设置的根目录，与 FileIncluder 的解析规则一致
    // 例：set_include_path("shaders") 后，compile_from_file("forward.vert", Vertex)
    //     读取 shaders/forward.vert，filename 传 "forward.vert" 给内部 compile()
    // virtual：CachedShaderCompiler override 此方法添加磁盘缓存层
    [[nodiscard]] virtual std::vector<uint32_t> compile_from_file(
        const std::string& path, ShaderStage stage);

protected:
    // 编译 GLSL 源码为 SPIR-V，含内存缓存 + include 追踪
    [[nodiscard]] std::vector<uint32_t> compile(
        const std::string& source, ShaderStage stage, const std::string& filename);

    // 子类编译后取回 include 依赖列表（用于磁盘缓存 .meta 写入）
    const CacheEntry* find_cache_entry(const std::string& source, ShaderStage stage) const;

    // 子类访问 include 根目录
    const std::string& include_path() const;
};
```

#### CachedShaderCompiler 接口（framework 层）

```cpp
// 继承 rhi::ShaderCompiler，在 compile_from_file 前加磁盘缓存层。
// 所有 pass 持有 rhi::ShaderCompiler*，多态自动生效，pass 层零改动。
class CachedShaderCompiler : public rhi::ShaderCompiler {
public:
    // 设置缓存子目录名（如 "shader_debug" / "shader_release"），未设置则不启用磁盘缓存
    void set_cache_category(const std::string& category);

    // 磁盘缓存查找 → fallback 到 compile() → 写回磁盘
    [[nodiscard]] std::vector<uint32_t> compile_from_file(
        const std::string& path, rhi::ShaderStage stage) override;
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

    // Set 2: Render Target 中间产物（2 个 set 对应 2 frames in flight，阶段五扩展）
    VkDescriptorSet get_set2(uint32_t frame_index) const;

    // 返回三个 Set 的 layout（Set 0 + Set 1 + Set 2），用于 graphics pipeline 创建
    std::array<VkDescriptorSetLayout, 3> get_graphics_set_layouts() const;
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
// GlobalUBO — std140 layout, 928 bytes (aligned to 16)
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
    float indirect_intensity;                    // offset 288 — 间接光照强度乘数（Phase 8 从 ibl_intensity 重命名）
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
    float shadow_distance_fade_width;           // offset 624 — 阴影远端衰减区间占 max_distance 的比例
    float _shadow_pad[3];                       // offset 628 — pad to 640 (vec4 alignment)
    glm::vec4 cascade_texel_world_size;         // offset 640 — 预计算每 cascade 的 texel 世界尺寸
    // --- Step 7 PCSS 新增 ---
    uint32_t shadow_mode;                       // offset 656 — 0=PCF, 1=PCSS
    uint32_t pcss_flags;                        // offset 660 — bit 0: PCSS_FLAG_BLOCKER_EARLY_OUT
    uint32_t pcss_blocker_samples;              // offset 664 — blocker search 采样数 (16/32)
    uint32_t pcss_pcf_samples;                  // offset 668 — PCF 采样数 (16/25/49)
    glm::vec4 cascade_light_size_uv;            // offset 672 — per-cascade LIGHT_SIZE_UV (blocker search U 方向半径, 基于 width_x)
    glm::vec4 cascade_pcss_scale;               // offset 688 — per-cascade NDC深度差→UV半影宽度缩放因子 (depth_range * 2tan(θ/2) / width_x, U 方向)
    glm::vec4 cascade_uv_scale_y;              // offset 704 — per-cascade UV 各向异性校正 (width_x / width_y), V 方向乘此比值
    // --- 阶段五新增 ---
    glm::mat4 inv_projection;                  // offset 720 — 深度 → view-space 位置重建 (GTAO)
    glm::mat4 prev_view_projection;            // offset 784 — 时域重投影 (当前世界坐标 → 上一帧 UV)
    uint32_t frame_index;                      // offset 848 — 当前帧索引 (PCSS 时域噪声变化等)
    uint32_t ao_so_mode;                       // offset 852 — 0=Lagarde, 1=GTSO (SO 评估方法)
    uint32_t _phase5_pad[2];                   // offset 856 — pad to 864 (vec4 alignment)
    // --- 阶段六新增 ---
    glm::mat4 inv_view;                        // offset 864 — 逆 view 矩阵（PT raygen primary ray 计算）
};  // total: 928 bytes (58 × 16)

// GPU 方向光 — std430 layout, 32 bytes per element
// 对应 shader: Set 0, Binding 1 (LightBuffer SSBO)
struct alignas(16) GPUDirectionalLight {
    glm::vec4 direction_and_intensity;          // xyz = direction, w = intensity
    glm::vec4 color_and_shadow;                 // xyz = color, w = cast_shadows (0.0 / 1.0)
};

// Per-instance 数据 — std430 layout, 128 bytes per element
// 对应 shader: Set 0, Binding 3 (InstanceBuffer SSBO)
// 阶段四准备工作引入（提前合并原计划阶段六 per-instance SSBO 迁移 + M3 Instancing）
// 未来扩展：M2 的 prev_model (motion vectors)、阶段六的 lightmap UV transform
struct GPUInstanceData {
    glm::mat4 model;                            // 64 bytes — 世界变换矩阵
    glm::vec4 normal_col0;                      // 16 bytes — normal matrix column 0 (xyz, w unused)
    glm::vec4 normal_col1;                      // 16 bytes — normal matrix column 1 (xyz, w unused)
    glm::vec4 normal_col2;                      // 16 bytes — normal matrix column 2 (xyz, w unused)
    uint32_t material_index;                    //  4 bytes — MaterialBuffer SSBO 索引
    uint32_t lightmap_index;                    //  4 bytes — bindless textures[] 索引，UINT32_MAX = 无（阶段八引入）
    uint32_t probe_index;                       //  4 bytes — ProbeBuffer SSBO 索引，UINT32_MAX = 无（阶段八引入）
    uint32_t _padding;                          //  4 bytes — 对齐到 128 bytes (16 的倍数)
};

// Per-draw push constant — 4 bytes
// 对应 shader: push_constant, stage = VERTEX
// 仅 shadow pass 使用，forward / prepass 不 push
struct PushConstantData {
    uint32_t cascade_index;                     //  4 bytes — shadow.vert 使用
};

// Emissive 三角形 — std430 layout, 96 bytes per element（阶段六 Step 12 引入）
// 对应 shader: Set 0, Binding 7 (EmissiveTriangleBuffer SSBO)
struct EmissiveTriangle {
    glm::vec3 v0;                               // offset  0 — 世界空间顶点 (+4B pad)
    glm::vec3 v1;                               // offset 16 (+4B pad)
    glm::vec3 v2;                               // offset 32 (+4B pad)
    glm::vec3 emission;                         // offset 48 — raw emissive_factor
    float area;                                 // offset 60 — 预计算世界空间三角形面积
    uint32_t material_index;                    // offset 64 — MaterialBuffer 索引
    glm::vec2 uv0;                              // offset 72 — 纹理坐标（NEE 采样点插值用）(+4B pad to vec2 align)
    glm::vec2 uv1;                              // offset 80
    glm::vec2 uv2;                              // offset 88
};  // total: 96 bytes

// PT Push Constants — 逐步演进（阶段六 raygen/closesthit/anyhit 共用）
// Step 6:  20B — max_bounces(u32) + sample_count(u32) + frame_seed(u32) + blue_noise_index(u32) + max_clamp(f32)
// Step 11: 28B — + env_sampling(u32) + directional_lights(u32)
// Step 12: 32B — + emissive_light_count(u32)
// Step 13: 36B — + lod_max_level(u32)
// 阶段七:  44B — + lightmap_width(u32) + lightmap_height(u32)
// 阶段七:  60B — + probe_pos_x(f32) + probe_pos_y(f32) + probe_pos_z(f32) + face_index(u32)

// PrimaryPayload — 逐步演进（raygen ↔ closesthit 通信）
// Step 6:  56B — color(12) + next_origin(12) + next_direction(12) + throughput_update(12) + hit_distance(4) + bounce(4)
// Step 11: 60B — + env_mis_weight(4)
// Step 12: 64B — + last_brdf_pdf(4)
// Step 13: 72B — + cone_width(4) + cone_spread(4)

// --- 阶段八引入 ---

// 间接光照模式（Application 层状态）
enum class IndirectLightingMode : uint8_t {
    IBL,             ///< IBL Split-Sum 近似，自由旋转
    LightmapProbe,   ///< 烘焙 lightmap + reflection probe，角度跳切
};

// Probe GPU 数据 — std430 layout, 48 bytes per element
// 对应 shader: Set 0, Binding 9 (ProbeBuffer SSBO，非 RT-only，PARTIALLY_BOUND)
struct GPUProbeData {
    glm::vec3 position;                         // offset  0 — probe 世界空间位置 (+4B pad)
    glm::vec3 aabb_min;                         // offset 16 — 视差校正 AABB 最小角 (+4B pad)（Phase 8.5，Phase 8 填零）
    glm::vec3 aabb_max;                         // offset 32 — 视差校正 AABB 最大角
    uint32_t cubemap_index;                     // offset 44 — bindless cubemaps[] 索引
};  // total: 48 bytes
```

---

### Layer 1 — 缓存模块接口（framework/cache.h）

纹理压缩和 IBL 缓存共用的轻量工具模块。只提供路径和哈希工具，不关心具体缓存格式。设计决策见 `m1-design-decisions-core.md`「缓存基础设施」。

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

    /// 删除指定 category 子目录下的所有缓存文件
    void clear_cache(std::string_view category);

    /// 删除 cache_root() 下的所有缓存文件（所有 category）
    void clear_all_cache();

    /// 原子写入二进制文件（write-to-temp + rename）
    /// 文件要么完整写入，要么不存在（崩溃安全）
    bool atomic_write_file(const std::filesystem::path& path,
                           const void* data, size_t size);
}
```

---

### Layer 1 — KTX2 读写模块接口（framework/ktx2.h）

最小 KTX2 读写模块，纹理缓存和 IBL 缓存共用。不依赖 libktx，只支持 6 种格式的 2D/cubemap + mip chain 读写。设计决策见 `m1-design-decisions-core.md`「KTX2 读写模块」。

```cpp
namespace himalaya::framework {

/// 从 KTX2 文件读取的数据
struct Ktx2Data {
    rhi::Format format;
    uint32_t base_width;
    uint32_t base_height;
    uint32_t face_count;     ///< 1 (2D) or 6 (cubemap)
    uint32_t level_count;

    struct Level {
        uint64_t offset;     ///< Byte offset into blob
        uint64_t size;       ///< Byte size of this level (all faces)
    };
    std::vector<Level> levels;   ///< levels[0] = base, levels[N-1] = smallest mip
    std::vector<uint8_t> blob;   ///< Contiguous mip data (KTX2 metadata stripped)
};

/// 写入 KTX2 时每级的数据描述
struct Ktx2WriteLevel {
    const void* data;        ///< This level's data (cubemap: all 6 faces concatenated)
    uint64_t size;           ///< Byte size
};

/// 写入 KTX2 文件。levels[0] = base level (最大), levels[N-1] = smallest mip。
/// cubemap: 每级的 data 包含 6 faces 连续排列。
bool write_ktx2(const std::filesystem::path& path,
                rhi::Format format,
                uint32_t base_width, uint32_t base_height,
                uint32_t face_count,
                std::span<const Ktx2WriteLevel> levels);

/// 读取 KTX2 文件。返回 nullopt 表示格式不支持或文件损坏。
/// 返回的 blob 仅包含 mip 数据（KTX2 header/DFD/level index 已剥离），levels 的 offset 索引 blob。
std::optional<Ktx2Data> read_ktx2(const std::filesystem::path& path);

}  // namespace himalaya::framework
```

---

### Layer 1 — IBL 模块接口（framework/ibl.h）

IBL 模块自管理全部资源——`init()` 中创建的所有 image 由模块自身持有，`destroy()` 先注销 bindless 条目再销毁底层资源。设计决策见 `m1-design-decisions-core.md`「IBL 资源所有权」。

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

    /// 原始 equirectangular 输入图像的尺寸（HDR Sun 坐标转换用）
    uint32_t equirect_width() const;
    uint32_t equirect_height() const;
};
```

---

### Layer 1 — BakeDataManager 接口（framework/bake_data_manager.h，阶段八引入）

管理烘焙产物的生命周期：扫描缓存目录、完整性校验、KTX2 加载/卸载、bindless 注册/注销、CPU probe-to-instance 分配。Renderer 持有实例。设计决策见 `m1-rt-decisions.md`「Phase 8 间接光照集成决策」。

```cpp
class BakeDataManager {
public:
    /// 已 bake 角度概要信息
    struct AngleInfo {
        uint32_t rotation;       ///< 角度（整数度，0-359）
        uint32_t lightmap_count; ///< 该角度的 lightmap 文件数
        uint32_t probe_count;    ///< 该角度的 probe 文件数
    };

    /// 初始化（接收子系统引用）
    void init(rhi::ResourceManager& rm, rhi::DescriptorManager& dm,
              rhi::SamplerHandle lightmap_sampler,
              rhi::SamplerHandle probe_sampler);

    /// 销毁（卸载当前角度 + 释放内部资源）
    void destroy();

    /// 扫描 cache_root()/bake/ 目录，逐角度完整性校验，构建可用角度列表
    void scan(std::span<const std::string> lightmap_keys,
              const std::string& probe_set_key);

    /// 加载指定角度的 lightmap + probe 数据（KTX2 → GPU → bindless → probe 分配）
    /// 在 begin_immediate() / end_immediate() scope 内执行
    /// bakeable_indices 将 bakeable 索引映射回全 instance 索引（parallel to lightmap_keys）
    void load_angle(uint32_t rotation_int,
                    std::span<const std::string> lightmap_keys,
                    std::span<const uint32_t> bakeable_indices,
                    const std::string& probe_set_key,
                    std::span<const MeshInstance> mesh_instances);

    /// 卸载当前角度（注销 bindless + 销毁 GPU image + 清空 per-instance 数据）
    void unload_angle();

    /// 查询可用角度列表
    std::span<const AngleInfo> available_angles() const;

    /// 是否有可用的 bake 数据
    bool has_bake_data() const;

    /// 是否已加载某个角度
    bool is_loaded() const;

    /// 当前加载的角度
    uint32_t loaded_rotation() const;

    /// 查询 per-instance lightmap bindless index（parallel to mesh_instances，UINT32_MAX = 无）
    std::span<const uint32_t> lightmap_indices() const;

    /// 查询 per-instance probe index（parallel to mesh_instances，UINT32_MAX = 无）
    std::span<const uint32_t> probe_indices() const;
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
    // temporal=true 时内部分配第二张 backing image，clear() 自动 swap current/history
    RGManagedHandle create_managed_image(const char* debug_name, const RGImageDesc& desc,
                                         bool temporal);

    // 每帧使用 managed image（返回当前帧的 RGResourceId）
    // initial layout 统一为 UNDEFINED（不追踪帧间状态）
    // final_layout 由调用方显式指定：
    //   UNDEFINED = 不插入帧末 barrier（每帧完全覆写的非 temporal 资源）
    //   SHADER_READ_ONLY_OPTIMAL = 帧末 transition（temporal current，确保 swap 后 history layout 正确）
    RGResourceId use_managed_image(RGManagedHandle handle, VkImageLayout final_layout);

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
> 阶段三所有 pass 统一放在 Layer 2（`passes/`），使用具体类（非虚基类），Renderer 持有具体类型成员。设计决策见 `m1-design-decisions-core.md`「Pass 类设计」。

每个 Pass 使用具体类（非虚函数），`setup()` 签名因 pass 而异。Attachment format 在 pass 内部硬编码。各 pass 的方法集允许不统一，只保留有实际作用的方法，但同功能的方法保持同名（如 `record()`、`destroy()`、`rebuild_pipelines()`）。

```cpp
// MSAA 相关 pass（ForwardPass、DepthPrePass）
class ForwardPass {  // 具体类，无基类
public:
    /// 一次性：创建 pipeline，存储服务指针（ctx_, rm_, dm_, sc_）
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
               uint32_t sample_count);

    /// MSAA 切换时重建 pipeline
    void on_sample_count_changed(uint32_t sample_count);

    /// 每帧：向 RG 注册资源使用声明 + execute lambda
    void record(RenderGraph& rg, const FrameContext& ctx);

    /// 重编译 shader 重建 pipeline（热重载）
    void rebuild_pipelines();

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
    void record(RenderGraph& rg, const FrameContext& ctx);
    void rebuild_pipelines();
    void destroy();
};

// 轻量 pass（SkyboxPass）— 无分辨率/MSAA 相关私有资源
class SkyboxPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void rebuild_pipelines();
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

    // --- Shadow draw groups（阶段四新增，Renderer 从全部 mesh_instances 构建） ---
    std::span<const MeshDrawGroup> shadow_opaque_groups;
    std::span<const MeshDrawGroup> shadow_mask_groups;

    // --- 渲染配置（非拥有引用，阶段四新增） ---
    const RenderFeatures* features = nullptr;
    const ShadowConfig* shadow_config = nullptr;

    // --- 帧参数 ---
    uint32_t frame_index = 0;
    uint32_t sample_count = 1;
};
```

#### RenderFeatures（阶段四引入）

Pass 运行时开关的控制结构体。定义在 `framework/scene_data.h`（与其他渲染合同一起）。DebugUI 直接操作 bool 字段，Renderer 据此决定是否调用 pass 的 `record()`。设计决策见 `m1-design-decisions-core.md`「Pass 运行时开关」。

```cpp
/// 可选渲染效果的运行时开关。DebugUI 操作，Renderer 消费。
/// 无默认值——Application 使用 designated initializers 显式初始化。
/// 随阶段推进扩展。
struct RenderFeatures {
    bool skybox;                     // 阶段四引入
    bool shadows;                    // 阶段四引入
    bool ao;                         // 阶段五引入
    bool contact_shadows;            // 阶段五引入
    bool lightmap_probe = false;     // 阶段八引入 — Lightmap/Probe 间接光照模式激活
    // 后处理 flags 随阶段十扩展
};
```

GlobalUBO 对应字段（阶段四引入）：

```cpp
uint32_t feature_flags;   // bitmask，shader 动态分支用
```

Shader 端常量定义在 `bindings.glsl`：

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
#define FEATURE_LIGHTMAP_PROBE  (1u << 3)   // 阶段八引入
```

#### ShadowConfig（阶段四引入）

Shadow 系统的运行时可调参数。定义在 `framework/scene_data.h`（与 RenderFeatures 同位）。Application 持有实例，DebugUI 直接操作字段。

```cpp
/// CSM 阴影配置参数。Application 持有，DebugUI 操作，Renderer/ShadowPass 消费。
/// 无默认值——Application 使用 designated initializers 显式初始化。
struct ShadowConfig {
    uint32_t cascade_count;    // 活跃 cascade 数量 (1-4)
    float split_lambda;        // PSSM 对数/线性混合 (0=线性, 1=对数)
    float max_distance;        // cascade 覆盖最远距离 (m)
    float slope_bias;          // 硬件 depth bias slope factor (D32Sfloat; constant bias 无效已移除)
    float normal_offset;       // shader-side 法线偏移强度
    uint32_t pcf_radius;       // PCF kernel 半径 (0=off, 1=3×3, 2=5×5, ..., 5=11×11)
    float blend_width;         // cascade blend region 占 cascade 范围的比例
    float distance_fade_width; // 阴影远端衰减区间占 max_distance 的比例（独立于 blend_width）
    // --- Step 7 PCSS 新增 ---
    uint32_t shadow_mode;          // 0=PCF, 1=PCSS
    float light_angular_diameter;  // 光源角直径 (弧度)，默认 0.00925 ≈ 0.53° (太阳)
    uint32_t pcss_flags;           // bit 0: blocker early-out (全 blocker 直接返回 0.0)
    uint32_t pcss_quality;         // 0=Low(16+16), 1=Medium(16+25), 2=High(32+49)
};
```

#### ShadowPass 接口（阶段四引入）

CSM 阴影 pass。自管理 shadow map 资源（非 RG managed），每帧 import 到 RG。设计决策见 `m1-design-decisions-core.md`「CSM 阴影系统」。

```cpp
class ShadowPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);

    /// 每帧：import shadow map → add RG pass → 内部循环 cascade 渲染
    void record(RenderGraph& rg, const FrameContext& ctx);

    void destroy();

    /// 运行时分辨率变更：销毁旧资源 + 创建新资源（固定 4 层）+ 重建 per-layer views
    /// cascade 数量为纯渲染参数，不触发资源重建
    /// 调用者保证 GPU 空闲（Renderer::handle_shadow_resolution_changed 的 vkQueueWaitIdle 保障）
    void on_resolution_changed(uint32_t resolution);

    /// 获取 shadow map 的 backing image（Renderer 更新 Set 2 binding 5 + binding 6 用）
    rhi::ImageHandle shadow_map_image() const;
};
```

#### Culling 泛化接口（阶段四 Step 6 引入）

纯几何 frustum 剔除。定义在 `framework/culling.h`。设计决策见 `m1-design-decisions-core.md`「Per-cascade 剔除与 Culling 模块重构」。

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

> 阶段三引入。Application 与 Renderer 的所有权划分见 `m1-design-decisions-core.md`「App 层设计」。

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
    float indirect_intensity;
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

    /// Shadow 分辨率运行时变更：等待 GPU 空闲，ShadowPass 重建资源，更新 Set 2。（阶段四新增）
    /// Cascade 数量为纯渲染参数，通过 ShadowConfig 读取，不需要专门 handler。
    void handle_shadow_resolution_changed(uint32_t new_resolution);

    // --- 场景加载用 accessor ---
    rhi::SamplerHandle default_sampler() const;
    const framework::DefaultTextures& default_textures() const;
    framework::MaterialSystem& material_system();

    // --- 阶段六 Step 9 降噪 accessor ---

    /// Returns the timeline semaphore + signal value for the current frame's
    /// denoise readback, or {VK_NULL_HANDLE, 0} if no denoise is pending.
    /// Application checks after render() and appends to vkQueueSubmit2
    /// signalSemaphoreInfos if non-null.
    struct DenoiseSemaphoreInfo {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint64_t value = 0;
    };
    [[nodiscard]] DenoiseSemaphoreInfo pending_denoise_signal() const;
};
```

命名空间：`himalaya::app`。文件位置：`app/include/himalaya/app/renderer.h` + `app/src/renderer.cpp`（dispatch + GPU data fill）、`renderer_init.cpp`（lifecycle）、`renderer_rasterization.cpp`（光栅化路径）、`renderer_pt.cpp`（PT 路径）。

#### SceneLoader 场景数据（app/scene_loader.h）

SceneLoader 加载 glTF 场景后暴露场景级元数据。设计决策见 `m1-design-decisions-core.md`「Shadow Max Distance 初始化」。

```cpp
class SceneLoader {
public:
    // ... 已有接口（场景加载、mesh/material 数据访问等）...

    /// 场景 AABB（所有 mesh instance 的 world_bounds 求并集），加载完成后可用。
    /// 用途：Application 初始化 ShadowConfig.max_distance 和相机初始位置/朝向。
    const AABB& scene_bounds() const;
};
```

#### Camera 场景聚焦（framework/camera.h）

Camera 新增纯计算方法，用于场景加载自动定位和 F 键 focus。设计决策见 `m1-design-decisions-core.md`「相机自动定位与 F 键 Focus」。

```cpp
struct Camera {
    // ... 已有接口 ...

    /// 计算能将给定 AABB 完整纳入视野的相机位置。
    /// 使用当前 yaw、pitch、fov，不修改相机状态。
    /// 退化 AABB（diagonal ≈ 0）时返回当前 position 不变。
    [[nodiscard]] glm::vec3 compute_focus_position(const AABB& bounds) const;
};
```

#### CameraController F 键 Focus（app/camera_controller.h）

CameraController 新增 focus target 指针，检测 F 键按下时自动定位。

```cpp
class CameraController {
public:
    // ... 已有接口 ...

    /// 设置 F 键 focus 的目标 AABB（nullptr = focus 禁用）。
    /// Application 在场景加载后调用，指向 SceneLoader::scene_bounds() 返回值。
    void set_focus_target(const AABB* bounds);

private:
    const AABB* focus_target_ = nullptr;
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
    uint  double_sided;                // offset 76  — glTF doubleSided（Phase 6 Step 12 引入，RT 背面穿透判定用）
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

`begin_immediate()` 是纯状态切换（设置 scope 标志、reset/begin 内部 command buffer），不返回 `CommandBuffer`。所有命令录制通过 `ResourceManager` 的 `upload_buffer()` / `upload_image()` / `upload_image_all_levels()` / `generate_mips()` 方法完成，它们内部直接访问 `Context::immediate_command_buffer` 录制。调用方无需也不应手动录制 immediate command。

`upload_buffer()` 等方法为录制模式：在活跃的 begin/end_immediate scope 内调用时，只录制 copy 命令到内部 command buffer，不自行 submit。staging buffer 由 Context 收集，`end_immediate()` submit + wait 完成后统一销毁。scope 外调用 `upload_buffer()` 会 assert 失败。

### Layer 0 — Debug Naming（rhi/context.h）

`VK_EXT_debug_utils` 对象命名由 Context 统一提供，所有 RHI 模块（ResourceManager、AccelerationStructureManager 等）通过 `context->set_debug_name()` 调用。

```cpp
class Context {
public:
    /// 为 Vulkan 对象设置调试名称（RenderDoc / Validation Layer 可见）。
    /// debug_utils 不可用时静默跳过。
    void set_debug_name(VkObjectType type, uint64_t handle, const char* name) const;
};
```

函数指针 `vkSetDebugUtilsObjectNameEXT` 在 `Context::init()` 中通过 `vkGetInstanceProcAddr` 加载并缓存。

---

### Shader 端 — 全局绑定布局（shaders/common/bindings.glsl）

```glsl
// Set 0: 全局 Buffer（每帧更新一次）
// Feature flags 常量（阶段四引入）
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
#define FEATURE_LIGHTMAP_PROBE  (1u << 3)   // 阶段八引入

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
    float shadow_distance_fade_width;       // 阴影远端衰减区间占 max_distance 的比例
    // pad to vec4 alignment
    vec4 cascade_texel_world_size;          // 预计算每 cascade 的 texel 世界尺寸
    // --- Step 7 PCSS 新增 ---
    uint shadow_mode;                       // 0=PCF, 1=PCSS
    uint pcss_flags;                        // bit 0: PCSS_FLAG_BLOCKER_EARLY_OUT
    uint pcss_blocker_samples;              // blocker search 采样数 (16/32)
    uint pcss_pcf_samples;                  // PCF 采样数 (16/25/49)
    vec4 cascade_light_size_uv;             // per-cascade LIGHT_SIZE_UV (blocker search U 方向半径, 基于 width_x)
    vec4 cascade_pcss_scale;                // per-cascade NDC深度差→UV半影宽度缩放因子 (U 方向)
    vec4 cascade_uv_scale_y;               // per-cascade UV 各向异性校正 (width_x / width_y)
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

// Set 0, Binding 4: TLAS（阶段六引入，RT shader 专用，光栅化 shader 不引用）
layout(set = 0, binding = 4) uniform accelerationStructureEXT scene_tlas;

// Set 0, Binding 5: 几何信息（阶段六引入，closest-hit shader 查询顶点/材质数据）
// GeometryInfo struct 在 Step 5/6 实现时定义（涉及 buffer_reference / uint64_t device address 编码选择）
layout(set = 0, binding = 5) readonly buffer GeometryInfoBuffer {
    GeometryInfo geometry_infos[];  // per-geometry: vertex/index buffer address + material_id
    // Indexed by: gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
};

// Set 0, Binding 6: Env Alias Table（阶段六 Step 11 引入，env importance sampling 用）
// EnvAliasEntry struct 定义在 bindings.glsl HIMALAYA_RT 区域内
layout(set = 0, binding = 6) readonly buffer EnvAliasTable {
    float total_luminance;
    uint  entry_count;
    EnvAliasEntry entries[];
};

#endif // HIMALAYA_RT

// Set 1: 持久纹理资产（Bindless 数组）
layout(set = 1, binding = 0) uniform sampler2D textures[];
layout(set = 1, binding = 1) uniform samplerCube cubemaps[];

// Set 2: Render Target（帧内中间产物，PARTIALLY_BOUND，按阶段逐步写入）
layout(set = 2, binding = 0) uniform sampler2D rt_hdr_color;            // 阶段三
layout(set = 2, binding = 1) uniform sampler2D rt_depth_resolved;       // 阶段五
layout(set = 2, binding = 2) uniform sampler2D rt_normal_resolved;      // 阶段五
layout(set = 2, binding = 3) uniform sampler2D rt_ao_texture;           // 阶段五
layout(set = 2, binding = 4) uniform sampler2D rt_contact_shadow_mask;  // 阶段五
layout(set = 2, binding = 5) uniform sampler2DArrayShadow rt_shadow_map;      // 阶段四 — 比较采样器 (PCF)
layout(set = 2, binding = 6) uniform sampler2DArray rt_shadow_map_depth;     // 阶段四 Step 7 — 深度读取 (PCSS blocker search)

// Per-draw 数据 (4 bytes, 阶段四准备工作缩减：model + material_index 迁移到 InstanceBuffer)
layout(push_constant) uniform PushConstants {
    uint cascade_index;          // shadow.vert 使用，forward/prepass 不 push
};
```

#### Descriptor Set 管理方式

Graphics pipeline 共享统一 layout `{Set 0, Set 1, Set 2}`。Compute pipeline 扩展为 `{Set 0, Set 1, Set 2, Set 3(push)}`。

- **Set 0**：per-frame 分配 2 个 descriptor set（对应 2 frames in flight），每帧绑定当前帧的 set
- **Set 1**：分配 1 个 descriptor set，加载时写入，长期持有
- **Set 2**：per-frame 分配 2 个 descriptor set（阶段五引入，对应 2 frames in flight），temporal binding 每帧更新当前帧的 copy
- **Set 3**：push descriptor set（阶段五引入，compute / RT pipeline），每次 dispatch 前 push 绑定（storage image 输出 + pass-specific 输入）

设计决策见 `m1-design-decisions-core.md`「Descriptor Set 三层架构」+「Set 2 — Render Target Descriptor Set」。

#### 数据分层对应关系

| 数据层 | 更新频率 | 绑定方式 | 内容 |
|--------|---------|----------|------|
| 全局数据 | 每帧一次 | Set 0, Binding 0 (UBO) | 相机矩阵、屏幕尺寸、曝光值 |
| 光源数据 | 每帧一次 | Set 0, Binding 1 (SSBO) | 方向光、点光源数组 |
| 材质数据 | 加载时一次 | Set 0, Binding 2 (SSBO) | PBR 参数、纹理 index |
| 2D 纹理数据 | 加载时一次 | Set 1, Binding 0 (Bindless) | 材质纹理、BRDF LUT |
| Cubemap 数据 | 初始化 / 加载时 | Set 1, Binding 1 (Bindless) | IBL cubemap |
| Render Target | init 时 / resize 时 + temporal 每帧 | Set 2, Binding 0-6 (Named, per-frame) | HDR color、depth、normal、AO、shadow map 等 |
| Per-instance 数据 | 每帧一次（cull 后填充） | Set 0, Binding 3 (SSBO) | 模型矩阵、材质 index（instancing 用）|
| TLAS | 场景加载时 | Set 0, Binding 4 (AS) | 场景加速结构（阶段六引入，RT shader 专用） |
| 几何信息 | 场景加载时 | Set 0, Binding 5 (SSBO) | per-geometry vertex/index address + material（阶段六引入） |
| Env Alias Table | IBL 加载时 | Set 0, Binding 6 (SSBO) | env importance sampling alias table + total_luminance（阶段六 Step 11 引入） |
| Per-draw 数据 | 每次绘制（仅 shadow） | Push Constant | cascade index |
| Sobol 方向数表 | init 时一次 | Set 3, Binding 3 (SSBO) | 128 维低差异序列方向数（阶段六 Step 6a 引入，ReferenceViewPass Set 3） |
| Compute / RT pass 私有 I/O | 每次 dispatch | Set 3 (Push Descriptor) | storage image 输出、pass-specific 输入（阶段五引入，阶段六 RT pass 复用） |

---

### 阶段五新增接口

#### RG Temporal API（阶段五引入）

RG managed image 支持 temporal 标记，内部管理 double buffer 和帧间 swap。设计决策见 `m1-design-decisions-core.md`「Temporal 基础设施」。

```cpp
// create_managed_image 新增 temporal 参数（无默认值，调用方显式传入）
RGManagedHandle create_managed_image(const char* debug_name,
                                      const RGImageDesc& desc,
                                      bool temporal);

// 获取上一帧的 resource ID（仅 temporal=true 的 managed image 可调用）
// 始终返回 valid RGResourceId（两张 backing image 在 create 时已分配）
// 首帧 / resize 后 history 内容未定义，通过 is_history_valid() 查询有效性
RGResourceId get_history_image(RGManagedHandle handle);

// 查询 history 内容是否有效（首帧 / resize 后返回 false）
// 调用方据此设置 temporal blend factor（无效时 blend_factor = 0）
bool is_history_valid(RGManagedHandle handle) const;
```

- `temporal=true` 时 RG 内部分配第二张 backing image（`history_backing`）
- `clear()` 时自动 swap current/history
- Resize 时重建两张，标记 history 无效
- `use_managed_image(handle, final_layout)` 返回当前帧写入目标（initial_layout = UNDEFINED）。Temporal current 调用方传 `final_layout = SHADER_READ_ONLY_OPTIMAL`（帧末 transition，确保 swap 后 history layout 正确）；非 temporal 传 `UNDEFINED`（不插入帧末 barrier）
- `get_history_image()` 始终返回 valid RGResourceId（history 有效时 initial_layout = SHADER_READ_ONLY_OPTIMAL，无效时 initial_layout = UNDEFINED）

#### Per-frame-in-flight Set 2（阶段五引入）

Set 2 从 1 份扩展为 2 份（对应 2 frames in flight），解决 temporal binding 的帧间竞争。设计决策见 `m1-design-decisions-core.md`「Per-frame-in-flight Set 2」。

```cpp
class DescriptorManager {
public:
    // Set 2 getter 变为 per-frame（阶段五变更）
    VkDescriptorSet get_set2(uint32_t frame_index) const;

    // 更新 Set 2 render target binding — 写入两份 copy（init/resize/MSAA 切换用）
    void update_render_target(uint32_t binding, ImageHandle image, SamplerHandle sampler) const;

    // 更新 Set 2 render target binding — 写入指定帧的 copy（temporal binding 每帧更新用）
    void update_render_target(uint32_t frame_index, uint32_t binding,
                              ImageHandle image, SamplerHandle sampler) const;

    // 返回 compute / RT pipeline 的 descriptor set layouts
    // = {set0_layout, set1_layout, set2_layout, set3_push_layout}
    // 封装 Set 0-2 全局 layout + 调用方提供的 Set 3 push descriptor layout
    std::vector<VkDescriptorSetLayout> get_dispatch_set_layouts(
        VkDescriptorSetLayout set3_push_layout) const;

    // ... 其余接口不变 ...
};
```

#### CommandBuffer Compute Helpers（阶段五引入）

Compute pass 绑定基础设施。`bind_compute_descriptor_sets` 与 `bind_descriptor_sets` 对称，使用 `VK_PIPELINE_BIND_POINT_COMPUTE`，用于 compute pass 绑定 Set 0-2 预分配全局 descriptor sets。Push descriptor helpers 封装 `vkCmdPushDescriptorSet`，Pass 层不接触 `VkWriteDescriptorSet` 等 Vulkan 类型。显式传 `ResourceManager&` 用于 `ImageHandle → VkImageView` 解析，保持 CommandBuffer 作为纯 `VkCommandBuffer` wrapper。设计决策见 `m1-design-decisions-core.md`「Compute Pass 绑定机制」。

```cpp
class CommandBuffer {
public:
    // ... 已有接口 ...

    /// Bind pre-allocated descriptor sets to the compute pipeline (Set 0-2).
    void bind_compute_descriptor_sets(VkPipelineLayout layout, uint32_t first_set,
                                      const VkDescriptorSet* sets, uint32_t count);

    /// Push a storage image binding for compute output.
    void push_storage_image(const ResourceManager& rm, VkPipelineLayout layout,
                            uint32_t set, uint32_t binding, ImageHandle image);

    /// Push a sampled image binding for compute input.
    void push_sampled_image(const ResourceManager& rm, VkPipelineLayout layout,
                            uint32_t set, uint32_t binding, ImageHandle image,
                            SamplerHandle sampler);
};
```

#### Compute Pipeline（阶段三 IBL 引入，阶段五 per-frame compute 复用）

```cpp
struct ComputePipelineDesc {
    VkShaderModule compute_shader = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayout> descriptor_set_layouts;
    std::vector<VkPushConstantRange> push_constant_ranges;
};

Pipeline create_compute_pipeline(VkDevice device, const ComputePipelineDesc& desc);
```

两种使用模式：
- **IBL init compute**（阶段三）：完全自定义 `descriptor_set_layouts`（push descriptor Set 0），不使用全局 Set
- **Per-frame compute / RT**（阶段五起）：通过 `dm.get_dispatch_set_layouts(set3_push)` 获取 `{Set 0, Set 1, Set 2, Set 3(push)}`，Set 0-2 与 graphics 共享，Set 3 per-pass 自定义

#### AOConfig（阶段五引入）

AO 特性的运行时配置。定义在 `framework/scene_data.h`。Application 持有实例，DebugUI 操作。

```cpp
struct AOConfig {
    float radius;              // 采样半径 (world-space meters)
    uint32_t directions;       // 搜索方向数 (2/4/8)
    uint32_t steps_per_dir;    // 每方向步数 (2/4/8)
    float bias;                // depth 比较偏移
    float intensity;           // AO 强度乘数
    float temporal_blend;      // history 混合因子 (0.0-1.0)
    bool use_gtso;             // true=GTSO (bent normal), false=Lagarde 近似
};
```

#### ContactShadowConfig（阶段五引入）

Contact Shadows 特性的运行时配置。定义在 `framework/scene_data.h`。

```cpp
struct ContactShadowConfig {
    uint32_t step_count;       // ray march 步数 (8/16/24/32)
    float max_distance;        // 最大搜索距离 (world-space meters)
    float base_thickness;      // 深度自适应 thickness 的基础值
};
```

#### RenderFeatures 扩展（阶段五）

```cpp
struct RenderFeatures {
    bool skybox;               // 阶段四引入
    bool shadows;              // 阶段四引入
    bool ao;                   // 阶段五引入
    bool contact_shadows;      // 阶段五引入
};
```

#### feature_flags 扩展（bindings.glsl，阶段五）

```glsl
#define FEATURE_SHADOWS         (1u << 0)
#define FEATURE_AO              (1u << 1)
#define FEATURE_CONTACT_SHADOWS (1u << 2)
#define FEATURE_LIGHTMAP_PROBE  (1u << 3)   // 阶段八引入
```

#### GlobalUniformData 扩展（阶段五）

```cpp
struct GlobalUniformData {
    // ... 阶段四已有字段 (720 bytes) ...

    // --- 阶段五新增 ---
    glm::mat4 inv_projection;            // depth → view-space position 重建
    glm::mat4 prev_view_projection;      // temporal reprojection
};
```

#### FrameContext 扩展（阶段五）

```cpp
struct FrameContext {
    // ... 阶段四已有字段 ...

    // --- 阶段五新增 ---
    RGResourceId depth_prev;             // 上一帧 resolved depth (temporal history)
    RGResourceId roughness;              // DepthPrePass roughness 输出 (R8, resolved)
    RGResourceId msaa_roughness;         // MSAA roughness; invalid when sample_count == 1
    RGResourceId ao_noisy;               // GTAO 原始输出 (RGBA8: RGB=bent normal, A=AO)
    RGResourceId ao_filtered;            // AO Temporal 滤波后 (RGBA8, Set 2 binding 3)
    RGResourceId contact_shadow_mask;    // Contact Shadow mask (R8, Set 2 binding 4)

    const AOConfig* ao_config = nullptr;
    const ContactShadowConfig* contact_shadow_config = nullptr;

    bool ao_history_valid = false;         // Renderer 查询 is_history_valid(managed_ao_filtered_)
};
```

#### Set 2 Layout 扩展（阶段五）

| Binding | 类型 | 名称 | 引入阶段 |
|---------|------|------|---------|
| 0 | `sampler2D` | hdr_color | 三 |
| 1 | `sampler2D` | depth_resolved | **五** |
| 2 | `sampler2D` | normal_resolved | **五** |
| 3 | `sampler2D` | ao_texture | **五** |
| 4 | `sampler2D` | contact_shadow_mask | **五** |
| 5 | `sampler2DArrayShadow` | shadow_map | 四 |
| 6 | `sampler2DArray` | shadow_map_depth | 四 |

bindings.glsl 阶段五新增：

```glsl
layout(set = 2, binding = 1) uniform sampler2D rt_depth_resolved;
layout(set = 2, binding = 2) uniform sampler2D rt_normal_resolved;
layout(set = 2, binding = 3) uniform sampler2D rt_ao_texture;
layout(set = 2, binding = 4) uniform sampler2D rt_contact_shadow_mask;
```

#### GTAOPass 接口（阶段五引入）

```cpp
class GTAOPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void rebuild_pipelines();
    void destroy();
};
```

#### AOTemporalPass 接口（阶段五引入）

```cpp
class AOTemporalPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void rebuild_pipelines();
    void destroy();
};
```

**Set 3 push descriptor layout（4 bindings）**：

| Binding | 类型 | 资源 | Sampler |
|---------|------|------|---------|
| 0 | `image2D` (storage, rgba8) | ao_filtered (output) | — |
| 1 | `sampler2D` (sampled) | ao_blurred (input) | nearest_clamp |
| 2 | `sampler2D` (sampled) | ao_history (input) | nearest_clamp |
| 3 | `sampler2D` (sampled) | depth_prev (input) | nearest_clamp |

Push constants: `float temporal_blend`（Renderer 根据 `ao_history_valid` 和 `ao_config->temporal_blend` 设置，无效时传 0.0）。

#### ContactShadowsPass 接口（阶段五引入）

```cpp
class ContactShadowsPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);
    void record(RenderGraph& rg, const FrameContext& ctx);
    void rebuild_pipelines();
    void destroy();
};
```

#### DepthPrePass 扩展（阶段五）

```cpp
class DepthPrePass {
public:
    // ... 已有接口不变 ...

    // 阶段五新增：roughness 输出的 managed image handle
    // setup() 内部创建 R8 roughness managed image
    // record() 声明 roughness 为额外 color attachment 输出
    // on_sample_count_changed() 同步更新 roughness sample count
};
```

#### 色温工具函数（阶段五 Step 10c 引入）

定义在 `framework/color_utils.h`。纯函数，无状态。

```cpp
namespace himalaya::framework {
    /// Kelvin → 线性空间 RGB。范围 2000K~12000K，6500K ≈ (1,1,1)。
    /// 基于 CIE 色彩匹配函数的分段多项式近似。
    glm::vec3 color_temperature_to_rgb(float kelvin);
}
```

#### AppConfig 扩展（阶段五 Step 10c）

```cpp
struct AppConfig {
    std::string scene_path;
    std::string env_path;

    /// HDR 文件路径 → 太阳像素坐标 (x, y) 映射。
    /// key 为 HDR 绝对路径（与 env_path 格式一致）。
    std::unordered_map<std::string, std::pair<int, int>> hdr_sun_coords;
};
```

#### LightSourceMode 扩展（阶段五 Step 10c）

```cpp
enum class LightSourceMode : uint8_t {
    Scene,    ///< glTF 场景方向光
    Fallback, ///< 用户手动 yaw/pitch 方向光
    HdrSun,   ///< HDR 太阳坐标推导方向，随 IBL 旋转同步
    None,     ///< 无方向光（仅 IBL）
};
```

#### DebugUIContext 扩展（阶段五 Step 10c）

```cpp
struct DebugUIContext {
    // ... 已有字段 ...

    // --- Step 10c: HDR Sun ---
    int& hdr_sun_x;                ///< HDR 太阳 X 像素坐标（mutable）
    int& hdr_sun_y;                ///< HDR 太阳 Y 像素坐标（mutable）
    float& hdr_sun_intensity;      ///< HDR Sun 光强（mutable）
    float& hdr_sun_color_temp;     ///< HDR Sun 色温 Kelvin（mutable）
    bool& hdr_sun_cast_shadows;    ///< HDR Sun 投影开关（mutable）
    bool has_hdr;                  ///< 是否有已加载的 HDR 环境

    // --- Step 10c: Fallback 色温 ---
    float& fallback_color_temp;    ///< Fallback 光色温 Kelvin（mutable）
};
```

---

### 阶段六新增/变更接口

#### Mesh 扩展（阶段六）

```cpp
struct Mesh {
    rhi::BufferHandle vertex_buffer;
    rhi::BufferHandle index_buffer;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t group_id = 0;      ///< glTF source mesh index (multi-geometry BLAS 分组依据)
    uint32_t material_id = 0;   ///< material_instances[] 索引 (primitive 固有属性)
};
```

`group_id`：来自同一 glTF mesh 的 primitive 共享此值。SceneASBuilder 按此分组构建 multi-geometry BLAS。光栅化路径不读此字段。

`material_id`：与 `MeshInstance.material_id` 值相同（冗余但解耦 RT 和光栅化路径）。SceneASBuilder 直接读取构建 Geometry Info，无需反查 MeshInstance。

#### SceneLoader 扩展（阶段六）

```cpp
class SceneLoader {
public:
    /// rt_supported: true 时 vertex/index buffer 额外加 ShaderDeviceAddress flag
    bool load(const std::string& path,
              rhi::ResourceManager& resource_manager,
              rhi::DescriptorManager& descriptor_manager,
              framework::MaterialSystem& material_system,
              const framework::DefaultTextures& default_textures,
              rhi::SamplerHandle default_sampler,
              bool rt_supported);  // 阶段六新增
};
```

#### BufferUsage 扩展（阶段六）

```cpp
enum class BufferUsage : uint32_t {
    // ... 已有值 ...
    ShaderDeviceAddress = 1 << 6,  ///< VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
};
```

#### ResourceManager 扩展（阶段六）

```cpp
class ResourceManager {
public:
    // ... 已有接口 ...

    /// 获取 buffer 的 device address（需要 ShaderDeviceAddress usage flag）。
    [[nodiscard]] VkDeviceAddress get_buffer_device_address(BufferHandle handle) const;
};
```

#### AccelerationStructureManager（阶段六）

```cpp
namespace himalaya::rhi {

/// BLAS 句柄（持有 VkAccelerationStructureKHR + backing buffer）
struct BLASHandle {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

/// TLAS 句柄（持有 VkAccelerationStructureKHR + backing buffer）
struct TLASHandle {
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

/// 单个 geometry 的构建输入（对应 BLAS 内一个三角形集合）
struct BLASGeometry {
    VkDeviceAddress vertex_buffer_address;
    VkDeviceAddress index_buffer_address;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertex_stride;  ///< sizeof(Vertex)
    bool opaque;             ///< true → OPAQUE_BIT (skip any-hit), false → NO_DUPLICATE_ANY_HIT_INVOCATION_BIT
};

/// BLAS 构建输入（1..N geometries，支持 multi-geometry BLAS）
struct BLASBuildInfo {
    std::span<const BLASGeometry> geometries;
};

/// 加速结构资源管理：BLAS/TLAS 创建、构建、销毁。
/// 顶点格式硬编码：position = R32G32B32_SFLOAT offset 0，index = UINT32。
class AccelerationStructureManager {
public:
    void init(Context* context);

    /// 批量构建 BLAS（单次 vkCmdBuildAccelerationStructuresKHR 并行构建全部）。
    /// PREFER_FAST_TRACE，每个 BLASBuildInfo 生成一个 BLAS。
    /// 内部分配 scratch buffer（各 BLAS 独立区域），构建完成后释放。
    /// 在 immediate command scope 内调用。
    [[nodiscard]] std::vector<BLASHandle> build_blas(std::span<const BLASBuildInfo> infos);

    /// 构建 TLAS。在 immediate command scope 内调用。
    [[nodiscard]] TLASHandle build_tlas(std::span<const VkAccelerationStructureInstanceKHR> instances);

    void destroy_blas(BLASHandle& handle);
    void destroy_tlas(TLASHandle& handle);

    void destroy();

private:
    Context* context_ = nullptr;
};

}  // namespace himalaya::rhi
```

#### RT Pipeline（阶段六）

```cpp
namespace himalaya::rhi {

/// RT pipeline 创建描述
struct RTPipelineDesc {
    VkShaderModule raygen = VK_NULL_HANDLE;
    VkShaderModule miss = VK_NULL_HANDLE;        ///< 环境 miss
    VkShaderModule shadow_miss = VK_NULL_HANDLE; ///< shadow miss
    VkShaderModule closesthit = VK_NULL_HANDLE;
    VkShaderModule anyhit = VK_NULL_HANDLE;      ///< alpha test + stochastic alpha (VK_NULL_HANDLE = no any-hit)
    uint32_t max_recursion_depth = 1;
    std::span<const VkDescriptorSetLayout> descriptor_set_layouts;
    std::span<const VkPushConstantRange> push_constant_ranges;
};

/// RT pipeline + SBT（生命周期绑定）
struct RTPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;

    /// SBT buffer（raygen + miss + hit regions）
    VkBuffer sbt_buffer = VK_NULL_HANDLE;
    VmaAllocation sbt_allocation = VK_NULL_HANDLE;

    /// SBT region 信息（trace_rays 命令使用）
    VkStridedDeviceAddressRegionKHR raygen_region{};
    VkStridedDeviceAddressRegionKHR miss_region{};
    VkStridedDeviceAddressRegionKHR hit_region{};
    VkStridedDeviceAddressRegionKHR callable_region{};  ///< 空（不使用 callable shader）

    void destroy(VkDevice device, VmaAllocator allocator);
};

/// 创建 RT pipeline + 构建 SBT。
/// Context 提供 device、allocator、RT 属性和动态加载的 RT 函数指针。
[[nodiscard]] RTPipeline create_rt_pipeline(const Context& ctx, const RTPipelineDesc& desc);

}  // namespace himalaya::rhi
```

#### CommandBuffer RT 扩展（阶段六）

与 compute 对称的 RT 绑定方法，使用 `VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR`。Step 7 ReferenceViewPass 录制时使用，避免通过 `handle()` 逃逸到原始 Vulkan。

```cpp
class CommandBuffer {
public:
    // ... 已有接口 ...

    /// 绑定 RT pipeline。
    void bind_rt_pipeline(const RTPipeline& rt_pipeline) const;

    /// 绑定预分配 descriptor sets 到 RT pipeline（Set 0-2）。
    void bind_rt_descriptor_sets(VkPipelineLayout layout, uint32_t first_set,
                                 const VkDescriptorSet* sets, uint32_t count) const;

    /// Push descriptors 到 RT pipeline（Set 3 per-pass I/O）。
    void push_rt_descriptor_set(VkPipelineLayout layout, uint32_t set,
                                std::span<const VkWriteDescriptorSet> writes) const;

    /// 录制 ray trace dispatch。
    void trace_rays(const RTPipeline& rt_pipeline, uint32_t width, uint32_t height) const;
};
```

#### GPU GeometryInfo 结构体（阶段六）

定义在 `framework/scene_data.h`，C++ 和 GLSL 端布局必须一致。

```cpp
/// Per-geometry GPU data (Set 0, Binding 5 SSBO element).
/// std430 layout, 24 bytes per element, aligned to 8.
/// Indexed by: gl_InstanceCustomIndexEXT + gl_GeometryIndexEXT
struct GPUGeometryInfo {
    uint64_t vertex_buffer_address;   ///< offset  0 — Vertex[] device address
    uint64_t index_buffer_address;    ///< offset  8 — uint32_t[] device address
    uint32_t material_buffer_offset;  ///< offset 16 — MaterialBuffer SSBO 索引
    uint32_t _padding;                ///< offset 20 — align to 24 (struct alignment = 8)
};
static_assert(sizeof(GPUGeometryInfo) == 24, "GPUGeometryInfo must be 24 bytes (std430)");
static_assert(alignof(GPUGeometryInfo) == 8);
static_assert(offsetof(GPUGeometryInfo, index_buffer_address) == 8);
static_assert(offsetof(GPUGeometryInfo, material_buffer_offset) == 16);
```

GLSL 端（`shaders/common/bindings.glsl`）：

```glsl
struct GeometryInfo {
    uint64_t vertex_buffer_address;
    uint64_t index_buffer_address;
    uint     material_buffer_offset;
    uint     _padding;
};

layout(set = 0, binding = 5) readonly buffer GeometryInfoBuffer {
    GeometryInfo geometry_infos[];
};
```

#### SceneASBuilder（阶段六）

```cpp
namespace himalaya::framework {

class SceneASBuilder {
public:
    /// 从场景数据构建 BLAS + TLAS + Geometry Info SSBO。
    /// 按 Mesh::group_id 分组构建 multi-geometry BLAS。
    /// 按 (group_id, transform) 去重构建 TLAS instances。
    /// 前提：mesh_instances 中同一 node 的 primitive 连续排列
    ///       （SceneLoader::build_mesh_instances() 保证）。
    /// materials 参数用于解析 Mesh::material_id → MaterialInstance::buffer_offset。
    /// 重复调用时自动 destroy 旧资源再重建（同 MaterialSystem::upload_materials() 模式）。
    void build(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::AccelerationStructureManager& as_mgr,
               std::span<const Mesh> meshes,
               std::span<const MeshInstance> instances,
               std::span<const MaterialInstance> materials);

    void destroy();

    [[nodiscard]] const rhi::TLASHandle& tlas_handle() const;
    [[nodiscard]] rhi::BufferHandle geometry_info_buffer() const;

private:
    std::vector<rhi::BLASHandle> blas_handles_;
    rhi::TLASHandle tlas_handle_{};
    rhi::BufferHandle geometry_info_buffer_{};
    rhi::ResourceManager* resource_manager_ = nullptr;         ///< destroy() 时释放 geometry_info_buffer_
    rhi::AccelerationStructureManager* as_mgr_ = nullptr;      ///< destroy() 时释放 BLAS/TLAS
};

}  // namespace himalaya::framework
```

#### EmissiveLightBuilder（阶段六 Step 12）

```cpp
namespace himalaya::framework {

class EmissiveLightBuilder {
public:
    /// 从场景数据构建 emissive 三角形列表 + alias table。
    /// 遍历 meshes，识别 emissive_factor > 0 的 primitive，
    /// 收集世界空间顶点、面积、功率，构建 power-weighted alias table。
    /// 无 emissive 三角形时跳过构建（emissive_count() 返回 0）。
    void build(rhi::Context& ctx, rhi::ResourceManager& rm,
               std::span<const Mesh> meshes,
               std::span<const MeshInstance> instances,
               std::span<const MaterialInstance> materials);

    void destroy();

    [[nodiscard]] uint32_t emissive_count() const;             ///< 0 = 无 emissive
    [[nodiscard]] rhi::BufferHandle triangle_buffer() const;   ///< Set 0 binding 7
    [[nodiscard]] rhi::BufferHandle alias_table_buffer() const;///< Set 0 binding 8

private:
    rhi::BufferHandle triangle_buffer_{};
    rhi::BufferHandle alias_table_buffer_{};
    uint32_t emissive_count_ = 0;
    rhi::ResourceManager* resource_manager_ = nullptr;
};

}  // namespace himalaya::framework
```

#### GlobalUniformData 扩展（阶段六）

```cpp
struct GlobalUniformData {
    // ... 阶段五已有字段 (864 bytes) ...

    // --- 阶段六新增 ---
    glm::mat4 inv_view{};  ///< offset 864 — inverse view matrix (PT raygen primary ray)
};
// total: 928 bytes (58 × 16)
```

bindings.glsl 对应追加：

```glsl
layout(set = 0, binding = 0) uniform GlobalUBO {
    // ... 阶段五已有字段 ...

    // --- 阶段六新增 ---
    mat4 inv_view;  // PT raygen shader 计算 primary ray
} global;
```

#### RenderMode 枚举（阶段六）

定义在 `framework/scene_data.h`。

```cpp
/// 渲染模式选择（Renderer 按此切换渲染路径）。
enum class RenderMode : uint8_t {
    Rasterization,  ///< 完整光栅化管线
    PathTracing,    ///< PT 参考视图
};
```

#### RenderInput 扩展（阶段六）

```cpp
struct RenderInput {
    // ... 已有字段 ...

    RenderMode render_mode;  ///< 阶段六新增：渲染模式选择
};
```

#### ReferenceViewPass（阶段六）

```cpp
namespace himalaya::passes {

class ReferenceViewPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc);

    /// 向 RG 注册 accumulation buffer + RT pass。
    void record(framework::RenderGraph& rg, const framework::FrameContext& ctx);

    /// 重置累积（sample count 归零，下一帧 shader 覆写）。
    void reset_accumulation();

    [[nodiscard]] uint32_t sample_count() const;
    [[nodiscard]] rhi::ImageHandle accumulation_image() const;

    void rebuild_pipelines();
    void on_resize();
    void destroy();

private:
    rhi::RTPipeline rt_pipeline_{};
    rhi::ImageHandle accumulation_image_{};
    uint32_t sample_count_ = 0;
};

}  // namespace himalaya::passes
```

**Set 3 push descriptor layout（4 bindings）**：

| Binding | 类型 | 资源 | 说明 |
|---------|------|------|------|
| 0 | `image2D` (storage, rgba32f) | accumulation buffer (output) | running average 累积 |
| 1 | `image2D` (storage, rgba16f) | aux albedo (output) | OIDN 辅助通道（bounce 0 写入） |
| 2 | `image2D` (storage, rgba16f) | aux normal (output) | OIDN 辅助通道（bounce 0 写入） |
| 3 | SSBO (readonly) | Sobol 方向数表 | 128 维 × 32 bit，16 KB，init 时上传 |

Push constants: `PTPushConstants`（见「PT Push Constants 布局」）。

#### Denoiser（阶段六）

```cpp
namespace himalaya::framework {

/// Denoise pipeline state machine.
enum class DenoiseState : uint8_t {
    Idle,             ///< Ready to accept a new denoise request.
    ReadbackPending,  ///< Request accepted; caller must register RG readback pass this frame.
    Processing,       ///< Background thread running (wait semaphore + OIDN execute).
    UploadPending,    ///< Background thread done; caller must register RG upload pass next frame.
};

/// Async OIDN denoiser — zero main-thread blocking.
///
/// Lifecycle: init() → [request/launch/poll/complete cycle] → destroy().
/// Readback and upload are RG Transfer Passes (caller-registered).
/// OIDN execution runs on a per-request std::jthread background thread.
/// Timeline semaphore synchronizes readback completion → background thread.
///
/// Memory ordering: state_ uses acquire/release semantics to ensure that
/// staging buffer writes (memcpy in background thread) are visible to the
/// main thread when it observes UploadPending. Background thread stores
/// with memory_order_release, main thread loads with memory_order_acquire.
///
/// Error handling: if oidnExecuteFilter() fails, the background thread
/// logs via spdlog::error and sets state_ → Idle (skipping upload).
class Denoiser {
public:
    /// Create OIDN device (GPU preferred, CPU fallback), RT filter,
    /// persistent staging buffers, and timeline semaphore.
    /// Logs device type after commit: GPU backend → info, CPU fallback → warn.
    void init(rhi::Context& ctx, rhi::ResourceManager& rm);

    /// Request a denoise operation. Records the accumulation_generation for
    /// later discard detection. State: Idle → ReadbackPending.
    /// Caller must register the readback copy pass in this frame's RG and
    /// signal the timeline semaphore on submit.
    void request_denoise(uint32_t accumulation_generation);

    /// Launch the background thread. State: ReadbackPending → Processing.
    /// Called in render() before submit — thread vkWaitSemaphores until GPU
    /// finishes readback, so early launch just means a longer wait.
    void launch_processing();

    /// Check if the background thread has finished and the result is still valid.
    /// If UploadPending and generation matches: returns true (caller registers upload pass).
    /// If UploadPending and generation mismatch: discards result, state → Idle, returns false.
    /// If not UploadPending: returns false.
    [[nodiscard]] bool poll_upload_ready(uint32_t current_generation);

    /// Called after the upload pass executes. State: UploadPending → Idle.
    void complete_upload();

    /// Current state (for UI display and trigger guard checks).
    [[nodiscard]] DenoiseState state() const;

    /// Timeline semaphore handle (caller adds to frame submit's signal list).
    [[nodiscard]] VkSemaphore timeline_semaphore() const;

    /// Next signal value for the timeline semaphore (caller uses in VkTimelineSemaphoreSubmitInfo).
    [[nodiscard]] uint64_t next_signal_value() const;

    /// Readback staging buffer handles (caller uses in RG readback copy pass).
    [[nodiscard]] rhi::BufferHandle readback_beauty() const;
    [[nodiscard]] rhi::BufferHandle readback_albedo() const;
    [[nodiscard]] rhi::BufferHandle readback_normal() const;

    /// Upload staging buffer handle (caller uses in RG upload pass).
    [[nodiscard]] rhi::BufferHandle upload_staging() const;

    /// Join background thread + rebuild staging buffers for new resolution.
    /// Forces state → Idle (discards any pending result — resize invalidates
    /// staging buffer sizes, and accumulation reset + generation++ is
    /// guaranteed by the caller).
    void on_resize(uint32_t width, uint32_t height);

    /// Join background thread + release all resources (OIDN device, filter,
    /// staging buffers, timeline semaphore). Forces state → Idle.
    void destroy();

    /// Join background thread + force state → Idle. Called before scene load
    /// (caller also does generation++ via accumulation reset).
    void abort();

private:
    std::jthread thread_;                          ///< Per-request background thread.
    std::atomic<DenoiseState> state_{DenoiseState::Idle}; ///< release/acquire ordering.
    uint32_t trigger_generation_ = 0;              ///< Generation recorded at request time.
    uint64_t semaphore_value_ = 0;                 ///< Monotonically increasing signal value.
    // OIDN resources
    // ... oidn::DeviceRef, oidn::FilterRef, oidn::BufferRef (beauty/albedo/normal/output)

    // Vulkan resources
    VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
    rhi::BufferHandle readback_beauty_;            ///< HOST_VISIBLE, persistent.
    rhi::BufferHandle readback_albedo_;
    rhi::BufferHandle readback_normal_;
    rhi::BufferHandle upload_staging_;              ///< HOST_VISIBLE, persistent.

    rhi::Context* ctx_ = nullptr;
    rhi::ResourceManager* rm_ = nullptr;
};

}  // namespace himalaya::framework
```

#### DescriptorManager 扩展（阶段六）

```cpp
class DescriptorManager {
public:
    // ... 已有接口 ...

    /// 写入 Set 0 binding 4（TLAS descriptor）。仅 rt_supported 时有效。
    void write_set0_tlas(const rhi::TLASHandle& tlas);

    /// 写入 Set 0 binding 6（Env Alias Table SSBO）。仅 rt_supported 时有效。
    void write_set0_env_alias_table(rhi::BufferHandle buffer, uint64_t size);

    /// 写入 Set 0 binding 7（EmissiveTriangleBuffer SSBO）。仅 rt_supported 时有效。Step 12 新增。
    void write_set0_emissive_triangles(rhi::BufferHandle buffer, uint64_t size);

    /// 写入 Set 0 binding 8（EmissiveAliasTable SSBO）。仅 rt_supported 时有效。Step 12 新增。
    void write_set0_emissive_alias_table(rhi::BufferHandle buffer, uint64_t size);

    // RT pipeline 复用 get_dispatch_set_layouts(set3_push_layout)，不再需要独立方法。
};
```

#### IBL 扩展（阶段六 Step 11）

```cpp
class IBL {
public:
    // ... 已有接口 ...

    /// Alias table buffer handle（Set 0 binding 6 写入用）。无 HDR 时 invalid。
    [[nodiscard]] rhi::BufferHandle alias_table_buffer() const;

    /// 环境贴图总亮度（PDF 归一化用）。无 HDR 时 0。
    [[nodiscard]] float total_luminance() const;

    /// Alias table 半分辨率宽度（= equirect_width / 2）。
    [[nodiscard]] uint32_t alias_table_width() const;

    /// Alias table 半分辨率高度（= equirect_height / 2）。
    [[nodiscard]] uint32_t alias_table_height() const;
};
```

#### Env Alias Table GPU 结构体（阶段六 Step 11）

Set 0 binding 6 SSBO 布局。头部嵌入元数据，后跟 entry 数组。

```cpp
/// Single alias table entry (std430, 8 bytes).
struct EnvAliasEntry {
    float prob;       ///< Vose probability threshold [0, 1]
    uint32_t alias;   ///< Alternative pixel index
};
```

GLSL 端（`bindings.glsl`，`#ifdef HIMALAYA_RT` 守卫内）：

```glsl
struct EnvAliasEntry {
    float prob;
    uint  alias;
};

layout(set = 0, binding = 6) readonly buffer EnvAliasTable {
    float total_luminance;      // 环境贴图总亮度（sin(theta) 加权）
    uint  entry_count;          // alias_table_width × alias_table_height
    EnvAliasEntry entries[];
};
```

#### PT Push Constants 布局（阶段六）

```glsl
layout(push_constant) uniform PTPushConstants {
    uint  max_bounces;       // offset  0
    uint  sample_count;      // offset  4
    uint  frame_seed;        // offset  8
    uint  blue_noise_index;  // offset 12 — bindless textures[] index
    float max_clamp;         // offset 16 — firefly clamping threshold (0 = disabled)
    // Step 11: uint env_sampling          // offset 20 (1 = env importance sampling)
    // Step 11: uint directional_lights    // offset 24 (1 = directional lights in PT)
    // Step 12: uint emissive_light_count  // offset 28 (0 = skip NEE emissive)
    // Step 13: uint  lod_max_level         // offset 32 (Ray Cones LOD clamp, default 4)
};  // Step 6: 20 bytes, Step 11: 28 bytes, Step 12: 32 bytes, Step 13: 36 bytes
```

#### PrimaryPayload 布局（阶段六）

```glsl
struct PrimaryPayload {             // location 0
    vec3  color;                    // 本次 bounce 辐射度
    vec3  next_origin;              // 下一条光线起点
    vec3  next_direction;           // 下一条光线方向
    vec3  throughput_update;        // 路径吞吐量乘数（含 Russian Roulette 补偿）
    float hit_distance;             // 命中距离（miss 时 -1 标记终止）
    uint  bounce;                   // 当前 bounce 索引（raygen 设置，OIDN aux bounce 0 判断用）
    // Step 11: float env_mis_weight     // BRDF 采样方向的 env MIS 权重
    // Step 12: float last_brdf_pdf      // 上一个 bounce 的 BRDF PDF（emissive MIS 用）
    // Step 13: float cone_width          // Ray Cone 累积宽度（世界空间）
    // Step 13: float cone_spread         // Ray Cone 扩展角（rad，含曲率修正，非常量）
};  // Step 6: 56B, Step 11: 60B, Step 12: 64B, Step 13: 72B
```

#### DebugUIContext / DebugUIActions PT 扩展（阶段六）

```cpp
struct DebugUIContext {
    // ... 已有字段 ...

    // --- 阶段六新增 ---
    framework::RenderMode& render_mode;         ///< 渲染模式（读写）
    bool rt_supported;                          ///< RT 硬件支持（只读，控制 UI 可见性）
    uint32_t pt_sample_count;                   ///< 当前累积采样数（只读）
    uint32_t& pt_target_samples;                ///< 目标采样数（读写，0 = 无限）
    uint32_t& pt_max_bounces;                   ///< 最大 bounce 数（读写，默认 8，范围 1-32）
    float& pt_max_clamp;                        ///< Firefly clamping 阈值（读写，默认 0.0 = 关闭，OIDN 足够强）
    float pt_elapsed_time;                      ///< 累积耗时秒（只读）
    bool& denoise_enabled;                      ///< 降噪功能开关（读写）
    bool& auto_denoise;                         ///< 自动降噪开关（读写）
    uint32_t& auto_denoise_interval;            ///< 自动降噪间隔（每 N 采样，读写）
    bool& show_denoised;                        ///< 显示降噪/原始切换（读写，false = Show Raw 暂停降噪）
    uint32_t last_denoised_sample_count;        ///< 触发时采样数（只读，0 = 从未降噪）
    framework::DenoiseState denoise_state;      ///< 当前降噪状态（只读，控制 UI 按钮灰掉）
};

struct DebugUIActions {
    // ... 已有字段 ...

    // --- 阶段六新增 ---
    bool pt_reset_requested = false;            ///< 重置累积按钮
    bool pt_denoise_requested = false;          ///< 手动降噪按钮（state != Idle || sample_count == 0 || !denoise_enabled || !show_denoised 时灰掉）
};
```

#### RGStage 扩展（阶段六）

```cpp
enum class RGStage : uint8_t {
    // ... 已有值 ...
    RayTracing,  ///< VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
};
```

---

### 阶段七新增/变更接口

#### RenderMode 扩展（阶段七）

```cpp
enum class RenderMode : uint8_t {
    Rasterization,  ///< 完整光栅化管线
    PathTracing,    ///< PT 参考视图
    Baking,         ///< 烘焙模式（阶段七新增）
};
```

#### BC6H 压缩通用工具（阶段七，从 IBL 提取）

```cpp
namespace himalaya::framework {

/// GPU BC6H 压缩的单个输入项。
struct BC6HCompressInput {
    rhi::ImageHandle* handle;  ///< [in/out] 源 image，压缩后替换为 BC6H 版本
    const char* debug_name;    ///< BC6H image 的 debug 名称
};

/// GPU BC6H 压缩（compute shader dispatch）。
/// 对每个输入 image 的每个 face × mip dispatch bc6h.comp，
/// 创建 BC6H 格式 image 替换原 handle，原 image 推入 deferred 销毁。
/// 支持 cubemap（6 face × N mip）和 2D（1 face × 1 mip）。
/// 源 image 必须在 SHADER_READ_ONLY 布局。
/// 在 immediate command scope 内调用。
void compress_bc6h(rhi::Context& ctx,
                   rhi::ResourceManager& rm,
                   rhi::ShaderCompiler& sc,
                   std::span<const BC6HCompressInput> inputs,
                   std::vector<std::function<void()>>& deferred);

}  // namespace himalaya::framework
```

#### Cubemap Prefilter 通用工具（阶段七，从 IBL 提取）

```cpp
namespace himalaya::framework {

/// 对 cubemap 执行 GGX importance sampling prefilter，生成 multi-mip chain。
/// 对每个 mip level dispatch prefilter.comp（roughness 通过 push constant 传入）。
/// dst_cubemap 必须已创建，带完整 mip chain，GENERAL 或 UNDEFINED 布局。
/// 在 immediate command scope 内调用。
void prefilter_cubemap(rhi::Context& ctx,
                       rhi::ResourceManager& rm,
                       rhi::ShaderCompiler& sc,
                       rhi::ImageHandle src_cubemap,
                       rhi::ImageHandle dst_cubemap,
                       uint32_t mip_count,
                       std::vector<std::function<void()>>& deferred);

}  // namespace himalaya::framework
```

#### BakeDenoiser（阶段七）

```cpp
namespace himalaya::framework {

/// Synchronous OIDN denoiser for offline baking.
///
/// Unlike the async Denoiser (reference view), BakeDenoiser runs
/// synchronously: readback → OIDN execute → return denoised data.
/// No state machine, no background thread, no timeline semaphore.
class BakeDenoiser {
public:
    /// Create OIDN device (GPU preferred, CPU fallback) + HDR filter.
    void init();

    /// Denoise a single HDR image with albedo/normal auxiliary channels.
    /// All buffers are CPU memory (readback results). Output written to
    /// caller-provided buffer. Blocking call.
    /// @param beauty   Input RGBA32F pixel data (w × h × 16 bytes).
    /// @param albedo   Auxiliary albedo R16G16B16A16F (w × h × 8 bytes, nullable).
    /// @param normal   Auxiliary normal R16G16B16A16F (w × h × 8 bytes, nullable).
    /// @param output   Output RGBA32F pixel data (w × h × 16 bytes, caller-allocated).
    /// @param w        Image width in pixels.
    /// @param h        Image height in pixels.
    /// @return true on success, false on OIDN error (logged).
    bool denoise(const void* beauty, const void* albedo, const void* normal,
                 void* output, uint32_t w, uint32_t h);

    void destroy();

private:
    // oidn::DeviceRef, oidn::FilterRef, oidn::BufferRef (beauty/albedo/normal/output)
};

}  // namespace himalaya::framework
```

#### LightmapUVGenerator（阶段七）

```cpp
namespace himalaya::framework {

/// Per-mesh xatlas output (or loaded from cache).
struct LightmapUVResult {
    std::vector<glm::vec2> lightmap_uvs;    ///< Per-vertex lightmap UV coordinates (normalized [0,1]).
    std::vector<uint32_t> new_indices;       ///< New index buffer (topology may change).
    std::vector<uint32_t> vertex_remap;      ///< new_vertex → original_vertex mapping.
};

/// Generates lightmap UV for a mesh using xatlas, or loads from cache.
/// The caller is responsible for skipping this call when the mesh already
/// has TEXCOORD_1 (no topology change needed in that case).
///
/// Cache key: xxHash of mesh vertex positions + indices.
/// Cache hit: load from disk, skip xatlas. Cache miss: run xatlas, save to disk.
[[nodiscard]] LightmapUVResult
generate_lightmap_uv(std::span<const Vertex> vertices,
                     std::span<const uint32_t> indices,
                     const std::string& mesh_hash);

}  // namespace himalaya::framework
```

#### LightmapBakerPass（阶段七）

```cpp
namespace himalaya::passes {

/// Lightmap baker pass — UV-space RT dispatch with per-texel accumulation.
///
/// For each lightmap texel, reads world position/normal from precomputed
/// maps, traces rays via the shared closesthit/miss/anyhit shaders.
/// Accumulates results into an RGBA32F buffer (running average).
class LightmapBakerPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
               rhi::BufferHandle sobol_buffer, uint32_t blue_noise_index);

    /// Record one frame of baking (1 SPP dispatch).
    void record(framework::RenderGraph& rg, const framework::FrameContext& ctx);

    void rebuild_pipelines();
    void destroy();

    void reset_accumulation();
    [[nodiscard]] uint32_t sample_count() const;

private:
    rhi::RTPipeline rt_pipeline_{};
    VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;
    uint32_t sample_count_ = 0;
    uint32_t frame_seed_ = 0;
    // ... PT parameters (max_bounces, env_sampling, etc.)
};

}  // namespace himalaya::passes
```

**LightmapBakerPass Set 3 push descriptor layout（6 bindings）**：

| Binding | 类型 | 资源 | 说明 |
|---------|------|------|------|
| 0 | `image2D` (storage, rgba32f) | accumulation buffer | running average 累积 |
| 1 | `image2D` (storage, rgba16f) | aux albedo | OIDN 辅助通道（closesthit bounce 0 写入） |
| 2 | `image2D` (storage, rgba16f) | aux normal | OIDN 辅助通道（closesthit bounce 0 写入） |
| 3 | SSBO (readonly) | Sobol 方向数表 | 共享，与 reference view 相同 |
| 4 | `sampler2D` (nearest, clamp) | position map | UV 空间 world position（RGBA32F, alpha=覆盖标记） |
| 5 | `sampler2D` (nearest, clamp) | normal map | UV 空间 world normal（RGBA32F） |

#### ProbeBakerPass（阶段七）

```cpp
namespace himalaya::passes {

/// Reflection probe baker pass — cubemap-space RT dispatch.
///
/// For each probe, dispatches 6-face RT tracing from probe position,
/// accumulates into an RGBA32F cubemap. After target SPP reached,
/// OIDN denoise → prefilter mip chain → BC6H compress → KTX2 persist.
class ProbeBakerPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::DescriptorManager& dm, rhi::ShaderCompiler& sc,
               rhi::BufferHandle sobol_buffer, uint32_t blue_noise_index);

    /// Record one frame of probe baking (1 SPP dispatch, 6 faces).
    void record(framework::RenderGraph& rg, const framework::FrameContext& ctx);

    void rebuild_pipelines();
    void destroy();

    void reset_accumulation();
    [[nodiscard]] uint32_t sample_count() const;

private:
    rhi::RTPipeline rt_pipeline_{};
    VkDescriptorSetLayout set3_layout_ = VK_NULL_HANDLE;
    uint32_t sample_count_ = 0;
    uint32_t frame_seed_ = 0;
};

}  // namespace himalaya::passes
```

**ProbeBakerPass Set 3 push descriptor layout（per-dispatch，4 bindings）**：

| Binding | 类型 | 资源 | 说明 |
|---------|------|------|------|
| 0 | `image2D` (storage, rgba32f) | accumulation face view | cubemap 单层 2D view，per-dispatch 切换 face |
| 1 | `image2D` (storage, rgba16f) | aux albedo (per-face) | closesthit bounce 0 写入 |
| 2 | `image2D` (storage, rgba16f) | aux normal (per-face) | closesthit bounce 0 写入 |
| 3 | SSBO (readonly) | Sobol 方向数表 | 共享 |

每帧 6 次 dispatch，每次切换 binding 0/1/2 到对应 face 的 image/view。

#### PosNormalMapPass（阶段七）

Lightmap baker 预处理阶段。光栅化 pass 将 mesh 在 lightmap UV 空间渲染，输出世界空间 position 和 normal 两张 RGBA32F render target。

```cpp
namespace himalaya::passes {

/// UV-space rasterization pass for lightmap position/normal map generation.
///
/// Maps lightmap UV1 to NDC, outputs world-space position (alpha=1.0
/// coverage marker) and normal to two RGBA32F render targets.
/// Used once per mesh instance before baker RT dispatch.
class PosNormalMapPass {
public:
    void setup(rhi::Context& ctx, rhi::ResourceManager& rm,
               rhi::ShaderCompiler& sc);

    /// Record a single draw call for one mesh instance.
    void record(rhi::CommandBuffer& cmd,
                const framework::Mesh& mesh,
                const glm::mat4& model,
                const glm::mat3& normal_matrix,
                rhi::ImageHandle position_map,
                rhi::ImageHandle normal_map,
                uint32_t width, uint32_t height);

    void rebuild_pipelines();
    void destroy();

private:
    rhi::Context* ctx_ = nullptr;
    rhi::ResourceManager* rm_ = nullptr;
    rhi::ShaderCompiler* sc_ = nullptr;
    rhi::Pipeline pipeline_{};
};

}  // namespace himalaya::passes
```

独立 Pass 类，Renderer 持有实例，与其他 Pass 统一管理。Push constant: `mat4 model` + `mat3 normal_matrix`（112 字节，无 descriptor set）。

#### PT Push Constants 扩展（阶段七）

```glsl
layout(push_constant) uniform PushConstants {
    // ---- 共享字段（阶段六已有，closesthit + raygen 共用） ----
    uint  max_bounces;          // 0
    uint  sample_count;         // 4
    uint  frame_seed;           // 8
    uint  blue_noise_index;     // 12
    float max_clamp;            // 16
    uint  env_sampling;         // 20
    uint  directional_lights;   // 24
    uint  emissive_light_count; // 28
    uint  lod_max_level;        // 32
    // ---- 阶段七新增（baker 专属，raygen 读取，closesthit 不读） ----
    uint  lightmap_width;       // 36 — lightmap baker: position/normal map 尺寸
    uint  lightmap_height;      // 40
    float probe_pos_x;          // 44 — probe baker: 世界空间位置
    float probe_pos_y;          // 48
    float probe_pos_z;          // 52
    uint  face_index;           // 56 — probe baker: 当前 cubemap face (0-5)，lightmap/reference view 忽略
};  // 60 bytes
```

超集布局：reference view、lightmap baker、probe baker 三个 RT pipeline 共用同一个 struct。各 raygen 只读自己需要的字段。Closesthit 不需要 `baker_mode`——aux imageStore 无条件执行，各 pipeline 通过 Set 3 push descriptor 绑定各自的 aux image。
