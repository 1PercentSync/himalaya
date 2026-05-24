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
| 8 | 输出 Pass | TonemappingPass → PresentPass，Step 1 初版使用 SRGB/UNORM view 切换；Step 5.5 修订为始终 SRGB view + shader 内颜色空间处理 |
| 9 | Descriptor 绑定 | Set 0（GlobalUBO）+ Push Descriptor（Set 3） |
| 10 | Compute Pass 拆分 | 多阶段 compute 管线录制在单个 RG pass 内，阶段间手动 buffer barrier |
| 11 | 颜色空间处理 | GS 侧原样输出 SH 值，gamma 由 PresentPass view 选择控制 |
| 12 | 抗锯齿 | 直接实现 Mip Splatting（3D 频率限制 + 2D mip filter） |
| 13 | 多 Primitive 处理 | 核心属性合并 + SH 按 degree 分组 dispatch |
| 14 | 背景色 | 纯黑（vec4(0)） |
| 15 | Buffer barrier 策略 | 多阶段 GS compute 在单个 RG pass 内，阶段间手动 `vkCmdPipelineBarrier2` |
| 16 | 可见 splat 数量 | 投影 pass atomic counter + indirect dispatch |
| 17 | Depth key 编码 | camera distance 的 floatBitsToUint，升序 = 前到后 |
| 18 | Early termination | transmittance < 1/255 |
| 19 | RenderMode 流转 | Application::render_mode_ 枚举 → RenderInput::render_mode → Renderer 分发 |
| 20 | GsColorSpace 枚举位置 | `scene_data.h`，与 `RenderMode` 同级（均为渲染配置枚举，`FrameContext` 不定义枚举） |
| 21 | UNORM ImageView 存储 | Step 1 初版决策；Step 5.5 废弃，改为删除 `unorm_image_views` 并始终使用 SRGB swapchain view |
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
| 33 | Tile binning 清零策略 | Step 5 初版决策；Step 5.5 废弃 count/scatter 清零流程，改为 entry generation + sorted range build |
| 34 | Tile scatter 写游标 | Step 5 初版决策；Step 5.5 废弃 `tile_cursors[]` atomic append，避免 tile 内顺序不确定 |
| 35 | tile_splat_ids 容量策略 | Step 5 初版决策；Step 5.5 废弃 `tile_splat_ids[]`，改为 entry capacity + dropped stats |
| 36 | Tile prefix sum | Step 5 初版决策；Step 5.5 废弃 `gs_tile_scan.comp` 主流程，改为 sorted tile entry range build |
| 37 | PresentPass 色彩方案修订 | 废弃 SRGB/UNORM swapchain view 切换；swapchain 与 ImGui 始终使用 SRGB view，GS `srgb_rec709_display` 在 `present.frag` 内手动 sRGB→linear 后写入 SRGB attachment |
| 38 | Tile Binning 重构方向 | 废弃 depth sort → tile count/scan/scatter 的 atomic append 方案；改为生成 per-tile entry，执行 depth stable sort → tile-id stable sort → sorted entry range build |
| 39 | Tile entry 排序 key | 不扩展 64-bit radix sort；复用现有 32-bit stable RadixSort 两次排序，先 depth 后 tile-id，依赖第二次稳定排序保留 tile 内 depth 顺序 |
| 40 | Entry 容量策略 | 固定 entry capacity：`min(max_splat_count * kAvgTilesBudget, kMaxSortableEntries)`；初始 `kAvgTilesBudget = 16`，`kMaxSortableEntries = 16 * 1024 * 1024` |
| 41 | 可控退化诊断 | entry 容量不足时安全丢弃并记录统计；只要 dropped/invalid/clamped 计数为 0，即认为容量策略未导致画面偏离理想结果 |
| 42 | GS runtime stats | 增加 GPU stats buffer + per-frame readback buffer，延迟 1-2 帧读回 visible splats、entry requested/written/dropped、invalid count、sort clamped 等统计 |
| 43 | Projection group barrier | 多 SH group projection dispatch 之间插入 compute→compute buffer barrier，覆盖 counter、projected splats、depth key/value 等共享输出 |
| 44 | Sort prepare clamp | `gs_sort_prepare.comp` 根据 `min(counter, max_element_count)` 写 indirect dispatch，避免 counter 超容量时调度大量空 workgroup |
| 45 | Sort 规模上限 | Step 5.5 显式保护现有 scan 的 `chunk_count <= 256` 规模假设；超过当前可排序范围时 clamp 并记录 `sort_clamped` |
| 46 | Node transform 修正边界 | position-only transform 属于 correctness bug，但涉及 GPU core layout 和 projection shader，拆到 Step 5.6 处理 |
| 47 | GsGpuData 接入边界 | Renderer 持有并上传 GsGpuData 属于 Step 2 集成遗漏，拆到 Step 5.7 处理 |
| 48 | Compute helper 整理 | 重复 Vulkan compute 样板抽到 RHI 层 `compute_utils`，拆到 Step 5.8 处理，允许修改 `rhi/CMakeLists.txt` |
| 49 | RenderMode UI | 保留 `Path Tracing` checkbox；checked=PT，unchecked=GS。GS 完成前 checkbox 保持 disabled，拆到 Step 5.9 清理内部 RenderMode 流转 |
| 50 | RadixSort 性能 | 现有 stable scatter 的 O(256²) local rank 暂不优化；Step 6.5 完整 GS 跑通后 profiling 决定是否优化 |

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
- [x] `record()` 根据 RenderMode 和 `gs_color_space` 选择 SRGB/UNORM view 作为 color attachment（Step 5.5 将回退为始终 SRGB view + shader 内处理）
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
- [x] 实现 ping-pong buffer 管理（key[2] + value[2] + histogram）
- [x] 实现 4-pass 编排（每 pass 处理 8 bit，循环：histogram → scan → scatter）
- [x] 实现 `record()`：录制到 command buffer，阶段间插入 buffer barrier
- [x] 编译验证

