# 当前阶段：M1 阶段七 — PT 烘焙器

> 目标：实现 GPU 路径追踪烘焙器，能烘焙 per-instance Lightmap 和自动放置的 Reflection Probe，输出 BC6H KTX2 文件。
> 烘焙过程中可在 ImGui 中观察 UV 空间的累积进度。
>
> RT 架构决策见 `milestone-1/m1-rt-decisions.md`，关键接口见 `milestone-1/m1-interfaces.md`。

---

## 实现步骤

### 依赖关系

```
Step 1: BC6H 压缩通用工具提取（从 IBL 重构为 framework 工具）
    ↓
Step 2: Cubemap prefilter 通用工具提取（从 IBL 重构为 framework 工具）
    ↓
Step 3: xatlas vcpkg 集成 + Lightmap UV 生成器（生成 + 缓存 + 检测 TEXCOORD_1）
    ↓
Step 4: Lightmap UV 拓扑应用（xatlas 输出替换 Mesh VB/IB + BLAS 重建）
    ↓
Step 5: BakeDenoiser（同步 OIDN 降噪）
    ↓
Step 6: PT Push Constants 扩展（baker 专属字段）
    ↓
Step 7: Position/Normal Map 光栅化 pass
    ↓
Step 8: Lightmap Baker Pass（raygen shader + RT pipeline + accumulation）
    ↓
Step 9: 烘焙模式渲染路径 + Lightmap 端到端流程
    ↓
Step 9.5: 审查修复（bug 修复 + aux 正确性 + finalize 管线）
    ↓
Step 10: Probe 自动放置
    ↓
Step 11: Probe Baker Pass（raygen shader + RT pipeline + cubemap accumulation）
    ↓
Step 12: Probe 端到端流程（降噪 + prefilter + 压缩 + 持久化）
    ↓
Step 13: ImGui 烘焙控制面板
```

### 总览

| Step | 主题 | 验证标准 |
|------|------|----------|
| 1 | BC6H 压缩工具提取 | IBL 初始化行为不变（BC6H cubemap 正确生成），无 validation 报错 |
| 2 | Cubemap prefilter 工具提取 | IBL prefiltered cubemap 与重构前 bit-identical（KTX2 缓存 hash 不变） |
| 3 | xatlas + Lightmap UV 生成器 | 无 TEXCOORD_1 的 mesh：xatlas 生成 lightmap UV + 缓存写入；有 TEXCOORD_1 的 mesh：跳过 xatlas |
| 4 | Lightmap UV 拓扑应用 | 场景加载后 Mesh::uv1 包含 lightmap UV，BLAS/TLAS 正确重建，光栅化和 PT 参考视图渲染无回归 |
| 5 | BakeDenoiser | 单元测试或手动验证：输入噪声 HDR buffer → 输出降噪结果，OIDN 无报错 |
| 6 | Push Constants 扩展 + 默认值 | RT shader 编译通过（push constant 60B 超集），reference view 行为不变（max_clamp 默认 0.0） |
| 7 | Position/Normal Map pass | RenderDoc：position map 和 normal map 在 UV 空间正确渲染（非零 texel 覆盖三角形区域） |
| 8 | Lightmap Baker Pass | RenderDoc：accumulation buffer 逐帧亮度增加，UV 空间可见光照分布 |
| 9 | 烘焙模式 + Lightmap 端到端 | 触发烘焙 → accumulation 全屏显示（居中保持宽高比，经 tonemapping）→ 累积 → 降噪 → BC6H → KTX2 写入磁盘，文件可被 read_ktx2() 正确加载 |
| 9.5 | 审查修复 | 无 validation 报错，参考视图行为不变，bake finalize 输出 KTX2，RenderDoc 确认 aux 包含正确的 texel albedo/normal |
| 10 | Probe 自动放置 | 日志输出 probe 数量 + 位置列表，墙内探针被正确剔除 |
| 11 | Probe Baker Pass | RenderDoc：probe accumulation cubemap 逐帧亮度增加 |
| 12 | Probe 端到端 | 触发烘焙 → 累积 → 降噪 → prefilter → BC6H → KTX2 写入磁盘 |
| 13 | ImGui 烘焙面板 | 烘焙参数可调，触发/取消/进度显示正常，accumulation 预览实时更新 |

---

### Step 1：BC6H 压缩通用工具提取

从 `IBL::compress_cubemaps_bc6h()` 提取为 framework 层独立工具。

- 新增 `framework/texture_compress.h`：`compress_bc6h()` 函数声明
- 新增 `framework/texture_compress.cpp`：从 `ibl_compress.cpp` 迁移核心逻辑（compile bc6h.comp → create pipeline → per face×mip dispatch → SSBO → copy to BC6H image），泛化签名接受 `std::span<const BC6HCompressInput>`
- `ibl_compress.cpp`：`IBL::compress_cubemaps_bc6h()` 改为调用 `compress_bc6h()` 的 thin wrapper（或直接删除，IBL::init() 直接调用新工具）
- `framework/CMakeLists.txt`：添加 `texture_compress.cpp` 源文件

**验证**：IBL 初始化后 skybox 和 prefiltered cubemap 正确显示（BC6H 压缩行为不变），无 validation 报错

#### 设计要点

`compress_bc6h()` 接受 `rhi::Context&`、`rhi::ResourceManager&`、`rhi::ShaderCompiler&`、输入列表、deferred cleanup。内部共享 pipeline + sampler + staging buffer（与原 IBL 实现相同）。2D 纹理（face_count=1, mip_count=1）自然走 face=0, mip=0 的单次 dispatch。

---

### Step 2：Cubemap Prefilter 通用工具提取

从 `IBL::compute_prefiltered()` 提取为 framework 层独立工具。

- 新增 `framework/cubemap_filter.h`：`prefilter_cubemap()` 函数声明
- 新增 `framework/cubemap_filter.cpp`：从 `ibl_compute.cpp` 迁移 prefilter dispatch 逻辑（compile prefilter.comp → per-mip dispatch with roughness push constant → barrier），泛化签名接受 src/dst cubemap handle + mip count
- `ibl_compute.cpp`：`IBL::compute_prefiltered()` 改为调用 `prefilter_cubemap()`
- `framework/CMakeLists.txt`：添加 `cubemap_filter.cpp` 源文件

