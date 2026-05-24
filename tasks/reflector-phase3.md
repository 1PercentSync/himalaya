# Reflector Phase 3：Gaussian Splatting 渲染

> 目标：实现 GS 实时渲染管线（Compute Tile-Based Rendering）。
> 技术决策见 `docs/technical-decisions.md` 第 22-23 节。
>
> 每完成一个复选框暂停等待审查。一个 Step 结束时应能编译通过。
> 编译验证 = C++ 代码编译通过。Shader 为运行时编译（shaderc），编译期不验证。

---

## 决策记录

| # | 问题 | 决定 |
|---|------|------|
| 1 | 渲染管线架构 | Compute Tile-Based Rendering（纯 compute 软光栅） |
| 2 | GPU 排序算法 | 自行实现 GPU Radix Sort |
| 3 | 排序 key 位宽 | 32-bit key（depth）+ 32-bit value（index）分离，4 pass |
| 4 | SH 求值时机 | 投影阶段 per-splat 一次 |
| 5 | GPU 数据布局 | 混合方案：核心属性打包 struct（48 bytes, std430）+ SH 独立 buffer，CPU 端同步改结构 |
| 6 | Tile 大小 | 16×16（256 threads/workgroup） |
| 7 | GS 输出目标 | 独立 RG managed image（R16G16B16A16F, Storage + Sampled） |
| 8 | 输出 Pass | TonemappingPass → PresentPass，push constant mode 分支 + SRGB/UNORM view 切换 |
| 9 | Descriptor 绑定 | Set 0（GlobalUBO）+ Push Descriptor（Set 3） |
| 10 | Compute Pass 拆分 | 四阶段，单 RG pass 内手动 buffer barrier |
| 11 | 颜色空间处理 | GS 侧原样输出 SH 值，gamma 由 PresentPass view 选择控制 |
| 12 | 抗锯齿 | 直接实现 Mip Splatting（3D 频率限制 + 2D mip filter） |
| 13 | 多 Primitive 处理 | 核心属性合并 + SH 按 degree 分组 dispatch |
| 14 | 背景色 | 纯黑（vec4(0)） |
| 15 | Buffer barrier 策略 | 四阶段在单个 RG pass 内，阶段间手动 vkCmdPipelineBarrier2 |
| 16 | 可见 splat 数量 | 投影 pass atomic counter + indirect dispatch |
| 17 | Depth key 编码 | camera distance 的 floatBitsToUint，升序 = 前到后 |
| 18 | Early termination | transmittance < 1/255 |
| 19 | RenderMode 流转 | Application::render_mode_ 枚举 → RenderInput::render_mode → Renderer 分发 |
| 20 | GsColorSpace 枚举位置 | `scene_data.h`，与 `RenderMode` 同级（均为渲染配置枚举，`FrameContext` 不定义枚举） |
| 21 | UNORM ImageView 存储 | `Swapchain` 类，`unorm_image_views` 与 `image_views` 平行管理（同一批 VkImage、同一生命周期） |
| 22 | Step 1 暂不改 Application/RenderInput | FrameContext 新增 `render_mode` / `gs_color_space`，`render_path_tracing()` 中填写默认值（PathTracing / Unknown），完整流转留待 Step 6 |
| 23 | GsGpuData 类持有者 | `framework/` 层定义类，`app/` 层 Renderer 持有实例，与 MaterialSystem / IBL 模式一致 |
| 24 | GPU Buffer debug name | `"GS Core SSBO"`、`"GS SH Degree N SSBO"`（N=0..3），与现有命名风格一致（如 `"GeometryInfo SSBO"`）|
| 25 | upload() 参数 | 直接收 `const GaussianSplatScene&`，与 `build_scene_rt()` 收 `span<const Mesh>` 同理 |
| 26 | GSSplatData2D 布局 | 64 bytes/splat（std430），定义在 `gaussian_splat_data.h`。vec3 alignment=16 导致 8 bytes padding，不可消除但可接受 |
| 27 | 投影输出 + counter buffer 持有者 | GsProjectionPass 持有（投影输出 SSBO + depth key/value buffer + counter buffer + indirect dispatch buffer），与 ReferenceViewPass 持有 accumulation 资源模式一致 |
| 28 | Indirect dispatch 填写时机 | Step 3 只写 atomic counter，不做 count→dispatch struct 转换。Indirect buffer 在 Step 3 创建但不填充；填充留到 Step 4（sort 编排开头加一个小 compute dispatch）|
| 29 | Projection workgroup size | 256，与 radix sort 独立（sort 的 workgroup 是 sort 自己的实现细节）。dispatch = `ceil(total_splat_count / 256)` |
| 30 | Step 3 排序输入遗漏修正 | Projection shader 在 `atomicAdd` 得到 `visible_index` 后写入 `depth_keys[visible_index] = floatBitsToUint(camera_distance)` 和 `splat_indices[visible_index] = visible_index`。后续排序后用 `splat_indices[sorted_i]` 索引 `splats_2d[]` |
| 31 | Sort prepare shader | 新增 `shaders/gs/gs_sort_prepare.comp`，单线程读取 visible counter，写入 `VkDispatchIndirectCommand(ceil(visible_count / workgroup_size), 1, 1)` |
| 32 | Radix sort scan 结构 | scan 使用多级方案：per-block scan → block-level scan → final combine。histogram 为 `digit × block_count` 二维表，scatter 必须保持稳定以保证 LSD radix sort 正确 |

