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
Step 6: PT Push Constants 扩展 + closesthit baker_mode 分支
    ↓
Step 7: Position/Normal Map 光栅化 pass
    ↓
Step 8: Lightmap Baker Pass（raygen shader + RT pipeline + accumulation）
    ↓
Step 9: 烘焙模式渲染路径 + Lightmap 端到端流程
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
| 6 | Push Constants + baker_mode | RT shader 编译通过（push constant 48B），reference view 行为不变（baker_mode=0） |
| 7 | Position/Normal Map pass | RenderDoc：position map 和 normal map 在 UV 空间正确渲染（非零 texel 覆盖三角形区域） |
| 8 | Lightmap Baker Pass | RenderDoc：accumulation buffer 逐帧亮度增加，UV 空间可见光照分布 |
| 9 | 烘焙模式 + Lightmap 端到端 | 触发烘焙 → 累积 → 降噪 → BC6H → KTX2 写入磁盘，文件可被 read_ktx2() 正确加载 |
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

### Step 3：xatlas vcpkg 集成 + Lightmap UV 生成器

引入 xatlas 依赖，实现 lightmap UV 生成 + 缓存。

- `vcpkg.json`：添加 `xatlas` 依赖
- 新增 `framework/lightmap_uv.h`：`LightmapUVResult` 结构体 + `generate_lightmap_uv()` 函数声明
- 新增 `framework/lightmap_uv.cpp`：
  - TEXCOORD_1 检测：遍历顶点 `uv1`，全为零则无 TEXCOORD_1 → 需要 xatlas
  - 缓存查找：`cache_path("lightmap_uv", mesh_hash, ".bin")`，命中则加载返回
  - xatlas 调用：`xatlas::Create()` → `xatlas::AddMesh()` → `xatlas::Generate()` → 提取 UV + remap + new indices
  - 缓存写入：header（magic + version + source mesh hash + atlas size + vertex/index counts）+ lightmap UV 数组 + new index buffer + vertex remap table
- `framework/CMakeLists.txt`：添加 `lightmap_uv.cpp` + link xatlas

**验证**：无 TEXCOORD_1 的 mesh → xatlas 生成 lightmap UV + 缓存文件写入 + 二次加载命中缓存；有 TEXCOORD_1 → 返回 nullopt

#### 设计要点

mesh_hash = `content_hash(vertex positions + indices)`（不含 UV、normal 等——只有几何拓扑决定 UV layout）。xatlas gutter 默认 2（padding texels around UV islands）。`generate_lightmap_uv()` 是纯 CPU 函数，不涉及 GPU 资源。

---

### Step 4：Lightmap UV 拓扑应用

将 xatlas 输出应用到 Mesh 的 vertex/index buffer。

- `scene_loader.cpp`：场景加载流程变更——load_meshes() 后、build_mesh_instances() 前，对每个 mesh 调用 `generate_lightmap_uv()`：
  - 返回 nullopt（有 TEXCOORD_1）→ 不改动
  - 返回 LightmapUVResult → 根据 remap table 构建新 vertex 数组（拆分顶点，`uv1` 写入 lightmap UV）+ 新 index 数组 → 销毁原 GPU buffer → 上传新 GPU buffer → 更新 Mesh 的 vertex_buffer / index_buffer / vertex_count / index_count
- `scene_loader.cpp`：CPU 端 `cpu_vertices_` / `cpu_indices_` 同步更新（emissive light builder 等依赖 CPU 数据）
- Lightmap 分辨率计算：遍历 mesh instance，计算世界空间表面积 → `clamp(round(sqrt(area) * texels_per_meter), min_res, max_res)` → 对齐到 4 的倍数。存储到新增的 per-instance 数据结构中（或 Renderer 侧 vector）
- 加载顺序确认：glTF → xatlas UV → rebuild VB/IB → build BLAS/TLAS → build emissive lights