**验证**：IBL prefiltered cubemap 与重构前一致（KTX2 缓存 hash 不变表明输出相同），PBR 镜面反射无视觉差异

#### 设计要点

`prefilter_cubemap()` 接受源 cubemap（SHADER_READ_ONLY）、目标 cubemap（已创建，带完整 mip chain）、mip count。内部创建 per-mip image view + push descriptor dispatch。目标 cubemap 的创建（分辨率、格式、mip count）由调用方负责——IBL 和 probe baker 的目标分辨率不同（512 vs 可配置），不应 hardcode。

---

### Step 3：xatlas 集成 + Lightmap UV 生成器

引入 xatlas 依赖，实现 lightmap UV 生成 + 缓存。

- 新增 `third_party/xatlas/`：从 [jpcy/xatlas](https://github.com/jpcy/xatlas) 复制 `xatlas.h` + `xatlas.cpp`（单文件库，MIT 许可）。按 `third_party/` 统一规范集成（自带 CMakeLists.txt，`include/himalaya/xatlas/` 头文件路径）
- `framework/CMakeLists.txt`：`target_link_libraries` 添加 `xatlas`
- 新增 `framework/lightmap_uv.h`：`LightmapUVResult` 结构体 + `generate_lightmap_uv()` 函数声明
- 新增 `framework/lightmap_uv.cpp`：
  - 缓存查找：`cache_path("lightmap_uv", mesh_hash, ".bin")`，命中则加载返回
  - xatlas 调用：`xatlas::Create()` → `xatlas::AddMesh()` → `xatlas::Generate()` → 提取 UV + remap + new indices
  - 缓存写入：header（vertex/index counts）+ lightmap UV 数组 + new index buffer + vertex remap table，write-to-temp + rename 原子写入，文件大小校验

**验证**：xatlas 生成 lightmap UV + 缓存文件写入 + 二次加载命中缓存

#### 设计要点

mesh_hash = `content_hash(vertex positions + indices)`（不含 UV、normal 等——只有几何拓扑决定 UV layout）。xatlas gutter 默认 2（padding texels around UV islands）。`generate_lightmap_uv()` 是纯 CPU 函数，不涉及 GPU 资源。

#### xatlas API 用法

```cpp
// 1. 创建 atlas
xatlas::Atlas *atlas = xatlas::Create();

// 2. 填充 MeshDecl
xatlas::MeshDecl decl;
decl.vertexPositionData = positions_ptr;       // const float* (xyz)
decl.vertexPositionStride = sizeof(Vertex);    // 跨距：整个 Vertex 结构体大小
decl.vertexCount = vertex_count;
decl.indexData = indices_ptr;                  // const uint32_t*
decl.indexCount = index_count;
decl.indexFormat = xatlas::IndexFormat::UInt32; // 项目用 uint32_t 索引

// 3. AddMesh（数据会被拷贝，调用后原数据可释放）
xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
// 检查 error == AddMeshError::Success

// 4. Generate（= ComputeCharts + PackCharts）
xatlas::PackOptions pack;
pack.padding = 2;  // gutter: UV island 间 2 texel 填充
xatlas::Generate(atlas, xatlas::ChartOptions(), pack);

// 5. 提取结果（仅单 mesh，meshes[0]）
const xatlas::Mesh &out = atlas->meshes[0];
// out.vertexCount  — 可能 > 输入顶点数（xatlas 在接缝处拆分顶点）
// out.vertexArray[i].uv[0/1] — 未归一化，范围 [0, atlas->width/height]
// out.vertexArray[i].xref     — 原始顶点索引（即 vertex_remap）
// out.indexArray              — 新索引缓冲
// out.indexCount              — 新索引数量
// atlas->width / atlas->height — atlas 尺寸

// 6. UV 归一化到 [0, 1]
float inv_w = 1.0f / static_cast<float>(atlas->width);
float inv_h = 1.0f / static_cast<float>(atlas->height);
for (uint32_t i = 0; i < out.vertexCount; ++i) {
    uv.x = out.vertexArray[i].uv[0] * inv_w;
    uv.y = out.vertexArray[i].uv[1] * inv_h;
}

// 7. 清理
xatlas::Destroy(atlas);
```

**关键注意点**：
- `vertexPositionStride` 必须正确设置为 `sizeof(framework::Vertex)`（xatlas 按 stride 跳跃读取 position）
- xatlas 输出 UV 是像素坐标（`[0, width]` × `[0, height]`），必须归一化后写入 `LightmapUVResult::lightmap_uvs`
- `Vertex::xref` 直接作为 `LightmapUVResult::vertex_remap`（new_vertex → original_vertex）
- `atlasIndex == -1` 表示顶点未被分配到任何 atlas（退化三角形等），UV 为 `(0, 0)`

---

### Step 4：Lightmap UV 拓扑应用

将 xatlas 输出应用到 Mesh 的 vertex/index buffer。

- `scene_loader.cpp`：在 `load_meshes()` 内部、GPU buffer 创建/上传之前，对每个无 TEXCOORD_1 的 primitive 调用 `generate_lightmap_uv()`（有 TEXCOORD_1 的 mesh 跳过调用，直接使用 uv1）：
  - 计算 mesh_hash：提取 positions (`vec3[]`) + indices (`uint32_t[]`) 拼成临时 buffer → `content_hash()`
  - 返回 LightmapUVResult → 根据 remap table 构建新 vertex 数组（拆分顶点，`uv1` 写入 lightmap UV）+ 新 index 数组 → 替换原 vertices/indices 变量
  - GPU buffer 创建和上传使用替换后的数据（只上传一次，无需 destroy + recreate）
- `scene_loader.cpp`：`cpu_vertices_` / `cpu_indices_` 自然同步（push_back 的是替换后的 vectors）
- 加载顺序确认：glTF parse → vertex/index construction → MikkTSpace tangent → TEXCOORD_1 read → xatlas UV (if needed) → GPU upload → build BLAS/TLAS → build emissive lights
- 注意：lightmap 分辨率计算不在此 step——分辨率在烘焙触发时计算（Step 9），因为参数（texels_per_meter 等）在 UI 中可调

**验证**：场景加载后光栅化渲染无回归（位置/法线/uv0 不变），PT 参考视图无回归，日志输出每个 mesh 的 lightmap UV 来源（TEXCOORD_1 / xatlas / cache）

#### 设计要点

**为什么在 load_meshes() 内部而非之后**：`load_meshes()` 内部 `upload_buffer()` 会将 `vkCmdCopyBuffer` 录制到 immediate command buffer。如果在 `load_meshes()` 之后（`end_immediate()` 之前）调用 `destroy_buffer()` 销毁旧 VB/IB，已录制的拷贝命令将引用已释放的 Vulkan 资源（UB）。在 GPU upload 前应用 xatlas 避免此问题，且只上传一次更高效。

xatlas 拆分顶点后新顶点的 position / normal / uv0 / tangent 从原始顶点复制（remap table 索引），只有 `uv1` 是 xatlas 生成的新值。local AABB 不变（xatlas 不改变 position）。退化 mesh（vertex_count == 0 或 index_count < 3）跳过 xatlas。BLAS 构建在另一个 immediate scope 中完成（`build_scene_rt()`），自然在 xatlas 之后。

---

### Step 5：BakeDenoiser

新建同步 OIDN 降噪模块。

- 新增 `framework/bake_denoiser.h`：BakeDenoiser 类（init / denoise / destroy）
- 新增 `framework/bake_denoiser.cpp`：
  - `init()`：OIDN device 创建（GPU 优先 fallback CPU）+ RT filter 创建（HDR, cleanAux）+ 持久 OIDNBuffer（beauty/albedo/normal/output）
  - `denoise(beauty, albedo, normal, output, w, h)`：memcpy to OIDNBuffer → `oidnExecuteFilter()` → memcpy from OIDNBuffer → 错误检查
  - `destroy()`：释放 OIDN 资源
- `framework/CMakeLists.txt`：添加 `bake_denoiser.cpp`

**验证**：构造 BakeDenoiser，输入一张噪声 HDR buffer（可从 reference view accumulation readback），降噪后输出明显更干净，无 OIDN 报错

#### 设计要点

BakeDenoiser 的 OIDNBuffer 尺寸在 `init()` 时未知——首次 `denoise()` 调用时根据 `w × h` 创建。后续调用若尺寸相同则复用，尺寸变化时释放旧 OIDNBuffer 再创建新的（OIDN buffer 不支持 resize）。与异步 `Denoiser` 完全独立，不共享任何状态。

像素格式与参考视图 Denoiser 一致（baker 和 reference view 共享 closesthit，aux image 格式相同）：beauty/output RGBA32F → `OIDN_FORMAT_FLOAT3` pixelByteStride=16，aux albedo/normal RGBA16F → `OIDN_FORMAT_HALF3` pixelByteStride=8。

---

### Step 6：PT Push Constants 扩展 + 默认值修正

扩展 push constant 超集布局，追加 baker 专属字段。Closesthit 不加条件分支——aux imageStore 保持 Phase 6 原样无条件执行，baker pipeline 绑定自己的 aux image。

- `shaders/rt/pt_common.glsl` 或各 RT shader：PushConstants struct 末尾追加 `uint lightmap_width` + `uint lightmap_height` + `float probe_pos_x/y/z` + `uint face_index`（Phase 6 最终 36B → 60B）。`face_index` = probe cubemap face 0-5（probe baker 用），lightmap/reference view 忽略
- `pt_common.glsl` 新增 `build_orthonormal_basis(vec3 N, out vec3 T, out vec3 B)` 函数（从 closesthit.rchit:392-398 提取，baker raygen 初始射线和 closesthit BRDF 采样共用）
- `renderer.h`：`max_clamp_` 默认值从 10.0f 改为 0.0f（firefly clamping 默认关闭，OIDN 降噪足够强）；`prev_max_clamp_` 同步改为 0.0f
- `passes/reference_view_pass.cpp`：push constant 填充追加 baker 字段为 0（reference view 不读）
- `passes/reference_view_pass.h`：push constant range 更新为 60B

**验证**：全部 RT shader 编译通过，reference view 行为与修改前完全一致（baker 字段为 0，closesthit 无改动）

---

### Step 7：Position/Normal Map 光栅化 pass

UV 空间光栅化预处理。

- 新增 `shaders/bake/pos_normal_map.vert`：`gl_Position = vec4(uv1 * 2.0 - 1.0, 0.0, 1.0)`，输出世界空间 position（`model * in_position`）和 normal（`normalize(normal_matrix * in_normal)`）
- 新增 `shaders/bake/pos_normal_map.frag`：写入两个 color attachment（position: RGBA32F, normal: RGBA32F）
- 新增 `passes/pos_normal_map_pass.h/.cpp`：`PosNormalMapPass` 类
  - `setup()`：编译 shader，创建 graphics pipeline（Dynamic Rendering，2 个 RGBA32F color attachment，无 depth）
  - `record()`：给定 CommandBuffer + mesh + transform + image handles + 分辨率，录制 draw call
  - `rebuild_pipelines()` / `destroy()`：热重载和销毁
  - Pipeline 生命周期跟随 Pass（setup 时创建，destroy 时销毁）
- Renderer 持有 `PosNormalMapPass` 实例，与其他 Pass 统一管理

**验证**：RenderDoc 捕获烘焙帧：position map 中非零 texel 覆盖 mesh 三角形在 UV 空间的投影区域，normal map 中法线方向合理（xyz 分量可视化）

#### 设计要点

Viewport 和 scissor 设置为 lightmap 分辨率（不使用负高度 viewport 翻转）。Cull mode = `VK_CULL_MODE_NONE`（UV 空间光栅化，三角形 winding 取决于 UV layout，不能做背面剔除）。Vertex input 与现有 forward.vert 相同（Vertex binding 0，5 个 attribute）。Push constant 传 instance 的 model matrix + normal matrix（112 字节）。不需要 depth buffer，不需要 descriptor set。Clear color = vec4(0)。Position map fragment shader 输出 `alpha = 1.0` 标记已覆盖 texel；baker raygen 检查 `position.a == 0.0` 跳过未覆盖 texel（避免 `position.xyz == vec3(0)` 哨兵值误判世界原点处的有效 texel）。

---

### Step 8：Lightmap Baker Pass

Lightmap 烘焙 RT pipeline + raygen shader + accumulation。

- 新增 `shaders/rt/lightmap_baker.rgen`：
  - `gl_LaunchIDEXT.xy` = lightmap texel 坐标
  - 读 position/normal map（push descriptor Set 3 binding 4/5）
  - `position.a == 0.0` → 跳过（未覆盖 texel，alpha 通道标记）
  - 从 world position 沿 normal 半球发射射线（cosine-weighted initial direction，TBN frame 从 N 用 frisvad/duff 构建）
  - 初始 Ray Cone：`cone_width = 0, cone_spread = 0`（lightmap 离线烘焙 4096 SPP，始终全分辨率纹理采样；secondary bounce 正常积累 spread）
  - 后续 bounce 调用 `pt_common.glsl` 中提取的 `vec3 trace_path(vec3 origin, vec3 direction, float initial_cone_width, float initial_cone_spread, ivec2 pixel)`。函数内部直接读 `pc.xxx`（push constant 是 per-dispatch uniform，不需要参数传递）。`env_mis_weight`/`last_brdf_pdf` 初始值 hardcode 在函数内部（1.0/0.0，三个 raygen 相同）。Baker 通过 `pc.max_clamp = 0` 禁用 firefly clamping（烘焙求无偏）。从 reference_view.rgen:89-150 提取，三个 raygen 共用。Raygen 职责仅为：计算初始 origin/direction/cone → 调用 trace_path() → 写 accumulation
  - Running average 累积到 accumulation buffer（Set 3 binding 0）
  - Push constant：共享 60B 超集 struct（lightmap_width/height 填入实际值，probe 字段为 0）
- 新增 `passes/lightmap_baker_pass.h/.cpp`：
  - `setup()`：编译 lightmap_baker.rgen，创建 RT pipeline（复用 closesthit/miss/anyhit shader modules）
  - `record()`：RG pass 注册 accumulation ReadWrite + aux albedo/normal Write + position/normal Read + push descriptors + trace_rays(lightmap_w, lightmap_h)
  - Set 3 layout：binding 0 accumulation (storage image) + binding 1 aux albedo (storage image, lightmap 分辨率) + binding 2 aux normal (storage image, lightmap 分辨率) + binding 3 Sobol SSBO + binding 4 position map (sampled) + binding 5 normal map (sampled)
- `framework/CMakeLists.txt` / `passes/CMakeLists.txt`：添加源文件
- `lightmap_baker_pass.h`：`lod_max_level_` 默认值从 4 改为 0，移除 `set_lod_max_level()` 接口（baker 始终全分辨率纹理采样，hardcode）
- `lightmap_baker_pass.h`：`max_bounces_` 默认值从 8 改为 32（与 baker 面板默认值一致）

**验证**：RenderDoc 捕获烘焙帧：accumulation buffer 在 UV 空间逐帧变亮，光照分布合理（窗户附近更亮，角落更暗）

#### 设计要点

Lightmap baker 和 reference view 共享 closesthit/miss/anyhit shader，closesthit 无条件写入 aux image（Phase 6 原样不动）。Baker 的 Set 3 binding 1/2 绑定与 accumulation 同分辨率的真正 aux image（RGBA16F），closesthit bounce 0 时写入。Baker 烘焙完成后将 aux image 连同 accumulation 一起 readback 传给 BakeDenoiser，获得完整辅助通道降噪。Aux image 随 accumulation 一起 per-instance 创建和销毁。

Position/normal map 通过 `sampler2D`（nearest, clamp）采样而非 storage image 读取——texel 坐标完美对齐，nearest sampling 等价于直接读取但不需要 image layout 为 GENERAL。

**trace_path() 提取细节**：

- `DIMS_PER_BOUNCE` 常量从 raygen/closesthit 迁移到 `pt_common.glsl`（两者都引用它计算 Sobol 维度偏移）
- trace_path() 包在 `#ifdef RAYGEN_SHADER` 内：raygen 在 `#include "rt/pt_common.glsl"` 前定义 `#define RAYGEN_SHADER`，closesthit 不定义。trace_path() 内调用 `traceRayEXT`（仅 raygen 可用），且引用 raygen 声明的 `payload` 变量（GLSL `#include` 是文本合并，同一编译单元内变量可见）
- **include 重排**：当前 `reference_view.rgen` 先 include pt_common.glsl（line 25），后声明 push constant block（line 29）和 payload（line 47）。trace_path() 引用 `pc.xxx` 和 `payload`，必须在它们声明之后。解决：raygen 中将 push constant + payload 声明移到 `#include "rt/pt_common.glsl"` 之前（或让 pt_common.glsl 在 `#ifdef RAYGEN_SHADER` 区域内声明 push constant 和 payload，统一管理）
- reference_view.rgen 重构为调用 trace_path()（行为不变，原 bounce loop 代码移入函数）

---

### Step 9：烘焙模式渲染路径 + Lightmap 端到端

将 lightmap 烘焙串联为完整的端到端流程。

#### 前置重构

- **PT 参数迁移**：PT 参数从 Renderer 迁移到 Application 层。新增 `PTConfig` 结构体（`scene_data.h`，沿用 `ShadowConfig` 模式），Application 持有实例，通过 `RenderInput::pt_config` 传递给 Renderer。移除 Renderer 上的 mutable reference accessors（`pt_max_bounces()` 等），DebugUIContext 直接引用 Application 的 `PTConfig` 实例
- **BakeConfig 结构体**：新增 `BakeConfig`（`scene_data.h`），Application 持有实例，通过 `RenderInput::bake_config` 传递给 Renderer。包含全部烘焙可调参数（texels_per_meter / min_res / max_res / lightmap_spp / probe_face_res / probe_spacing / probe_spp / max_bounces / env_sampling / emissive_nee / allow_tearing）

#### 主要变更

- `framework/scene_data.h`：`RenderMode` 新增 `Baking`
- `app/renderer.h`：新增 `render_baking()` 私有方法 + 烘焙状态机字段（BakeState 枚举、current instance index、accumulation buffer handle、finalize pending flag 等）
- 新增 `app/renderer_bake.cpp`：
  - 烘焙触发时计算 per-instance lightmap 分辨率：遍历 `cpu_vertices_` + `cpu_indices_` + instance transform 现算世界空间表面积（纯 CPU，不预存） → `clamp(round(sqrt(area) * texels_per_meter), min_res, max_res)` → 对齐到 4 的倍数
  - `render_baking()` 状态机：
    - `BakingLightmaps`：当前 instance 烘焙中 → 每帧调用 lightmap_baker_pass_.record() → 达到目标采样数后 → 设 finalize pending flag（不在当前帧 finalize——command buffer 正在录制中，无法嵌套 immediate scope）
    - `BakingProbes`：（Step 12 实现）
    - `Complete`：所有烘焙完成
  - **Bake finalize 时机**：沿用参考视图 denoiser 的延迟模式（`upload_pending_completion_` 模式）。Renderer 设 finalize flag → 下一帧 Application 的 `begin_frame()` fence wait 后、`render()` 前检查 flag → Application 驱动 `begin_immediate()` / `end_immediate()`，调用 Renderer 的 finalize 方法执行：readback accumulation + aux → BakeDenoiser::denoise()（含辅助通道）→ upload 降噪结果到 accumulation buffer → compress_bc6h() 采样 accumulation → readback BC6H → write_ktx2() → 释放资源 → 推进到下一个 instance
  - Position/normal map + aux albedo/normal image 创建 / 销毁（per-instance，lightmap 分辨率）
  - Accumulation buffer 创建 / 销毁（per-instance，lightmap 分辨率，usage: `Storage | TransferSrc | TransferDst | Sampled`——Storage 用于 RT dispatch，TransferSrc 用于 readback + blit 预览，TransferDst 用于 upload 降噪结果，Sampled 用于 bc6h.comp 采样）
  - `sample_count`：per-instance 独立计数（每个 instance 从 0 开始累积到 target SPP），通过 `pc.sample_count` 传入
  - `frame_seed`：全局单调递增计数器（不 per-instance 重置），保证不同 dispatch 种子唯一
  - Baker push constant hardcode：`pc.max_clamp = 0`（禁用 firefly clamping）、`pc.lod_max_level = 0`（全分辨率纹理采样）
  - Baker GlobalUBO override 由 Application 在 RenderInput 中设置：`ibl_intensity = 1.0`（归一化烘焙），`lights = {}`（空 span → `directional_light_count = 0`，不烘焙方向光）
  - Baker 独立 push constant：`pc.max_bounces` / `pc.env_sampling` / `pc.emissive_light_count` 从 `RenderInput::bake_config` 读取（独立于参考视图）
  - Baker allow tearing：Application 层设置 present mode（`BakeConfig::allow_tearing`，沿用 PT allow tearing 模式）
  - Cache key 扩展：lightmap key = `scene_hash + geometry_hash + transform_hash + hdr_hash + scene_textures_hash`（per-instance），probe set key = `scene_hash + hdr_hash + scene_textures_hash`（不含 position——位置是 bake 产物）。`rotation_int`（0-359 整数度数）编码在文件名后缀
  - `scene_textures_hash`：场景加载时复用纹理缓存 pipeline 已算的 `source_hashes`（per-texture 源文件字节 hash），按 glTF 纹理数组索引顺序拼接后 hash，存储在 SceneLoader 上
  - Lightmap 文件：`<lm_hash>_rot<NNN>.ktx2`；Probe 文件：`<probe_set_hash>_rot<NNN>_probe<III>.ktx2` + `<probe_set_hash>_rot<NNN>_manifest.bin`（存 probe_count + positions vec3[]）。同一角度重新 bake 覆盖旧文件
  - 完整性校验：Phase 8 逐角度检查所有 instance lightmap 文件 + manifest + 所有 probe 文件齐全才视为有效角度
  - `render_baking()` 每帧帧流程：`fill_common_gpu_data()` → RG import Set 0 资源 + TLAS → baker RT pass → clear `managed_hdr_color_` 为黑色 + `vkCmdBlitImage` 将 accumulation blit 到 `managed_hdr_color_`（居中，保持宽高比，`VK_FILTER_LINEAR`）→ tonemapping pass → ImGui render pass → swapchain present
- `app/renderer.cpp`：`render()` switch 新增 `RenderMode::Baking` → `render_baking()`
- `app/application.cpp`：`begin_frame()` 后检查 `renderer_.bake_finalize_pending()`，驱动 immediate scope finalize
- `app/application.cpp`：Baking 模式 RenderInput 覆盖（`ibl_intensity = 1.0`、`lights = {}`）
- `app/application.cpp`：baker allow tearing（沿用 PT allow tearing 模式，检查 `bake_config_.allow_tearing`）
- 退化 instance（vertex_count=0 / index_count<3）和透明 instance（AlphaMode::Blend）跳过 lightmap bake
- KTX2 / manifest 写入使用 write-to-temp + rename 原子写入
- `rotation_int = round(angle_deg) % 360`（0-359）
- Bake 期间：Load Scene / Load HDR / Reload Shaders / PT checkbox 灰显禁止，bake 参数 slider 灰显锁定，PT 面板不显示，resize 不中断 bake
- 进入 Baking 模式前记录当前 RenderMode，Cancel/Complete 后恢复到记录值（而非硬编码 Rasterization）
- 进入 Baking 模式前调用 `abort_denoise()`（与 resize / scene load 一致），确保参考视图异步 Denoiser 归 Idle
- Cancel 后显示信息："Bake cancelled. N/M instances completed. Incomplete angle will not appear in available list."

**验证**：切换到 Baking 模式 → accumulation 全屏显示（居中保持宽高比，经 tonemapping）→ lightmap 逐 instance 烘焙（跳过退化/透明 instance）→ 每个 instance 烘焙完成后 KTX2 文件出现在 cache 目录 → `read_ktx2()` 能正确加载 → 所有 instance 完成后状态转 Complete

---

### Step 9.5：审查修复（Steps 1-9 回顾）

Steps 1-9 完成后全面审查发现的 bug 修复、同步正确性修复和 aux 通道正确性改造。

#### 9.5a：杂项修复

- `renderer.h`：删除未使用的 `bake_frame_seed_` 成员（`LightmapBakerPass` 内部自行管理 `frame_seed_`）
- `pos_normal_map_pass.cpp`：`record()` 入口检查 `pipeline_.pipeline != VK_NULL_HANDLE`，无效时 early return
- `lightmap_baker_pass.cpp`：`record()` 入口检查 `rt_pipeline_.pipeline != VK_NULL_HANDLE`，无效时 early return
- `renderer_bake.cpp`：lightmap cache key 的 `geometry_hash` 扩展为 `vertices_hash + indices_hash`（分别 hash 再拼接，与 `scene_textures_hash` 风格一致），修复仅 hash 顶点而遗漏索引的问题

#### 9.5b：首个 instance 启动 + 图像初始化

- `renderer_bake.cpp`：`start_bake()` 末尾调用 `begin_bake_instance(0, mesh_instances, meshes)`，guard `bake_total_instances_ > 0`（无 bakeable instance 时直接 `BakeState::Complete`）。`start_bake()` 需在 immediate scope 内调用（Step 13 的 Application 调用侧 `begin_immediate()` / `end_immediate()` 包裹）
- `renderer_bake.cpp`：`begin_bake_instance()` 中 accumulation 创建后 barrier UNDEFINED→GENERAL + `vkCmdClearColorImage` vec4(0)（清除未覆盖 texel 的 VRAM 垃圾）；aux 图像 barrier UNDEFINED→TRANSFER_DST（为 9.5c 的 blit 预填充准备）

#### 9.5c：Baker aux 通道正确性

**问题**：closesthit 无条件写入 aux albedo/normal，写入的是射线命中面的属性——对 lightmap baker 语义错误（应是被 bake texel 自身的表面属性）。OIDN 设置 `cleanAux=true`，错误的 aux 会引入伪影。

**方案**：closesthit 加守卫跳过 lightmap baker 的 aux 写入，PosNormalMap pass 扩展为 3 render target 输出 albedo map，blit 预填充 aux 图像。

**closesthit 守卫**：

```glsl
if (pc.lightmap_width == 0u) {
    imageStore(aux_albedo_image, ivec2(gl_LaunchIDEXT.xy), vec4(albedo, 0.0));
    imageStore(aux_normal_image, ivec2(gl_LaunchIDEXT.xy), vec4(N, 0.0));
}
```

Uniform branch（push constant），零开销。Reference view 和 probe baker（`lightmap_width == 0`）保持原有行为。

**PosNormalMap pass 扩展**：

- `pos_normal_map.vert`：新增 `layout(location = 2) out vec2 frag_uv0;`，pass through `in_uv0`
- `pos_normal_map.frag`：引入 `bindings.glsl`（MaterialBuffer + bindless textures），新增 `layout(location = 2) out vec4 out_albedo;`。从 `material_buffer[material_index]` 读 `base_color_factor` + `base_color_texture_index`，有纹理时采样 `textures[index]` 乘以 factor，输出 `vec4(base_color.rgb, 1.0)`
- `PosNormalMapPushConstants`：新增 `uint32_t material_index`（112→116 字节，仍在 128B 限制内）
- `PosNormalMapPass::setup()`：新增 `DescriptorManager&` 参数（获取 Set 0/1 layout 构建 pipeline layout）
- `PosNormalMapPass` pipeline：3 个 color attachment `{RGBA32F, RGBA32F, RGBA16F}`
- `PosNormalMapPass::record()`：新增 `material_index` + `frame_index` 参数，绑定 Set 0/1

**Aux 预填充（begin_bake_instance）**：

新增 `bake_albedo_map_`（RGBA16F，usage: ColorAttachment | TransferSrc），与其他 per-instance 图像统一在 `destroy_bake_instance_images()` 中销毁。

Barrier 和操作序列（全在 immediate scope 内）：

```
Batch 1（创建后初始转换）:
  pos_map, normal_map, albedo_map  →  COLOR_ATTACHMENT_OPTIMAL
  accumulation                     →  GENERAL  + clear vec4(0)
  aux_albedo, aux_normal           →  TRANSFER_DST_OPTIMAL

Rasterize（3 color attachment：pos_map, normal_map, albedo_map）

Batch 2（光栅化后 → blit 前）:
  pos_map     →  SHADER_READ_ONLY
  normal_map  →  TRANSFER_SRC_OPTIMAL
  albedo_map  →  TRANSFER_SRC_OPTIMAL

Blit  albedo_map → aux_albedo    （RGBA16F → RGBA16F）
Blit  normal_map → aux_normal    （RGBA32F → RGBA16F，vkCmdBlitImage 自动格式转换）

Batch 3（blit 后 → RT 就绪）:
  normal_map  →  SHADER_READ_ONLY
  aux_albedo  →  GENERAL
  aux_normal  →  GENERAL
```

blit 后 aux 图像包含正确的 per-texel 表面属性。closesthit 守卫确保 RT dispatch 不覆写。

#### 9.5d：RG 单次 import

- `LightmapBakerPass::record()` 接口改为接受外部 RG resource ID（accumulation / aux_albedo / aux_normal / position_map / normal_map），移除内部 `import_image()` 调用
- `render_baking()` 统一 import baker 图像一次，传 RG resource ID 给 `lightmap_baker_pass_.record()` 和 blit preview pass，消除同一 VkImage 被 RG 双重 import 导致的 barrier 缺失

#### 9.5e：Finalize 管线

`bake_finalize()` 实现完整管线（在 immediate scope 内执行）：

1. Readback accumulation（RGBA32F）→ CPU beauty buffer
2. Readback aux_albedo（RGBA16F）→ CPU albedo buffer（blit 预填充的正确数据）
3. Readback aux_normal（RGBA16F）→ CPU normal buffer（blit 预填充的正确数据）
4. `BakeDenoiser::denoise(beauty, albedo, normal, output, w, h)`（cleanAux=true，aux 来自光栅化，确实干净）
5. Upload 降噪结果到 accumulation buffer（覆写原始累积数据）
6. `compress_bc6h()` 采样 accumulation → BC6H image
7. Readback BC6H → CPU buffer
8. `write_ktx2()` 原子写入（write-to-temp + rename）
9. 释放当前 instance 图像
10. 推进到下一个 instance（`begin_bake_instance(next)`）或转 Complete

所有子系统已就绪：BakeDenoiser（Step 5）、compress_bc6h（Step 1）、write_ktx2（Phase 6 已有）。

**验证**：无 validation 报错，参考视图行为无回归，触发烘焙 → accumulation 正确显示（无垃圾 texel）→ finalize 输出 KTX2 → `read_ktx2()` 能正确加载。RenderDoc 捕获 begin_bake_instance 帧：position map（alpha 通道覆盖区）、normal map（法线方向可视化）、albedo map（材质颜色，比较 forward pass 确认一致）、aux_albedo/aux_normal（blit 后与 albedo_map/normal_map 内容一致）

---

### Step 10：Probe 自动放置

均匀网格 + 两级 RT 几何过滤（面朝向 + 封闭体检测）。

- 新增 `shaders/bake/probe_filter.comp`：compute shader，每个 invocation 处理一个候选点 × N 条蒙特卡洛射线 `rayQueryEXT`，输出 pass/fail 到 SSBO
- 新增 `framework/probe_placement.h`：`ProbeGrid` 结构体（positions vector + grid params + filter stats）+ `generate_probe_grid()` 函数
- 新增 `framework/probe_placement.cpp`：
  - 在场景 AABB 内按 grid spacing 放置 3D 网格候选点
  - 计算封闭体判定阈值：`enclosure_threshold = enclosure_threshold_factor * AABB_longest_edge`
  - dispatch `probe_filter.comp`（蒙特卡洛球面采样 + `rayQueryEXT`），读回结果执行两级过滤：
    - **Rule 1（一票否决）**：任一射线命中 single-sided 几何体的**背面** → 判定为几何体内部，剔除
    - **Rule 2（封闭体检测）**：所有射线均命中 + 全部命中面为 double-sided + 最大命中距离 < `enclosure_threshold` → 判定为 double-sided 封闭体内部，剔除
  - 返回有效 probe 位置列表 + 过滤统计（候选数、Rule 1 剔除数、Rule 2 剔除数、通过数）
- `framework/CMakeLists.txt`：添加源文件

**验证**：Sponza 场景，grid spacing 1m → 日志输出候选/Rule 1 剔除/Rule 2 剔除/通过数量，墙内探针被正确剔除（可通过临时 ImGui 3D 可视化或日志坐标人工验证）

#### 设计要点

**前置假设**：场景中 non-double-sided 几何体的背面不会出现在合法渲染区域内。此假设对标准场景（Sponza 等）成立，允许 Rule 1 零误判。

**射线方向**：Fibonacci 球面均匀采样（quasi-random，比纯随机覆盖更均匀），射线数量通过 `BakeConfig::filter_ray_count` 可调（默认 64）。采样方向在 shader 内部由 invocation index 确定性生成，无需额外 buffer。

**每条射线的判定流程**：
1. `rayQueryEXT` 获取 committed closest hit（不使用 `TerminateOnFirstHitEXT`——需要最近命中点的准确距离和面朝向）
2. Miss → 标记为"开放方向"
3. Hit → 通过 `rayQueryGetIntersectionFrontFaceEXT` 判断正面/背面，通过 `rayQueryGetIntersectionInstanceCustomIndexEXT` + MaterialBuffer 查询 `double_sided` 标志
4. Hit single-sided 背面 → Rule 1 立即判定剔除（整个 invocation 可 early-out）
5. Hit double-sided → 记录距离，累计 double-sided 命中数
6. Hit single-sided 正面 → 标记存在非 double-sided 命中（Rule 2 不可能触发）

**封闭体阈值**：`enclosure_threshold = enclosure_threshold_factor * AABB_longest_edge`。系数通过 `BakeConfig::enclosure_threshold_factor` 可调（默认 0.05）。ImGui 参数面板在系数 slider 旁显示计算出的绝对阈值（如 "= 1.50 m"）。

RT 几何过滤在烘焙触发时执行（非场景加载时——grid spacing 等参数在 UI 中可调）。Compute shader 使用 `rayQueryEXT`（Vulkan 1.2+ ray query 扩展，Phase 6 Step 1 已启用）+ `gl_RayFlagsOpaqueEXT`（所有几何体视为不透明，traversal 自动提交最近命中）。单次 dispatch 处理全部候选点（每个 invocation 一个候选点 × N 条射线）。

Compute pipeline 为一次性使用：烘焙触发时在 immediate scope 内创建 → dispatch → 读回结果 → 销毁 pipeline。与 pos/normal map pipeline（持久存活）不同——placement 只执行一次，不值得持久占用资源。

TLAS 访问：`probe_filter.comp` 共用 `bindings.glsl` 的 Set 0 声明，通过 `bind_compute_descriptor_sets()` 绑定已有 Set 0（TLAS 在 binding 4，`build_scene_rt()` 后已有效）。MaterialBuffer 在 Set 0 binding 1，已包含 `double_sided` 标志。与现有 compute pass（GTAO 等）访问 Set 0 的模式一致。

---

### Step 11：Probe Baker Pass

Probe 烘焙 RT pipeline + raygen shader + cubemap accumulation。

- 新增 `shaders/rt/probe_baker.rgen`：
  - `gl_LaunchIDEXT.xy` = cubemap face texel 坐标
  - Probe 位置通过 push constant 读取（`probe_pos_x/y/z`，共享 60B 超集 struct）
  - 6 个 face 的方向矩阵从 face index 推导（cube face → world direction）
  - 从 probe 位置沿 face 方向发射射线
  - 初始 Ray Cone：`cone_width = 0, cone_spread = atan(2.0 / face_resolution)`（cubemap texel 张角，类似 reference view 的像素张角）
  - 后续 bounce 调用 `pt_common.glsl` 共享 trace_path() 函数
  - Push constant：共享 60B 超集 struct（probe_pos 填入当前 probe 位置，face_index 填入当前 face，lightmap 字段为 0）
  - 6 个 face 共享同一个 per-probe sample_count：每帧 6 个 face 各 dispatch 一次，帧结束后 sample_count +1（不是 +6），Sobol 序列按 sample_count 索引保证时序一致
- 新增 `passes/probe_baker_pass.h/.cpp`：
  - `setup()`：编译 probe_baker.rgen，创建 RT pipeline
  - `record()`：每帧 **6 次 dispatch**（每 face 一次，face index 通过 push constant `face_index` 字段传入）。每次 dispatch 绑定 accumulation cubemap 的 per-layer 2D view + 对应 face 的 aux 2D image。保持 closesthit 用 `ivec2(gl_LaunchIDEXT.xy)` 写 aux 不变（已确认 closesthit 不使用 screen_size 等变换，直接用 `gl_LaunchIDEXT.xy`）
  - Set 3 layout（per-dispatch 切换绑定）：binding 0 accumulation face view（image2D storage，cubemap 单层 2D view）+ binding 1 aux albedo（image2D storage，per-face）+ binding 2 aux normal（image2D storage，per-face）+ binding 3 Sobol SSBO
  - Aux image 管理：2 个 RGBA16F image2DArray × 6 layer（1 albedo array + 1 normal array）。Per-face 2D view 在 probe 开始烘焙时通过 `create_layer_view()` 一次性创建（accumulation 6 + aux albedo 6 + aux normal 6 = 18 个 view），所有 dispatch 复用，probe 完成时随 image 一起销毁。与 accumulation cubemap 的 per-face view 模式一致

**验证**：RenderDoc：probe accumulation cubemap 6 个 face 逐帧变亮，环境内容正确（各 face 方向对应场景不同方向）

---

### Step 12：Probe 端到端流程

将 probe 烘焙串联进烘焙状态机。

- `renderer_bake.cpp` 状态机扩展：
  - `BakingLightmaps` 完成后 → `BakingProbes`
  - `BakingProbes`：调用 probe_placement 获取 probe 列表 → 写入 manifest.bin（probe_count + positions）→ 逐 probe 烘焙：
    - 创建 accumulation cubemap（RGBA32F, face_w × face_w × 6 face）+ aux albedo/normal cubemap（RGBA16F × 6 face）
    - 每帧 dispatch probe_baker_pass_.record()
    - 达到目标采样�� → GPU idle → readback accumulation + aux → BakeDenoiser（6 face 逐面降噪，含辅助通道）→ upload → prefilter_cubemap() → compress_bc6h() → readback → write_ktx2() → 释放 → 下一个 probe
  - 所有 probe 完成 → `Complete`

**验证**：触发烘焙 → lightmap 全部完成 → probe 逐个烘焙 → 每个 probe 的 KTX2 文件出现在 cache → read_ktx2() 加载正确（BC6H cubemap with mip chain）→ 状态转 Complete

#### 设计要点

Probe 逐面降噪（OIDN 不支持 cubemap 感知降噪）可能在 face 边缘产生接缝。M1 已知限制：prefilter mip chain 在高 roughness（>0.3）时模糊接缝；mip 0（完美镜面反射）在 2048 SPP + OIDN 下接缝可接受。

Cache key 和文件命名详见 Step 9。Probe position 是 bake 产物（`generate_probe_grid()` 计算结果），存储在 `<probe_set_hash>_rot<NNN>_manifest.bin` 中，Phase 8 加载时直接读取，不重新运行 placement。

---

### Step 13：ImGui 烘焙控制面板

- `app/debug_ui.cpp`：新增 Baking collapsing header（始终显示，默认折叠）：
  - **参数配置**（默认值见 `m1-rt-decisions.md` 烘焙参数表）：lightmap texels_per_meter(10) + min_resolution(32) + max_resolution(2048) + lightmap SPP(4096) + probe face 分辨率(512) + probe grid spacing(1m) + probe filter ray count(64) + probe enclosure threshold factor(0.05)（slider 旁显示 "= X.XX m" 绝对阈值）+ probe SPP(2048) + baker max_bounces(32) + baker env_sampling(ON) + baker emissive_nee(ON) + baker allow_tearing(OFF)
  - **烘焙触发**：Start Bake 按钮（仅 rt_supported + 有场景 + 有 HDR + RenderMode != Baking + IBL 模式时可用）。按钮旁显示当前 IBL 旋转角度（round 后整数度），tooltip 说明"将以此角度 bake，结果按角度缓存"。点击后切换 RenderMode 到 Baking（进入 Baking 模式是唯一入口，不通过 RenderMode combo）
  - **Bake 期间 UI 锁定**：所有 bake 参数 slider 灰显、Start Bake 灰显、Load Scene / Load HDR / Reload Shaders / PT checkbox 灰显、PT 面板不显示、Allow Tearing 不支持 IMMEDIATE 时灰显
  - **进度显示**：当前阶段（Lightmaps / Probes / Complete）+ 当前项编号/总数 + 采样数/目标 + 吞吐量（SPP/s）+ 当前项耗时 + 总进度百分比 + 总耗时
  - **取消**：Cancel 按钮 → 中止烘焙，RenderMode 恢复到进入 Baking 前的模式。显示信息 "Bake cancelled. N/M instances completed. Incomplete angle will not appear in available list."
  - **已 bake 角度列表**：显示当前 scene+HDR 下所有已 bake 的旋转角度（目录扫描 `<hash>_rot*.ktx2`），点击可在 Lightmap/Probe 模式下切换角度。每个角度显示 lightmap 数量 + probe 数量
  - **Cache 操作**：Cache 面板新增 Clear Bake Cache 按钮（清除全部 bake 缓存）
- `app/renderer.h`：暴露烘焙状态（BakeState、current index、sample count、吞吐量、耗时等）给 DebugUIContext
- `app/config.cpp`：烘焙参数持久化（texels_per_meter、probe spacing、baker max_bounces、baker env_sampling、baker emissive_nee、baker allow_tearing 等）

**验证**：所有参数 slider 功能正常，Start/Cancel 按钮正确触发/中止，进度信息和吞吐量实时更新，accumulation 全屏预览（经 blit + tonemapping）在烘焙进行中显示实时画面，已 bake 角度列表正确显示