---

## Step 0：数据结构重构

- [x] 定义 `GaussianSplatCore` 打包 struct（position + pad + rotation + scale + opacity = 48 bytes），添加 `static_assert` 校验 size 和 offset
- [x] 将 `GaussianSplatPrimitive` 中 4 个独立 vector 替换为 `vector<GaussianSplatCore>`
- [x] 修改 `GaussianSplatLoader`：先读入临时 vector，再交织填入 `GaussianSplatCore` 数组
- [x] 更新 AABB 计算逻辑（从 `cores[i].position` 读取）
- [x] 编译验证

> PLY 转换器不需要修改——它输出 glTF 文件（磁盘格式不变），CPU 端打包由 loader 在加载时完成。

## Step 1：PresentPass 重构

- [x] 重命名 `TonemappingPass` → `PresentPass`（头文件、源文件、类名）
- [x] 更新 `Renderer` 中的成员名（`tonemapping_pass_` → `present_pass_`）、include 路径、所有引用
- [x] 重命名 shader 文件 `tonemapping.frag` → `present.frag`
- [x] 添加 push constant struct（`uint mode`）和 pipeline layout 中的 push constant range
- [x] 更新 shader：根据 mode 分支（PT 走 exposure + ACES，GS 走 passthrough）
- [x] 修改 Swapchain 创建：添加 `VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR` + `VkImageFormatListCreateInfo`（SRGB + UNORM 兼容格式列表）
- [x] 为每个 swapchain image 创建额外的 UNORM VkImageView（Renderer 或 Swapchain 层管理）
- [x] `FrameContext` 新增 `gs_color_space` 字段
- [x] `record()` 根据 RenderMode 和 `gs_color_space` 选择 SRGB/UNORM view 作为 color attachment
- [x] 编译验证

## Step 2：GPU Buffer 上传

- [x] 创建 `framework/include/himalaya/framework/gs_gpu_data.h`（GS GPU buffer 管理类，持有 core SSBO + SH SSBO handles）
- [x] 创建 `framework/src/gs_gpu_data.cpp`
- [x] 实现 `upload()`：staging → device local 上传核心属性（多 primitive 合并，position 应用 transform）
- [x] 实现 SH 上传：按 max_sh_degree 分组，同 degree 的 primitive SH 数据拼接为一个 SSBO（累计系数布局）
- [x] 记录每个 SH degree 分组的 (splat_offset, splat_count, sh_degree) 供投影 pass dispatch 使用
- [x] 实现 `destroy()`：场景卸载时销毁所有 buffer
- [x] 编译验证

## Step 3：投影与剔除 Pass