**验证**：场景加载后光栅化渲染无回归（位置/法线/uv0 不变），PT 参考视图无回归，日志输出每个 mesh 的 lightmap UV 来源（TEXCOORD_1 / xatlas / cache）和计算的 lightmap 分辨率

#### 设计要点

xatlas 拆分顶点后新顶点的 position / normal / uv0 / tangent 从原始顶点复制（remap table 索引），只有 `uv1` 是 xatlas 生成的新值。BLAS 重建不是额外开销——在同一个场景加载 immediate scope 中完成。

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

BakeDenoiser 的 OIDNBuffer 尺寸在 `init()` 时未知——首次 `denoise()` 调用时根据 `w × h` 创建（或 resize）。后续调用若尺寸相同则复用。与异步 `Denoiser` 完全独立，不共享任何状态。

---

### Step 6：PT Push Constants 扩展 + closesthit baker_mode

扩展 push constant 布局，closesthit 加 baker_mode 条件分支。

- `shaders/rt/pt_common.glsl` 或各 RT shader：PushConstants struct 末尾追加 `uint baker_mode`（36B → 40B）+ `uint lightmap_width` + `uint lightmap_height`（40B → 48B）
- `shaders/rt/closesthit.rchit`：OIDN aux imageStore 加条件：`if (baker_mode == 0u && payload.bounce == 0u)`
- `passes/reference_view_pass.cpp`：push constant 填充追加 `baker_mode = 0`、`lightmap_width = 0`、`lightmap_height = 0`
- `passes/reference_view_pass.h`：Set 3 push descriptor layout 的 push constant range 更新为 48B

**验证**：全部 RT shader 编译通过，reference view 行为与修改前完全一致（baker_mode=0，条件分支走原路径）

---

### Step 7：Position/Normal Map 光栅化 pass

UV 空间光栅化预处理。

- 新增 `shaders/bake/pos_normal_map.vert`：`gl_Position = vec4(uv1 * 2.0 - 1.0, 0.0, 1.0)`，输出世界空间 position（`model * in_position`）和 normal（`normalize(normal_matrix * in_normal)`）
- 新增 `shaders/bake/pos_normal_map.frag`：写入两个 color attachment（position: RGBA32F, normal: RGBA32F）
- Renderer（`renderer_bake.cpp`）：
  - 创建 position/normal map graphics pipeline（Dynamic Rendering，2 个 color attachment，无 depth）
  - 录制函数：给定 mesh + instance transform + lightmap 分辨率，创建两张 RGBA32F image，录制 draw call
  - Pipeline cache：pipeline 生命周期跟随 Renderer（init 时创建，destroy 时销毁）

**验证**：RenderDoc 捕获烘焙帧：position map 中非零 texel 覆盖 mesh 三角形在 UV 空间的投影区域，normal map 中法线方向合理（xyz 分量可视化）

#### 设计要点

Viewport 和 scissor 设置为 lightmap 分辨率。Vertex input 与现有 forward.vert 相同（Vertex binding 0，5 个 attribute）。Push constant 传 instance 的 model matrix + normal matrix。不需要 depth buffer。Clear color = vec4(0)（未覆盖 texel position = 0 → baker raygen 跳过）。

---

### Step 8：Lightmap Baker Pass

Lightmap 烘焙 RT pipeline + raygen shader + accumulation。

- 新增 `shaders/rt/lightmap_baker.rgen`：
  - `gl_LaunchIDEXT.xy` = lightmap texel 坐标
  - 读 position/normal map（push descriptor Set 3 binding 4/5）
  - position == vec3(0) → 跳过（未覆盖 texel）
  - 从 world position 沿 normal 半球发射射线（cosine-weighted initial direction）
  - 后续 bounce 与 reference_view.rgen 相同逻辑（loop + throughput + Russian Roulette）
  - Running average 累积到 accumulation buffer（Set 3 binding 0）
  - Push constant：共享 48B struct，`baker_mode = 1`