> 输入为 Step 3 的 depth key + splat index，输出为排好序的 key-value 对。

## Step 5：Tile Binning

- [x] 创建 `shaders/gs/gs_tile_count.comp`（每个可见 splat 遍历其覆盖 tile，atomicAdd 每 tile 计数）
- [x] 创建 `shaders/gs/gs_tile_scan.comp`（对 `tile_counts[]` 做 exclusive prefix sum，输出 `tile_offsets[]`）
- [x] 创建 `shaders/gs/gs_tile_scatter.comp`（再次遍历可见 splat，按 offset + atomicAdd 写入 tile_splat_ids）
- [x] 创建 tile buffer：`tile_offsets[]`、`tile_counts[]`、`tile_cursors[]`、`tile_splat_ids[]`（tile 数量依赖屏幕分辨率，resize 时重建；`tile_splat_ids` 使用保守容量上限）
- [x] 创建 `passes/include/himalaya/passes/gs_tile_binning_pass.h` 和 `passes/src/gs_tile_binning_pass.cpp`
- [x] 编译验证

> 输入为 Step 4 排序后的 splat index + Step 3 的 2D 属性（tile 覆盖范围）。

## Step 5.5：Correctness Fixes + Tile Entry Pipeline

- [x] 回退 PresentPass / Swapchain 的 UNORM view 方案：删除 mutable swapchain、UNORM image view、SRGB/UNORM view 选择逻辑
- [x] 更新 `present.frag` push constant：加入 `gs_color_space`；GS `srgb_rec709_display` 输入先做精确 piecewise sRGB→linear，`lin_rec709_display` 直接输出 linear
- [x] Projection 多 SH group dispatch 后插入 compute→compute buffer barrier，确保 counter 和 projection 输出对下一组可见
- [x] `gs_sort_prepare.comp` 增加 `max_element_count` clamp，indirect dispatch 只覆盖 `min(counter, capacity)`
- [x] 明确 RadixSort 当前最大可排序 entry 数（`16 * 1024 * 1024`）并在 C++ 侧保护 scan `chunk_count <= 256` 假设
- [x] 将 Tile Binning 重构为 tile entry 生成：写入 `entry_depth_keys[]`、`entry_tile_ids[]`、`entry_splat_ids[]`、`entry_indices[]`
- [x] 实现 entry capacity 策略：`min(max_splat_count * 16, 16 * 1024 * 1024)`；容量不足时安全丢弃并累计 dropped count
- [x] 用现有 32-bit stable RadixSort 执行两次排序：先按 depth，再按 tile-id 稳定排序
- [x] 新增 gather pass：depth-sorted entry index → tile sort key/value
- [x] 新增 tile range build pass：从 sorted tile ids 生成 `tile_offsets[]` / `tile_counts[]`，替代原 `gs_tile_scan.comp` prefix-sum 流程
- [x] 增加 GS runtime stats GPU buffer 与 per-frame delayed readback buffer，缓存 visible splats、entry requested/written/dropped、invalid entries、sort clamped
- [x] Debug/log 中暴露可控退化指标；`GsColorSpace::Unknown` 在 GS 模式按 warning + `LinRec709Display` fallback 处理
- [x] 编译验证