- [x] 创建 `shaders/gs/gs_projection.comp`（视锥剔除 + 3D covariance 构建 + 2D covariance 投影 + 椭圆主轴分解）
- [x] 实现 SH 求值（degree 0-3，通过 push constant `sh_degree` 分支）
- [x] 实现 Mip Splatting 3D 频率限制（scale clamp to min_threshold）
- [x] 实现 Mip Splatting 2D mip filter（cov2d += 0.3 * I）
- [x] 实现 atomic counter 写入可见 splat 数（indirect dispatch buffer 仅创建，填充留到 Step 4）
- [x] 创建投影输出中间 buffer 数据布局（`GSSplatData2D` 数组 + depth key/value buffer；实际 GPU allocation 放到 `GsProjectionPass`）
- [x] 创建 `passes/include/himalaya/passes/gs_projection_pass.h` 和 `passes/src/gs_projection_pass.cpp`
- [x] 编译验证

> 依赖 Step 2 的 GPU buffer 作为输入。

## Step 4：GPU Radix Sort

- [x] 修正 Step 3 遗漏：Projection shader 写入排序输入 `depth_keys[]` 和 `splat_indices[]`，其中 value 使用 `visible_index`
- [x] 创建 `shaders/gs/gs_sort_prepare.comp`（visible counter → `VkDispatchIndirectCommand`）
- [x] 创建 `shaders/gs/gs_sort_histogram.comp`（per-workgroup digit 频率统计）
- [x] 创建 `shaders/gs/gs_sort_scan.comp`（prefix sum，多级 scan）
- [x] 创建 `shaders/gs/gs_sort_scatter.comp`（按 prefix sum 结果稳定 scatter key+value）
- [x] 创建 `framework/include/himalaya/framework/radix_sort.h` 和 `framework/src/radix_sort.cpp`
- [ ] 实现 ping-pong buffer 管理（key[2] + value[2] + histogram）
- [ ] 实现 4-pass 编排（每 pass 处理 8 bit，循环：histogram → scan → scatter）
- [ ] 实现 `record()`：录制到 command buffer，阶段间插入 buffer barrier
- [ ] 编译验证

> 输入为 Step 3 的 depth key + splat index，输出为排好序的 key-value 对。

## Step 5：Tile Binning

- [ ] 创建 `shaders/gs/gs_tile_count.comp`（每个可见 splat 遍历其覆盖 tile，atomicAdd 每 tile 计数）
- [ ] 实现 prefix sum 计算 per-tile 偏移（复用 Step 4 的 scan shader 或独立实现）
- [ ] 创建 `shaders/gs/gs_tile_scatter.comp`（再次遍历可见 splat，按 offset + atomicAdd 写入 tile_splat_ids）
- [ ] 创建 tile buffer：`tile_offsets[]`、`tile_counts[]`、`tile_splat_ids[]`（tile 数量依赖屏幕分辨率，resize 时重建）
- [ ] 创建 `passes/include/himalaya/passes/gs_tile_binning_pass.h` 和 `passes/src/gs_tile_binning_pass.cpp`
- [ ] 编译验证

> 输入为 Step 4 排序后的 splat index + Step 3 的 2D 属性（tile 覆盖范围）。

## Step 6：Tile Rendering + 集成

- [ ] 创建 `shaders/gs/gs_tile_render.comp`（16×16 workgroup，前到后 alpha blend，transmittance < 1/255 early termination）
- [ ] 实现 2D Gaussian 求值（从椭圆主轴计算 Mahalanobis distance，3σ 截断）
- [ ] 实现 imageStore 写入 GS color buffer（背景初始化为黑色）
- [ ] 创建 `passes/include/himalaya/passes/gs_tile_render_pass.h` 和 `passes/src/gs_tile_render_pass.cpp`
- [ ] 创建 GS managed color buffer（R16G16B16A16F, Storage + Sampled, 屏幕尺寸）
- [ ] 在 Renderer 中实现 `render_gaussian_splatting()`：四阶段录制在单个 RG pass 内，阶段间手动 `vkCmdPipelineBarrier2`
- [ ] `RenderInput` 新增 `render_mode` 字段，`Application` 中 `pt_mode_` 替换为 `RenderMode render_mode_`
- [ ] Renderer::render() 根据 `render_mode` 分发（PT / GS / imgui_only fallback）
- [ ] GS pass 实现 `on_resize()`（重建 tile 相关 buffer）
- [ ] 添加 DebugUI：RenderMode 下拉菜单、splat count 显示
- [ ] 编译验证