- 新增 `passes/lightmap_baker_pass.h/.cpp`：
  - `setup()`：编译 lightmap_baker.rgen，创建 RT pipeline（复用 closesthit/miss/anyhit shader modules）
  - `record()`：RG pass 注册 accumulation ReadWrite + position/normal Read + push descriptors + trace_rays(lightmap_w, lightmap_h)
  - Set 3 layout：binding 0 accumulation (storage image) + binding 1/2 dummy aux (1×1 storage image) + binding 3 Sobol SSBO + binding 4 position map (sampled) + binding 5 normal map (sampled)
- `framework/CMakeLists.txt` / `passes/CMakeLists.txt`：添加源文件

**验证**：RenderDoc 捕获烘焙帧：accumulation buffer 在 UV 空间逐帧变亮，光照分布合理（窗户附近更亮，角落更暗）

#### 设计要点

Lightmap baker 和 reference view 共享 closesthit/miss/anyhit shader（baker_mode=1 跳过 aux 写入）。Dummy aux images（1×1 RGBA16F storage）在 Renderer init 时创建，baker 所有 instance 共用。Position/normal map 通过 `sampler2D`（nearest, clamp）采样而非 storage image 读取——texel 坐标完美对齐，nearest sampling 等价于直接读取但不需要 image layout 为 GENERAL。

---

### Step 9：烘焙模式渲染路径 + Lightmap 端到端

将 lightmap 烘焙串联为完整的端到端流程。

- `framework/scene_data.h`：`RenderMode` 新增 `Baking`
- `app/renderer.h`：新增 `render_baking()` 私有方法 + 烘焙状态机字段（BakeState 枚举、current instance index、accumulation buffer handle 等）
- 新增 `app/renderer_bake.cpp`：
  - `render_baking()` 状态机：
    - `BakingLightmaps`：当前 instance 烘焙中 → 每帧调用 lightmap_baker_pass_.record() → 达到目标采样数后 → GPU idle → readback accumulation → BakeDenoiser::denoise() → upload denoised → compress_bc6h() → readback BC6H → write_ktx2() → 释放资源 → 下一个 instance
    - `BakingProbes`：（Step 12 实现）
    - `Complete`：所有烘焙完成
  - Position/normal map 创建 + 光栅化录制（Step 7 的 pipeline）
  - Accumulation buffer 创建 / 销毁（per-instance，lightmap 分辨率）
- `app/renderer.cpp`：`render()` switch 新增 `RenderMode::Baking` → `render_baking()`
- `app/debug_ui.cpp`：Rendering section 的 RenderMode combo 新增 Baking（仅 rt_supported 时显示）

**验证**：切换到 Baking 模式 → lightmap 逐 instance 烘焙 → 每个 instance 烘焙完成后 KTX2 文件出现在 cache 目录 → `read_ktx2()` 能正确加载 → 所有 instance 完成后状态转 Complete

---

### Step 10：Probe 自动放置

均匀网格 + RT 几何过滤。

- 新增 `framework/probe_placement.h`：`ProbeGrid` 结构体（positions vector + grid params）+ `generate_probe_grid()` 函数
- 新增 `framework/probe_placement.cpp`：
  - 在场景 AABB 内按 grid spacing 放置 3D 网格候选点
  - 对每个候选点向 6 个轴对齐方向（+X/-X/+Y/-Y/+Z/-Z）发射 RT 射线（`traceRayEXT` 或 CPU 端 `vkCmdTraceRaysKHR`）
  - 若 >= 5/6 个方向在极短距离内命中几何体（< 0.1m）→ 判定为墙内，剔除
  - 返回有效 probe 位置列表
- `framework/CMakeLists.txt`：添加源文件

**验证**：Sponza 场景，grid spacing 1m → 日志输出候选/通过/剔除数量，墙内探针被正确剔除（可通过临时 ImGui 3D 可视化或日志坐标人工验证）

#### 设计要点