## Step 5.6：GS Covariance GPU Layout + Node Transform 修正

- [x] 保留 CPU `GaussianSplatCore` 布局，新增 GPU 专用 core layout（position + opacity + world covariance 3×3）及 layout static_assert
- [x] `GsGpuData::upload()` 中计算 local covariance：`R * S² * Rᵀ`
- [x] `GsGpuData::upload()` 中将 node linear transform 合入 covariance：`M * C_local * Mᵀ`，position 继续变换到 world space
- [x] 更新 `gs_projection.comp`：直接读取 world covariance，不再从 rotation/scale 重建 covariance
- [x] 将 Mip Splatting 3D filter 适配为 covariance lower-bound 近似：`cov_world += min_variance * I`
- [x] 编译验证

## Step 5.7：GsGpuData 接入 Renderer

- [x] Renderer 持有 `framework::GsGpuData`，在 init/destroy 中初始化和销毁
- [x] 新增 `Renderer::upload_gs_scene(const GaussianSplatScene&)` 与 `Renderer::destroy_gs_scene()`
- [x] 新增 `Renderer::gs_splat_count()` 只读 accessor，供 DebugUI / stats 使用
- [x] `Application::switch_gs_scene()` 在成功加载 CPU GS scene 后通过 immediate scope 上传 GPU 数据
- [x] 切换或清空 GS scene 前销毁旧 GS GPU 数据，Renderer::destroy() 兜底销毁
- [x] 编译验证

## Step 5.8：RHI Compute Utility Refactor

- [ ] 新增 `rhi/include/himalaya/rhi/compute_utils.h` 和 `rhi/src/compute_utils.cpp`
- [ ] 修改 `rhi/CMakeLists.txt`，加入 `src/compute_utils.cpp`
- [ ] 抽取 storage buffer binding、push descriptor layout、descriptor buffer info、compute pipeline creation、buffer barrier helper
- [ ] 迁移 `RadixSort` 到 RHI compute utility
- [ ] 迁移 `GsProjectionPass` / `GsTileBinningPass` 到 RHI compute utility
- [ ] 编译验证

## Step 5.9：RenderMode Flow Cleanup

- [ ] `RenderInput` 新增 `framework::RenderMode render_mode`
- [ ] `Application::render()` 将 `pt_mode_` bool 转换为 `RenderMode` 传入 Renderer（checked=PT，unchecked=GS）
- [ ] `Renderer::render()` 根据 `input.render_mode` 分发；GS 路径完成前仍 fallback 到 imgui-only / 黑屏路径
- [ ] DebugUI 保留 `Path Tracing` checkbox；GS 完成前 checkbox disabled，不允许取消勾选
- [ ] 更新 UI tooltip 文案，移除过时的 “coming in Phase 2”
- [ ] 编译验证

## Step 6：Tile Rendering + 集成

- [ ] 创建 `shaders/gs/gs_tile_render.comp`（16×16 workgroup，前到后 alpha blend，transmittance < 1/255 early termination）
- [ ] 实现 2D Gaussian 求值（从椭圆主轴计算 Mahalanobis distance，3σ 截断）
- [ ] 实现 imageStore 写入 GS color buffer（背景初始化为黑色）
- [ ] 创建 `passes/include/himalaya/passes/gs_tile_render_pass.h` 和 `passes/src/gs_tile_render_pass.cpp`
- [ ] 创建 GS managed color buffer（R16G16B16A16F, Storage + Sampled, 屏幕尺寸）
- [ ] 在 Renderer 中实现 `render_gaussian_splatting()`：Projection → tile entry generation/sort/range build → tile render 录制在单个 RG pass 内，阶段间手动 `vkCmdPipelineBarrier2`
- [ ] Renderer::render() 在 GS 模式且 GS scene 已上传时走 `render_gaussian_splatting()`；无 GS scene 时走 imgui-only fallback
- [ ] GS pass 实现 `on_resize()`（重建屏幕尺寸相关 buffer）
- [ ] DebugUI 解锁 `Path Tracing` checkbox，显示 GS splat count 与 runtime stats
- [ ] 编译验证

## Step 6.5：GS Performance Review

- [ ] Profiling 完整 GS pipeline GPU 时间，重点记录 Projection、两次 RadixSort、tile range build、tile render
- [ ] 检查 `gs_sort_scatter.comp` 的 O(256²) local-rank 是否成为瓶颈
- [ ] 根据 entry count、dropped count、sort clamped、GPU 时间决定是否优化 RadixSort stable scatter 或扩展 sort 规模