RT 几何过滤在 immediate scope 中执行（场景加载后一次性）。射线用 `gl_RayFlagsTerminateOnFirstHitEXT`（只需 hit/miss，不需要最近距离）。可选用 compute shader dispatch traceRayEXT（ray query）代替 CPU 逐射线——M1 场景 probe 数量有限（<100），CPU 端 submit 逐射线也可接受。具体实现时按 probe 数量选择。

---

### Step 11：Probe Baker Pass

Probe 烘焙 RT pipeline + raygen shader + cubemap accumulation。

- 新增 `shaders/rt/probe_baker.rgen`：
  - `gl_LaunchIDEXT.xy` = cubemap face texel 坐标
  - Probe 位置通过 push constant 或 GlobalUBO 传入
  - 6 个 face 的方向矩阵从 face index 推导（cube face → world direction）
  - 从 probe 位置沿 face 方向发射射线
  - 后续 bounce 逻辑与 lightmap baker 相同
  - Accumulation 写入 cubemap 的对应 face（image2DArray，layer = face）
  - Push constant：共享 48B struct（baker_mode=1），lightmap_width/height 替换为 probe face 分辨率
- 新增 `passes/probe_baker_pass.h/.cpp`：
  - `setup()`：编译 probe_baker.rgen，创建 RT pipeline
  - `record()`：RG pass 注册 accumulation cubemap ReadWrite + push descriptors + trace_rays(face_w, face_h)（dispatch 一次覆盖一个 face，6 次 dispatch 或 launch_size.z = 6）
  - Set 3 layout：binding 0 accumulation cubemap（image2DArray storage）+ binding 1/2 dummy aux + binding 3 Sobol SSBO

**验证**：RenderDoc：probe accumulation cubemap 6 个 face 逐帧变亮，环境内容正确（各 face 方向对应场景不同方向）

---

### Step 12：Probe 端到端流程

将 probe 烘焙串联进烘焙状态机。

- `renderer_bake.cpp` 状态机扩展：
  - `BakingLightmaps` 完成后 → `BakingProbes`
  - `BakingProbes`：调用 probe_placement 获取 probe 列表 → 逐 probe 烘焙：
    - 创建 accumulation cubemap（RGBA32F, face_w × face_w × 6 face）
    - 每帧 dispatch probe_baker_pass_.record()
    - 达到目标采样数 → GPU idle → readback → BakeDenoiser（6 face 逐面降噪）→ upload → prefilter_cubemap() → compress_bc6h() → readback → write_ktx2() → 释放 → 下一个 probe
  - 所有 probe 完成 → `Complete`

**验证**：触发烘焙 → lightmap 全部完成 → probe 逐个烘焙 → 每个 probe 的 KTX2 文件出现在 cache → read_ktx2() 加载正确（BC6H cubemap with mip chain）→ 状态转 Complete

---

### Step 13：ImGui 烘焙控制面板

- `app/debug_ui.cpp`：新增 Baking collapsing header：
  - **参数配置**：lightmap texels_per_meter slider + min/max resolution + lightmap 目标采样数 + probe face 分辨率 + probe grid spacing + probe 目标采样数
  - **烘焙触发**：Start Bake 按钮（仅 rt_supported + RenderMode != Baking 时可用）→ 切换 RenderMode 到 Baking
  - **进度显示**：当前阶段（Lightmaps / Probes / Complete）+ 当前项编号/总数 + 采样数/目标 + 总进度百分比
  - **Accumulation 预览**：ImGui::Image() 显示当前 accumulation buffer（注册为 ImGui 纹理 descriptor）
  - **取消**：Cancel 按钮 → 中止烘焙，RenderMode 恢复 Rasterization
- `app/renderer.h`：暴露烘焙状态（BakeState、current index、sample count 等）给 DebugUIContext
- `app/config.cpp`：烘焙参数持久化（texels_per_meter、probe spacing 等）

**验证**：所有参数 slider 功能正常，Start/Cancel 按钮正确触发/中止，进度信息实时更新，accumulation 预览在烘焙进行中显示实时画面
